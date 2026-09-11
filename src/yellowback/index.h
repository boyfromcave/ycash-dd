// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_INDEX_H
#define YCASH_YELLOWBACK_INDEX_H

#include "arith_uint256.h"
#include "fs.h"
#include "pubkey.h"
#include "sync.h"
#include "validationinterface.h"
#include "yellowback/db.h"
#include "yellowback/params.h"
#include "yellowback/state.h"
#include "yellowback/view.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CBlockHeader;
class CBlockIndex;
class CTxMemPool;
class UniValue;

/**
 * The Yellowback index (plan V2, §4.2a, §4.3): the overlay state kept
 * synchronous with chainActive by three hook calls in main.cpp —
 * CheckConnect in ConnectBlock's post-verification window, CommitConnect
 * after view.SetBestBlock, UndoDisconnect at the end of DisconnectBlock —
 * plus the N1 descendant clause in AcceptBlockHeader, MP-1 in
 * AcceptToMemoryPool and the mempool sweep in ConnectTip. Every hook is
 * called with cs_main held (asserted), takes cs_yellowback inside, and is a
 * no-op while the index is unhealthy (BLK-3: only a storage failure can make
 * the node accept a block it would otherwise reject, and then enforcement is
 * off until -reindex-yellowback).
 *
 * DigiByte keeps DigiDollar state in the chainstate itself and rejects in
 * consensus (ref/digibyte/src/validation.cpp); Ycash's chainstate flushes
 * lazily (main.cpp FLUSH_STATE_IF_NEEDED) while this index commits one batch
 * per block, so after a crash the index may be ahead of chainActive and
 * SyncToChain undo-walks back (UNDO_KEEP = 4096, V2; mapping.md §13).
 *
 * Lock order (a recorded decision, N25): cs_main -> cs_wallet -> mempool.cs
 * -> cs_yellowback. RemoveInvalidVaultSpends takes mempool.cs before
 * cs_yellowback; TemplateView holds cs_yellowback inside CreateNewBlock's
 * LOCK2(cs_main, mempool.cs); no RPC may take mempool.cs after cs_yellowback.
 *
 * Node-local bookkeeping that never feeds a rule (§3.10): the Rejected table
 * (X keys, sync writes, N8), the valve's header notes and state (ACT-7), the
 * enforcement flag, the quote holder (stamped by the RPC — no clock here,
 * M11), the payout key and the signal flag.
 */
namespace yellowback {

/** Undo records older than this many blocks below the tip are pruned (V2: 4,096, the crash walk). */
static const int UNDO_KEEP = 4096;
/** ACT-7 / P2: the most headers the valve notes per rejected root. */
static const int VALVE_NOTE_CAP = 64;

/** The miner-side configuration read only on the miner path and in the hook's "what to do" branch (§3.10). */
struct MinerConfig
{
    std::optional<CKeyID> payoutKey;   //!< -yellowbackpayoutaddress or a P2PKH -mineraddress (MINER-2); none => no tag
    bool signal;                       //!< -yellowbacksignal (L4 default per network)
    bool enforce;                      //!< -yellowbackenforce (the kill switch, V13)
    int64_t quoteMaxAge;               //!< -yellowbackquotemaxage, seconds
    std::string templatePolicy;        //!< "strict" | "consensus" (V14)
    bool requireHealthy;               //!< -yellowbackrequirehealthy (K24)

    MinerConfig() : signal(false), enforce(true), quoteMaxAge(1800), templatePolicy("strict"), requireHealthy(false) {}
};

/** The quote yed_setquote stores (MINER-1). receivedAt is stamped by the RPC. */
struct QuoteHolder
{
    uint64_t priceMicroUsd;   //!< 0 = none
    uint16_t sourceMask;
    int64_t receivedAt;

    QuoteHolder() : priceMicroUsd(0), sourceMask(0), receivedAt(0) {}
};

/** What the next template's tag would be (yed_getinfo.miner, yed_setquote.nextTag, TemplateInfo). */
struct MinerStatus
{
    std::string kind;                  //!< "quote" | "signal" | "none"
    bool signal;                       //!< the signal bit the tag carries
    std::optional<CKeyID> payoutKey;
    std::optional<int64_t> quoteAgeSeconds;
    bool registered;                   //!< REG-1 at the tip
    bool eligible;                     //!< in E(tip)

