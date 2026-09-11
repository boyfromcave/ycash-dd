// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

/**
 * Yellowback wallet-context RPCs (plan §4.4): addresses, balances, mint,
 * send, redeem, submit, abort, co-sign, positions, history, lock maintenance.
 * Lock order: cs_main -> cs_wallet -> cs_yellowback (B15).
 */

#include "chainparams.h"
#include "coins.h"
#include "core_io.h"
#include "experimental_features.h"
#include "init.h"
#include "key_io.h"
#include "main.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "txmempool.h"
#include "utilstrencodings.h"
#include "wallet/wallet.h"
#include "yellowback/address.h"
#include "yellowback/index.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/policy.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/txbuilder.h"
#include "yellowback/wallet.h"

#include <univalue.h>

#include <algorithm>

using namespace yellowback;

void EnsureWalletIsUnlocked();

namespace {

YellowbackWallet& EnsureYW()
{
    if (!fExperimentalYellowback || !g_yellowback) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (Yellowback requires -experimentalfeatures -yellowback)");
    }
    if (!pwalletMain || !g_yellowbackWallet) throw JSONRPCError(RPC_WALLET_ERROR, "wallet is disabled");
    return *g_yellowbackWallet;
}

void EnsureHealthy(const YellowbackIndex& index)
{
    if (!index.IsHealthy()) {
        throw JSONRPCError(RPC_MISC_ERROR, "yellowback index unhealthy: " + index.UnhealthyReason() + "; restart with -reindex-yellowback");
    }
}

int IndexHeight(const YellowbackIndex& index)
{
    std::optional<TipRecord> tip = index.GetTip();
    return tip.has_value() ? tip->height : -1;
}

/** Commit a built transaction through the wallet: stage-(i) locks first (B4), then CommitTransaction. */
uint256 Commit(YellowbackWallet& yw, const BuiltTx& built, CReserveKey* reservekey)
{
    yw.LockOwn(built.ownYedOutputs);
    CWalletTx wtx(pwalletMain, CTransaction(built.tx));
    std::optional<std::reference_wrapper<CReserveKey>> rk;
    if (reservekey) rk = std::ref(*reservekey);
    if (!pwalletMain->CommitTransaction(wtx, rk)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "transaction commit failed: the transaction was rejected by the mempool");
    }
    return wtx.GetHash();
}

CScript ParseYedAddress(const std::string& s, const yellowback::Params& params)
{
    CKeyID id;
    if (!DecodeAddress(s, params, id)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "not a Yellowback address for this network (expected prefix '" + EncodeAddress(CKeyID(), params).substr(0, 2) + "')");
    return GetScriptForDestination(id);
}

std::string ScriptToYedAddress(const CScript& s, const yellowback::Params& params)
{
    CTxDestination dest;
    if (!ExtractDestination(s, dest)) return "";
    if (const CKeyID* id = std::get_if<CKeyID>(&dest)) return EncodeAddress(*id, params);
    return "";
}

} // namespace

UniValue yed_getnewaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error("yed_getnewaddress\n\nA fresh Yellowback (YED) address from the keypool, added to the address book.\n");
    YellowbackWallet& yw = EnsureYW();
    LOCK2(cs_main, pwalletMain->cs_wallet);
    if (!pwalletMain->IsLocked()) pwalletMain->TopUpKeyPool();
    CPubKey key;
    if (!pwalletMain->GetKeyFromPool(key)) throw JSONRPCError(RPC_WALLET_ERROR, "keypool ran out; call keypoolrefill first");
    pwalletMain->SetAddressBook(key.GetID(), "", "receive");
    return EncodeAddress(key.GetID(), yw.Index()->GetParams());
}

UniValue yed_validateaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error("yed_validateaddress \"address\"\n\nDecode a Yellowback address; reports whether it is mine.\n");
    YellowbackWallet& yw = EnsureYW();
    UniValue o(UniValue::VOBJ);
    CKeyID id;
    bool valid = DecodeAddress(params[0].get_str(), yw.Index()->GetParams(), id);
    o.pushKV("isvalid", valid);
    if (valid) {
        LOCK(pwalletMain->cs_wallet);
        o.pushKV("address", params[0].get_str());
        o.pushKV("keyid", id.GetHex());
        o.pushKV("ismine", pwalletMain->HaveKey(id));
        KeyIO keyIO(::Params());
        o.pushKV("transparentAddress", keyIO.EncodeDestination(id));
    }
    return o;
}

