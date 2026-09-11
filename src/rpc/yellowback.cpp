// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

/**
 * Yellowback node-context RPCs (plan §4.4). They work without a wallet (C21)
 * and are gated by -experimentalfeatures -yellowback. Every reply that reads
 * the index reports the index height it answers for; "synced" is whether
 * that height is chainActive.Tip().
 *
 * Lock order: cs_main, then cs_yellowback (plan B15).
 */

#include "chainparams.h"
#include "coins.h"
#include "consensus/upgrades.h"
#include "core_io.h"
#include "experimental_features.h"
#include "key_io.h"
#include "main.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "txmempool.h"
#include "utilstrencodings.h"
#include "yellowback/index.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/view.h"

#include <univalue.h>

using namespace yellowback;

static const int YELLOWBACK_RPC_VERSION = 1;

namespace {

YellowbackIndex& EnsureIndex()
{
    if (!fExperimentalYellowback || !g_yellowback) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (Yellowback requires -experimentalfeatures -yellowback)");
    }
    return *g_yellowback;
}

void EnsureHealthy(const YellowbackIndex& index)
{
    AssertLockHeld(index.cs_yellowback);
    if (!index.IsHealthy()) {
        throw JSONRPCError(RPC_MISC_ERROR, "yellowback index unhealthy: " + index.UnhealthyReason() + "; restart with -reindex-yellowback");
    }
}

int IndexHeight(const YellowbackIndex& index)
{
    std::optional<TipRecord> tip = index.GetTip();
    return tip.has_value() ? tip->height : -1;
}

std::string ScriptAddress(const CScript& scriptPubKey)
{
    CTxDestination dest;
    if (!ExtractDestination(scriptPubKey, dest)) return "";
    KeyIO keyIO(::Params());
    return keyIO.EncodeDestination(dest);
}

UniValue OutPointToJSON(const COutPoint& out)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", out.hash.GetHex());
    o.pushKV("vout", (int64_t)out.n);
    return o;
}

UniValue VaultToJSON(const COutPoint& out, const VaultRecord& v)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", out.hash.GetHex());
    o.pushKV("vout", (int64_t)out.n);
    o.pushKV("status", VaultStatusName(v.Status()));
    o.pushKV("ownerPubKey", HexStr(v.ownerPubKey.begin(), v.ownerPubKey.end()));
    const CPubKey owner = v.OwnerKey();
    o.pushKV("ownerKeyId", owner.IsValid() ? owner.GetID().GetHex() : "");
    o.pushKV("termClass", v.termClass < NUM_CLASSES ? std::string(1, (char)('A' + v.termClass)) : std::to_string((int)v.termClass));
    o.pushKV("lockHeight", (int64_t)v.lockHeight);
    o.pushKV("claimHeight", (int64_t)v.claimHeight);
    o.pushKV("collateralZat", v.collateralZat);
    o.pushKV("collateral", ValueFromAmount(v.collateralZat));
    o.pushKV("mintedCents", v.mintedCents);
    o.pushKV("mintHeight", v.mintHeight);
    o.pushKV("refHeight", v.refHeight);
    o.pushKV("feePaidZat", v.feePaidZat);
    o.pushKV("voidReason", v.voidReason);
    o.pushKV("closeHeight", v.closeHeight);
    o.pushKV("closingTxid", v.IsOpen() ? NullUniValue : UniValue(v.closingTxid.GetHex()));
    o.pushKV("burnedCents", v.burnedCents);
    o.pushKV("unbacked", v.unbacked);
    return o;
}

