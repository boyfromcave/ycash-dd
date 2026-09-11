// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_POLICY_H
#define YCASH_YELLOWBACK_POLICY_H

#include "amount.h"
#include "coins.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "yellowback/index.h"
#include "yellowback/script.h"
#include "yellowback/view.h"

#include <string>

/**
 * The miner-side glue (plan §4.2, §4.4; libbitcoin_server) and the chain-side
 * helpers the wallet builder needs (cs_main): the coinbase tag a template
 * carries (MINER-1..3), a coins view built as signrawtransaction does, the
 * signer branch ID and full script verification before broadcast. The
 * prototype's co-signer rule set was removed with the federation; v2's
 * RED-1..4 are block-validity rules and live in state.cpp.
 *
 * policy::TagScript is the one GetTime() call outside rpc/yellowback.cpp
 * (M11, §3.10): it decides what *this* miner publishes and is read by no rule.
 * The template filter (FilterTemplate, TPL-1..3) is Phase 4.
 */
namespace yellowback {

namespace policy {

/**
 * MINER-1..3 with the clock as a parameter: a quote tag iff a quote younger
 * than -yellowbackquotemaxage is held, else a signal-only tag iff
 * -yellowbacksignal, else an empty script; the signal bit iff
 * -yellowbacksignal and -yellowbackenforce and the valve has not tripped and
 * the tip is not past ENFORCE_UNTIL_HEIGHT (L3, L7, L8); payoutKey from
 * -yellowbackpayoutaddress (MINER-2; none => empty script); empty while the
 * index is unhealthy (MINER-3). Returns the push `0x24 ‖ 36 bytes`
 * (TagPush) that COINBASE_FLAGS carries (V5).
 */
CScript BuildTagScript(const YellowbackIndex& index, int64_t now);

/** = BuildTagScript(index, GetTime()). Called by CreateNewBlock under cs_main. */
CScript TagScript(const YellowbackIndex& index);

} // namespace policy

/**
 * Populate `view` with the transaction's inputs from pcoinsTip behind the
 * mempool, as signrawtransaction does (rawtransaction.cpp:906-921).
 * Requires cs_main.
 */
void FetchInputs(const CTransaction& tx, CCoinsViewCache& view);

/** The branch ID every Yellowback signer uses (§3.4): CurrentEpochBranchId(chainActive.Height() + 1). Requires cs_main. */
uint32_t SignerBranchId();

/** True iff every input of `tx` passes VerifyScript under STANDARD_SCRIPT_VERIFY_FLAGS against `view`. */
bool VerifyAllInputs(const CTransaction& tx, const CCoinsViewCache& view, uint32_t branchId, std::string& error);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_POLICY_H