UniValue yed_getbalance(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
            "yed_getbalance\n\nConfirmed YED (from the index) and unconfirmed YED (mempool transactions that would assign YED to this wallet).\n"
            "Result: { \"confirmedCents\": n, \"unconfirmedCents\": n, \"height\": n }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    int64_t confirmed = yw.ConfirmedCents();
    int64_t unconfirmed = 0;
    {
        LOCK(mempool.cs);
        for (const auto& entry : mempool.mapTx) {
            const CTransaction& tx = entry.GetTx();
            std::optional<FoundPayload> fp = FindPayload(tx);
            if (!fp.has_value()) continue;
            if (fp->payload.type == PayloadType::MINT) {
                if (tx.vout.size() > 1 && yw.IsMineScript(tx.vout[1].scriptPubKey)) unconfirmed += fp->payload.cents;
            } else if (fp->payload.type == PayloadType::TRANSFER || fp->payload.type == PayloadType::REDEEM) {
                for (const Assignment& a : fp->payload.assignments) {
                    if (yw.IsMineScript(tx.vout[a.vout].scriptPubKey)) unconfirmed += a.cents;
                }
            }
        }
    }
    UniValue o(UniValue::VOBJ);
    o.pushKV("confirmedCents", confirmed);
    o.pushKV("unconfirmedCents", unconfirmed);
    o.pushKV("height", IndexHeight(index));
    return o;
}

UniValue yed_listunspent(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error("yed_listunspent\n\nYED outputs that are mine, with whether each is spendable (not spent by an unconfirmed transaction).\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    UniValue arr(UniValue::VARR);
    for (const YedCoin& c : yw.AllCoins()) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", c.outpoint.hash.GetHex());
        o.pushKV("vout", (int64_t)c.outpoint.n);
        o.pushKV("cents", c.token.cents);
        o.pushKV("valueZat", c.token.nValue);
        o.pushKV("address", ScriptToYedAddress(c.token.scriptPubKey, index.GetParams()));
        o.pushKV("height", c.token.height);
        o.pushKV("confirmations", IndexHeight(index) - c.token.height + 1);
        o.pushKV("spentUnconfirmed", pwalletMain->IsSpent(c.outpoint.hash, c.outpoint.n));
        o.pushKV("locked", pwalletMain->IsLockedCoin(c.outpoint.hash, c.outpoint.n));
        arr.push_back(o);
    }
    return arr;
}