UniValue TxLogToJSON(const uint256& txid, const TxLogRecord& l)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("height", l.height);
    o.pushKV("type", TxLogTypeName(l.Type()));
    o.pushKV("path", l.path);
    o.pushKV("verdict", l.verdict);
    o.pushKV("yedIn", l.yedIn);
    o.pushKV("yedOut", l.yedOut);
    o.pushKV("burned", l.burned);
    o.pushKV("feeZat", l.feeZat);
    o.pushKV("payee", l.hasPayee ? UniValue(l.payee.GetHex()) : NullUniValue);
    UniValue assigned(UniValue::VARR);
    for (const AssignedOutput& a : l.assigned) {
        UniValue e = OutPointToJSON(a.outpoint);
        e.pushKV("cents", a.cents);
        e.pushKV("address", ScriptAddress(a.scriptPubKey));
        assigned.push_back(e);
    }
    o.pushKV("assigned", assigned);
    UniValue spent(UniValue::VARR);
    for (const AssignedOutput& a : l.spentTokens) {
        UniValue e = OutPointToJSON(a.outpoint);
        e.pushKV("cents", a.cents);
        e.pushKV("address", ScriptAddress(a.scriptPubKey));
        spent.push_back(e);
    }
    o.pushKV("spentTokens", spent);
    UniValue closed(UniValue::VARR);
    for (const COutPoint& c : l.closedVaults) closed.push_back(OutPointToJSON(c));
    o.pushKV("closedVaults", closed);
    return o;
}

UniValue PriceOrNull(std::optional<int64_t> v)
{
    return v.has_value() ? UniValue(v.value()) : NullUniValue;
}

UniValue ActivationToJSON(const Activation& a)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("status", ActivationStatusName(a.Status()));
    o.pushKV("lockInHeight", a.lockInHeight);
    o.pushKV("activateHeight", a.activateHeight);
    return o;
}

UniValue HaltMaskToJSON(uint32_t mask)
{
    UniValue arr(UniValue::VARR);
    for (const std::string& n : HaltMaskNames(mask)) arr.push_back(n);
    return arr;
}

/** A yed_gethistory row (§3.6 Snapshots; undefined prices render null). */
UniValue SnapshotToJSON(int height, const Snapshot& s)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", height);
    o.pushKV("blockHash", s.blockHash.GetHex());
    o.pushKV("tagged", s.tagged);
    o.pushKV("quote", s.quote);
    o.pushKV("signalCount", (int64_t)s.signalCount);
    o.pushKV("activation", ActivationToJSON(s.activation));
    o.pushKV("pFast", PriceOrNull(s.PFast()));
    o.pushKV("pMid", PriceOrNull(s.PMid()));
    o.pushKV("pSlow", PriceOrNull(s.PSlow()));
    o.pushKV("pMint", PriceOrNull(s.PMint()));
    o.pushKV("pClaim", PriceOrNull(s.PClaim()));
    o.pushKV("sigmaMultBps", s.sigmaMultBps);
    o.pushKV("issuedZat", s.issuedZat);
    o.pushKV("supplyCents", s.supplyCents);
    o.pushKV("collateralZat", s.collateralZat);
    o.pushKV("globalRatioBps", PriceOrNull(s.GlobalRatioBps()));
    o.pushKV("haltMask", HaltMaskToJSON(s.haltMask));
    return o;
}

UniValue PayloadToJSON(const Payload& p)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("type", PayloadTypeName(p.type));
    switch (p.type) {
    case PayloadType::MINT:
        o.pushKV("termClass", (int)p.termClass);
        o.pushKV("cents", (int64_t)p.cents);
        o.pushKV("lockHeight", (int64_t)p.lockHeight);
        o.pushKV("refHeight", (int64_t)p.refHeight);
        o.pushKV("ownerPubKey", HexStr(p.ownerKeyBytes.begin(), p.ownerKeyBytes.end()));
        o.pushKV("feeVout", (int)p.feeVout);
        break;
    case PayloadType::TRANSFER:
    case PayloadType::REDEEM: {
        if (p.type == PayloadType::REDEEM) {
            o.pushKV("refHeight", (int64_t)p.refHeight);
            o.pushKV("feeVout", (int)p.feeVout);
        }
        UniValue as(UniValue::VARR);
        for (const Assignment& a : p.assignments) {
            UniValue e(UniValue::VOBJ);
            e.pushKV("vout", (int)a.vout);
            e.pushKV("cents", (int64_t)a.cents);
            as.push_back(e);
        }
        o.pushKV("assignments", as);
        o.pushKV("assignedCents", p.AssignedCents());
        break;
    }
    }
    return o;
}

