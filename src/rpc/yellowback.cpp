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
    o.pushKV("ownerKeyId", v.ownerPubKey.IsValid() ? v.ownerPubKey.GetID().GetHex() : "");
    o.pushKV("tier", (int)v.tier);
    o.pushKV("lockHeight", (int64_t)v.lockHeight);
    o.pushKV("collateralZat", v.collateralZat);
    o.pushKV("collateral", ValueFromAmount(v.collateralZat));
    o.pushKV("mintedCents", v.mintedCents);
    o.pushKV("mintHeight", v.mintHeight);
    o.pushKV("rosterIndex", v.rosterIndex);
    if (v.Status() == VaultStatus::VOID) o.pushKV("voidReason", v.voidReason);
    if (v.Status() == VaultStatus::CLOSED) {
        o.pushKV("wasActive", v.wasActive);
        o.pushKV("closeHeight", v.closeHeight);
        o.pushKV("closingTxid", v.closingTxid.GetHex());
        o.pushKV("burnedCents", v.burnedCents);
        o.pushKV("errBpsAtClose", v.errBpsAtClose);
        o.pushKV("requiredBurnAtClose", v.requiredBurnAtClose);
        o.pushKV("unbacked", v.wasActive && v.burnedCents < v.requiredBurnAtClose);
    }
    return o;
}

UniValue TxLogToJSON(const uint256& txid, const TxLogRecord& l)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", txid.GetHex());
    o.pushKV("height", l.height);
    o.pushKV("type", l.type == 0 ? "none" : PayloadTypeName((PayloadType)l.type));
    o.pushKV("verdict", l.verdict);
    o.pushKV("yedIn", l.yedIn);
    o.pushKV("yedOut", l.yedOut);
    o.pushKV("burned", l.burned);
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
    o.pushKV("anchorSpend", l.anchorSpend);
    o.pushKV("priceRecorded", l.priceRecorded);
    return o;
}

UniValue SnapshotToJSON(int height, const Snapshot& s)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", height);
    o.pushKV("blockhash", s.blockHash.GetHex());
    o.pushKV("supplyCents", s.supplyCents);
    o.pushKV("collateralZat", s.collateralZat);
    if (s.priceDefined) o.pushKV("priceMicroUsd", s.price);
    else o.pushKV("priceMicroUsd", NullUniValue);
    o.pushKV("healthPct", s.healthPct);
    o.pushKV("dcaBps", s.dcaBps);
    o.pushKV("errBps", s.errBps);
    o.pushKV("mintFrozen", s.mintFrozen);
    return o;
}

UniValue PayloadToJSON(const Payload& p)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("type", PayloadTypeName(p.type));
    switch (p.type) {
    case PayloadType::MINT:
        o.pushKV("tier", (int)p.tier);
        o.pushKV("cents", (int64_t)p.cents);
        o.pushKV("lockHeight", (int64_t)p.lockHeight);
        o.pushKV("evalHeight", (int64_t)p.evalHeight);
        o.pushKV("ownerPubKey", HexStr(p.ownerPubKey.begin(), p.ownerPubKey.end()));
        break;
    case PayloadType::TRANSFER:
    case PayloadType::REDEEM: {
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
    case PayloadType::PRICE:
        o.pushKV("priceMicroUsd", (int64_t)p.priceMicroUsd);
        break;
    }
    return o;
}

