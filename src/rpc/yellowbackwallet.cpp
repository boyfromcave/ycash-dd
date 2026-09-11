// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

/**
 * Yellowback wallet-context RPCs (plan §4.5, §4.6; doc/yellowback-rpc.md is
 * the contract): addresses, balances, mint, send, redeem (incl. the VOID
 * release, L14), claim, sweep (L10), positions, history, lock maintenance.
 * Lock order: cs_main -> cs_wallet -> cs_yellowback (§4.3).
 *
 * Every refusal carries a stable identifier as the first token of its
 * message (M8): the builders throw std::runtime_error("<identifier>: …") and
 * ThrowBuildError maps the identifier to RPC_INVALID_PARAMETER (a bad
 * argument), RPC_WALLET_ERROR (funds, locking) or RPC_VERIFY_REJECTED (a rule).
 *
 * K7: yed_mint / yed_redeem / yed_claim run the index's MempoolCheck (the
 * MP-1 predicate) before CommitTransaction, which records the transaction
 * before it tries the mempool (wallet.cpp:5725, 5743) and must therefore
 * never be reached by a transaction MP-1 would refuse.
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
#include <cstring>
#include <functional>

using namespace yellowback;

void EnsureWalletIsUnlocked();

namespace {

const char* const SWEEP_ACKNOWLEDGEMENT = "I understand this leaves YED unbacked";

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
        throw JSONRPCError(RPC_MISC_ERROR, "yellowback-unhealthy: " + index.UnhealthyReason() + "; restart with -reindex-yellowback");
    }
}

int IndexHeight(const YellowbackIndex& index)
{
    std::optional<TipRecord> tip = index.GetTip();
    return tip.has_value() ? tip->height : -1;
}

bool StartsWith(const std::string& s, const char* prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

/** Map a builder failure to the RPC error code its identifier belongs to (doc/yellowback-rpc.md, Error identifiers). */
[[noreturn]] void ThrowBuildError(const std::runtime_error& e)
{
    const std::string msg = e.what();
    static const char* const RULE[] = { "mintpol-", "mint-unsatisfiable", "vault-locked", "claim-not-yet", "claim-not-underwater",
                                        "sweep-not-abandoned", "mempool-check-failed", nullptr };
    static const char* const PARAM[] = { "mint-bad-lock", "bad-mint-amount", "bad-xfer-amount", "vault-not-found", "vault-not-active",
                                         "vault-not-owned", "not-a-yellowback-address", "sweep-acknowledgement-missing", "bad-address", nullptr };
    for (const char* const* p = RULE; *p; p++) if (StartsWith(msg, *p)) throw JSONRPCError(RPC_VERIFY_REJECTED, msg);
    for (const char* const* p = PARAM; *p; p++) if (StartsWith(msg, *p)) throw JSONRPCError(RPC_INVALID_PARAMETER, msg);
    throw JSONRPCError(RPC_WALLET_ERROR, msg);
}

/** Commit a built transaction through the wallet: stage-(i) locks first (§4.6), then CommitTransaction. */
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

/**
 * K7: refuse with `mempool-check-failed:<verdict>` unless the index's MP-1 predicate admits the
 * transaction — MempoolCheckReason, the very predicate AcceptToMemoryPool applies (the expiry
 * bound and RED-1..4 over the one-transaction pseudo-block at tip + 1). cs_main held.
 */
void MempoolGate(YellowbackIndex& index, const CTransaction& tx)
{
    std::optional<std::string> why = index.MempoolCheckReason(tx);
    if (!why.has_value()) return;
    throw JSONRPCError(RPC_VERIFY_REJECTED, "mempool-check-failed:" + why.value() + ": an enforcing miner would refuse this vault spend (MP-1)");
}

CScript ParseYedAddress(const std::string& s, const yellowback::Params& params)
{
    CKeyID id;
    if (!DecodeAddress(s, params, id)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "not-a-yellowback-address: expected a Yellowback address of this network (prefix '" + EncodeAddress(CKeyID(), params).substr(0, 2) + "')");
    }
    return GetScriptForDestination(id);
}

std::string ScriptToYedAddress(const CScript& s, const yellowback::Params& params)
{
    CTxDestination dest;
    if (!ExtractDestination(s, dest)) return "";
    if (const CKeyID* id = std::get_if<CKeyID>(&dest)) return EncodeAddress(*id, params);
    return "";
}

