// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/index.h"

#include "chain.h"
#include "chainparams.h"
#include "main.h"
#include "util.h"
#include "utilstrencodings.h"
#include "yellowback/script.h"
#include "yellowback/state.h"

namespace yellowback {

YellowbackIndex* g_yellowback = nullptr;
CAmount g_yellowbackFee = DEFAULT_YELLOWBACK_FEE;
int g_yellowbackMintLag = DEFAULT_REF_LAG;

YellowbackIndex::YellowbackIndex(const Params& params, const fs::path& dir, size_t cacheSize, bool fWipe)
    : params(params), db(new YellowbackDB(dir, cacheSize, false, fWipe)), healthy(true), stopped(false)
{
    if (fWipe) LogPrintf("yellowback: index wiped (-reindex-yellowback)\n");
}

YellowbackIndex::~YellowbackIndex() {}

void YellowbackIndex::SetUnhealthy(const std::string& reason)
{
    if (healthy) LogPrintf("yellowback: index unhealthy: %s; restart with -reindex-yellowback\n", reason);
    healthy = false;
    unhealthyReason = reason;
}

void YellowbackIndex::Stop()
{
    LOCK(cs_yellowback);
    stopped = true;
}

void YellowbackIndex::Flush(bool fSync)
{
    LOCK(cs_yellowback);
    db->Commit(fSync);
}

std::optional<TipRecord> YellowbackIndex::GetTip() const
{
    AssertLockHeld(cs_yellowback);
    return State(*db).GetTip();
}

bool YellowbackIndex::IsSynced() const
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs_yellowback);
    const CBlockIndex* chainTip = chainActive.Tip();
    std::optional<TipRecord> tip = GetTip();
    if (!chainTip || chainTip->nHeight < params.startHeight) return !tip.has_value();
    return tip.has_value() && tip->blockHash == chainTip->GetBlockHash();
}

uint256 YellowbackIndex::GetStateHash() const
{
    AssertLockHeld(cs_yellowback);
    return StateHash(*db, params.network);
}

void YellowbackIndex::Wipe(const std::string& why)
{
    LogPrintf("yellowback: wiping index (%s)\n", why);
    db->Wipe();
}

bool YellowbackIndex::ApplyOne(const CBlock& block, int height, const uint256& hash, std::string& error)
{
    if (testBeforeApply) testBeforeApply();
    UndoRecord undo;
    // The block subsidy is EvaluateBlock's argument (N22): state.cpp links against nothing in main.cpp.
    const CAmount subsidy = GetBlockSubsidy(height, ::Params().GetConsensus());
    std::optional<std::string> err = ApplyBlock(*db, params, block, height, hash, subsidy, undo);
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
    LogPrint("yellowback", "applied block %d %s\n", height, hash.ToString());
    return true;
}

bool YellowbackIndex::UndoOne(const uint256& hash, std::string& error)
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
    LogPrint("yellowback", "undid block %d %s\n", undo.height, hash.ToString());
    return true;
}

