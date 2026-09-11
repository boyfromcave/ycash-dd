// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

/**
 * Yellowback node-context RPCs (plan §4.5; the contract is doc/yellowback-rpc.md,
 * rpcversion 2, whose fenced json blocks define every return shape and error
 * identifier). They work without a wallet and are gated by
 * -experimentalfeatures -yellowback. The index is synchronous with chainActive
 * (V2), so every reply is for the tip.
 *
 * While the index is unhealthy every command refuses with `yellowback-unhealthy`
 * except yed_getinfo, yed_getblockverdict, yed_gettag, yed_decodepayload and
 * yed_setquote (M8). Every refusal is RPC_INVALID_PARAMETER (a bad argument),
 * RPC_WALLET_ERROR (funds) or RPC_VERIFY_REJECTED (a rule) with a message that
 * begins with a stable identifier.
 *
 * Lock order: cs_main, then cs_yellowback (never mempool.cs after cs_yellowback).
 * The clock has one home here: yed_setquote stamps receivedAt (M11).
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
#include "utiltime.h"
#include "yellowback/address.h"
#include "yellowback/index.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/policy.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/tag.h"
#include "yellowback/view.h"

#include <univalue.h>

using namespace yellowback;

static const int YELLOWBACK_RPC_VERSION = 2;

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
        throw JSONRPCError(RPC_VERIFY_REJECTED, "yellowback-unhealthy: " + index.UnhealthyReason() + "; restart with -reindex-yellowback");
    }
}

int IndexHeight(const YellowbackIndex& index)
{
    return index.TipHeight();
}

std::string P2PKHAddress(const CKeyID& key)
{
    KeyIO keyIO(::Params());
    return keyIO.EncodeDestination(CTxDestination(key));
}

UniValue PayeeOrNull(bool has, const uint160& key)
{
    return has ? UniValue(P2PKHAddress(CKeyID(key))) : NullUniValue;
}

UniValue OutPointToJSON(const COutPoint& out)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", out.hash.GetHex());
    o.pushKV("vout", (int64_t)out.n);
    return o;
}

std::string ClassLetter(uint8_t termClass)
{
    return termClass < NUM_CLASSES ? std::string(1, (char)('A' + termClass)) : std::to_string((int)termClass);
}

const char* ActivationLower(ActivationStatus s)
{
    switch (s) {
    case ActivationStatus::SIGNALING: return "signaling";
    case ActivationStatus::LOCKED_IN: return "locked_in";
    case ActivationStatus::ACTIVE: return "active";
    }
    return "signaling";
}

std::string TypeLower(TxLogType t)
{
    switch (t) {
    case TxLogType::MINT: return "mint";
    case TxLogType::TRANSFER: return "transfer";
    case TxLogType::REDEEM: return "redeem";
    case TxLogType::NONE: break;
    }
    return "none";
}

UniValue PriceOrNull(std::optional<int64_t> v)
{
    return v.has_value() ? UniValue(v.value()) : NullUniValue;
}

/** The pClaim below which the vault is underwater: ceil(mintedCents * CLAIM_THRESHOLD_BPS * COIN / collateralZat); null for no debt. */
UniValue UnderwaterAt(const VaultRecord& v, const yellowback::Params& p)
{
    if (v.mintedCents <= 0 || v.collateralZat <= 0) return NullUniValue;
    arith_uint256 rhs = arith_uint256(v.mintedCents) * arith_uint256(p.claimThresholdBps) * arith_uint256(COIN);
    arith_uint256 q = (rhs + arith_uint256(v.collateralZat) - 1) / arith_uint256(v.collateralZat);
    if (q > arith_uint256((uint64_t)std::numeric_limits<int64_t>::max())) return NullUniValue;
    return UniValue((int64_t)q.GetLow64());
}

UniValue VaultToJSON(const COutPoint& out, const VaultRecord& v, const YellowbackIndex& index, const std::optional<Snapshot>& tipSnap, bool abandoned)
{
    const yellowback::Params& p = index.GetParams();
    const int tip = index.TipHeight();
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", out.hash.GetHex());
    o.pushKV("vout", (int64_t)out.n);
    o.pushKV("status", VaultStatusName(v.Status()));
    o.pushKV("ownerPubKey", HexStr(v.ownerPubKey.begin(), v.ownerPubKey.end()));
    const CPubKey owner = v.OwnerKey();
    o.pushKV("ownerKeyId", owner.IsValid() ? owner.GetID().GetHex() : "");
    o.pushKV("ownerAddress", owner.IsValid() ? EncodeAddress(owner.GetID(), p) : "");
    o.pushKV("termClass", ClassLetter(v.termClass));
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
    if (v.Status() == VaultStatus::ACTIVE && tip >= v.claimHeight && tipSnap.has_value()) {
        claimable = IsUnderwater(v.collateralZat, tipSnap->PClaim(), v.mintedCents, p.claimThresholdBps);
    }
    o.pushKV("claimable", claimable);
    o.pushKV("underwaterAt", v.Status() == VaultStatus::VOID ? NullUniValue : UnderwaterAt(v, p));
    o.pushKV("voidReason", v.voidReason);
    // K3 / L10: a VOID vault's claim path is unpoliced after claimHeight; an ACTIVE vault's under abandonment.
    if (v.Status() == VaultStatus::VOID || (v.Status() == VaultStatus::ACTIVE && abandoned)) o.pushKV("sweepBefore", (int64_t)v.claimHeight);
    return o;
}

UniValue AssignedToJSON(const std::vector<AssignedOutput>& v)
{
    UniValue arr(UniValue::VARR);
    for (const AssignedOutput& a : v) {
        UniValue e(UniValue::VOBJ);
        e.pushKV("vout", (int64_t)a.outpoint.n);
        e.pushKV("cents", a.cents);
        arr.push_back(e);
    }
    return arr;
}

UniValue OutPointsToJSON(const std::vector<AssignedOutput>& v)
{
    UniValue arr(UniValue::VARR);
    for (const AssignedOutput& a : v) arr.push_back(OutPointToJSON(a.outpoint));
    return arr;
}

UniValue OutPointsToJSON(const std::vector<COutPoint>& v)
{
    UniValue arr(UniValue::VARR);
    for (const COutPoint& c : v) arr.push_back(OutPointToJSON(c));
    return arr;
}

