// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_INDEX_H
#define YCASH_YELLOWBACK_INDEX_H

#include "fs.h"
#include "sync.h"
#include "validationinterface.h"
#include "yellowback/db.h"
#include "yellowback/params.h"
#include "yellowback/view.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

/**
 * The Yellowback index: a CValidationInterface subscriber that applies every
 * connected block and undoes every disconnected block on the wallet
 * notifier thread (plan D7, §4.3). Zero lines in main.cpp.
 *
 * Lock order (B15): cs_main -> cs_wallet -> cs_yellowback. The ChainTip handler
 * runs on ThreadNotifyWallets, which must not take cs_main; it takes only
 * cs_yellowback. RPCs take cs_main (and cs_wallet) first, then cs_yellowback.
 *
 * Every handler is wrapped in try/catch: on any exception the index logs,
 * marks itself unhealthy and returns (the notifier does not wrap block
 * callbacks, §8.3). The node never fails a block because of Yellowback.
 */
namespace yellowback {

/** Undo records older than this many blocks below the tip are pruned (B10). */
static const int UNDO_KEEP = 1000;

class YellowbackIndex final : public CValidationInterface
{
public:
    YellowbackIndex(const Params& params, const fs::path& dir, size_t cacheSize, bool fWipe);
    ~YellowbackIndex();

    /**
     * Bring the database in line with chainActive at startup: undo while the
     * stored tip is not in the active chain, then apply forward from disk.
     * Wipes and rebuilds when the schema or network differs, an undo record is
     * missing, or the chain is below startHeight. Takes cs_main itself.
     * Returns false only when the index ends up unhealthy.
     */
    bool SyncToChain();

    /** Make every later handler return at once (D2). */
    void Stop();
    /** Commit anything pending and fsync. */
    void Flush(bool fSync);

    mutable CCriticalSection cs_yellowback;

    const Params& GetParams() const { return params; }
    bool IsHealthy() const { return healthy; }
    std::string UnhealthyReason() const { return unhealthyReason; }
    bool IsStopped() const { return stopped; }

    /** The stored tip (cs_yellowback). */
    std::optional<TipRecord> GetTip() const;
    /** True iff the stored tip is chainActive.Tip() (cs_main and cs_yellowback). */
    bool IsSynced() const;
    /** The state view (cs_yellowback). Callers other than the handlers must not write to it; dry runs go through an OverlayStateView. */
    StateView& View() const { return *db; }
    StateView& MutableView() { return *db; }
    uint256 GetStateHash() const;

    /** Mark unhealthy (also used by unit tests to exercise the RPC refusal). */
    void SetUnhealthy(const std::string& reason);

    /**
     * Wallet hooks (plan §4.5, stages ii and iii of coin locking), set by the
     * wallet layer when there is one. Called on the notifier thread without
     * cs_yellowback held; they must not take cs_main.
     */
    std::function<void(const CTransaction&)> onSyncTransaction;
    std::function<void()> onReconcile;

protected:
    void ChainTip(const CBlockIndex* pindex, const CBlock* pblock, std::optional<std::pair<SproutMerkleTree, SaplingMerkleTree>> added) override;
    void SyncTransaction(const CTransaction& tx, const CBlock* pblock, const int nHeight) override;

private:
    bool ApplyOne(const CBlock& block, int height, const uint256& hash, std::string& error);
    bool UndoOne(const uint256& hash, std::string& error);
    void Wipe(const std::string& why);
    void HandleConnect(const CBlockIndex* pindex, const CBlock& block);
    void HandleDisconnect(const CBlockIndex* pindex);

    Params params;
    std::unique_ptr<YellowbackDB> db;
    bool healthy;
    std::string unhealthyReason;
    bool stopped;
};

/** The node's index, or nullptr when -yellowback is off. */
extern YellowbackIndex* g_yellowback;
/** -yellowbackfee (>= DEFAULT_YELLOWBACK_FEE, C16) and -yellowbackmintlag (C4). */
extern CAmount g_yellowbackFee;
extern int g_yellowbackMintLag;

/**
 * Build the parameters for the running network from configuration
 * (regtest genesis arguments, -yellowbacksupplycap). Returns an error string
 * on a misconfiguration (C2: the three regtest arguments must appear
 * together and only on regtest).
 */
std::optional<std::string> ParamsFromArgs(const std::string& networkId, Params& out);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_INDEX_H