/** Dry-run §3.7 for a transaction at the next height without touching the index. */
UniValue DryRun(YellowbackIndex& index, const CTransaction& tx)
{
    AssertLockHeld(index.cs_yellowback);
    OverlayStateView overlay(index.MutableView());
    State st(overlay);
    const int height = IndexHeight(index) + 1;
    bool relevant = false;
    TxLogRecord log = ProcessTx(st, index.GetParams(), tx, height, relevant);
    UniValue o = TxLogToJSON(tx.GetHash(), log);
    o.pushKV("relevant", relevant);
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
            "  \"anchor\": {...},\n"
            "  \"rosterIndex\": n,\n"
            "  \"params\": {...}\n"
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
    o.pushKV("genesisAnchor", OutPointToJSON(p.genesisAnchor));
    o.pushKV("height", tip.has_value() ? tip->height : -1);
    o.pushKV("blockhash", tip.has_value() ? tip->blockHash.GetHex() : "");
    o.pushKV("chainHeight", chainActive.Height());
    o.pushKV("synced", index.IsSynced());
    o.pushKV("healthy", index.IsHealthy());
    o.pushKV("unhealthyReason", index.UnhealthyReason());
    AnchorRecord a = st.GetAnchor();
    UniValue anchor(UniValue::VOBJ);
    anchor.pushKV("valid", a.valid);
    anchor.pushKV("txid", a.outpoint.hash.GetHex());
    anchor.pushKV("vout", (int64_t)a.outpoint.n);
    anchor.pushKV("valueZat", a.nValue);
    anchor.pushKV("address", ScriptAddress(a.scriptPubKey));
    o.pushKV("anchor", anchor);
    o.pushKV("rosterIndex", (int64_t)st.Rosters().size() - 1);
    UniValue pp(UniValue::VOBJ);
    pp.pushKV("minMintCents", p.minMint);
    pp.pushKV("maxMintCents", p.maxMint);
    pp.pushKV("minOutputCents", p.minOutput);
    pp.pushKV("maxOutputCents", p.maxOutput);
    pp.pushKV("supplyCapCents", p.supplyCap);
    UniValue tiers(UniValue::VARR);
    for (int i = 0; i < NUM_TIERS; i++) {
        UniValue t(UniValue::VOBJ);
        t.pushKV("tier", i);
        t.pushKV("blocks", p.tierBlocks[i]);
        t.pushKV("ratioPct", p.tierRatioPct[i]);
        tiers.push_back(t);
    }
    pp.pushKV("tiers", tiers);
    pp.pushKV("rosterGrace", p.rosterGrace);
    pp.pushKV("volCooldown", p.volCooldown);
    pp.pushKV("priceMaxAge", PRICE_MAX_AGE);
    pp.pushKV("mintWindow", MINT_WINDOW);
    pp.pushKV("mintEvalLag", g_yellowbackMintLag);
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
            "\nSupply, collateral, vault counts, price, health, DCA, ERR and mint-freeze state at the index tip.\n");

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
    o.pushKV("supplyCapCents", index.GetParams().supplyCap);
    if (snap.has_value()) {
        if (snap->priceDefined) o.pushKV("priceMicroUsd", snap->price);
        else o.pushKV("priceMicroUsd", NullUniValue);
        std::optional<uint32_t> src = st.PriceSourceHeight((uint32_t)h);
        o.pushKV("priceHeight", src.has_value() ? (int64_t)src.value() : -1);
        o.pushKV("priceAge", src.has_value() ? (int64_t)(h - (int)src.value()) : -1);
        o.pushKV("healthPct", snap->healthPct);
        o.pushKV("dcaBps", snap->dcaBps);
        o.pushKV("errBps", snap->errBps);
        o.pushKV("mintFrozen", snap->mintFrozen);
        Volatility vol = st.GetVolatility();
        o.pushKV("lastBreachHeight", vol.lastBreachHeight);
        o.pushKV("mintFrozenUntil", vol.lastBreachHeight >= 0 ? vol.lastBreachHeight + index.GetParams().volCooldown : -1);
    } else {
        o.pushKV("priceMicroUsd", NullUniValue);
        o.pushKV("priceHeight", -1);
        o.pushKV("priceAge", -1);
        o.pushKV("healthPct", HEALTH_CAP);
        o.pushKV("dcaBps", 10000);
        o.pushKV("errBps", 10000);
        o.pushKV("mintFrozen", false);
        o.pushKV("lastBreachHeight", -1);
        o.pushKV("mintFrozenUntil", -1);
    }
    return o;
}