UniValue TxLogToJSON(const uint256& txid, const TxLogRecord& l)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("height", l.height);
    o.pushKV("type", TypeLower(l.Type()));
    o.pushKV("path", l.path);
    o.pushKV("verdict", l.verdict);
    o.pushKV("yedIn", l.yedIn);
    o.pushKV("yedOut", l.yedOut);
    o.pushKV("burned", l.burned);
    o.pushKV("feeZat", l.feeZat);
    o.pushKV("payee", PayeeOrNull(l.hasPayee, l.payee));
    o.pushKV("assigned", AssignedToJSON(l.assigned));
    o.pushKV("spentTokens", OutPointsToJSON(l.spentTokens));
    o.pushKV("closedVaults", OutPointsToJSON(l.closedVaults));
    o.pushKV("expired", false);
    return o;
}

UniValue ActivationToJSON(const Activation& a)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("status", ActivationLower(a.Status()));
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

UniValue TagToJSON(const std::optional<CoinbaseTag>& tag)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("found", tag.has_value());
    if (!tag.has_value()) {
        o.pushKV("kind", "none");
        return o;
    }
    o.pushKV("kind", tag->IsQuote() ? "quote" : "signal");
    o.pushKV("version", (int)TAG_VERSION);
    o.pushKV("signal", tag->Signal());
    o.pushKV("priceMicroUsd", (int64_t)tag->priceMicroUsd);
    o.pushKV("sourceMask", (int)tag->sourceMask);
    o.pushKV("payoutAddress", P2PKHAddress(CKeyID(tag->payoutKey)));
    return o;
}

UniValue PayloadToJSON(const Payload& p)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("valid", true);
    o.pushKV("version", (int)p.version);
    o.pushKV("type", PayloadTypeName(p.type));
    o.pushKV("reason", "");
    switch (p.type) {
    case PayloadType::MINT:
        o.pushKV("termClass", ClassLetter(p.termClass));
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

/** A stored block by hash or height (all digits => a height on the active chain). cs_main held. */
const CBlockIndex* BlockIndexOf(const std::string& arg)
{
    AssertLockHeld(cs_main);
    if (!arg.empty() && arg.find_first_not_of("0123456789") == std::string::npos) {
        int64_t h = atoi64(arg);
        if (h < 0 || h > chainActive.Height()) throw JSONRPCError(RPC_INVALID_PARAMETER, "height out of range");
        return chainActive[(int)h];
    }
    uint256 hash = ParseHashV(arg, "blockhash");
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi == mapBlockIndex.end()) throw JSONRPCError(RPC_INVALID_PARAMETER, "block not found");
    return mi->second;
}

/** The quote tags in (h - window, h] bounded below by startHeight. */
int QuoteTagsIn(const State& st, const yellowback::Params& p, int h, int window)
{
    int n = 0;
    for (int64_t t = std::max<int64_t>((int64_t)h - window + 1, p.startHeight); t <= h; t++) {
        std::optional<TagRecord> tag = st.GetTag((uint32_t)t);
        if (tag.has_value() && tag->IsQuote()) n++;
    }
    return n;
}

int HeightArg(const UniValue& v, int def)
{
    if (v.isNull()) return def;
    if (!v.isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "height must be a number");
    return v.get_int();
}

} // namespace

UniValue yed_getinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "yed_getinfo\n"
            "\nYellowback index status, activation, miner state and parameters (doc/yellowback-rpc.md).\n"
            "Never refuses while the index is unhealthy.\n"
            "\nExamples:\n" + HelpExampleCli("yed_getinfo", "") + HelpExampleRpc("yed_getinfo", ""));

    YellowbackIndex& index = EnsureIndex();
    const int64_t now = GetTime();
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    const yellowback::Params& p = index.GetParams();
    State st(index.View());
    std::optional<TipRecord> tip = st.GetTip();
    const int h = tip.has_value() ? tip->height : -1;
    std::optional<Snapshot> snap = h >= 0 ? st.GetSnapshot((uint32_t)h) : std::nullopt;
    const MinerConfig& cfg = index.GetMinerConfig();
    const PayeePolicy& pp = index.GetPayeePolicy();
    const MinerStatus ms = index.GetMinerStatus(now);

    UniValue o(UniValue::VOBJ);
    o.pushKV("rpcversion", YELLOWBACK_RPC_VERSION);
    o.pushKV("enabled", true);
    o.pushKV("network", p.network);
    o.pushKV("height", h);
    o.pushKV("blockhash", tip.has_value() ? tip->blockHash.GetHex() : "");
    o.pushKV("chainHeight", chainActive.Height());
    o.pushKV("startHeight", p.startHeight);
    o.pushKV("healthy", index.IsHealthy());
    o.pushKV("unhealthyReason", index.UnhealthyReason());
    o.pushKV("enforcing", index.IsEnforcing());
    o.pushKV("valveTripped", index.ValveTripped());
    o.pushKV("sunset", index.IsSunset());
    o.pushKV("rejectedBlocks", index.RejectedCount());
    o.pushKV("suppressedBlocks", index.SuppressedCount());
    o.pushKV("templatePolicy", cfg.templatePolicy);
    o.pushKV("abandoned", index.IsAbandoned());

    UniValue act(UniValue::VOBJ);
    const Activation a = snap.has_value() ? snap->activation : st.GetActivation();
    act.pushKV("status", ActivationLower(a.Status()));
    act.pushKV("lockInHeight", a.lockInHeight);
    act.pushKV("activateHeight", a.activateHeight);
    act.pushKV("signalCount", snap.has_value() ? (int64_t)snap->signalCount : 0);
    act.pushKV("window", p.signalWindow);
    o.pushKV("activation", act);

    UniValue miner(UniValue::VOBJ);
    miner.pushKV("payoutAddress", ms.payoutKey.has_value() ? UniValue(P2PKHAddress(ms.payoutKey.value())) : NullUniValue);
    miner.pushKV("signal", ms.signal);
    miner.pushKV("quoteKind", ms.kind);
    miner.pushKV("quoteAgeSeconds", ms.quoteAgeSeconds.has_value() ? UniValue(ms.quoteAgeSeconds.value()) : NullUniValue);
    miner.pushKV("registered", ms.registered);
    miner.pushKV("eligible", ms.eligible);
    o.pushKV("miner", miner);

    UniValue prm(UniValue::VOBJ);
    prm.pushKV("startHeight", p.startHeight);
    prm.pushKV("enforceUntilHeight", p.enforceUntilHeight);
    prm.pushKV("sigmaRefBps", p.sigmaRefBps);
    prm.pushKV("supplyCapBps", p.supplyCapBps);
    prm.pushKV("refLag", g_yellowbackMintLag);
    prm.pushKV("refWindow", p.refWindow);
    prm.pushKV("grace", p.grace);
    prm.pushKV("payeeWindow", p.payeeWindow);
    prm.pushKV("feeMinZat", p.feeMin);
    prm.pushKV("feeBps", p.feeBps);
    prm.pushKV("tokenValueZat", p.tokenValue);
    prm.pushKV("feeZat", g_yellowbackFee);
    prm.pushKV("valveBlocks", p.valveBlocks);
    prm.pushKV("abandonBlocks", p.abandonBlocks);
    UniValue windows(UniValue::VOBJ);
    windows.pushKV("fast", p.pFastWindow);
    windows.pushKV("mid", p.pMidWindow);
    windows.pushKV("slow", p.pSlowWindow);
    windows.pushKV("signal", p.signalWindow);
    prm.pushKV("windows", windows);
    UniValue minFill(UniValue::VOBJ);
    minFill.pushKV("fast", p.pFastMinFill);
    minFill.pushKV("mid", p.pMidMinFill);
    minFill.pushKV("slow", p.pSlowMinFill);
    prm.pushKV("minFill", minFill);
    UniValue classes(UniValue::VARR);
    for (int i = 0; i < NUM_CLASSES; i++) {
        UniValue t(UniValue::VOBJ);
        t.pushKV("class", ClassLetter((uint8_t)i));
        t.pushKV("minBlocks", p.classMin[i]);
        t.pushKV("maxBlocks", p.classMax[i]);
        t.pushKV("baseRatioBps", p.baseRatioBps[i]);
        classes.push_back(t);
    }
    prm.pushKV("classes", classes);
    UniValue policy(UniValue::VOBJ);
    policy.pushKV("penaltyBlocks", pp.penaltyBlocks);
    policy.pushKV("accuracyWindow", pp.accuracyWindow);
    policy.pushKV("tiltBps", pp.tiltBps);
    policy.pushKV("preferredPayee", pp.preferred.has_value() ? UniValue(P2PKHAddress(pp.preferred.value())) : NullUniValue);
    prm.pushKV("policy", policy);
    o.pushKV("params", prm);
    return o;
}

