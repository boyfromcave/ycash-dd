// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "ydollar/index.h"

#include "chain.h"
#include "chainparams.h"
#include "main.h"
#include "util.h"
#include "utilstrencodings.h"
#include "ydollar/script.h"
#include "ydollar/state.h"

namespace ydollar {

YDollarIndex* g_ydollar = nullptr;
CAmount g_ydollarFee = DEFAULT_YD_FEE;
int g_ydollarMintLag = DEFAULT_MINT_EVAL_LAG;

YDollarIndex::YDollarIndex(const Params& params, const fs::path& dir, size_t cacheSize, bool fWipe)
    : params(params), db(new YDollarDB(dir, cacheSize, false, fWipe)), healthy(true), stopped(false)
{
    if (fWipe) LogPrintf("ydollar: index wiped (-reindex-ydollar)\n");
}

YDollarIndex::~YDollarIndex() {}

void YDollarIndex::SetUnhealthy(const std::string& reason)
{
    if (healthy) LogPrintf("ydollar: index unhealthy: %s; restart with -reindex-ydollar\n", reason);
    healthy = false;
    unhealthyReason = reason;
}

void YDollarIndex::Stop()
{
    LOCK(cs_ydollar);
    stopped = true;
}

void YDollarIndex::Flush(bool fSync)
{
    LOCK(cs_ydollar);
    db->Commit(fSync);
}

std::optional<TipRecord> YDollarIndex::GetTip() const
{
    AssertLockHeld(cs_ydollar);
    return State(*db).GetTip();
}

bool YDollarIndex::IsSynced() const
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs_ydollar);
    const CBlockIndex* chainTip = chainActive.Tip();
    std::optional<TipRecord> tip = GetTip();
    if (!chainTip || chainTip->nHeight < params.startHeight) return !tip.has_value();
    return tip.has_value() && tip->blockHash == chainTip->GetBlockHash();
}

uint256 YDollarIndex::GetStateHash() const
{
    AssertLockHeld(cs_ydollar);
    return StateHash(*db);
}

void YDollarIndex::Wipe(const std::string& why)
{
    LogPrintf("ydollar: wiping index (%s)\n", why);
    db->Wipe();
}

bool YDollarIndex::ApplyOne(const CBlock& block, int height, const uint256& hash, std::string& error)
{
    UndoRecord undo;
    std::optional<std::string> err = ApplyBlock(*db, params, block, height, hash, undo);
    if (err.has_value()) {
        db->Discard();
        error = err.value();
        return false;
    }
    if (height < params.startHeight) return true; // nothing happened
    db->Write(keys::Undo(hash), SerializeRecord(undo));
    // Prune undo below tip - UNDO_KEEP (B10).
    int old = height - UNDO_KEEP;
    if (old >= params.startHeight) {
        std::optional<Snapshot> snap = State(*db).GetSnapshot((uint32_t)old);
        if (snap.has_value()) db->Erase(keys::Undo(snap->blockHash));
    }
    if (!db->Commit(false)) {
        error = "database write failed";
        return false;
    }
    LogPrint("ydollar", "applied block %d %s\n", height, hash.ToString());
    return true;
}

bool YDollarIndex::UndoOne(const uint256& hash, std::string& error)
{
    std::string raw;
    UndoRecord undo;
    if (!db->Read(keys::Undo(hash), raw) || !DeserializeRecord(raw, undo)) {
        error = "undo record missing for " + hash.ToString();
        return false;
    }
    UndoBlock(*db, undo);
    db->Erase(keys::Undo(hash));
    if (!db->Commit(false)) {
        error = "database write failed";
        return false;
    }
    LogPrint("ydollar", "undid block %d %s\n", undo.height, hash.ToString());
    return true;
}

