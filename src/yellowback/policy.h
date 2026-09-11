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
 * Wallet-side helpers that need the chain (cs_main): a coins view built
 * exactly as signrawtransaction does — pcoinsTip behind CCoinsViewMemPool
 * (C1) — the signer branch ID, and full script verification of a transaction
 * before it is broadcast. The prototype's co-signer rule set (RED-0..8) was
 * removed with the federation; v2's RED-1..4 are block-validity rules and
 * live in state.cpp (plan §4.2).
 */
namespace yellowback {

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