UniValue yed_getstatehash(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
            "yed_getstatehash ( height )\n"
            "\nSHA-256 over the §3.6 preimage of every index table (TxLog, Rejected and Undo excluded).\n"
            "If height is given it must equal the index height.\n"
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
            "\nTotals plus the tip snapshot: supply, collateral, vault counts, prices, sigma, global ratio, cap, halts.\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int h = IndexHeight(index);
    Totals t = st.GetTotals();
    std::optional<Snapshot> snap = h >= 0 ? st.GetSnapshot((uint32_t)h) : std::nullopt;
    const Snapshot s = snap.has_value() ? snap.value() : Snapshot::Virtual();
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", h);
    o.pushKV("supplyCents", t.supplyCents);
    o.pushKV("collateralZat", t.collateralZat);
    o.pushKV("activeVaults", (int64_t)t.activeVaults);
    o.pushKV("voidVaults", (int64_t)t.voidVaults);
    o.pushKV("closedVaults", (int64_t)t.closedVaults);
    o.pushKV("claimedVaults", (int64_t)t.claimedVaults);
    o.pushKV("unbackedCents", t.unbackedCents);
    o.pushKV("issuedZat", s.issuedZat);
    o.pushKV("pFast", PriceOrNull(s.PFast()));
    o.pushKV("pMid", PriceOrNull(s.PMid()));
    o.pushKV("pSlow", PriceOrNull(s.PSlow()));
    o.pushKV("pMint", PriceOrNull(s.PMint()));
    o.pushKV("pClaim", PriceOrNull(s.PClaim()));
    o.pushKV("sigmaMultBps", s.sigmaMultBps);
    o.pushKV("globalRatioBps", PriceOrNull(s.GlobalRatioBps()));
    std::optional<Cents> cap = SupplyCapCents(s.issuedZat, s.PMint(), index.GetParams().supplyCapBps);
    o.pushKV("supplyCapCents", PriceOrNull(cap));
    o.pushKV("haltMask", HaltMaskToJSON(s.haltMask));
    o.pushKV("mintingAllowed", s.activation.IsActive() && s.haltMask == 0 && (!cap.has_value() || t.supplyCents < cap.value()));
    return o;
}

UniValue yed_getprice(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
            "yed_getprice ( height )\n"
            "\nThe snapshot prices at height (default the tip) with the window fills that produced them and that block's tag.\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    State st(index.View());
    const int tip = IndexHeight(index);
    const int h = HeightArg(params.size() > 0 ? params[0] : NullUniValue, tip);
    if (h < 0 || h > tip) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("height must be between 0 and %d", tip));
    // Below startHeight every price is undefined: the virtual snapshot (§3.6), no fill, no tag lookup of the index.
    std::optional<Snapshot> snap = h >= p.startHeight ? st.GetSnapshot((uint32_t)h) : std::nullopt;
    const Snapshot s = snap.has_value() ? snap.value() : Snapshot::Virtual();
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", h);
    o.pushKV("pFast", PriceOrNull(s.PFast()));
    o.pushKV("pMid", PriceOrNull(s.PMid()));
    o.pushKV("pSlow", PriceOrNull(s.PSlow()));
    o.pushKV("pMint", PriceOrNull(s.PMint()));
    o.pushKV("pClaim", PriceOrNull(s.PClaim()));
    UniValue fill(UniValue::VOBJ);
    const std::pair<const char*, int> windows[] = { { "fast", p.pFastWindow }, { "mid", p.pMidWindow }, { "slow", p.pSlowWindow } };
    for (const auto& w : windows) {
        UniValue f(UniValue::VOBJ);
        f.pushKV("quoteTags", h >= p.startHeight ? QuoteTagsIn(st, p, h, w.second) : 0);
        f.pushKV("window", w.second);
        f.pushKV("minFill", p.MinFill(w.second));
        fill.pushKV(w.first, f);
    }
    o.pushKV("fill", fill);
    std::optional<CoinbaseTag> tag;
    const CBlockIndex* pindex = chainActive[h];
    CBlock block;
    if (pindex && ReadBlockFromDisk(block, pindex, ::Params().GetConsensus()) && !block.vtx.empty() && !block.vtx[0].vin.empty()) {
        tag = FindTag(block.vtx[0].vin[0].scriptSig, h);
    }
    o.pushKV("tag", TagToJSON(tag));
    return o;
}

