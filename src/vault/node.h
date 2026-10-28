// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_VAULT_NODE_H
#define YCASH_VAULT_NODE_H

#include "vault/db.h"
#include "vault/state.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CBlock;
class CBlockIndex;
class CBlockUndo;
class CChainParams;
class CCoinsViewCache;
class CTransaction;
class CTxMemPool;

namespace Consensus { struct Params; }

/**
 * The node's wiring of the vault primitive (docs/plans/yellowback-upgrade-plan.md §15.6, U-17,
 * U-18): the global set-state database, the coin accessors validation builds, the mempool
 * re-check after a tip change, and the start-up reconciliation. Every node has the database
 * (no flag gates it); it stays empty before UPGRADE_VAULT activates. All functions here
 * require cs_main: the database changes only under it, at block boundaries, after a block's
 * parallel script checks have finished.
 */
namespace vault {

/** <datadir>/vaults/, created in init.cpp for every node (nullptr in contexts without one). */
extern VaultDB* g_vaultdb;

/** An immutable view of the set state at the database's tip (the state after chainActive's tip;
 *  the parent of the next block), or nullptr without a database. */
std::shared_ptr<const SetSnapshot> TipSnapshot();

/** Ancestor hashes of `prev` (inclusive) for the module ejection hook (U-25): heights above
 *  prev's or below 0 read as nullopt. Block index entries live for the process, so the
 *  function may outlive the caller's cs_main section, but it is only called under it. */
BlockHashFn AncestorHashes(const CBlockIndex* prev);

/** True iff the database is at the parent of `pindex`: its tip marker is pindex->pprev, or it has
 *  no tip and pindex is the first block at which UPGRADE_VAULT is active. */
bool AtParentOf(const CBlockIndex* pindex, const Consensus::Params& params);

/** Coins from a CCoinsViewCache; a coin at MEMPOOL_HEIGHT reads as `mempoolHeight`. */
class ViewCoinAccessor : public CoinAccessor
{
public:
    ViewCoinAccessor(const CCoinsViewCache& view, int64_t mempoolHeight) : view(view), mempoolHeight(mempoolHeight) {}
    bool GetSpentCoin(const COutPoint& prevout, SpentCoin& out) const override;

private:
    const CCoinsViewCache& view;
    int64_t mempoolHeight;
};

/** The spent coins of `block` (connected at `height`) from its undo data, for replay. A coin's
 *  height comes from the undo record when it carries one, else from the block itself (a coin
 *  created earlier in the block), else from the template-output index in `base` (the state
 *  before the block), else 0: a coin created before activation (never indexed) or not a
 *  template, whose height is no rule's input — S-0 rejects a template spend of it, live and on
 *  replay alike (audit 01/V-1). False only for an undo record that does not match the block. */
bool CoinsFromUndo(const CBlock& block, const CBlockUndo& undo, int64_t height, const KVReader& base, MapCoinAccessor& out);

/** Does `tx` spend a V- or I-shaped coin (inputs read from `view`)? */
bool HasTemplateInput(const CTransaction& tx, const CCoinsViewCache& view);

/** Does `tx` touch the primitive: an act or a V/I-shaped output, an input spending a V/I-shaped
 *  coin, or an input spending an indexed member bond? Inputs are read from `view`. */
bool IsVaultRelevant(const CTransaction& tx, const CCoinsViewCache& view);

/** After a tip change (ConnectTip, DisconnectTip): re-run CheckTx for `nextHeight` on every
 *  mempool transaction that touches the primitive, and the scripts of its template inputs
 *  against the new snapshot, and evict failures (set state, I-2 age, membership and dormancy
 *  change with height and with the acts just connected or disconnected). When a reorg has
 *  just dropped `nextHeight` below activation, evicts every transaction that spends or
 *  creates a template output; otherwise a no-op before activation. When a reorg has just
 *  dropped `nextHeight` below Falcon (IsPQFalconActive, no branch-ID change), also re-runs the
 *  inputs of every transaction with an OP_CHECKPQSIG at `nextHeight`'s flags and evicts failures. */
void RecheckMempool(CTxMemPool& pool, int nextHeight, const Consensus::Params& params);

/** The block whose vault undo record connecting `pindex` prunes (UNDO_KEEP below it, audit 07/F-7):
 *  its ancestor at pindex->nHeight - UNDO_KEEP when that is at or above the activation height. */
std::optional<uint256> UndoToPrune(const CBlockIndex* pindex, const Consensus::Params& params);

/** Reconcile logs its replay progress every this many blocks (cf. DigiByte v9.26.7 b544af7fa3). */
static const int RECONCILE_PROGRESS_INTERVAL = 1000;

/** Start-up reconciliation (U-18): disconnect the database back to chainActive using its own
 *  undo, then replay chainActive's blocks from disk up to the tip. False with `err` set when
 *  the database cannot be brought to the tip (a missing block or undo file). Logs progress every
 *  RECONCILE_PROGRESS_INTERVAL blocks and polls `interrupt` (init passes ShutdownRequested) before
 *  each block: an interrupted replay returns false with `*interrupted` set and is never reported
 *  complete; every block replayed so far was committed with its tip marker, so the next start
 *  resumes from there. */
bool Reconcile(const CChainParams& chainparams, std::string& err,
               const std::function<bool()>& interrupt = std::function<bool()>(), bool* interrupted = nullptr);

/** One signer set's locked value, stored and re-derived (AuditLockedValue). */
struct SetValueAudit
{
    SetId setId;
    CAmount stored = 0;
    CAmount derived = 0;
};

/**
 * The invariant audit of the set state (cf. DigiByte v9.26.7 19c8b1c272, the Yellowback index's
 * AuditIndexRecords): every set's lockedValue re-derived as the sum of the values of the unspent V
 * outputs ('v' records of kind V) whose setId is that set, in one pass, read only. Returns one line
 * per mismatch (empty = consistent); `sets`, when given, receives every set's pair.
 */
std::vector<std::string> AuditLockedValue(const KVReader& kv, std::vector<SetValueAudit>* sets = nullptr);

/** A digest of every state key ('s', 'm', 'b', 'v') in byte order: equal on two nodes iff
 *  their set state is equal (vault_getinfo "statehash"). */
uint256 StateHash(const KVReader& kv);

/** Mempool DoS class for a vault::CheckTx failure (W3-N-4). True only for a byte-level reason
 *  no honest pre-upgrade relay produces, and only when an act output is longer than
 *  MAX_OP_RETURN_RELAY (83). `bad-vault-act-sigs` counts only when DecodeAct itself rejected the
 *  output for more than MAX_ACT_SIGS signatures; a count mismatch from ApplyAct depends on set
 *  state and stays false. */
bool IsByteLevelActFailure(const std::string& reason, const CTransaction& tx);

} // namespace vault

#endif // YCASH_VAULT_NODE_H