    MinerStatus() : kind("none"), signal(false), registered(false), eligible(false) {}
};

/** -yellowbacktestfault (regtest only, §4.5): a storage fault at one hook once, a template disagreement once, or no valve. */
struct TestFault
{
    enum Hook { NONE, CHECK, COMMIT, UNDO };
    Hook storageHook;
    std::optional<int> height;        //!< fire at this height only (default: the next call)
    bool armed;                       //!< consumed on the first firing
    bool templateFault;               //!< FilterTemplate disagrees with EvaluateBlock once (TPL-3)
    bool noValve;                     //!< ACT-7 disabled (yellowback_runbook.py)

    TestFault() : storageHook(NONE), armed(false), templateFault(false), noValve(false) {}
};

class YellowbackIndex;

/**
 * RAII holder of cs_yellowback owning an OverlayStateView over the index at
 * the tip (TPL-1; N25). CreateNewBlock keeps it for the selection loop inside
 * its LOCK2(cs_main, mempool.cs); MempoolCheck and the dry-run RPCs use one
 * per call. The caller holds cs_main for its lifetime.
 */
class TemplateView
{
public:
    explicit TemplateView(YellowbackIndex& index);
    TemplateView(TemplateView&&) = default;
    TemplateView& operator=(TemplateView&&) = default;
    ~TemplateView();

    OverlayStateView& Overlay() { return *overlay; }
    /** The height the template is built for (index tip + 1). */
    int NextHeight() const { return nextHeight; }
    const YellowbackIndex& Index() const { return *index; }
    YellowbackIndex& Index() { return *index; }

private:
    YellowbackIndex* index;
    std::unique_ptr<CCriticalBlock> lock;
    std::unique_ptr<OverlayStateView> overlay;
    int nextHeight;
};

class YellowbackIndex final : public CValidationInterface
{
public:
    YellowbackIndex(const Params& params, const fs::path& dir, size_t cacheSize, bool fWipe);
    ~YellowbackIndex();

    /**
     * Bring the database in line with chainActive at startup: undo while the
     * stored tip is not in the active chain (covers the index being ahead of
     * an unflushed chainstate after a crash, V2), then apply forward from
     * disk. Wipes and rebuilds when the schema or network differs, an undo
     * record is missing, or the chain is below startHeight. Takes cs_main.
     * Returns false only when the index ends up unhealthy.
     */
    bool SyncToChain();

    /** Make every later hook return at once (shutdown). */
    void Stop();
    /** Commit anything pending and fsync. */
    void Flush(bool fSync);

    mutable CCriticalSection cs_yellowback;

    const Params& GetParams() const { return params; }
    /** The parameter set in force at `height` (§3.1 versioning; one set per release today). */
    const Params& ParamsAt(int height) const { return SelectParams(paramSets, height); }
    bool IsHealthy() const { return healthy; }
    std::string UnhealthyReason() const { return unhealthyReason; }
    bool IsStopped() const { return stopped; }

    /** The stored tip (cs_yellowback). */
    std::optional<TipRecord> GetTip() const;
    /** The stored tip height, -1 when empty (cs_yellowback). */
    int TipHeight() const;
    /**
     * Transitional (V2 made the index synchronous, so this is true whenever the
     * index is healthy and chainActive is at or below startHeight or equal to
     * the stored tip); kept so txbuilder.cpp compiles until Phase 6 drops it.
     */
    bool IsSynced() const;
    /** The state view (cs_yellowback). Nothing but the hooks writes to it; dry runs go through an OverlayStateView. */
    StateView& View() const { return *db; }
    StateView& MutableView() { return *db; }
    uint256 GetStateHash() const;

    /** Mark unhealthy (also used by unit tests to exercise the RPC refusal). */
    void SetUnhealthy(const std::string& reason);

    // ------------------------------------------------------------------ hooks (cs_main held)

    /**
     * ConnectBlock's check (BLK-1/BLK-2; the :3191-3193 window). H = pindex->nHeight.
     * H < startHeight => nullopt. chainActive.Contains(pindex) => nullopt (a re-verification,
     * K6). Consistency: (indexTip == null && H == startHeight) || indexTip.blockHash ==
     * pindex->pprev->GetBlockHash(), else SetUnhealthy("tip-mismatch") => nullopt. Always
     * evaluates (cache key {block.GetHash(), indexTipHash, paramsHash}, N9); returns a reason
     * iff blockInvalid && enforcementOn && -yellowbackenforce && !valveTripped &&
     * !IsInitialBlockDownload() && !fReindex && !fImporting (clauses 1-2) &&
     * !NetworkAlreadyBuiltOn(pindex) (clause 3, L11: accepted, logged, counted in
     * suppressedBlocks); on a reason writes Rejected with sync = true (N8; never under
     * fJustCheck, whose block is never marked). A storage exception => SetUnhealthy(), nullopt
     * (BLK-3). Never reads pindex->phashBlock (K5: TestBlockValidity's indexDummy has none).
     */
    std::optional<std::string> CheckConnect(const CBlock& block, const CBlockIndex* pindex, bool fJustCheck);
    /** The commit after view.SetBestBlock (never under fJustCheck): same guards; reuses the cache else re-evaluates; one batch; false => unhealthy (the block still connects). */
    bool CommitConnect(const CBlock& block, const CBlockIndex* pindex);
    /** DisconnectBlock's undo (inside `if (updateIndices)`): guard indexTip.blockHash == pindex->GetBlockHash(); false => unhealthy. */
    bool UndoDisconnect(const CBlockIndex* pindex);