/** A payee key hash as the P2PKH address (s1…/sm…), null when there is none (contract, Conventions). */
UniValue PayeeToJSON(const std::optional<CKeyID>& payee)
{
    if (!payee.has_value()) return NullUniValue;
    KeyIO keyIO(::Params());
    return UniValue(keyIO.EncodeDestination(CTxDestination(payee.value())));
}

UniValue PayeeToJSON(bool hasPayee, const uint160& payee)
{
    return PayeeToJSON(hasPayee ? std::optional<CKeyID>(CKeyID(payee)) : std::nullopt);
}

std::string ClassName(uint8_t termClass)
{
    return termClass < NUM_CLASSES ? std::string(1, (char)('A' + termClass)) : std::to_string((int)termClass);
}

/** The pClaim below which the vault is underwater: ceil(mintedCents * thresholdBps * COIN / collateralZat); nullopt for a VOID vault. */
std::optional<int64_t> UnderwaterAt(const VaultRecord& v, const yellowback::Params& p)
{
    if (v.mintedCents <= 0 || v.collateralZat <= 0 || v.Status() == VaultStatus::VOID) return std::nullopt;
    arith_uint256 rhs = arith_uint256(v.mintedCents) * arith_uint256(p.claimThresholdBps) * arith_uint256(COIN);
    arith_uint256 at = CeilDiv(rhs, arith_uint256(v.collateralZat));
    if (!FitsInt64(at)) return std::nullopt;
    return (int64_t)at.GetLow64();
}

/**
 * The yed_getvault row (contract): the Vaults record plus claimable / underwaterAt / sweepBefore.
 * Mirrors VaultToJSON in rpc/yellowback.cpp (Phase 3) field for field.
 */
UniValue VaultRow(const COutPoint& out, const VaultRecord& v, const State& st, const yellowback::Params& p, int tipHeight, bool abandoned)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", out.hash.GetHex());
    o.pushKV("vout", (int64_t)out.n);
    o.pushKV("status", VaultStatusName(v.Status()));
    o.pushKV("ownerPubKey", HexStr(v.ownerPubKey.begin(), v.ownerPubKey.end()));
    const CPubKey owner = v.OwnerKey();
    o.pushKV("ownerKeyId", owner.IsValid() ? owner.GetID().GetHex() : "");
    o.pushKV("ownerAddress", owner.IsValid() ? EncodeAddress(owner.GetID(), p) : "");
    o.pushKV("termClass", ClassName(v.termClass));
    o.pushKV("lockHeight", (int64_t)v.lockHeight);
    o.pushKV("claimHeight", (int64_t)v.claimHeight);
    o.pushKV("collateralZat", v.collateralZat);
    o.pushKV("collateral", ValueFromAmount(v.collateralZat));
    o.pushKV("mintedCents", v.mintedCents);
    o.pushKV("mintHeight", v.mintHeight);
    o.pushKV("refHeight", v.refHeight);
    o.pushKV("feePaidZat", v.feePaidZat);
    o.pushKV("closeHeight", v.IsOpen() ? NullUniValue : UniValue(v.closeHeight));
    o.pushKV("closingTxid", v.IsOpen() ? "" : v.closingTxid.GetHex());
    o.pushKV("burnedCents", v.burnedCents);
    o.pushKV("unbacked", v.unbacked);
    bool claimable = false;
    if (v.Status() == VaultStatus::ACTIVE && tipHeight >= v.claimHeight) {
        std::optional<Snapshot> S = SnapshotAt(st, p, tipHeight);
        std::optional<MicroUsd> pClaim = S.has_value() ? S->PClaim() : std::nullopt;
        claimable = IsUnderwater(v.collateralZat, pClaim, v.mintedCents, p.claimThresholdBps);
    }
    o.pushKV("claimable", claimable);
    std::optional<int64_t> at = UnderwaterAt(v, p);
    o.pushKV("underwaterAt", at.has_value() ? UniValue(at.value()) : NullUniValue);
    o.pushKV("voidReason", v.voidReason);
    // K3 / L10: a VOID vault's claim path is unpoliced after claimHeight; an ACTIVE vault's is, under abandonment.
    if (v.Status() == VaultStatus::VOID || (v.Status() == VaultStatus::ACTIVE && abandoned)) o.pushKV("sweepBefore", (int64_t)v.claimHeight);
    return o;
}

