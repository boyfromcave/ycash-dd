// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/policy.h"

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "main.h"
#include "policy/policy.h"
#include "script/interpreter.h"
#include "script/script_error.h"
#include "txmempool.h"
#include "utiltime.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/state.h"
#include "yellowback/tag.h"

namespace yellowback {

namespace policy {

CScript BuildTagScript(const YellowbackIndex& index, int64_t now)
{
    const MinerStatus s = index.GetMinerStatus(now);
    if (s.kind == "none" || !s.payoutKey.has_value()) return CScript();
    const QuoteHolder q = index.GetQuote();
    CoinbaseTag tag;
    tag.flags = s.signal ? 0x01 : 0x00;
    tag.priceMicroUsd = s.kind == "quote" ? q.priceMicroUsd : TAG_PRICE_SIGNAL_ONLY;
    tag.sourceMask = s.kind == "quote" ? q.sourceMask : 0;
    tag.payoutKey = s.payoutKey.value();
    if (!IsValidTag(tag)) return CScript();
    return TagPush(tag);
}

CScript TagScript(const YellowbackIndex& index)
{
    // The miner's own clock, deciding what it publishes; read by no rule (M11).
    return BuildTagScript(index, GetTime());
}

} // namespace policy

void FetchInputs(const CTransaction& tx, CCoinsViewCache& view)
{
    AssertLockHeld(cs_main);
    LOCK(mempool.cs);
    CCoinsViewMemPool viewMempool(pcoinsTip, mempool);
    CCoinsViewCache tmp(&viewMempool);
    for (const CTxIn& txin : tx.vin) {
        const CCoins* coins = tmp.AccessCoins(txin.prevout.hash);
        if (coins) {
            *view.ModifyCoins(txin.prevout.hash) = *coins;
        }
    }
}

uint32_t SignerBranchId()
{
    AssertLockHeld(cs_main);
    return CurrentEpochBranchId(chainActive.Height() + 1, ::Params().GetConsensus());
}

bool VerifyAllInputs(const CTransaction& tx, const CCoinsViewCache& view, uint32_t branchId, std::string& error)
{
    PrecomputedTransactionData txdata(tx);
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const CCoins* coins = view.AccessCoins(tx.vin[i].prevout.hash);
        if (!coins || !coins->IsAvailable(tx.vin[i].prevout.n)) {
            error = strprintf("input %u is unknown or spent", i);
            return false;
        }
        const CTxOut& prev = coins->vout[tx.vin[i].prevout.n];
        ScriptError serror = SCRIPT_ERR_OK;
        if (!VerifyScript(tx.vin[i].scriptSig, prev.scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS,
                          TransactionSignatureChecker(&tx, i, prev.nValue, txdata), branchId, &serror)) {
            error = strprintf("input %u fails script verification: %s", i, ScriptErrorString(serror));
            return false;
        }
    }
    return true;
}

} // namespace yellowback