bool YDollarIndex::SyncToChain()
{
    LOCK(cs_main);
    LOCK(cs_ydollar);
    if (!params.IsConfigured()) {
        SetUnhealthy("ydollar parameters are not configured for this network (no genesis anchor)");
        return false;
    }

    std::optional<TipRecord> tip = State(*db).GetTip();
    if (tip.has_value() && (tip->schemaVersion != SCHEMA_VERSION || tip->network != params.network)) {
        Wipe("schema or network changed");
        tip = std::nullopt;
    }

    const CBlockIndex* chainTip = chainActive.Tip();
    if (!chainTip || chainTip->nHeight < params.startHeight) {
        if (tip.has_value()) Wipe("chain below start height (reindex?)");
        LogPrintf("ydollar: chain below start height %d; index empty\n", params.startHeight);
        return true;
    }

    // Walk back while the stored tip is not in the active chain (reorg while offline).
    while (tip.has_value()) {
        const CBlockIndex* pindex = chainActive[tip->height];
        if (pindex && pindex->GetBlockHash() == tip->blockHash) break;
        std::string error;
        if (!UndoOne(tip->blockHash, error)) {
            Wipe(error);
            tip = std::nullopt;
            break;
        }
        tip = State(*db).GetTip();
    }

    int from = tip.has_value() ? tip->height + 1 : params.startHeight;
    if (from <= chainTip->nHeight) {
        LogPrintf("ydollar: syncing index from height %d to %d\n", from, chainTip->nHeight);
    }
    for (int h = from; h <= chainTip->nHeight; h++) {
        const CBlockIndex* pindex = chainActive[h];
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, ::Params().GetConsensus())) {
            SetUnhealthy(strprintf("failed to read block %d from disk", h));
            return false;
        }
        std::string error;
        if (!ApplyOne(block, h, pindex->GetBlockHash(), error)) {
            SetUnhealthy(error);
            return false;
        }
    }
    db->Commit(true);
    std::optional<TipRecord> finalTip = State(*db).GetTip();
    LogPrintf("ydollar: index at height %d\n", finalTip.has_value() ? finalTip->height : -1);
    return true;
}

void YDollarIndex::HandleConnect(const CBlockIndex* pindex, const CBlock& block)
{
    const int h = pindex->nHeight;
    if (h < params.startHeight) return;
    std::optional<TipRecord> tip = State(*db).GetTip();
    std::string error;
    if (h == params.startHeight) {
        if (tip.has_value()) {
            // Re-delivery of the start block (e.g. after SyncToChain applied it): idempotent.
            std::optional<Snapshot> snap = State(*db).GetSnapshot((uint32_t)h);
            if (snap.has_value() && snap->blockHash == pindex->GetBlockHash()) return;
            SetUnhealthy(strprintf("start block %s delivered while index tip is at %d", pindex->GetBlockHash().ToString(), tip->height));
            return;
        }
        if (!ApplyOne(block, h, pindex->GetBlockHash(), error)) SetUnhealthy(error);
        return;
    }
    if (tip.has_value() && h <= tip->height) {
        std::optional<Snapshot> snap = State(*db).GetSnapshot((uint32_t)h);
        if (snap.has_value() && snap->blockHash == pindex->GetBlockHash()) return; // already applied (E4)
        SetUnhealthy(strprintf("block %d %s delivered out of order (tip %d)", h, pindex->GetBlockHash().ToString(), tip->height));
        return;
    }
    if (!tip.has_value() || !pindex->pprev || pindex->pprev->GetBlockHash() != tip->blockHash) {
        SetUnhealthy(strprintf("block %d does not extend the index tip", h));
        return;
    }
    if (!ApplyOne(block, h, pindex->GetBlockHash(), error)) SetUnhealthy(error);
}