/** Dry-run §3.8 for a transaction at the next height without touching the index. */
UniValue DryRun(YellowbackIndex& index, const CTransaction& tx)
{
    AssertLockHeld(index.cs_yellowback);
    OverlayStateView overlay(index.MutableView());
    State st(overlay);
    const int height = IndexHeight(index) + 1;
    TxOutcome r = ProcessTx(st, index.GetParams(), tx, height);
    UniValue o = TxLogToJSON(tx.GetHash(), r.log);
    o.pushKV("relevant", r.relevant);
    o.pushKV("vaultSpend", r.vaultSpend);
    o.pushKV("blockInvalid", r.redFailed);
    o.pushKV("dryRun", true);
    return o;
}

} // namespace

UniValue yed_getinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "yed_getinfo\n"
            "\nYellowback index status and parameters.\n"
            "\nResult:\n"
            "{\n"
            "  \"enabled\": true,\n"
            "  \"rpcversion\": n,             (numeric) the yed_* RPC contract version\n"
            "  \"network\": \"main|test|regtest\",\n"
            "  \"startHeight\": n,\n"
            "  \"height\": n,                 (numeric) index height, -1 when empty\n"
            "  \"blockhash\": \"hex\",\n"
            "  \"chainHeight\": n,\n"
            "  \"synced\": true|false,        (boolean) index tip == chain tip\n"
            "  \"healthy\": true|false,\n"
            "  \"unhealthyReason\": \"...\",\n"
            "  \"activation\": {...},\n"
            "  \"params\": {...}         (object) the four hashed regtest values and the rest of the set\n"
            "}\n"
            "\nExamples:\n" + HelpExampleCli("yed_getinfo", "") + HelpExampleRpc("yed_getinfo", ""));

    YellowbackIndex& index = EnsureIndex();
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    const yellowback::Params& p = index.GetParams();
    State st(index.View());
    std::optional<TipRecord> tip = st.GetTip();

    UniValue o(UniValue::VOBJ);
    o.pushKV("enabled", true);
    o.pushKV("rpcversion", YELLOWBACK_RPC_VERSION);
    o.pushKV("network", p.network);
    o.pushKV("startHeight", p.startHeight);
    o.pushKV("height", tip.has_value() ? tip->height : -1);
    o.pushKV("blockhash", tip.has_value() ? tip->blockHash.GetHex() : "");
    o.pushKV("chainHeight", chainActive.Height());
    o.pushKV("synced", index.IsSynced());
    o.pushKV("healthy", index.IsHealthy());
    o.pushKV("unhealthyReason", index.UnhealthyReason());
    o.pushKV("activation", ActivationToJSON(st.GetActivation()));
    UniValue pp(UniValue::VOBJ);
    // The four hashed values first (M13, N18), then the rest of the set.
    pp.pushKV("startHeight", p.startHeight);
    pp.pushKV("sigmaRefBps", p.sigmaRefBps);
    pp.pushKV("supplyCapBps", p.supplyCapBps);
    pp.pushKV("enforceUntilHeight", p.enforceUntilHeight);
    pp.pushKV("minMintCents", p.minMint);
    pp.pushKV("maxMintCents", p.maxMint);
    pp.pushKV("minOutputCents", p.minOutput);
    pp.pushKV("maxOutputCents", p.maxOutput);
    UniValue classes(UniValue::VARR);
    for (int i = 0; i < NUM_CLASSES; i++) {
        UniValue t(UniValue::VOBJ);
        t.pushKV("termClass", i);
        t.pushKV("minLockBlocks", p.classMin[i]);
        t.pushKV("maxLockBlocks", p.classMax[i]);
        t.pushKV("baseRatioBps", p.baseRatioBps[i]);
        classes.push_back(t);
    }
    pp.pushKV("classes", classes);
    pp.pushKV("grace", p.grace);
    pp.pushKV("refWindow", p.refWindow);
    pp.pushKV("refLag", g_yellowbackMintLag);
    pp.pushKV("tokenValueZat", TOKEN_VALUE);
    pp.pushKV("feeZat", g_yellowbackFee);
    o.pushKV("params", pp);
    return o;
}