UniValue yed_getactivation(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "yed_getactivation\n"
            "\nThe ACT-1..7 state at the tip with the network constants and a signal-count history.\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    State st(index.View());
    const int tip = IndexHeight(index);
    std::optional<Snapshot> snap = tip >= 0 ? st.GetSnapshot((uint32_t)tip) : std::nullopt;
    const Activation a = snap.has_value() ? snap->activation : st.GetActivation();
    const uint32_t mask = snap.has_value() ? snap->haltMask : 0;
    UniValue o(UniValue::VOBJ);
    o.pushKV("status", ActivationLower(a.Status()));
    o.pushKV("lockInHeight", a.lockInHeight);
    o.pushKV("activateHeight", a.activateHeight);
    o.pushKV("signalCount", snap.has_value() ? (int64_t)snap->signalCount : 0);
    o.pushKV("window", p.signalWindow);
    o.pushKV("threshold", p.activationThreshold);
    o.pushKV("participationFloor", p.participationFloor);
    o.pushKV("enforcementFloor", p.enforcementFloor);
    o.pushKV("enforcementResume", p.enforcementResume);
    o.pushKV("mintHalted", (mask & HALT_PARTICIPATION) != 0);
    o.pushKV("enforcementSuspended", (mask & HALT_ENFORCEMENT) != 0);
    o.pushKV("enforcing", index.IsEnforcing());
    o.pushKV("valveTripped", index.ValveTripped());
    o.pushKV("sunset", index.IsSunset());
    o.pushKV("enforceUntilHeight", p.enforceUntilHeight);
    UniValue history(UniValue::VARR);
    const int step = std::max(1, p.signalWindow / 8);
    std::vector<int> heights;
    for (int i = 0; i < 8; i++) {
        const int h = tip - i * step;
        if (h < p.startHeight) break;
        heights.push_back(h);
    }
    for (auto it = heights.rbegin(); it != heights.rend(); ++it) {
        std::optional<Snapshot> s = st.GetSnapshot((uint32_t)*it);
        UniValue row(UniValue::VOBJ);
        row.pushKV("height", *it);
        row.pushKV("signalCount", s.has_value() ? (int64_t)s->signalCount : 0);
        history.push_back(row);
    }
    o.pushKV("history", history);
    return o;
}

UniValue yed_listminers(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw std::runtime_error(
            "yed_listminers ( height window )\n"
            "\nOne row per payoutKey with a quote tag in (height - window, height] (window defaults to PAYEE_WINDOW).\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    const PayeePolicy& pp = index.GetPayeePolicy();
    State st(index.View());
    const int tip = IndexHeight(index);
    const int h = HeightArg(params.size() > 0 ? params[0] : NullUniValue, tip);
    const int window = HeightArg(params.size() > 1 ? params[1] : NullUniValue, p.payeeWindow);
    if (h < p.startHeight || h > tip) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("height must be between %d and %d", p.startHeight, tip));
    if (window <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "window must be positive");

    struct Row { int lastTagHeight; uint64_t lastQuote; int quoteTags; };
    std::map<uint160, Row> rows;
    int total = 0;
    for (int64_t t = std::max<int64_t>((int64_t)h - window + 1, p.startHeight); t <= h; t++) {
        std::optional<TagRecord> tag = st.GetTag((uint32_t)t);
        if (!tag.has_value() || !tag->IsQuote()) continue;
        total++;
        Row& r = rows[tag->payoutKey];
        r.lastTagHeight = (int)t;
        r.lastQuote = tag->priceMicroUsd;
        r.quoteTags++;
    }
    std::vector<CKeyID> eligible = EligiblePayees(index.View(), p, h);
    std::vector<std::pair<int, uint160>> order;
    for (const auto& kv : rows) order.push_back(std::make_pair(kv.second.lastTagHeight, kv.first));
    std::sort(order.begin(), order.end(), [](const std::pair<int, uint160>& a, const std::pair<int, uint160>& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    UniValue out(UniValue::VARR);
    for (const auto& e : order) {
        const CKeyID key(e.second);
        const Row& r = rows[e.second];
        UniValue o(UniValue::VOBJ);
        o.pushKV("payoutAddress", P2PKHAddress(key));
        o.pushKV("lastTagHeight", r.lastTagHeight);
        o.pushKV("lastQuote", (int64_t)r.lastQuote);
        o.pushKV("quoteTags", r.quoteTags);
        o.pushKV("share", total > 0 ? (int64_t)(10000LL * r.quoteTags / total) : 0);
        o.pushKV("registered", Registered(index.View(), p, key, h));
        bool elig = false;
        for (const CKeyID& k : eligible) if (k == key) { elig = true; break; }
        o.pushKV("eligible", elig);
        // REG-2: the height the penalty of the latest penalised judgement ends (t + PEER_LAG + N_PENALTY).
        int64_t until = 0;
        if (Penalized(index.View(), p, key, h, pp.penaltyBlocks)) {
            for (int64_t t = (int64_t)h - p.peerLag - 1; t >= std::max<int64_t>((int64_t)h - p.peerLag - pp.penaltyBlocks, p.startHeight); t--) {
                std::optional<TagRecord> tag = st.GetTag((uint32_t)t);
                if (!tag.has_value() || !tag->IsQuote() || !(tag->payoutKey == key)) continue;
                std::optional<Judgement> j = st.GetJudgement((uint32_t)t);
                if (j.has_value() && j->penalized) { until = t + p.peerLag + pp.penaltyBlocks; break; }
            }
        }
        o.pushKV("penalizedUntil", until);
        // REG-3 over the node's accuracy window.
        int quoted = 0, inBand = 0;
        {
            const int64_t hi = (int64_t)h - p.peerLag;
            const int64_t lo = hi - std::max(0, pp.accuracyWindow) + 1;
            for (int64_t t = std::max<int64_t>(lo, p.startHeight); t <= hi; t++) {
                std::optional<TagRecord> tag = st.GetTag((uint32_t)t);
                if (!tag.has_value() || !tag->IsQuote() || !(tag->payoutKey == key)) continue;
                std::optional<Judgement> j = st.GetJudgement((uint32_t)t);
                if (!j.has_value() || !j->evaluated) continue;
                quoted++;
                if (j->inBand) inBand++;
            }
        }
        o.pushKV("accuracyBps", quoted > 0 ? UniValue((int64_t)(10000LL * inBand / quoted)) : NullUniValue);
        o.pushKV("quoted", quoted);
        o.pushKV("inBand", inBand);
        out.push_back(o);
    }
    return out;
}

UniValue yed_gettag(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_gettag \"height|blockhash\"\n"
            "\nDecode the coinbase tag (TAG-1..5) of a stored block; all digits => a height on the active chain. Allowed while unhealthy.\n");

    YellowbackIndex& index = EnsureIndex();
    const yellowback::Params& p = index.GetParams();
    LOCK(cs_main);
    const CBlockIndex* pindex = BlockIndexOf(params[0].get_str());
    std::optional<CoinbaseTag> tag;
    if (pindex->nHeight >= p.startHeight) {
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, ::Params().GetConsensus())) throw JSONRPCError(RPC_INVALID_PARAMETER, "block not on disk");
        if (!block.vtx.empty() && !block.vtx[0].vin.empty()) tag = FindTag(block.vtx[0].vin[0].scriptSig, pindex->nHeight);
    }
    return TagToJSON(tag);
}