UniValue yed_getprotectionstatus(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "yed_getprotectionstatus\n"
            "\nThe three protection systems at the index tip (plan D12): DCA (collateral multiplier by health band),\n"
            "ERR (burn ratio by health band) and the volatility mint freeze, with the prices the freeze compares.\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const yellowback::Params& p = index.GetParams();
    const int h = IndexHeight(index);
    std::optional<Snapshot> snap = h >= 0 ? st.GetSnapshot((uint32_t)h) : std::nullopt;
    Volatility vol = st.GetVolatility();
    const int health = snap.has_value() ? snap->healthPct : HEALTH_CAP;
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", h);
    o.pushKV("healthPct", health);
    UniValue dca(UniValue::VOBJ);
    dca.pushKV("bps", DcaBps(health));
    dca.pushKV("band", health >= 150 ? "healthy" : health >= 120 ? "warning" : health >= 110 ? "critical" : "emergency");
    o.pushKV("dca", dca);
    UniValue err(UniValue::VOBJ);
    err.pushKV("bps", ErrBps(health));
    err.pushKV("active", health < 100);
    err.pushKV("burnMultiplierBps", (int64_t)(10000LL * 10000 / ErrBps(health)));
    o.pushKV("err", err);
    UniValue v(UniValue::VOBJ);
    v.pushKV("mintFrozen", snap.has_value() ? snap->mintFrozen : false);
    v.pushKV("lastBreachHeight", vol.lastBreachHeight);
    v.pushKV("frozenUntil", vol.lastBreachHeight >= 0 ? vol.lastBreachHeight + p.volCooldown : -1);
    v.pushKV("cooldownBlocks", p.volCooldown);
    auto pushPrice = [&](const char* name, int at) {
        if (at < 0) { v.pushKV(name, NullUniValue); return; }
        std::optional<MicroUsd> pr = st.PriceInEffect((uint32_t)at);
        if (pr.has_value()) v.pushKV(name, pr.value()); else v.pushKV(name, NullUniValue);
    };
    pushPrice("priceNow", h);
    pushPrice("priceShortWindow", h - p.volWindowShort);
    pushPrice("priceLongWindow", h - p.volWindowLong);
    v.pushKV("shortWindowBlocks", p.volWindowShort);
    v.pushKV("longWindowBlocks", p.volWindowLong);
    v.pushKV("shortThresholdBps", VOL_1H_BPS);
    v.pushKV("longThresholdBps", VOL_24H_BPS);
    o.pushKV("volatility", v);
    o.pushKV("mintingAllowed", snap.has_value() && snap->priceDefined && health >= 100 && !snap->mintFrozen);
    return o;
}

UniValue yed_getprice(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
            "yed_getprice ( height )\n"
            "\nThe price in effect at the given height (default: index tip): the most recent attestation\n"
            "at or below it that is at most " + std::to_string(PRICE_MAX_AGE) + " blocks old.\n"
            "\nResult:\n{ \"height\": n, \"priceMicroUsd\": n|null, \"sourceHeight\": n, \"age\": n }\n");

    YellowbackIndex& index = EnsureIndex();
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    int h = IndexHeight(index);
    if (params.size() > 0 && !params[0].isNull()) h = params[0].get_int();
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", h);
    std::optional<uint32_t> src = h >= 0 ? st.PriceSourceHeight((uint32_t)h) : std::nullopt;
    if (src.has_value()) {
        o.pushKV("priceMicroUsd", st.GetPriceAt(src.value()).value());
        o.pushKV("sourceHeight", (int64_t)src.value());
        o.pushKV("age", (int64_t)(h - (int)src.value()));
    } else {
        o.pushKV("priceMicroUsd", NullUniValue);
        o.pushKV("sourceHeight", -1);
        o.pushKV("age", -1);
    }
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
    if (v->Status() == VaultStatus::ACTIVE) {
        const int h = IndexHeight(index);
        std::optional<Snapshot> snap = st.GetSnapshot((uint32_t)h);
        int errBps = snap.has_value() ? snap->errBps : 10000;
        o.pushKV("requiredBurnCents", RequiredBurn(v->mintedCents, errBps));
    } else if (v->Status() == VaultStatus::VOID) {
        o.pushKV("requiredBurnCents", 0);
    }
    o.pushKV("indexHeight", IndexHeight(index));
    return o;
}

