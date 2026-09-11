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
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/state.h"

namespace yellowback {

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