UniValue yed_mint(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw std::runtime_error(
            "yed_mint cents tier ( \"from\" )\n"
            "\nMint YED: locks the required YEC collateral in a vault for the tier's period and creates the YED.\n"
            "The collateral requirement is fixed at the evaluation height (index tip minus the mint lag) and known before signing.\n"
            "Back up wallet.dat afterwards: the vault owner key is a fresh keypool key.\n"
            "\nArguments:\n"
            "1. cents   (numeric, required) amount of YED to mint, in cents\n"
            "2. tier    (numeric, required) lock tier\n"
            "3. \"from\"  (string, optional) fund the collateral from this address: an s1... address (its confirmed\n"
            "             outputs only) or a ys1... address (its Sapling notes, spent in the same transaction; the change\n"
            "             returns to it). Default: any confirmed transparent output of the wallet.\n"
            "\nResult: { \"txid\", \"vault\", \"lockHeight\", \"evalHeight\", \"expiryHeight\", \"collateralZat\", \"collateral\", \"fundedFrom\", \"from\", \"warning\" }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    int64_t cents = params[0].get_int64();
    int tier = params[1].get_int();
    const std::string from = params.size() > 2 ? params[2].get_str() : "";
    BuiltTx built;
    CReserveKey reservekey(pwalletMain);
    uint256 txid;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        EnsureWalletIsUnlocked();
        {
            LOCK(index.cs_yellowback);
            EnsureHealthy(index);
            try {
                built = BuildMint(yw, cents, tier, reservekey, from);
            } catch (const std::runtime_error& e) {
                throw JSONRPCError(RPC_WALLET_ERROR, e.what());
            }
        }
        if (!built.NeedsProving()) txid = Commit(yw, built, &reservekey);
    }
    if (built.NeedsProving()) {
        // Sapling shape (I2): the spend proofs take seconds; no lock is held while they are made.
        try {
            FinishSapling(built);
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
        LOCK2(cs_main, pwalletMain->cs_wallet);
        txid = Commit(yw, built, &reservekey);
    }
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("vault", txid.GetHex() + ":0");
    o.pushKV("lockHeight", (int64_t)built.lockHeight);
    o.pushKV("evalHeight", built.evalHeight);
    o.pushKV("expiryHeight", (int64_t)built.tx.nExpiryHeight);
    o.pushKV("collateralZat", built.collateralZat);
    o.pushKV("collateral", ValueFromAmount(built.collateralZat));
    o.pushKV("fundedFrom", built.fundedFrom);
    o.pushKV("from", from);
    o.pushKV("ownerKeyId", built.freshKey.GetID().GetHex());
    o.pushKV("warning", built.warning.empty() ? "back up wallet.dat: the vault owner key is a fresh keypool key" : built.warning);
    return o;
}

static UniValue DoSend(YellowbackWallet& yw, const std::vector<std::pair<CScript, int64_t>>& recipients)
{
    YellowbackIndex& index = *yw.Index();
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    BuiltTx built;
    CReserveKey reservekey(pwalletMain);
    {
        LOCK(index.cs_yellowback);
        EnsureHealthy(index);
        try {
            built = BuildTransfer(yw, recipients, reservekey);
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
    }
    uint256 txid = Commit(yw, built, &reservekey);
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("changeCents", built.changeCents);
    o.pushKV("expiryHeight", (int64_t)built.tx.nExpiryHeight);
    return o;
}

UniValue yed_send(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw std::runtime_error("yed_send \"yedaddress\" cents\n\nSend YED to a Yellowback address. Refuses s1… addresses and change below $1.00 (C20).\n");
    YellowbackWallet& yw = EnsureYW();
    CScript dest = ParseYedAddress(params[0].get_str(), yw.Index()->GetParams());
    int64_t cents = params[1].get_int64();
    return DoSend(yw, { { dest, cents } });
}

UniValue yed_sendmany(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error("yed_sendmany {\"yedaddress\":cents,...}\n\nSend YED to up to 14 Yellowback addresses in one transaction.\n");
    YellowbackWallet& yw = EnsureYW();
    UniValue obj = params[0].get_obj();
    std::vector<std::pair<CScript, int64_t>> recipients;
    for (const std::string& name : obj.getKeys()) {
        recipients.push_back({ ParseYedAddress(name, yw.Index()->GetParams()), obj[name].get_int64() });
    }
    return DoSend(yw, recipients);
}

UniValue yed_redeem(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw std::runtime_error(
            "yed_redeem \"vaultTxid\" ( \"to\" )\n"
            "\nBuild and owner-sign the redemption of a vault: burns the required YED and returns the collateral.\n"
            "No network I/O and nothing is broadcast: the vault script of this prototype still needs the retired\n"
            "federation's signatures, so the returned hex cannot be completed by this node (v2 replaces this command).\n"
            "\nArguments:\n"
            "1. \"vaultTxid\"  (string, required) the mint transaction id (the vault is its output 0)\n"
            "2. \"to\"         (string, optional) where the collateral goes: an s1... address, or a ys1... address\n"
            "                  (paid as a Sapling output). Default: a fresh transparent address of this wallet.\n"
            "\nResult: { \"hex\", \"vault\", \"requiredBurnCents\", \"burnCents\", \"changeCents\", \"collateralTo\", \"shielded\", \"expiryHeight\" }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    uint256 vaultTxid = ParseHashV(params[0], "vaultTxid");
    const std::string to = params.size() > 1 ? params[1].get_str() : "";
    BuiltTx built;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        EnsureWalletIsUnlocked();
        {
            LOCK(index.cs_yellowback);
            EnsureHealthy(index);
            try {
                built = BuildRedeem(yw, vaultTxid, to);
            } catch (const std::runtime_error& e) {
                throw JSONRPCError(RPC_WALLET_ERROR, e.what());
            }
        }
        if (!built.NeedsProving()) {
            try {
                SignRedeem(built, *pwalletMain, SignerBranchId());
            } catch (const std::runtime_error& e) {
                throw JSONRPCError(RPC_WALLET_ERROR, e.what());
            }
        }
    }
    if (built.NeedsProving()) {
        // Sapling shape (I2): prove the collateral output with no lock held, then re-lock to sign
        // the vault and YED inputs (scriptSigs are outside the ZIP-243 digest).
        try {
            FinishSapling(built);
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
        LOCK2(cs_main, pwalletMain->cs_wallet);
        EnsureWalletIsUnlocked();
        try {
            SignRedeem(built, *pwalletMain, SignerBranchId());
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
    }
    const CTransaction ownerSignedTx(built.tx);
    UniValue o(UniValue::VOBJ);
    o.pushKV("hex", EncodeHexTx(ownerSignedTx));
    o.pushKV("vault", vaultTxid.GetHex() + ":0");
    o.pushKV("requiredBurnCents", built.requiredBurn);
    o.pushKV("burnCents", built.burnCents);
    o.pushKV("changeCents", built.changeCents);
    o.pushKV("collateralTo", built.collateralTo);
    o.pushKV("shielded", !ownerSignedTx.vShieldedOutput.empty());
    o.pushKV("expiryHeight", (int64_t)built.tx.nExpiryHeight);
    return o;
}

UniValue yed_listpositions(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error("yed_listpositions ( \"status\" )\n\nThis wallet's vaults (owner key held), with canRedeem, requiredBurnCents and unlockHeight.\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    std::string status = params.size() > 0 && !params[0].isNull() ? params[0].get_str() : "";
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int h = IndexHeight(index);
    std::optional<Snapshot> snap = h >= 0 ? st.GetSnapshot((uint32_t)h) : std::nullopt;
    UniValue arr(UniValue::VARR);
    index.View().Iterate("V", [&](const std::string& k, const std::string& raw) {
        VaultRecord v;
        if (!DeserializeRecord(raw, v)) return true;
        if (!yw.IsMineVault(v)) return true;
        if (!status.empty() && status != VaultStatusName(v.Status())) return true;
        const uint256 txid(std::vector<unsigned char>(k.begin() + 1, k.begin() + 33));
        UniValue o(UniValue::VOBJ);
        o.pushKV("vaultTxid", txid.GetHex());
        o.pushKV("status", VaultStatusName(v.Status()));
        o.pushKV("mintedCents", v.mintedCents);
        o.pushKV("collateralZat", v.collateralZat);
        o.pushKV("lockHeight", (int64_t)v.lockHeight);
        o.pushKV("unlockHeight", (int64_t)v.lockHeight);
        o.pushKV("tier", (int)v.tier);
        o.pushKV("mintHeight", v.mintHeight);
        o.pushKV("rosterIndex", v.rosterIndex);
        o.pushKV("ownerKeyId", v.ownerPubKey.GetID().GetHex());
        const bool active = v.Status() == VaultStatus::ACTIVE;
        int64_t requiredBurn = 0;
        if (active) requiredBurn = RequiredBurn(v.mintedCents, snap.has_value() ? snap->errBps : 10000);
        o.pushKV("requiredBurnCents", requiredBurn);
        bool priceOk = !active || (snap.has_value() && snap->priceDefined);
        o.pushKV("canRedeem", v.IsOpen() && h >= (int)v.lockHeight && priceOk && v.rosterIndex >= 0);
        if (v.Status() == VaultStatus::VOID) o.pushKV("voidReason", v.voidReason);
        if (v.Status() == VaultStatus::CLOSED) {
            o.pushKV("closeHeight", v.closeHeight);
            o.pushKV("closingTxid", v.closingTxid.GetHex());
            o.pushKV("burnedCents", v.burnedCents);
        }
        arr.push_back(o);
        return true;
    });
    return arr;
}

UniValue yed_listtransactions(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw std::runtime_error(
            "yed_listtransactions ( count skip )\n"
            "\nThis wallet's Yellowback history from the index (mint, send, receive, burn, redeem), newest first,\n"
            "plus wallet transactions with a payload that expired unmined (\"expired\": true).\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    int count = params.size() > 0 && !params[0].isNull() ? params[0].get_int() : 100;
    int skip = params.size() > 1 && !params[1].isNull() ? params[1].get_int() : 0;
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int h = IndexHeight(index);
    const yellowback::Params& p = index.GetParams();

    struct Row { int height; UniValue o; };
    std::vector<Row> rows;
    index.View().Iterate("L", [&](const std::string& k, const std::string& raw) {
        TxLogRecord l;
        if (!DeserializeRecord(raw, l)) return true;
        int64_t received = 0, spent = 0;
        bool closedMine = false;
        for (const AssignedOutput& a : l.assigned) if (yw.IsMineScript(a.scriptPubKey)) received += a.cents;
        for (const AssignedOutput& a : l.spentTokens) if (yw.IsMineScript(a.scriptPubKey)) spent += a.cents;
        for (const COutPoint& c : l.closedVaults) {
            std::optional<VaultRecord> v = st.GetVault(c);
            if (v.has_value() && yw.IsMineVault(v.value())) closedMine = true;
        }
        if (received == 0 && spent == 0 && !closedMine) return true;
        std::string type;
        int64_t amount = 0;
        if (l.type == (uint8_t)PayloadType::MINT && received > 0) { type = "mint"; amount = received; }
        else if (closedMine) { type = "redeem"; amount = -spent; }
        else if (spent > 0 && received < spent && l.type == 0) { type = "burn"; amount = -(spent - received); }
        else if (spent > 0 && l.type != 0 && l.verdict != verdict::TRANSFER_OK && l.verdict != verdict::REDEEM_OK) { type = "burn"; amount = -(spent - received); }
        else if (spent > 0 && l.type == (uint8_t)PayloadType::TRANSFER) { type = "send"; amount = received - spent; }
        else if (spent > received) { type = "send"; amount = -(spent - received); }
        else { type = "receive"; amount = received - spent; }
        UniValue o(UniValue::VOBJ);
        const uint256 txid(std::vector<unsigned char>(k.begin() + 1, k.begin() + 33));
        o.pushKV("txid", txid.GetHex());
        o.pushKV("height", l.height);
        o.pushKV("confirmations", h - l.height + 1);
        o.pushKV("type", type);
        o.pushKV("verdict", l.verdict);
        o.pushKV("yedIn", l.yedIn);
        o.pushKV("yedOut", l.yedOut);
        o.pushKV("burned", l.burned);
        o.pushKV("amountCents", amount);
        o.pushKV("expired", false);
        rows.push_back({ l.height, o });
        return true;
    });
    // Wallet transactions with a payload that are in neither the index nor the mempool and are past their expiry (F6).
    for (const auto& kv : pwalletMain->mapWallet) {
        const CWalletTx& wtx = kv.second;
        std::optional<FoundPayload> fp = FindPayload(wtx);
        if (!fp.has_value()) continue;
        if (st.GetTxLog(kv.first).has_value() || mempool.exists(kv.first)) continue;
        if (wtx.nExpiryHeight == 0 || (int64_t)wtx.nExpiryHeight > (int64_t)chainActive.Height()) continue;
        if (wtx.GetDepthInMainChain() > 0) continue;
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", kv.first.GetHex());
        o.pushKV("height", -1);
        o.pushKV("confirmations", 0);
        o.pushKV("type", fp->payload.type == PayloadType::MINT ? "mint" : fp->payload.type == PayloadType::REDEEM ? "redeem" : "send");
        o.pushKV("verdict", "expired");
        o.pushKV("yedIn", 0);
        o.pushKV("yedOut", 0);
        o.pushKV("burned", 0);
        o.pushKV("amountCents", fp->payload.type == PayloadType::MINT ? (int64_t)fp->payload.cents : fp->payload.AssignedCents());
        o.pushKV("expired", true);
        rows.push_back({ (int)wtx.nExpiryHeight, o });
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.height > b.height; });
    UniValue arr(UniValue::VARR);
    for (size_t i = skip; i < rows.size() && (int)arr.size() < count; i++) arr.push_back(rows[i].o);
    (void)p;
    return arr;
}

UniValue yed_lockcoins(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error("yed_lockcoins\n\nRe-run Yellowback coin locking against the index (maintenance). Returns the outpoints locked.\n");
    YellowbackWallet& yw = EnsureYW();
    yw.Reconcile();
    UniValue arr(UniValue::VARR);
    for (const COutPoint& o : yw.Locked()) {
        UniValue e(UniValue::VOBJ);
        e.pushKV("txid", o.hash.GetHex());
        e.pushKV("vout", (int64_t)o.n);
        arr.push_back(e);
    }
    return arr;
}

static const CRPCCommand commands[] =
{ //  category     name                    actor (function)        okSafeMode
  //  -----------  ----------------------  ----------------------  ----------
    { "yellowback", "yed_getnewaddress",    &yed_getnewaddress,     true  },
    { "yellowback", "yed_validateaddress",  &yed_validateaddress,   true  },
    { "yellowback", "yed_getbalance",       &yed_getbalance,        false },
    { "yellowback", "yed_listunspent",      &yed_listunspent,       false },
    { "yellowback", "yed_mint",             &yed_mint,              false },
    { "yellowback", "yed_send",             &yed_send,              false },
    { "yellowback", "yed_sendmany",         &yed_sendmany,          false },
    { "yellowback", "yed_redeem",           &yed_redeem,            false },
    { "yellowback", "yed_listpositions",    &yed_listpositions,     false },
    { "yellowback", "yed_listtransactions", &yed_listtransactions,  false },
    { "yellowback", "yed_lockcoins",        &yed_lockcoins,         false },
};

void RegisterYellowbackWalletRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