UniValue yed_setquote(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw std::runtime_error(
            "yed_setquote priceMicroUsd sourceMask\n"
            "\nStore the miner's quote (0 clears it) with receivedAt = now (MINER-1). Allowed while unhealthy.\n");

    YellowbackIndex& index = EnsureIndex();
    if (!params[0].isNum() || !params[1].isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "priceMicroUsd and sourceMask must be numbers");
    const int64_t price = params[0].get_int64();
    const int64_t mask = params[1].get_int64();
    if (price != 0 && (price < PRICE_MIN || price > PRICE_MAX)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("quote-out-of-range: priceMicroUsd must be 0 or between %d and %d", PRICE_MIN, PRICE_MAX));
    }
    if (mask < 0 || mask > 0xFFFF) throw JSONRPCError(RPC_INVALID_PARAMETER, "sourceMask must be a 16-bit value");
    if (!index.GetMinerConfig().payoutKey.has_value()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "no-payout-address: set -yellowbackpayoutaddress (or a P2PKH -mineraddress) so the node can emit a tag");
    }
    const int64_t now = GetTime();      // the clock's one home (M11)
    index.SetQuote((uint64_t)price, (uint16_t)mask, now);
    const MinerStatus ms = index.GetMinerStatus(now);
    UniValue o(UniValue::VOBJ);
    o.pushKV("priceMicroUsd", price);
    o.pushKV("sourceMask", mask);
    o.pushKV("receivedAt", now);
    UniValue next(UniValue::VOBJ);
    next.pushKV("kind", ms.kind);
    next.pushKV("signal", ms.signal);
    next.pushKV("payoutAddress", ms.payoutKey.has_value() ? UniValue(P2PKHAddress(ms.payoutKey.value())) : NullUniValue);
    o.pushKV("nextTag", next);
    return o;
}

UniValue yed_getfeepayee(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw std::runtime_error(
            "yed_getfeepayee refHeight collateralZat ( \"selectorHex\" )\n"
            "\nE(refHeight) (FEE-2), the FEE-1 fee for the collateral and the FEE-W default choice for the selector.\n");

    YellowbackIndex& index = EnsureIndex();
    if (!params[0].isNum() || !params[1].isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "refHeight and collateralZat must be numbers");
    const int refHeight = params[0].get_int();
    const int64_t collateral = params[1].get_int64();
    if (collateral < 0 || collateral > MAX_MONEY) throw JSONRPCError(RPC_INVALID_PARAMETER, "collateralZat out of range");
    std::vector<unsigned char> selector;
    if (params.size() > 2 && !params[2].isNull()) {
        if (!IsHex(params[2].get_str())) throw JSONRPCError(RPC_INVALID_PARAMETER, "selectorHex must be hex");
        selector = ParseHex(params[2].get_str());
    }
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    const PayeePolicy& pp = index.GetPayeePolicy();
    if (refHeight < p.startHeight || refHeight > IndexHeight(index)) throw JSONRPCError(RPC_INVALID_PARAMETER, "refHeight out of range");
    std::vector<CKeyID> eligible = EligiblePayees(index.View(), p, refHeight);
    if (eligible.empty()) throw JSONRPCError(RPC_VERIFY_REJECTED, "fee-no-eligible-payee: no pool quoted in the payee window (FEE-0)");
    UniValue o(UniValue::VOBJ);
    UniValue el(UniValue::VARR);
    for (const CKeyID& k : eligible) el.push_back(P2PKHAddress(k));
    o.pushKV("eligible", el);
    o.pushKV("feeZat", FeeZat(collateral, p.feeMin, p.feeBps));
    PayeePolicy noPref = pp;
    noPref.preferred = std::nullopt;
    std::optional<CKeyID> pick = DefaultPayee(index.View(), p, refHeight, selector, noPref);
    UniValue def(UniValue::VOBJ);
    if (pick.has_value()) {
        def.pushKV("payoutAddress", P2PKHAddress(pick.value()));
        const int acc = AccuracyBps(index.View(), p, pick.value(), refHeight, pp.accuracyWindow);
        def.pushKV("weight", (int64_t)(10000 + (int64_t)pp.tiltBps * acc / 10000));
    }
    o.pushKV("default", def);
    if (pp.preferred.has_value()) {
        for (const CKeyID& k : eligible) {
            if (k == pp.preferred.value()) { o.pushKV("preferred", P2PKHAddress(k)); break; }
        }
    }
    UniValue policy(UniValue::VOBJ);
    policy.pushKV("penaltyBlocks", pp.penaltyBlocks);
    policy.pushKV("accuracyWindow", pp.accuracyWindow);
    policy.pushKV("tiltBps", pp.tiltBps);
    o.pushKV("policy", policy);
    return o;
}