/**
 * Build, sign and commit a vault spend (yed_redeem / yed_claim / yed_sweep). The Sapling shape
 * releases every lock for the proving step, then re-locks to sign (§4.6). `gate` runs the K7
 * MempoolCheck before CommitTransaction.
 */
BuiltTx RunVaultSpend(YellowbackWallet& yw, std::function<BuiltTx()> build, bool ownerPath, bool gate, uint256& txid)
{
    YellowbackIndex& index = *yw.Index();
    BuiltTx built;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        EnsureWalletIsUnlocked();
        LOCK(index.cs_yellowback);
        EnsureHealthy(index);
        try {
            built = build();
            if (!built.NeedsProving()) SignVaultSpend(built, *pwalletMain, SignerBranchId(), ownerPath);
        } catch (const std::runtime_error& e) {
            ThrowBuildError(e);
        }
        if (!built.NeedsProving()) {
            if (gate) MempoolGate(index, CTransaction(built.tx));
            txid = Commit(yw, built, nullptr);
            return built;
        }
    }
    // Sapling shape: prove the collateral note with no lock held, then re-lock to sign the vault
    // and YED inputs (scriptSigs are outside the ZIP-243 digest).
    try {
        FinishSapling(built);
    } catch (const std::runtime_error& e) {
        throw JSONRPCError(RPC_WALLET_ERROR, e.what());
    }
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    try {
        SignVaultSpend(built, *pwalletMain, SignerBranchId(), ownerPath);
    } catch (const std::runtime_error& e) {
        ThrowBuildError(e);
    }
    if (gate) MempoolGate(index, CTransaction(built.tx));
    txid = Commit(yw, built, nullptr);
    return built;
}

UniValue SpendResult(const uint256& txid, const BuiltTx& built)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("burnedCents", built.burnCents);
    o.pushKV("feeZat", built.feeZat);
    o.pushKV("payee", PayeeToJSON(built.payee));
    o.pushKV("collateralOut", built.collateralOut);
    o.pushKV("to", built.collateralTo);
    return o;
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
        throw std::runtime_error(
            "yed_validateaddress \"address\"\n\nDecode a Yellowback address; reports whether it is mine. Never throws for a bad address:\n"
            "isvalid is false and reason is \"not-a-yellowback-address\".\n");
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
    o.pushKV("reason", valid ? "" : "not-a-yellowback-address");
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
    LOCK(mempool.cs);              // §4.3 lock order: mempool.cs before cs_yellowback
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    int64_t confirmed = yw.ConfirmedCents();
    int64_t unconfirmed = 0;
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
    UniValue o(UniValue::VOBJ);
    o.pushKV("confirmedCents", confirmed);
    o.pushKV("unconfirmedCents", unconfirmed);
    o.pushKV("height", IndexHeight(index));
    return o;
}