void YDollarIndex::HandleDisconnect(const CBlockIndex* pindex)
{
    const int h = pindex->nHeight;
    if (h < params.startHeight) return;
    std::optional<TipRecord> tip = State(*db).GetTip();
    if (!tip.has_value() || tip->blockHash != pindex->GetBlockHash()) {
        // Not our tip: skip if we never had this block, else we are out of step.
        std::optional<Snapshot> snap = State(*db).GetSnapshot((uint32_t)h);
        if (!snap.has_value() || snap->blockHash != pindex->GetBlockHash()) return;
        SetUnhealthy(strprintf("disconnect of block %d that is not the index tip", h));
        return;
    }
    std::string error;
    if (!UndoOne(pindex->GetBlockHash(), error)) SetUnhealthy(error);
}

void YDollarIndex::ChainTip(const CBlockIndex* pindex, const CBlock* pblock, std::optional<std::pair<SproutMerkleTree, SaplingMerkleTree>> added)
{
    LOCK(cs_ydollar);
    if (stopped || !healthy) return;
    try {
        if (added.has_value()) {
            if (!pblock) {
                SetUnhealthy("ChainTip connect without block data");
                return;
            }
            HandleConnect(pindex, *pblock);
        } else {
            HandleDisconnect(pindex);
        }
    } catch (const std::exception& e) {
        db->Discard();
        SetUnhealthy(std::string("exception in ChainTip: ") + e.what());
    } catch (...) {
        db->Discard();
        SetUnhealthy("unknown exception in ChainTip");
    }
}

void YDollarIndex::SyncTransaction(const CTransaction& tx, const CBlock* pblock, const int nHeight)
{
    // Wallet pre-locking of assigned outputs is added with the wallet layer (plan B4, Phase 3).
}

std::optional<std::string> ParamsFromArgs(const std::string& networkId, Params& out)
{
    const bool haveStart = mapArgs.count("-ydollarstartheight");
    const bool haveAnchor = mapArgs.count("-ydollargenesisanchor");
    const bool haveRoster = mapArgs.count("-ydollargenesisroster");
    const bool haveCap = mapArgs.count("-ydollarsupplycap");
    if (networkId != "regtest") {
        if (haveStart || haveAnchor || haveRoster || haveCap) {
            return std::string("-ydollarstartheight, -ydollargenesisanchor, -ydollargenesisroster and -ydollarsupplycap are regtest-only");
        }
        out = ParamsForNetwork(networkId);
        return std::nullopt;
    }
    if (!(haveStart && haveAnchor && haveRoster)) {
        return std::string("regtest -ydollar requires -ydollarstartheight, -ydollargenesisanchor and -ydollargenesisroster together");
    }
    int64_t startHeight = GetArg("-ydollarstartheight", 0);
    if (startHeight <= 0) return std::string("-ydollarstartheight must be a positive height");
    std::string anchor = GetArg("-ydollargenesisanchor", "");
    size_t colon = anchor.find(':');
    if (colon == std::string::npos || !IsHex(anchor.substr(0, colon)) || anchor.substr(0, colon).size() != 64) {
        return std::string("-ydollargenesisanchor must be <txid>:<n>");
    }
    uint32_t n = 0;
    try {
        n = (uint32_t)std::stoul(anchor.substr(colon + 1));
    } catch (...) {
        return std::string("-ydollargenesisanchor must be <txid>:<n>");
    }
    COutPoint outpoint(uint256S(anchor.substr(0, colon)), n);
    std::string rosterHex = GetArg("-ydollargenesisroster", "");
    if (!IsHex(rosterHex)) return std::string("-ydollargenesisroster must be the hex roster script");
    std::vector<unsigned char> rosterBytes = ParseHex(rosterHex);
    CScript rosterScript(rosterBytes.begin(), rosterBytes.end());
    Roster roster;
    if (!ParseRosterScript(rosterScript, roster)) {
        return std::string("-ydollargenesisroster is not a k-of-n CHECKMULTISIG script with 1 <= k <= n <= 13 compressed keys");
    }
    int64_t cap = GetArg("-ydollarsupplycap", 0);
    if (cap < 0) return std::string("-ydollarsupplycap must be >= 0");
    out = RegtestParams((int)startHeight, outpoint, rosterScript, cap);
    return std::nullopt;
}

} // namespace ydollar