UniValue yed_getstatehash(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
            "yed_getstatehash ( height )\n"
            "\nSHA-256 over the canonical serialisation of every index table (undo excluded).\n"
            "Tests assert equality across nodes and rebuilds. If height is given it must equal the index height.\n"
            "\nResult:\n{ \"height\": n, \"blockhash\": \"hex\", \"statehash\": \"hex\" }\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    std::optional<TipRecord> tip = index.GetTip();
    int h = tip.has_value() ? tip->height : -1;
    if (params.size() > 0 && !params[0].isNull() && params[0].get_int() != h) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("index is at height %d, not %d", h, params[0].get_int()));
    }
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", h);
    o.pushKV("blockhash", tip.has_value() ? tip->blockHash.GetHex() : "");
    o.pushKV("statehash", index.GetStateHash().GetHex());
    return o;
}

UniValue yed_getstats(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "yed_getstats\n"
            "\nSupply, collateral, vault counts, prices, sigma, global ratio and halts at the index tip.\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int h = IndexHeight(index);
    Totals t = st.GetTotals();
    std::optional<Snapshot> snap = h >= 0 ? st.GetSnapshot((uint32_t)h) : std::nullopt;
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", h);
    o.pushKV("supplyCents", t.supplyCents);
    o.pushKV("collateralZat", t.collateralZat);
    o.pushKV("collateral", ValueFromAmount(t.collateralZat));
    o.pushKV("activeVaults", (int64_t)t.activeVaults);
    o.pushKV("voidVaults", (int64_t)t.voidVaults);
    o.pushKV("closedVaults", (int64_t)t.closedVaults);
    o.pushKV("claimedVaults", (int64_t)t.claimedVaults);
    o.pushKV("unbackedCents", t.unbackedCents);
    const Snapshot s = snap.has_value() ? snap.value() : Snapshot::Virtual();
    o.pushKV("issuedZat", s.issuedZat);
    o.pushKV("pFast", PriceOrNull(s.PFast()));
    o.pushKV("pMid", PriceOrNull(s.PMid()));
    o.pushKV("pSlow", PriceOrNull(s.PSlow()));
    o.pushKV("pMint", PriceOrNull(s.PMint()));
    o.pushKV("pClaim", PriceOrNull(s.PClaim()));
    o.pushKV("sigmaMultBps", s.sigmaMultBps);
    o.pushKV("globalRatioBps", PriceOrNull(s.GlobalRatioBps()));
    o.pushKV("supplyCapCents", PriceOrNull(SupplyCapCents(s.issuedZat, s.PMint(), index.GetParams().supplyCapBps)));
    o.pushKV("haltMask", HaltMaskToJSON(s.haltMask));
    o.pushKV("mintingAllowed", s.activation.IsActive() && s.haltMask == 0);
    return o;
}

UniValue yed_getvault(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_getvault \"txid\"\n"
            "\nThe vault record created by the MINT transaction txid (vault outpoint txid:0).\n");

    YellowbackIndex& index = EnsureIndex();
    uint256 txid = ParseHashV(params[0], "txid");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    COutPoint out(txid, 0);
    std::optional<VaultRecord> v = st.GetVault(out);
    if (!v.has_value()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "vault not found");
    UniValue o = VaultToJSON(out, v.value());
    o.pushKV("indexHeight", IndexHeight(index));
    return o;
}

UniValue yed_listvaults(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 4)
        throw std::runtime_error(
            "yed_listvaults ( \"status\" count skip )\n"
            "\nVaults, optionally filtered by status (ACTIVE|VOID|CLOSED|CLAIMED); paged (default count 100).\n");

    YellowbackIndex& index = EnsureIndex();
    std::string status = params.size() > 0 && !params[0].isNull() ? params[0].get_str() : "";
    int count = params.size() > 1 && !params[1].isNull() ? params[1].get_int() : 100;
    int skip = params.size() > 2 && !params[2].isNull() ? params[2].get_int() : 0;
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "count and skip must be >= 0");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    UniValue list(UniValue::VARR);
    int seen = 0, total = 0;
    index.View().Iterate("V", [&](const std::string& k, const std::string& raw) {
        VaultRecord v;
        if (!DeserializeRecord(raw, v)) return true;
        if (!status.empty() && status != VaultStatusName(v.Status())) return true;
        total++;
        if (seen++ < skip) return true;
        if ((int)list.size() >= count) return true;
        COutPoint out(keys::OutPointHashOf(k), keys::OutPointIndexOf(k));
        list.push_back(VaultToJSON(out, v));
        return true;
    });
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", IndexHeight(index));
    o.pushKV("total", total);
    o.pushKV("vaults", list);
    return o;
}