UniValue yed_listunspent(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error("yed_listunspent\n\nYED outputs that are mine, with whether each is spent by an unconfirmed transaction and whether it is locked.\n");
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
            "yed_mint cents lockBlocks ( \"from\" )\n"
            "\nMint YED: locks the required YEC collateral in a vault for lockBlocks blocks (the term class follows) and creates the YED.\n"
            "The collateral requirement, the enforcement fee and its payee are fixed at the reference height (index tip minus the\n"
            "mint lag) and known before signing. Back up wallet.dat afterwards: the vault owner key is a fresh keypool key.\n"
            "\nArguments:\n"
            "1. cents       (numeric, required) amount of YED to mint, in cents\n"
            "2. lockBlocks  (numeric, required) lock length in blocks (class A, B or C by range)\n"
            "3. \"from\"      (string, optional) fund the collateral from this address: an s1... address (its confirmed\n"
            "               outputs only) or a ys1... address (its Sapling notes, spent in the same transaction; the change\n"
            "               returns to it). Default: any confirmed transparent output of the wallet.\n"
            "\nResult: { \"txid\", \"vault\", \"termClass\", \"lockHeight\", \"claimHeight\", \"collateralZat\", \"feeZat\", \"payee\", \"fundedFrom\", \"warning\" }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    int64_t cents = params[0].get_int64();
    int lockBlocks = params[1].get_int();
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
                built = BuildMint(yw, cents, lockBlocks, reservekey, from);
            } catch (const std::runtime_error& e) {
                ThrowBuildError(e);
            }
            if (!built.NeedsProving()) MempoolGate(index, CTransaction(built.tx));   // K7 (trivially true for a mint)
        }
        if (!built.NeedsProving()) txid = Commit(yw, built, &reservekey);
    }
    if (built.NeedsProving()) {
        // Sapling shape (§4.6): the spend proofs take seconds; no lock is held while they are made.
        try {
            FinishSapling(built);
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_WALLET_ERROR, e.what());
        }
        LOCK2(cs_main, pwalletMain->cs_wallet);
        {
            LOCK(index.cs_yellowback);
            EnsureHealthy(index);
            MempoolGate(index, CTransaction(built.tx));
        }
        txid = Commit(yw, built, &reservekey);
    }
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("vault", txid.GetHex() + ":0");
    o.pushKV("termClass", ClassName((uint8_t)built.termClass));
    o.pushKV("lockHeight", (int64_t)built.lockHeight);
    o.pushKV("claimHeight", (int64_t)built.claimHeight);
    o.pushKV("collateralZat", built.collateralZat);
    o.pushKV("feeZat", built.feeZat);
    o.pushKV("payee", PayeeToJSON(built.payee));
    o.pushKV("fundedFrom", built.fundedFrom);
    o.pushKV("warning", built.warning);
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
            ThrowBuildError(e);
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
        throw std::runtime_error("yed_send \"yedaddress\" cents\n\nSend YED to a Yellowback address. Refuses other addresses (not-a-yellowback-address) and change below $1.00 (change-floor).\n");
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
            "\nRedeem an own vault in one step (V24). ACTIVE: burns exactly the vault's debt from this wallet's YED, pays the\n"
            "enforcement fee to an eligible pool and sends the rest of the collateral to \"to\"; refused unless an enforcing\n"
            "miner would accept it (mempool-check-failed:<verdict>). VOID: releases the collateral with no burn, no fee and\n"
            "no payload (L14). Both need the tip at or past lockHeight (vault-locked).\n"
            "\nArguments:\n"
            "1. \"vaultTxid\"  (string, required) the mint transaction id (the vault is its output 0)\n"
            "2. \"to\"         (string, optional) where the collateral goes: an s1... address, or a ys1... address\n"
            "                  (paid as a Sapling output). Default: a fresh transparent address of this wallet.\n"
            "\nResult: { \"txid\", \"burnedCents\", \"feeZat\", \"payee\", \"collateralOut\", \"to\" }\n");
    YellowbackWallet& yw = EnsureYW();
    uint256 vaultTxid = ParseHashV(params[0], "vaultTxid");
    const std::string to = params.size() > 1 ? params[1].get_str() : "";
    uint256 txid;
    // The VOID release spends no ACTIVE vault, so the K7 gate is a no-op for it; gate both alike.
    BuiltTx built = RunVaultSpend(yw, [&]() { return BuildRedeem(yw, vaultTxid, to); }, true, true, txid);
    return SpendResult(txid, built);
}

UniValue yed_claim(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw std::runtime_error(
            "yed_claim \"vaultTxid\" ( \"to\" )\n"
            "\nClaim somebody's underwater vault (yed_listclaimable): the claim-path spend at or past claimHeight, burning the\n"
            "vault's debt from this wallet's YED, paying the enforcement fee from the collateral and the rest to \"to\".\n"
            "Refused unless an enforcing miner would accept it (mempool-check-failed:<verdict>).\n"
            "\nArguments: as yed_redeem.\nResult: { \"txid\", \"burnedCents\", \"feeZat\", \"payee\", \"collateralOut\", \"to\" }\n");
    YellowbackWallet& yw = EnsureYW();
    uint256 vaultTxid = ParseHashV(params[0], "vaultTxid");
    const std::string to = params.size() > 1 ? params[1].get_str() : "";
    uint256 txid;
    BuiltTx built = RunVaultSpend(yw, [&]() { return BuildClaim(yw, vaultTxid, to); }, false, true, txid);
    return SpendResult(txid, built);
}