UniValue yed_getvault(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_getvault \"txid\"\n"
            "\nThe vault record created by the MINT transaction txid (vault outpoint txid:0) plus derived fields.\n");

    YellowbackIndex& index = EnsureIndex();
    uint256 txid = ParseHashV(params[0], "txid");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    COutPoint out(txid, 0);
    std::optional<VaultRecord> v = st.GetVault(out);
    if (!v.has_value()) throw JSONRPCError(RPC_INVALID_PARAMETER, "vault-not-found: no vault at " + txid.GetHex() + ":0");
    const int tip = IndexHeight(index);
    std::optional<Snapshot> snap = tip >= 0 ? st.GetSnapshot((uint32_t)tip) : std::nullopt;
    return VaultToJSON(out, v.value(), index, snap, index.IsAbandoned());
}

UniValue yed_listvaults(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw std::runtime_error(
            "yed_listvaults ( \"status\" count skip )\n"
            "\nVaults in outpoint order, optionally filtered by status (ACTIVE|VOID|CLOSED|CLAIMED); paged (default count 100).\n");

    YellowbackIndex& index = EnsureIndex();
    std::string status = params.size() > 0 && !params[0].isNull() ? params[0].get_str() : "";
    int count = params.size() > 1 && !params[1].isNull() ? params[1].get_int() : 100;
    int skip = params.size() > 2 && !params[2].isNull() ? params[2].get_int() : 0;
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "count and skip must be >= 0");
    if (!status.empty() && status != "ACTIVE" && status != "VOID" && status != "CLOSED" && status != "CLAIMED") {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "status must be ACTIVE, VOID, CLOSED or CLAIMED");
    }
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int tip = IndexHeight(index);
    std::optional<Snapshot> snap = tip >= 0 ? st.GetSnapshot((uint32_t)tip) : std::nullopt;
    const bool abandoned = index.IsAbandoned();
    UniValue list(UniValue::VARR);
    int seen = 0;
    index.View().Iterate("V", [&](const std::string& k, const std::string& raw) {
        VaultRecord v;
        if (!DeserializeRecord(raw, v)) return true;
        if (!status.empty() && status != VaultStatusName(v.Status())) return true;
        if (seen++ < skip) return true;
        if ((int)list.size() >= count) return false;
        COutPoint out(keys::OutPointHashOf(k), keys::OutPointIndexOf(k));
        list.push_back(VaultToJSON(out, v, index, snap, abandoned));
        return true;
    });
    return list;
}

UniValue yed_listclaimable(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "yed_listclaimable\n"
            "\nACTIVE vaults past claimHeight that are underwater at the tip snapshot (RED-4 would pass).\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    State st(index.View());
    const int tip = IndexHeight(index);
    std::optional<Snapshot> snap = tip >= 0 ? st.GetSnapshot((uint32_t)tip) : std::nullopt;
    UniValue list(UniValue::VARR);
    if (!snap.has_value() || !snap->PClaim().has_value()) return list;
    const MicroUsd pClaim = snap->PClaim().value();
    index.View().Iterate("V", [&](const std::string& k, const std::string& raw) {
        VaultRecord v;
        if (!DeserializeRecord(raw, v)) return true;
        if (v.Status() != VaultStatus::ACTIVE || tip < v.claimHeight) return true;
        if (!IsUnderwater(v.collateralZat, pClaim, v.mintedCents, p.claimThresholdBps)) return true;
        COutPoint out(keys::OutPointHashOf(k), keys::OutPointIndexOf(k));
        const CPubKey owner = v.OwnerKey();
        UniValue o(UniValue::VOBJ);
        o.pushKV("vault", strprintf("%s:%u", out.hash.GetHex(), out.n));
        o.pushKV("ownerAddress", owner.IsValid() ? EncodeAddress(owner.GetID(), p) : "");
        o.pushKV("collateralZat", v.collateralZat);
        o.pushKV("mintedCents", v.mintedCents);
        o.pushKV("feeZat", FeeZat(v.collateralZat, p.feeMin, p.feeBps));
        o.pushKV("claimHeight", (int64_t)v.claimHeight);
        o.pushKV("underwaterAt", UnderwaterAt(v, p));
        o.pushKV("pClaim", pClaim);
        list.push_back(o);
        return true;
    });
    return list;
}

UniValue yed_gettxinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_gettxinfo \"txid\"\n"
            "\nThe index's record of a confirmed transaction that created or spent Tokens/Vaults (N7).\n");

    YellowbackIndex& index = EnsureIndex();
    uint256 txid = ParseHashV(params[0], "txid");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    std::optional<TxLogRecord> log = st.GetTxLog(txid);
    if (!log.has_value()) throw JSONRPCError(RPC_INVALID_PARAMETER, "tx-not-found: " + txid.GetHex() + " is not in the Yellowback index");
    return TxLogToJSON(txid, log.value());
}

UniValue yed_decodepayload(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_decodepayload \"hex\"\n"
            "\nDecode a version-2 Yellowback payload (the data push, a whole OP_RETURN script, or a raw transaction). Allowed while unhealthy.\n");

    EnsureIndex();
    std::string hex = params[0].get_str();
    if (!IsHex(hex)) throw JSONRPCError(RPC_INVALID_PARAMETER, "not hex");
    std::vector<unsigned char> bytes = ParseHex(hex);
    Payload p;
    if (DecodePayload(bytes, p)) return PayloadToJSON(p);
    {
        CScript s(bytes.begin(), bytes.end());
        auto data = ExtractOpReturnData(s);
        if (data.has_value() && DecodePayload(data.value(), p)) return PayloadToJSON(p);
    }
    CTransaction tx;
    if (DecodeHexTx(tx, hex)) {
        auto fp = FindPayload(tx);
        if (fp.has_value()) {
            UniValue o = PayloadToJSON(fp->payload);
            o.pushKV("opReturnIndex", (int64_t)fp->opReturnIndex);
            return o;
        }
    }
    UniValue o(UniValue::VOBJ);
    o.pushKV("valid", false);
    o.pushKV("version", 0);
    o.pushKV("type", "none");
    o.pushKV("reason", "malformed: not a version-2 Yellowback payload, OP_RETURN script or transaction carrying one");
    return o;
}