    /**
     * The N1 clause in AcceptBlockHeader and the ACT-7 odometer: true => the caller answers
     * DoS(0, "bad-prevblk-yellowback"). Notes the header {hash -> root, work} only within the P2
     * bounds (target <= parent target * 132 / 100; fewer than VALVE_NOTE_CAP notes per root);
     * trips the valve in place when the noted work reaches tip.nChainWork + valveBlocks *
     * GetBlockProof(*tip) (ReconsiderBlock loop, Rejected cleared, SetMiscWarning +
     * CAlert::Notify, P1); returns false once tripped so the stock chain is accepted from the
     * next announcement on (P3).
     */
    bool NoteHeaderOnRejectedChain(const CBlockHeader& header);
    /** Walks pprev through BLOCK_FAILED_CHILD marks to the first BLOCK_FAILED_VALID ancestor; its hash in Rejected? */
    bool IsRejectedAncestor(const CBlockIndex* pindexPrev) const;
    /** BLK-2 clause 3 (L11): pindexBestHeader descends from pindex and carries valveBlocks of work above the tip. */
    bool NetworkAlreadyBuiltOn(const CBlockIndex* pindex) const;

    /** ConnectTip's sweep (N5): takes mempool.cs, then cs_yellowback; removes every vault spend MempoolCheck now fails, with descendants. */
    void RemoveInvalidVaultSpends(CTxMemPool& pool);
    /**
     * MP-1: true unless some input spends an ACTIVE vault (O(inputs) lookups, no SNAP, N6) and
     * the spend either lacks the expiry bound (nExpiryHeight == 0 or > refHeight + REF_WINDOW)
     * or fails RED-1..4 at the next height over a two-transaction pseudo-block (§4.3). True for
     * every vault spend while IsAbandoned() (L13). Mints and transfers are never refused.
     */
    bool MempoolCheck(const CTransaction& tx);
    /** MempoolCheck's reason: nullopt = admitted; "mempool-expiry" or the RED verdict (K7; `mempool-check-failed:<verdict>`). */
    std::optional<std::string> MempoolCheckReason(const CTransaction& tx);

    /** RAII holder of cs_yellowback over an overlay at the tip (caller holds cs_main, and mempool.cs when in CreateNewBlock). */
    yellowback::TemplateView TemplateView();

    /** The §4.6 abandonment predicate (L10, L12): ENFORCEMENT set for abandonBlocks consecutive snapshots ending at the tip; from Snapshots alone. */
    bool IsAbandoned() const;

    // ------------------------------------------------------------------ enforcement state (node-local)

    bool EnforceFlag() const { return miner.enforce; }
    /** ACT-5's node-side conjuncts: -yellowbackenforce && healthy && !valveTripped && !sunset. */
    bool IsEnforcing() const;
    bool ValveTripped() const { return valveTripped; }
    /** L8: the tip has reached ENFORCE_UNTIL_HEIGHT (the next block is past it); false when the set has no sunset. */
    bool IsSunset() const;
    int RejectedCount() const;
    int SuppressedCount() const { return suppressedBlocks; }
    /** Every hash in Rejected (cs_yellowback). */
    std::vector<uint256> RejectedHashes() const;
    std::optional<RejectedRecord> GetRejected(const uint256& blockHash) const;
    /** Erase Rejected (the kill-switch loop after ReconsiderBlock; -reindex-yellowback wipes it with everything else). */
    void ClearRejected();
    /** The valve's note map size (unit tests). */
    size_t ValveNoteCount() const;

    // ------------------------------------------------------------------ miner side