UniValue yed_gettxinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_gettxinfo \"txid\"\n"
            "\nThe index's record of a confirmed transaction (type, verdict, cents in/out/burned, assigned\n"
            "outputs, closed vaults), or a dry run for a transaction still in the mempool (\"dryRun\": true).\n");

    YellowbackIndex& index = EnsureIndex();
    uint256 txid = ParseHashV(params[0], "txid");
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    std::optional<TxLogRecord> log = st.GetTxLog(txid);
    if (log.has_value()) {
        UniValue o = TxLogToJSON(txid, log.value());
        o.pushKV("confirmations", IndexHeight(index) - log->height + 1);
        o.pushKV("dryRun", false);
        return o;
    }
    CTransaction tx;
    if (mempool.lookup(txid, tx)) {
        UniValue o = DryRun(index, tx);
        o.pushKV("confirmations", 0);
        return o;
    }
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "transaction is neither in the Yellowback index nor in the mempool");
}

UniValue yed_decodepayload(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_decodepayload \"hex\"\n"
            "\nDecode a Yellowback OP_RETURN payload (the data push, or a whole OP_RETURN script, or a raw transaction).\n");

    std::string hex = params[0].get_str();
    if (!IsHex(hex)) throw JSONRPCError(RPC_INVALID_PARAMETER, "not hex");
    std::vector<unsigned char> bytes = ParseHex(hex);
    Payload p;
    // Raw payload?
    if (DecodePayload(bytes, p)) return PayloadToJSON(p);
    // An OP_RETURN script?
    {
        CScript s(bytes.begin(), bytes.end());
        auto data = ExtractOpReturnData(s);
        if (data.has_value() && DecodePayload(data.value(), p)) return PayloadToJSON(p);
    }
    // A transaction?
    CTransaction tx;
    if (DecodeHexTx(tx, hex)) {
        auto fp = FindPayload(tx);
        if (!fp.has_value()) throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction carries no well-formed Yellowback payload");
        UniValue o = PayloadToJSON(fp->payload);
        o.pushKV("opReturnIndex", (int64_t)fp->opReturnIndex);
        return o;
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, "not a Yellowback payload");
}

UniValue yed_validaterawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_validaterawtransaction \"hex\"\n"
            "\nDry-run the Yellowback state rules for a raw transaction as if it confirmed in the next block,\n"
            "without changing the index. Returns the verdict, type, cents in/out/burned and assigned outputs.\n");

    YellowbackIndex& index = EnsureIndex();
    CTransaction tx;
    if (!DecodeHexTx(tx, params[0].get_str())) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    UniValue o = DryRun(index, tx);
    o.pushKV("indexHeight", IndexHeight(index));
    o.pushKV("synced", index.IsSynced());
    auto fp = FindPayload(tx);
    if (fp.has_value()) o.pushKV("payload", PayloadToJSON(fp->payload));
    return o;
}