UniValue yed_validaterawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_validaterawtransaction \"hex\"\n"
            "\nDry run of §3.8 at the tip over a one-transaction pseudo-block (MempoolCheck's predicate) plus script verification.\n");

    YellowbackIndex& index = EnsureIndex();
    CTransaction tx;
    if (!DecodeHexTx(tx, params[0].get_str())) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const int next = IndexHeight(index) + 1;
    const yellowback::Params& p = index.ParamsAt(next);
    // The same pseudo-block MempoolCheck evaluates (K7).
    CMutableTransaction cb;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vin[0].scriptSig = CScript() << next;
    CBlock pseudo;
    pseudo.vtx.push_back(CTransaction(cb));
    pseudo.vtx.push_back(tx);
    OverlayStateView overlay(index.MutableView());
    BlockEvaluation ev = EvaluateBlock(overlay, p, pseudo, next, uint256(), 0);
    const uint256 txid = tx.GetHash();
    TxLogRecord log;
    bool relevant = false;
    for (const auto& e : ev.txlogs) if (e.first == txid) { log = e.second; relevant = true; }
    UniValue o(UniValue::VOBJ);
    CCoinsViewCache view(pcoinsTip);
    FetchInputs(tx, view);
    std::string err;
    o.pushKV("valid", VerifyAllInputs(tx, view, SignerBranchId(), err));
    o.pushKV("verdict", relevant ? log.verdict : std::string(verdict::OK));
    o.pushKV("type", relevant ? TypeLower(log.Type()) : std::string("none"));
    o.pushKV("path", log.path);
    o.pushKV("yedIn", log.yedIn);
    o.pushKV("yedOut", log.yedOut);
    o.pushKV("burned", log.burned);
    o.pushKV("feeZat", log.feeZat);
    o.pushKV("payee", PayeeOrNull(log.hasPayee, log.payee));
    o.pushKV("blockValid", !ev.blockInvalid);
    bool expiryOk = true;
    if (log.Type() == TxLogType::REDEEM) {
        std::optional<FoundPayload> fp = FindPayload(tx);
        int64_t refHeight = -1;
        if (fp.has_value() && fp->payload.type == PayloadType::REDEEM) refHeight = fp->payload.refHeight;
        expiryOk = tx.nExpiryHeight != 0 && refHeight >= 0 && (int64_t)tx.nExpiryHeight <= refHeight + p.refWindow;
    }
    o.pushKV("wouldBeRejected", index.MempoolCheckReason(tx).has_value());
    o.pushKV("mempoolExpiryOk", expiryOk);
    UniValue unconfirmed(UniValue::VARR);
    for (const CTxIn& in : tx.vin) {
        const CCoins* coins = pcoinsTip->AccessCoins(in.prevout.hash);
        if (!coins || !coins->IsAvailable(in.prevout.n)) unconfirmed.push_back(OutPointToJSON(in.prevout));
    }
    o.pushKV("unconfirmedInputs", unconfirmed);
    return o;
}

UniValue yed_getblockverdict(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_getblockverdict \"blockhash\"\n"
            "\nEvaluateBlock on a stored block whose parent is the index tip (N12): explains a rejection. Allowed while unhealthy.\n");

    YellowbackIndex& index = EnsureIndex();
    uint256 hash = ParseHashV(params[0], "blockhash");
    LOCK(cs_main);
    LOCK(index.cs_yellowback);
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi == mapBlockIndex.end()) throw JSONRPCError(RPC_INVALID_PARAMETER, "block not found");
    const CBlockIndex* pindex = mi->second;
    std::optional<TipRecord> tip = index.GetTip();
    const bool parentIsTip = tip.has_value() ? (pindex->pprev && pindex->pprev->GetBlockHash() == tip->blockHash)
                                             : (pindex->nHeight == index.GetParams().startHeight);
    if (!parentIsTip) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("verdict-parent-not-tip: the block's parent is not the index tip (%d); the state at another parent would need an undo replay",
                                                            tip.has_value() ? tip->height : -1));
    }
    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, ::Params().GetConsensus())) throw JSONRPCError(RPC_INVALID_PARAMETER, "block not on disk");
    const int h = pindex->nHeight;
    OverlayStateView overlay(index.MutableView());
    BlockEvaluation ev = EvaluateBlock(overlay, index.ParamsAt(h), block, h, hash, GetBlockSubsidy(h, ::Params().GetConsensus()));
    UniValue o(UniValue::VOBJ);
    o.pushKV("blockInvalid", ev.blockInvalid);
    o.pushKV("reason", ev.reason);
    o.pushKV("enforcementOn", ev.enforcementOn);
    UniValue txs(UniValue::VARR);
    for (const auto& e : ev.txlogs) {
        UniValue t(UniValue::VOBJ);
        t.pushKV("txid", e.first.GetHex());
        t.pushKV("type", TypeLower(e.second.Type()));
        t.pushKV("path", e.second.path);
        t.pushKV("verdict", e.second.verdict);
        t.pushKV("yedIn", e.second.yedIn);
        t.pushKV("yedOut", e.second.yedOut);
        t.pushKV("feeZat", e.second.feeZat);
        t.pushKV("payee", PayeeOrNull(e.second.hasPayee, e.second.payee));
        t.pushKV("closedVaults", OutPointsToJSON(e.second.closedVaults));
        txs.push_back(t);
    }
    o.pushKV("transactions", txs);
    return o;
}