UniValue yed_sweep(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw std::runtime_error(
            std::string("yed_sweep \"vaultTxid\" \"acknowledgement\" ( \"to\" )\n"
            "\nSweep an own ACTIVE vault under abandonment (L10): an owner-path spend with no burn and no fee, built only while\n"
            "yed_getinfo.abandoned is true (sweep-not-abandoned otherwise). The vault is then CLOSED with unbacked = true.\n"
            "After claimHeight the claim path is anyone-can-spend and nobody enforces RED-4: sweep before it or lose the\n"
            "collateral. Also returns the raw hex so it can be submitted to any other node.\n"
            "\nArguments:\n"
            "1. \"vaultTxid\"        (string, required)\n"
            "2. \"acknowledgement\"  (string, required) exactly \"") + SWEEP_ACKNOWLEDGEMENT + "\"\n"
            "3. \"to\"               (string, optional) as yed_redeem\n"
            "\nResult: { \"txid\", \"hex\", \"collateralOut\", \"to\", \"unbackedCents\" }\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    uint256 vaultTxid = ParseHashV(params[0], "vaultTxid");
    if (params[1].get_str() != SWEEP_ACKNOWLEDGEMENT) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("sweep-acknowledgement-missing: the second argument must be exactly \"") + SWEEP_ACKNOWLEDGEMENT + "\"");
    }
    const std::string to = params.size() > 2 ? params[2].get_str() : "";
    uint256 txid;
    int64_t unbackedCents = 0;
    BuiltTx built = RunVaultSpend(yw, [&]() {
        // The §4.6 predicate from Snapshots alone (L12), never the node's own -yellowbackenforce.
        if (!index.IsAbandoned()) throw std::runtime_error(strprintf("sweep-not-abandoned: the chain does not show abandonment (ENFORCEMENT set for %d blocks)", index.GetParams().abandonBlocks));
        BuiltTx b = BuildSweep(yw, vaultTxid, to);
        std::optional<VaultRecord> v = State(index.View()).GetVault(COutPoint(vaultTxid, 0));
        unbackedCents = v.has_value() ? v->mintedCents : 0;
        return b;
    }, true, false, txid);
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("hex", EncodeHexTx(CTransaction(built.tx)));
    o.pushKV("collateralOut", built.collateralOut);
    o.pushKV("to", built.collateralTo);
    o.pushKV("unbackedCents", unbackedCents);
    return o;
}