    void SetMinerConfig(const MinerConfig& cfg) { miner = cfg; }
    const MinerConfig& GetMinerConfig() const { return miner; }
    void SetPayeePolicy(const PayeePolicy& p) { payeePolicy = p; }
    const PayeePolicy& GetPayeePolicy() const { return payeePolicy; }
    /** yed_setquote: the RPC stamps receivedAt (the only clock, M11). */
    void SetQuote(uint64_t priceMicroUsd, uint16_t sourceMask, int64_t receivedAt);
    QuoteHolder GetQuote() const;
    /** What the next template's tag would be, given `now` (cs_yellowback). */
    MinerStatus GetMinerStatus(int64_t now) const;
    /**
     * The getblocktemplate "yellowback" object (§4.4; V26): the tag the template carries
     * (COINBASE_FLAGS as CreateNewBlock set it, decoded), the quote's age, the miner's
     * standing and the node's state. Caller holds cs_main; `now` is the RPC's clock (M11).
     */
    UniValue TemplateInfo(int64_t now) const;

    /** Parse -yellowbacktestfault (regtest only); an error string on a bad spec. */
    std::optional<std::string> SetTestFault(const std::string& spec);
    const TestFault& GetTestFault() const { return testFault; }
    /** TPL-3's unit companion: true once after -yellowbacktestfault=template. */
    bool ConsumeTemplateFault();

    /**
     * Wallet hooks (plan §4.6, stages ii and iii of coin locking), set by the
     * wallet layer when there is one. Called on the notifier thread without
     * cs_yellowback held; they must not take cs_main.
     */
    std::function<void(const CTransaction&)> onSyncTransaction;
    std::function<void()> onReconcile;
    /** Test only: called at the start of every evaluation (fault injection for the exception boundary, N25). */
    std::function<void()> testBeforeApply;

protected:
    void ChainTip(const CBlockIndex* pindex, const CBlock* pblock, std::optional<std::pair<SproutMerkleTree, SaplingMerkleTree>> added) override;
    void SyncTransaction(const CTransaction& tx, const CBlock* pblock, const int nHeight) override;

private:
    struct Evaluation
    {
        uint256 blockHash;
        uint256 tipHash;
        uint256 paramsHash;
        BlockEvaluation ev;
        std::map<std::string, std::optional<std::string>> writes;
        bool valid;
        Evaluation() : valid(false) {}
    };
    struct HeaderNote
    {
        uint256 root;
        arith_uint256 work;
        uint32_t nBits;
    };

    bool ApplyOne(const CBlock& block, int height, const uint256& hash, std::string& error);
    bool UndoOne(const uint256& hash, std::string& error);
    void Wipe(const std::string& why);
    void LoadRejected();
    uint256 ParamsHash(const Params& p) const;
    /** Evaluate (or reuse the cache) for a block on top of the stored tip; fills `cache`. */
    const Evaluation& Evaluate(const CBlock& block, int height, const uint256& tipHash);
    /** The consistency guard shared by CheckConnect and CommitConnect; false => already marked unhealthy. */
    bool TipMatches(const CBlockIndex* pindex, std::optional<TipRecord>& tip, const char* hook);
    void MaybeFault(TestFault::Hook hook, int height);
    std::optional<uint256> RejectedRootOf(const CBlockIndex* pindex) const;
    void TripValve(const uint256& root);
    bool IsAbandonedLocked() const;
    bool IsSunsetLocked() const;
    std::optional<std::string> MempoolCheckLocked(const CTransaction& tx);

    Params params;
    std::vector<Params> paramSets;
    std::unique_ptr<YellowbackDB> db;
    bool healthy;
    std::string unhealthyReason;
    bool stopped;

    MinerConfig miner;
    PayeePolicy payeePolicy;
    QuoteHolder quote;
    TestFault testFault;

    Evaluation cache;
    std::set<uint256> rejected;                 //!< the Rejected table, mirrored in memory (loaded at open)
    std::map<uint256, HeaderNote> notes;        //!< ACT-7 odometer: refused headers -> root, accumulated work
    std::map<uint256, int> notesPerRoot;
    bool valveTripped;
    int suppressedBlocks;
    bool sunsetLogged;
};

/** The node's index, or nullptr when -yellowback is off. */
extern YellowbackIndex* g_yellowback;
/** -yellowbackfee (>= DEFAULT_YELLOWBACK_FEE) and -yellowbackmintlag (REF_LAG). */
extern CAmount g_yellowbackFee;
extern int g_yellowbackMintLag;

/**
 * Build the parameters for the running network from configuration (the four
 * regtest-only flags of §3.1: -yellowbackstartheight, -yellowbacksigmaref,
 * -yellowbacksupplycapbps, -yellowbackenforceuntil). Returns an error string
 * on a misconfiguration (start height missing, or a regtest flag on another
 * network).
 */
std::optional<std::string> ParamsFromArgs(const std::string& networkId, Params& out);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_INDEX_H