UniValue yed_estimatecollateral(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw std::runtime_error(
            "yed_estimatecollateral cents lockBlocks ( pMintMicroUsd )\n"
            "\nThe collateral (zatoshi, rounded up to 1,000) required to mint cents with a lock of lockBlocks\n"
            "(the term class follows from it, V19) at the snapshot the wallet would reference (index tip minus\n"
            "the ref lag), or at the given P_mint with that snapshot's sigma multiplier.\n");

    YellowbackIndex& index = EnsureIndex();
    int64_t cents = params[0].get_int64();
    int64_t lockBlocks = params[1].get_int64();
    const yellowback::Params& p = index.GetParams();
    const int termClass = p.ClassForLockBlocks(lockBlocks);
    if (termClass < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "lockBlocks is in no term class");
    if (cents < p.minMint || cents > p.maxMint) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("cents must be between %d and %d", p.minMint, p.maxMint));
    }
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int tipH = IndexHeight(index);
    const int refH = tipH - g_yellowbackMintLag;
    std::optional<Snapshot> snap = SnapshotAt(st, p, refH);
    std::optional<MicroUsd> pMint;
    int sigmaMultBps = snap.has_value() ? snap->sigmaMultBps : p.sigmaMultMaxBps;
    if (params.size() > 2 && !params[2].isNull()) pMint = params[2].get_int64();
    else if (snap.has_value()) pMint = snap->PMint();
    UniValue o(UniValue::VOBJ);
    o.pushKV("cents", cents);
    o.pushKV("termClass", termClass);
    o.pushKV("lockBlocks", lockBlocks);
    o.pushKV("refHeight", refH);
    o.pushKV("sigmaMultBps", sigmaMultBps);
    o.pushKV("minRatioBps", MinRatioBps(p.baseRatioBps[termClass], sigmaMultBps));
    if (!pMint.has_value()) {
        o.pushKV("pMint", NullUniValue);
        o.pushKV("requiredZat", NullUniValue);
        o.pushKV("error", verdict::MINT_HALTED_NO_PRICE);
        return o;
    }
    o.pushKV("pMint", pMint.value());
    auto req = RequiredCollateralRounded((Cents)cents, MinRatioBps(p.baseRatioBps[termClass], sigmaMultBps), pMint.value());
    if (!req.has_value()) {
        o.pushKV("requiredZat", NullUniValue);
        o.pushKV("error", verdict::MINT_UNSATISFIABLE);
        return o;
    }
    o.pushKV("requiredZat", req.value());
    o.pushKV("required", ValueFromAmount(req.value()));
    o.pushKV("lockHeight", (int64_t)refH + lockBlocks);
    o.pushKV("claimHeight", (int64_t)refH + lockBlocks + p.grace);
    o.pushKV("expiryHeight", (int64_t)refH + p.refWindow);
    return o;
}

UniValue yed_gethistory(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw std::runtime_error(
            "yed_gethistory fromHeight toHeight\n"
            "\nPer-block snapshots (§3.6: tag, signal count, activation, prices, sigma, issuance, totals, halts) for the range; at most 10,000 blocks.\n");

    YellowbackIndex& index = EnsureIndex();
    int from = params[0].get_int();
    int to = params[1].get_int();
    if (from < 0 || to < from || to - from >= 10000) throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid range");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    UniValue arr(UniValue::VARR);
    for (int h = from; h <= to; h++) {
        std::optional<Snapshot> s = st.GetSnapshot((uint32_t)h);
        if (s.has_value()) arr.push_back(SnapshotToJSON(h, s.value()));
    }
    return arr;
}

static const CRPCCommand commands[] =
{ //  category   name                          actor (function)              okSafeMode
  //  ---------  ----------------------------  ----------------------------  ----------
    { "yellowback", "yed_getinfo",                 &yed_getinfo,                  true  },
    { "yellowback", "yed_getstatehash",            &yed_getstatehash,             true  },
    { "yellowback", "yed_getstats",                &yed_getstats,                 true  },
    { "yellowback", "yed_getvault",                &yed_getvault,                 true  },
    { "yellowback", "yed_listvaults",              &yed_listvaults,               true  },
    { "yellowback", "yed_gettxinfo",               &yed_gettxinfo,                true  },
    { "yellowback", "yed_decodepayload",           &yed_decodepayload,            true  },
    { "yellowback", "yed_validaterawtransaction",  &yed_validaterawtransaction,   true  },
    { "yellowback", "yed_estimatecollateral",      &yed_estimatecollateral,       true  },
    { "yellowback", "yed_gethistory",              &yed_gethistory,               true  },
};

void RegisterYellowbackRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