UniValue yed_listpositions(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
            "yed_listpositions ( \"status\" )\n"
            "\nThis wallet's vaults (owner key held): every yed_getvault field plus canRedeem (ACTIVE or VOID at or past\n"
            "lockHeight; for VOID the release), canClaim (claimable and the wallet holds the debt) and canSweep (ACTIVE while abandoned).\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    std::string status = params.size() > 0 && !params[0].isNull() ? params[0].get_str() : "";
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const yellowback::Params& p = index.GetParams();
    const int h = IndexHeight(index);
    const bool abandoned = index.IsAbandoned();
    const int64_t balance = yw.ConfirmedCents();
    UniValue arr(UniValue::VARR);
    index.View().Iterate("V", [&](const std::string& k, const std::string& raw) {
        VaultRecord v;
        if (!DeserializeRecord(raw, v)) return true;
        if (!yw.IsMineVault(v)) return true;
        if (!status.empty() && status != VaultStatusName(v.Status())) return true;
        const COutPoint out(keys::OutPointHashOf(k), keys::OutPointIndexOf(k));
        UniValue o = VaultRow(out, v, st, p, h, abandoned);
        const bool active = v.Status() == VaultStatus::ACTIVE;
        o.pushKV("canRedeem", v.IsOpen() && h >= v.lockHeight);
        o.pushKV("canClaim", o["claimable"].get_bool() && balance >= v.mintedCents);
        o.pushKV("canSweep", active && abandoned && h >= v.lockHeight);
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
            "\nThis wallet's Yellowback history from the index, newest first: type is mint, send, receive, burn, redeem,\n"
            "claim (this wallet claimed), claimed (an own vault was claimed), sweep (an own vault swept), plus own\n"
            "transactions with a payload that expired unmined (\"expired\": true, verdict \"expired\").\n");
    YellowbackWallet& yw = EnsureYW();
    YellowbackIndex& index = *yw.Index();
    int count = params.size() > 0 && !params[0].isNull() ? params[0].get_int() : 100;
    int skip = params.size() > 1 && !params[1].isNull() ? params[1].get_int() : 0;
    LOCK2(cs_main, pwalletMain->cs_wallet);
    LOCK(mempool.cs);              // §4.3 lock order: mempool.cs (mempool.exists below) before cs_yellowback
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int h = IndexHeight(index);

    struct Row { int height; UniValue o; };
    std::vector<Row> rows;
    index.View().Iterate("L", [&](const std::string& k, const std::string& raw) {
        TxLogRecord l;
        if (!DeserializeRecord(raw, l)) return true;
        const uint256 txid = keys::OutPointHashOf(k);
        int64_t received = 0, spent = 0;
        bool closedMine = false, unbacked = false;
        for (const AssignedOutput& a : l.assigned) if (yw.IsMineScript(a.scriptPubKey)) received += a.cents;
        for (const AssignedOutput& a : l.spentTokens) if (yw.IsMineScript(a.scriptPubKey)) spent += a.cents;
        for (const COutPoint& c : l.closedVaults) {
            std::optional<VaultRecord> v = st.GetVault(c);
            if (v.has_value() && yw.IsMineVault(v.value())) {
                closedMine = true;
                if (v->unbacked) unbacked = true;
            }
        }
        bool mintMine = false;
        if (l.Type() == TxLogType::MINT) {
            std::optional<VaultRecord> v = st.GetVault(COutPoint(txid, 0));
            mintMine = v.has_value() && yw.IsMineVault(v.value());
        }
        if (received == 0 && spent == 0 && !closedMine && !mintMine) return true;
        std::string type;
        int64_t amount = received - spent;
        const bool ok = l.verdict == verdict::OK;
        if (l.Type() == TxLogType::MINT) { type = "mint"; }
        else if (closedMine) {
            if (ok) type = l.path == "claim" ? "claimed" : "redeem";
            else type = l.path == "owner" ? "sweep" : "claimed";
        }
        else if (l.Type() == TxLogType::REDEEM && ok && l.path == "claim" && spent > 0) { type = "claim"; }
        else if (spent > 0 && !ok && l.verdict != verdict::BURNED) { type = "burn"; }
        else if (spent > received) { type = "send"; }
        else { type = "receive"; }
        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", txid.GetHex());
        o.pushKV("height", l.height);
        o.pushKV("confirmations", h - l.height + 1);
        o.pushKV("type", type);
        o.pushKV("verdict", l.verdict);
        o.pushKV("path", l.path);
        o.pushKV("yedIn", l.yedIn);
        o.pushKV("yedOut", l.yedOut);
        o.pushKV("burned", l.burned);
        o.pushKV("amountCents", amount);
        o.pushKV("feeZat", l.feeZat);
        o.pushKV("payee", PayeeToJSON(l.hasPayee, l.payee));
        o.pushKV("unbacked", unbacked);
        o.pushKV("expired", false);
        rows.push_back({ l.height, o });
        return true;
    });
    // Wallet transactions with a payload that are in neither the index nor the mempool and are past their expiry (§4.6).
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
        o.pushKV("path", "");
        o.pushKV("yedIn", 0);
        o.pushKV("yedOut", 0);
        o.pushKV("burned", 0);
        o.pushKV("amountCents", 0);
        o.pushKV("feeZat", 0);
        o.pushKV("payee", NullUniValue);
        o.pushKV("unbacked", false);
        o.pushKV("expired", true);
        rows.push_back({ (int)wtx.nExpiryHeight, o });
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.height > b.height; });
    UniValue arr(UniValue::VARR);
    for (size_t i = skip; i < rows.size() && (int)arr.size() < count; i++) arr.push_back(rows[i].o);
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
    { "yellowback", "yed_claim",            &yed_claim,             false },
    { "yellowback", "yed_sweep",            &yed_sweep,             false },
    { "yellowback", "yed_listpositions",    &yed_listpositions,     false },
    { "yellowback", "yed_listtransactions", &yed_listtransactions,  false },
    { "yellowback", "yed_lockcoins",        &yed_lockcoins,         false },
};

void RegisterYellowbackWalletRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
