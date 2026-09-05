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
        throw std::runtime_error("yed_listunspent\n\nYED outputs that are mine, with whether each is spendable (not reserved by a pending redemption, not spent by an unconfirmed transaction).\n");
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
        o.pushKV("reserved", yw.IsReserved(c.outpoint));
        o.pushKV("spentUnconfirmed", pwalletMain->IsSpent(c.outpoint.hash, c.outpoint.n));
        o.pushKV("locked", pwalletMain->IsLockedCoin(c.outpoint.hash, c.outpoint.n));
        arr.push_back(o);
    }
    return arr;
}

UniValue yed_mint(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw std::runtime_error(
            "yed_mint cents tier\n"
            "\nMint YED: locks the required YEC collateral in a vault for the tier's period and creates the YED.\n"
            "The collateral requirement is fixed at the evaluation height (index tip minus the mint lag) and known before signing.\n"
            "Back up wallet.dat afterwards: the vault owner key is a fresh keypool key.\n"
            "\nResult: { \"txid\", \"vault\", \"lockHeight\", \"evalHeight\", \"expiryHeight\", \"collateralZat\", \"collateral\", \"warning\" }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    int64_t cents = params[0].get_int64();
    int tier = params[1].get_int();
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    BuiltTx built;
    CReserveKey reservekey(pwalletMain);
    {
        LOCK(index.cs_yellowback);
        EnsureHealthy(index);
        try {
            built = BuildMint(yw, cents, tier, reservekey);
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
    }
    uint256 txid = Commit(yw, built, &reservekey);
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("vault", txid.GetHex() + ":0");
    o.pushKV("lockHeight", (int64_t)built.lockHeight);
    o.pushKV("evalHeight", built.evalHeight);
    o.pushKV("expiryHeight", (int64_t)built.tx.nExpiryHeight);
    o.pushKV("collateralZat", built.collateralZat);
    o.pushKV("collateral", ValueFromAmount(built.collateralZat));
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
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_redeem \"vaultTxid\"\n"
            "\nBuild and owner-sign the redemption of a vault: burns the required YED and returns the collateral.\n"
            "No network I/O: returns the hex to pass to each operator's yed_cosignredeem (or /cosign) and then to\n"
            "yed_submitredeem. Records a pending redemption for the vault; yed_abortredeem clears it.\n"
            "\nResult: { \"hex\", \"vault\", \"roster\": {k,n,index,pubkeys}, \"requiredBurnCents\", \"burnCents\", \"changeCents\", \"expiryHeight\", \"deadlineHeight\" }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    uint256 vaultTxid = ParseHashV(params[0], "vaultTxid");
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    BuiltTx built;
    std::vector<RosterRecord> rosters;
    VaultRecord vault;
    {
        LOCK(index.cs_yellowback);
        EnsureHealthy(index);
        try {
            built = BuildRedeem(yw, vaultTxid);
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
        State st(index.View());
        rosters = st.GetRosters();
        vault = st.GetVault(COutPoint(vaultTxid, 0)).value();
    }
    PendingRedemption pr;
    pr.vault = COutPoint(vaultTxid, 0);
    pr.ownerSignedTx = CTransaction(built.tx);
    pr.reservedInputs = built.yedInputs;
    pr.createdHeight = chainActive.Height();
    pr.expiryHeight = built.tx.nExpiryHeight;
    if (!yw.AddPending(pr)) throw JSONRPCError(RPC_WALLET_ERROR, "a redemption of this vault is already pending");

    UniValue o(UniValue::VOBJ);
    o.pushKV("hex", EncodeHexTx(pr.ownerSignedTx));
    o.pushKV("vault", vaultTxid.GetHex() + ":0");
    UniValue r(UniValue::VOBJ);
    Roster roster;
    if (vault.rosterIndex >= 0 && vault.rosterIndex < (int)rosters.size() && ParseRosterScript(rosters[vault.rosterIndex].script, roster)) {
        r.pushKV("index", vault.rosterIndex);
        r.pushKV("k", (int64_t)roster.k);
        r.pushKV("n", (int64_t)roster.n());
        UniValue keys(UniValue::VARR);
        for (const CPubKey& key : roster.keys) keys.push_back(HexStr(key.begin(), key.end()));
        r.pushKV("pubkeys", keys);
    }
    o.pushKV("roster", r);
    o.pushKV("requiredBurnCents", built.requiredBurn);
    o.pushKV("burnCents", built.burnCents);
    o.pushKV("changeCents", built.changeCents);
    o.pushKV("expiryHeight", (int64_t)built.tx.nExpiryHeight);
    o.pushKV("deadlineHeight", (int64_t)built.tx.nExpiryHeight - (int64_t)TX_EXPIRING_SOON_THRESHOLD - 1);
    return o;
}

UniValue yed_submitredeem(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_submitredeem \"hex\"\n"
            "\nVerify a co-signed redemption against the pending record (SUB-1: identical except for added quorum\n"
            "signatures; every input verifies; at or past lockHeight; not expiring soon) and broadcast it.\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    CTransaction tx;
    if (!DecodeHexTx(tx, params[0].get_str())) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    if (tx.vin.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "no inputs");
    LOCK2(cs_main, pwalletMain->cs_wallet);
    std::optional<PendingRedemption> pr = yw.GetPending(tx.vin[0].prevout);
    if (!pr.has_value()) throw JSONRPCError(RPC_INVALID_PARAMETER, "no pending redemption for vin[0]'s vault (SUB-1)");
    std::string why;
    if (!SameExceptSignatures(pr->ownerSignedTx, tx, why)) throw JSONRPCError(RPC_VERIFY_REJECTED, "SUB-1: returned transaction differs from the one yed_redeem produced: " + why);
    VaultRecord vault;
    {
        LOCK(index.cs_yellowback);
        EnsureHealthy(index);
        std::optional<VaultRecord> v = State(index.View()).GetVault(pr->vault);
        if (!v.has_value() || !v->IsOpen()) throw JSONRPCError(RPC_VERIFY_REJECTED, "vault is no longer open");
        vault = v.value();
    }
    if (chainActive.Height() < (int)vault.lockHeight) throw JSONRPCError(RPC_VERIFY_REJECTED, strprintf("chain height %d is below lockHeight %u (C22)", chainActive.Height(), vault.lockHeight));
    if ((int64_t)tx.nExpiryHeight < (int64_t)chainActive.Height() + 1 + (int64_t)TX_EXPIRING_SOON_THRESHOLD) {
        throw JSONRPCError(RPC_VERIFY_REJECTED, "transaction is expiring too soon; yed_abortredeem and start over");
    }
    CCoinsView dummy;
    CCoinsViewCache view(&dummy);
    FetchInputs(tx, view);
    std::string error;
    if (!VerifyAllInputs(tx, view, SignerBranchId(), error)) throw JSONRPCError(RPC_VERIFY_REJECTED, "SUB-1: " + error);

    BuiltTx built;
    built.tx = CMutableTransaction(tx);
    for (const Assignment& a : FindPayload(tx).has_value() ? FindPayload(tx)->payload.assignments : std::vector<Assignment>()) {
        if (yw.IsMineScript(tx.vout[a.vout].scriptPubKey)) built.ownYedOutputs.push_back(COutPoint(tx.GetHash(), a.vout));
    }
    uint256 txid = Commit(yw, built, nullptr);
    yw.RemovePending(pr->vault);
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("quorumSignatures", (int64_t)CountQuorumSignatures(tx));
    return o;
}

UniValue yed_abortredeem(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error("yed_abortredeem \"vaultTxid\"\n\nClear the pending redemption of a vault that was never broadcast; its YED inputs become selectable again.\n");
    YellowbackWallet& yw = EnsureYW();
    uint256 vaultTxid = ParseHashV(params[0], "vaultTxid");
    UniValue o(UniValue::VOBJ);
    o.pushKV("aborted", yw.RemovePending(COutPoint(vaultTxid, 0)));
    return o;
}

UniValue yed_cosignredeem(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_cosignredeem \"hex\"\n"
            "\nFederation member: verify a redemption (RED-0..8) and add this node's roster signature in roster order.\n"
            "Refuses with the failing rule; \"transient\" refusals (RED-0 unsynced, RED-2 not yet at lockHeight here)\n"
            "should be retried after the next block. Never signs twice for the same vault within one height.\n"
            "\nResult: { \"hex\", \"quorumSignatures\", \"k\", \"complete\", \"check\": {...} }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    CTransaction tx;
    if (!DecodeHexTx(tx, params[0].get_str())) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    CCoinsView dummy;
    CCoinsViewCache view(&dummy);
    FetchInputs(tx, view);
    RedeemCheck check;
    Roster roster;
    {
        LOCK(index.cs_yellowback);
        check = CheckRedeem(index, view, tx, chainActive.Height());
        if (check.ok) {
            std::vector<RosterRecord> rosters = State(index.View()).GetRosters();
            ParseRosterScript(rosters[check.vaultRecord.rosterIndex].script, roster);
        }
    }
    if (!check.ok) {
        UniValue err(UniValue::VOBJ);
        err.pushKV("rule", check.rule);
        err.pushKV("reason", check.reason);
        err.pushKV("transient", check.transient);
        throw JSONRPCError(RPC_VERIFY_REJECTED, check.rule + ": " + check.reason + (check.transient ? " (transient)" : ""));
    }
    if (!yw.MarkCosigned(check.vault, check.indexHeight)) {
        throw JSONRPCError(RPC_VERIFY_REJECTED, "already co-signed this vault at this height");
    }
    CMutableTransaction mtx(tx);
    unsigned int n;
    try {
        n = AddCosignature(mtx, check.vaultScript, check.vaultRecord.collateralZat, roster, *pwalletMain, check.branchId);
    } catch (const std::runtime_error& e) {
        throw JSONRPCError(RPC_WALLET_ERROR, e.what());
    }
    UniValue o(UniValue::VOBJ);
    o.pushKV("hex", EncodeHexTx(CTransaction(mtx)));
    o.pushKV("quorumSignatures", (int64_t)n);
    o.pushKV("k", (int64_t)roster.k);
    o.pushKV("complete", n >= roster.k);
    UniValue c(UniValue::VOBJ);
    c.pushKV("burned", check.burned);
    c.pushKV("requiredBurn", check.requiredBurn);
    c.pushKV("fee", check.fee);
    c.pushKV("indexHeight", check.indexHeight);
    o.pushKV("check", c);
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
        o.pushKV("pending", yw.GetPending(COutPoint(txid, 0)).has_value());
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
    { "yellowback", "yed_submitredeem",     &yed_submitredeem,      false },
    { "yellowback", "yed_abortredeem",      &yed_abortredeem,       true  },
    { "yellowback", "yed_cosignredeem",     &yed_cosignredeem,      false },
    { "yellowback", "yed_listpositions",    &yed_listpositions,     false },
    { "yellowback", "yed_listtransactions", &yed_listtransactions,  false },
    { "yellowback", "yed_lockcoins",        &yed_lockcoins,         false },
};

void RegisterYellowbackWalletRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
