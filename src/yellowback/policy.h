// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_POLICY_H
#define YCASH_YELLOWBACK_POLICY_H

#include "amount.h"
#include "coins.h"
#include "primitives/transaction.h"
#include "yellowback/index.h"
#include "yellowback/script.h"
#include "yellowback/view.h"

#include <string>

/**
 * Policy checks of plan §3.8 (RED-0..8): what a federation member verifies
 * before co-signing a redemption and what the wallet verifies before
 * broadcasting one. They never change the state machine (D18); they are
 * the rules §9 would promote to consensus.
 *
 * Pure given the index (under cs_yellowback), the chain tip (under cs_main)
 * and a coins view the caller built exactly as signrawtransaction does —
 * pcoinsTip behind CCoinsViewMemPool (C1) — so the fee and
 * AreInputsStandard see every input.
 */
namespace yellowback {

struct RedeemCheck
{
    bool ok;
    bool transient;          //!< RED-0 unsynced / RED-2 not yet at lockHeight on this node (E2)
    std::string rule;        //!< failing rule id, e.g. "RED-3"
    std::string reason;      //!< human-readable detail

    COutPoint vault;
    VaultRecord vaultRecord;
    CScript vaultScript;     //!< reconstructed from the index (C5)
    CAmount fee;
    int64_t yedIn;
    int64_t yedOut;
    int64_t burned;
    int64_t requiredBurn;
    int indexHeight;
    uint32_t branchId;
    uint256 sighash;         //!< SIGHASH_ALL over vaultScript for vin[0]

    RedeemCheck() : ok(false), transient(false), fee(0), yedIn(0), yedOut(0), burned(0), requiredBurn(0), indexHeight(-1), branchId(0) {}
};

/**
 * Run RED-0..8 for `tx` against the index and `view`. Requires cs_main and
 * cs_yellowback held by the caller. `view` must be populated with every
 * input the caller could find (missing inputs are a refusal, D1).
 */
RedeemCheck CheckRedeem(const YellowbackIndex& index, const CCoinsViewCache& view, const CTransaction& tx, int chainHeight);

/**
 * Populate `view` with the transaction's inputs from pcoinsTip behind the
 * mempool, as signrawtransaction does (rawtransaction.cpp:906-921).
 * Requires cs_main.
 */
void FetchInputs(const CTransaction& tx, CCoinsViewCache& view);

/** The branch ID every Yellowback signer uses (§3.3): CurrentEpochBranchId(chainActive.Height() + 1). Requires cs_main. */
uint32_t SignerBranchId();

/** True iff every input of `tx` passes VerifyScript under STANDARD_SCRIPT_VERIFY_FLAGS against `view`. */
bool VerifyAllInputs(const CTransaction& tx, const CCoinsViewCache& view, uint32_t branchId, std::string& error);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_POLICY_H