UniValue yed_listvaults(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 4)
        throw std::runtime_error(
            "yed_listvaults ( \"status\" rosterIndex count skip )\n"
            "\nVaults, optionally filtered by status (ACTIVE|VOID|CLOSED) and roster index; paged (default count 100).\n"
            "Also returns the number of open (ACTIVE or VOID) vaults per roster index for the rotation runbook.\n");

    YellowbackIndex& index = EnsureIndex();
    std::string status = params.size() > 0 && !params[0].isNull() ? params[0].get_str() : "";
    int rosterIndex = params.size() > 1 && !params[1].isNull() ? params[1].get_int() : -2;
    int count = params.size() > 2 && !params[2].isNull() ? params[2].get_int() : 100;
    int skip = params.size() > 3 && !params[3].isNull() ? params[3].get_int() : 0;
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "count and skip must be >= 0");
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    UniValue list(UniValue::VARR);
    std::map<int, int> openPerRoster;
    int seen = 0, total = 0;
    index.View().Iterate("V", [&](const std::string& k, const std::string& raw) {
        VaultRecord v;
        if (!DeserializeRecord(raw, v)) return true;
        if (v.IsOpen()) openPerRoster[v.rosterIndex]++;
        if (!status.empty() && status != VaultStatusName(v.Status())) return true;
        if (rosterIndex != -2 && v.rosterIndex != rosterIndex) return true;
        total++;
        if (seen++ < skip) return true;
        if ((int)list.size() >= count) return true;
        COutPoint out(uint256(std::vector<unsigned char>(k.begin() + 1, k.begin() + 33)), 0);
        list.push_back(VaultToJSON(out, v));
        return true;
    });
    UniValue o(UniValue::VOBJ);
    o.pushKV("height", IndexHeight(index));
    o.pushKV("total", total);
    o.pushKV("vaults", list);
    UniValue per(UniValue::VOBJ);
    for (const auto& kv : openPerRoster) per.pushKV(std::to_string(kv.first), kv.second);
    o.pushKV("openVaultsPerRoster", per);
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
            "yed_estimatecollateral cents tier ( priceMicroUsd )\n"
            "\nThe collateral (zatoshi, rounded up to 1,000) required to mint cents at tier, at the price and\n"
            "DCA multiplier of the height the wallet would evaluate at (index tip minus the mint lag), or at\n"
            "the given price with the current DCA.\n");

    YellowbackIndex& index = EnsureIndex();
    int64_t cents = params[0].get_int64();
    int tier = params[1].get_int();
    const yellowback::Params& p = index.GetParams();
    if (!p.IsValidTier(tier)) throw JSONRPCError(RPC_INVALID_PARAMETER, "tier must be 0..4");
    if (cents < p.minMint || cents > p.maxMint) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("cents must be between %d and %d", p.minMint, p.maxMint));
    }
    LOCK(index.cs_yellowback);
    EnsureHealthy(index);
    State st(index.View());
    const int tipH = IndexHeight(index);
    const int evalH = tipH - g_yellowbackMintLag;
    std::optional<Snapshot> snap = evalH >= p.startHeight ? st.GetSnapshot((uint32_t)evalH) : std::nullopt;
    std::optional<MicroUsd> price;
    int dcaBps = snap.has_value() ? snap->dcaBps : 10000;
    if (params.size() > 2 && !params[2].isNull()) price = params[2].get_int64();
    else if (snap.has_value() && snap->priceDefined) price = snap->price;
    UniValue o(UniValue::VOBJ);
    o.pushKV("cents", cents);
    o.pushKV("tier", tier);
    o.pushKV("ratioPct", p.tierRatioPct[tier]);
    o.pushKV("lockBlocks", p.tierBlocks[tier]);
    o.pushKV("evalHeight", evalH);
    o.pushKV("dcaBps", dcaBps);
    if (!price.has_value()) {
        o.pushKV("priceMicroUsd", NullUniValue);
        o.pushKV("requiredZat", NullUniValue);
        o.pushKV("error", verdict::BAD_ORACLE_PRICE);
        return o;
    }
    o.pushKV("priceMicroUsd", price.value());
    auto req = RequiredCollateralRounded(cents, p.tierRatioPct[tier], dcaBps, price.value());
    if (!req.has_value()) {
        o.pushKV("requiredZat", NullUniValue);
        o.pushKV("error", "collateral-out-of-range");
        return o;
    }
    o.pushKV("requiredZat", req.value());
    o.pushKV("required", ValueFromAmount(req.value()));
    o.pushKV("lockHeight", (int64_t)evalH + p.tierBlocks[tier] + MINT_WINDOW);
    o.pushKV("unlockHeight", (int64_t)evalH + p.tierBlocks[tier] + MINT_WINDOW);
    o.pushKV("expiryHeight", (int64_t)evalH + MINT_WINDOW);
    return o;
}

UniValue yed_gethistory(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw std::runtime_error(
            "yed_gethistory fromHeight toHeight\n"
            "\nPer-block snapshots (supply, collateral, price, health, DCA, ERR, mint freeze) for the range; at most 10,000 blocks.\n");

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
    { "yellowback", "yed_getprice",                &yed_getprice,                 true  },
    { "yellowback", "yed_getprotectionstatus",     &yed_getprotectionstatus,     true  },
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
