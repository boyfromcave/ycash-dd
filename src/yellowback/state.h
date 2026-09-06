// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_STATE_H
#define YCASH_YELLOWBACK_STATE_H

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "yellowback/params.h"
#include "yellowback/payload.h"
#include "yellowback/view.h"

#include <optional>
#include <string>

/**
 * The Yellowback state machine: rules §3.7 (IN-1..3, TX-0, MINT-1..7, XFER-1..3,
 * PRICE-1..3, SNAP, UNDO) as pure functions of (block or transaction, state
 * view, height, params). Nothing here may read the clock, the mempool, the
 * wallet or configuration (§3.10, D19): this is the shape a consensus rule
 * would call in Phase B (§9).
 *
 * DigiByte enforces the equivalent rules in consensus
 * (ref/digibyte/src/digidollar/validation.cpp) and rejects a failing
 * transaction; an overlay cannot reject, so a failing MINT registers a VOID
 * vault and a failing TRANSFER burns (plan D18, the Runes "cenotaph" rule).
 */
namespace yellowback {

/** Stable verdict strings (reused by yed_gettxinfo, yed_validaterawtransaction and the wallet). */
namespace verdict {
extern const char* const NON_YELLOWBACK;
extern const char* const MINT_OK;
extern const char* const TRANSFER_OK;
extern const char* const REDEEM_OK;
extern const char* const PRICE_OK;
extern const char* const PRICE_NOT_ANCHOR;
extern const char* const PRICE_ROTATION;
extern const char* const PRICE_BAD_RANGE;
extern const char* const COINBASE;
extern const char* const MINT_NO_VAULT;
extern const char* const BAD_MINT_TIER;
extern const char* const BAD_MINT_AMOUNT;
extern const char* const BAD_MINT_LOCK_HEIGHT;
extern const char* const BAD_MINT_LOCK_TIER_DURATION;
extern const char* const BAD_MINT_EVAL_HEIGHT;
extern const char* const BAD_MINT_EVAL_SNAPSHOT;
extern const char* const BAD_MINT_OUTPUTS;
extern const char* const BAD_MINT_OWNER_KEY;
extern const char* const BAD_MINT_VAULT_SCRIPT;
extern const char* const BAD_ORACLE_PRICE;
extern const char* const MINT_BLOCKED_ERR;
extern const char* const MINT_FROZEN;
extern const char* const BAD_MINT_COLLATERAL;
extern const char* const MINT_SUPPLY_CAP;
extern const char* const BAD_MINT_TOKEN_OUTPUT;
extern const char* const XFER_NO_INPUT;
extern const char* const BAD_XFER_AMOUNT;
extern const char* const XFER_OVER_ASSIGNED;
} // namespace verdict

/**
 * Apply one transaction's effect on the state (§3.7). Inputs are processed
 * before outputs. Returns the TxLog entry; `relevant` is false when the
 * transaction touched nothing Yellowback (no payload, no Yellowback outpoint
 * spent), in which case the state is unchanged and nothing is logged.
 */
TxLogRecord ProcessTx(State& st, const Params& params, const CTransaction& tx, int height, bool& relevant);

/**
 * Apply a whole block: initialise Anchor/Rosters at startHeight (E6), process
 * every transaction in order, write Snapshots[H] and the tip. Every mutation
 * is recorded in `undo`. Returns an error string if the index must become
 * unhealthy (genesis anchor mismatch, B9); the caller then discards the
 * writes. Blocks below startHeight are ignored and touch nothing.
 */
std::optional<std::string> ApplyBlock(StateView& view, const Params& params, const CBlock& block, int height, const uint256& blockHash, UndoRecord& undo);

/** Restore every key recorded in `undo` to its pre-block value (byte-identical, §3.7 UNDO). */
void UndoBlock(StateView& view, const UndoRecord& undo);

/** The snapshot values for height H computed from the current totals, prices and volatility. */
Snapshot ComputeSnapshot(const State& st, const Params& params, int height, const uint256& blockHash, Volatility& vol);

/**
 * The roster a MINT confirming at height H may reference (MINT-3): the
 * current roster, or the previous one during ROSTER_GRACE after a reveal.
 * Returns the indices to try, most recent first.
 */
std::vector<uint32_t> MintableRosters(const std::vector<RosterRecord>& rosters, const Params& params, int height);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_STATE_H