bool YellowbackIndex::SyncToChain()
{
    LOCK(cs_main);
    LOCK(cs_yellowback);
    if (!params.IsConfigured()) {
        SetUnhealthy("yellowback parameters are not configured for this network (no start height)");
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
        LogPrintf("yellowback: chain below start height %d; index empty\n", params.startHeight);
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
        LogPrintf("yellowback: syncing index from height %d to %d\n", from, chainTip->nHeight);
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
    LogPrintf("yellowback: index at height %d\n", finalTip.has_value() ? finalTip->height : -1);
    return true;
}

void YellowbackIndex::ApplyConnected(const CBlockIndex* pindex, const CBlock& block)
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

void YellowbackIndex::ApplyDisconnected(const CBlockIndex* pindex)
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

void YellowbackIndex::ChainTip(const CBlockIndex* pindex, const CBlock* pblock, std::optional<std::pair<SproutMerkleTree, SaplingMerkleTree>> added)
{
    bool applied = false;
    {
        LOCK(cs_yellowback);
        if (stopped || !healthy) return;
        try {
            if (added.has_value()) {
                if (!pblock) {
                    SetUnhealthy("ChainTip connect without block data");
                    return;
                }
                ApplyConnected(pindex, *pblock);
                applied = true;
            } else {
                ApplyDisconnected(pindex);
            }
        } catch (const std::exception& e) {
            db->Discard();
            SetUnhealthy(std::string("exception in ChainTip: ") + e.what());
        } catch (...) {
            db->Discard();
            SetUnhealthy("unknown exception in ChainTip");
        }
    }
    // Stage (iii) of coin locking runs after every applied block, outside cs_yellowback (B15).
    // Nothing is unlocked on disconnect (C3).
    if (applied && onReconcile) {
        try {
            onReconcile();
        } catch (const std::exception& e) {
            LogPrintf("yellowback: reconcile failed: %s\n", e.what());
        } catch (...) {
            LogPrintf("yellowback: reconcile failed\n");
        }
    }
}

void YellowbackIndex::SyncTransaction(const CTransaction& tx, const CBlock* pblock, const int nHeight)
{
    // Stage (ii) of coin locking (B4): pre-lock every output a well-formed payload assigns to a
    // script that is mine, for mempool and block transactions alike. Never touches the state.
    if (stopped || !onSyncTransaction) return;
    if (nHeight < params.startHeight) return;
    try {
        onSyncTransaction(tx);
    } catch (const std::exception& e) {
        LogPrintf("yellowback: SyncTransaction hook failed: %s\n", e.what());
    } catch (...) {
        LogPrintf("yellowback: SyncTransaction hook failed\n");
    }
}

// ---- Phase 3 provides this; transitional stub (Phase 6 worktree only, replaced at merge) ----
bool YellowbackIndex::IsAbandoned() const
{
    LOCK(cs_yellowback);
    State st(*db);
    std::optional<TipRecord> tip = st.GetTip();
    if (!tip.has_value()) return false;
    if (params.abandonBlocks <= 0) return false;
    for (int h = tip->height; h > tip->height - params.abandonBlocks; h--) {
        if (h < params.startHeight) return false;
        std::optional<Snapshot> s = st.GetSnapshot((uint32_t)h);
        if (!s.has_value() || !(s->haltMask & HALT_ENFORCEMENT)) return false;
    }
    return true;
}

bool YellowbackIndex::MempoolCheck(const CTransaction& tx)
{
    LOCK(cs_yellowback);
    if (!healthy) return true;
    State st(*db);
    std::optional<TipRecord> tip = st.GetTip();
    if (!tip.has_value()) return true;
    bool spendsActive = false;
    for (const CTxIn& in : tx.vin) {
        std::optional<VaultRecord> v = st.GetVault(in.prevout);
        if (v.has_value() && v->Status() == VaultStatus::ACTIVE) { spendsActive = true; break; }
    }
    if (!spendsActive) return true;
    if (IsAbandoned()) return true;   // L13: MP-1 stands down under abandonment
    CMutableTransaction coinbase;
    coinbase.vin.push_back(CTxIn());
    CBlock block;
    block.vtx.push_back(CTransaction(coinbase));
    block.vtx.push_back(tx);
    OverlayStateView overlay(*db);
    BlockEvaluation ev = EvaluateBlock(overlay, params, block, tip->height + 1, uint256(), 0);
    return !ev.blockInvalid;
}
// ---- end transitional stub ----

void YellowbackIndex::TestChainTip(const CBlockIndex* pindex, const CBlock* pblock, bool connect)
{
    std::optional<std::pair<SproutMerkleTree, SaplingMerkleTree>> added;
    if (connect) added = std::make_pair(SproutMerkleTree(), SaplingMerkleTree());
    ChainTip(pindex, pblock, added);
}

std::optional<std::string> ParamsFromArgs(const std::string& networkId, Params& out)
{
    // The four regtest-only flags of §3.1 (M13): -yellowbackstartheight (required) and the three
    // overrides -yellowbacksigmaref, -yellowbacksupplycapbps, -yellowbackenforceuntil. Every value
    // is hashed into the state hash so mismatched test nodes fail loudly.
    static const char* const REGTEST_FLAGS[] = { "-yellowbackstartheight", "-yellowbacksigmaref", "-yellowbacksupplycapbps", "-yellowbackenforceuntil" };
    if (networkId != "regtest") {
        for (const char* f : REGTEST_FLAGS) {
            if (mapArgs.count(f)) return std::string(f) + " is regtest-only";
        }
        out = ParamsForNetwork(networkId);
        return std::nullopt;
    }
    if (!mapArgs.count("-yellowbackstartheight")) return std::string("regtest -yellowback requires -yellowbackstartheight");
    int64_t startHeight = GetArg("-yellowbackstartheight", 0);
    if (startHeight <= 0 || startHeight > 0x7FFFFFFF) return std::string("-yellowbackstartheight must be a positive height");
    int64_t sigmaRef = GetArg("-yellowbacksigmaref", 0);
    if (sigmaRef < 0 || sigmaRef > 0x7FFFFFFF) return std::string("-yellowbacksigmaref must be >= 0");
    int64_t capBps = GetArg("-yellowbacksupplycapbps", 0);
    if (capBps < 0 || capBps > 10000) return std::string("-yellowbacksupplycapbps must be between 0 and 10000");
    int64_t until = GetArg("-yellowbackenforceuntil", 0);
    if (until < 0 || until > 0x7FFFFFFF) return std::string("-yellowbackenforceuntil must be >= 0");
    out = RegtestParams((int)startHeight, (int)sigmaRef, (int)capBps, (int)until);
    return std::nullopt;
}

} // namespace yellowback