UniValue yed_estimatecollateral(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw std::runtime_error(
            "yed_estimatecollateral cents lockBlocks ( priceMicroUsd )\n"
            "\nThe collateral (zatoshi, rounded up to 1,000) to mint cents with a lock of lockBlocks at the reference snapshot\n"
            "R = tip - REF_LAG (or at the given P_mint). Does not apply MINTPOL-1.\n");

    YellowbackIndex& index = EnsureIndex();
    if (!params[0].isNum() || !params[1].isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "cents and lockBlocks must be numbers");
    int64_t cents = params[0].get_int64();
    int64_t lockBlocks = params[1].get_int64();
    const yellowback::Params& p = index.GetParams();
    const int termClass = p.ClassForLockBlocks(lockBlocks);
    if (termClass < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("mint-bad-lock: lockBlocks %d is in no term class", lockBlocks));
    if (cents < p.minMint || cents > p.maxMint) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("cents must be between %d and %d", p.minMint, p.maxMint));
    }
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int tipH = IndexHeight(index);
    const int refH = tipH - g_yellowbackMintLag;
    const int64_t lockHeight = (int64_t)refH + lockBlocks;
    if (lockHeight + p.grace >= LOCKTIME_THRESHOLD) throw JSONRPCError(RPC_INVALID_PARAMETER, "mint-bad-lock: lockHeight + GRACE exceeds LOCKTIME_THRESHOLD");
    std::optional<Snapshot> snap = SnapshotAt(st, p, refH);
    std::optional<MicroUsd> pMint;
    int sigmaMultBps = snap.has_value() ? snap->sigmaMultBps : p.sigmaMultMaxBps;
    if (params.size() > 2 && !params[2].isNull()) pMint = params[2].get_int64();
    else if (snap.has_value()) pMint = snap->PMint();
    const int minRatio = MinRatioBps(p.baseRatioBps[termClass], sigmaMultBps);
    UniValue o(UniValue::VOBJ);
    if (!pMint.has_value()) {
        o.pushKV("requiredZat", NullUniValue);
    } else {
        auto req = RequiredCollateralRounded((Cents)cents, minRatio, pMint.value());
        if (!req.has_value()) throw JSONRPCError(RPC_VERIFY_REJECTED, "mint-unsatisfiable: the required collateral exceeds MAX_MONEY");
        o.pushKV("requiredZat", req.value());
    }
    o.pushKV("termClass", ClassLetter((uint8_t)termClass));
    o.pushKV("lockHeight", lockHeight);
    o.pushKV("claimHeight", lockHeight + p.grace);
    o.pushKV("minRatioBps", minRatio);
    o.pushKV("baseRatioBps", p.baseRatioBps[termClass]);
    o.pushKV("sigmaMultBps", sigmaMultBps);
    o.pushKV("pMint", PriceOrNull(pMint));
    o.pushKV("refHeight", refH);
    return o;
}

UniValue yed_estimatefee(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "yed_estimatefee collateralZat\n"
            "\nFEE-1: max(FEE_MIN, collateralZat * FEE_BPS / 10^4).\n");

    YellowbackIndex& index = EnsureIndex();
    if (!params[0].isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "collateralZat must be a number");
    const int64_t collateral = params[0].get_int64();
    if (collateral < 0 || collateral > MAX_MONEY) throw JSONRPCError(RPC_INVALID_PARAMETER, "collateralZat out of range");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    UniValue o(UniValue::VOBJ);
    o.pushKV("feeZat", FeeZat(collateral, p.feeMin, p.feeBps));
    return o;
}

UniValue yed_gethistory(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw std::runtime_error(
            "yed_gethistory from to\n"
            "\nThe Snapshots records (§3.6) for startHeight <= from <= h <= to <= tip; at most 2,016 rows per call.\n");

    YellowbackIndex& index = EnsureIndex();
    if (!params[0].isNum() || !params[1].isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "from and to must be numbers");
    int from = params[0].get_int();
    int to = params[1].get_int();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    const yellowback::Params& p = index.GetParams();
    const int tip = IndexHeight(index);
    if (from < p.startHeight) from = p.startHeight;   // rows exist from startHeight only (index_start_height_above_tip)
    if (from < 0 || to < from || to > tip || to - from >= 2016) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("range must satisfy %d <= from <= to <= %d with at most 2016 rows", p.startHeight, tip));
    }
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
    { "yellowback", "yed_getprice",                &yed_getprice,                 true  },
    { "yellowback", "yed_getactivation",           &yed_getactivation,            true  },
    { "yellowback", "yed_listminers",              &yed_listminers,               true  },
    { "yellowback", "yed_gettag",                  &yed_gettag,                   true  },
    { "yellowback", "yed_setquote",                &yed_setquote,                 true  },
    { "yellowback", "yed_getfeepayee",             &yed_getfeepayee,              true  },
    { "yellowback", "yed_getvault",                &yed_getvault,                 true  },
    { "yellowback", "yed_listvaults",              &yed_listvaults,               true  },
    { "yellowback", "yed_listclaimable",           &yed_listclaimable,            true  },
    { "yellowback", "yed_gettxinfo",               &yed_gettxinfo,                true  },
    { "yellowback", "yed_decodepayload",           &yed_decodepayload,            true  },
    { "yellowback", "yed_validaterawtransaction",  &yed_validaterawtransaction,   true  },
    { "yellowback", "yed_getblockverdict",         &yed_getblockverdict,          true  },
    { "yellowback", "yed_estimatecollateral",      &yed_estimatecollateral,       true  },
    { "yellowback", "yed_estimatefee",             &yed_estimatefee,              true  },
    { "yellowback", "yed_gethistory",              &yed_gethistory,               true  },
};

void RegisterYellowbackRPCCommands(CRPCTable &tableRPC)
{
    // Plan section 8.3: "For a node without -yellowback: nothing; every code path is v4.5.0's."
    // A registered-but-refusing command is still a command: it shows in `help`, in the `== Yellowback ==`
    // category header and in any tooling that enumerates the RPC surface, so a stock-configured
    // fork binary would not look like v4.5.0 to an operator.  InitExperimentalMode() has already
    // run when RegisterAllCoreRPCCommands is called (init.cpp), so the flag is readable here.
    // Found by qa/rpc-tests/yellowback_stock_node.py.
    if (!fExperimentalYellowback) return;
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
