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

namespace {

RedeemCheck Refuse(RedeemCheck c, const char* rule, const std::string& reason, bool transient = false)
{
    c.ok = false;
    c.transient = transient;
    c.rule = rule;
    c.reason = reason;
    return c;
}

} // namespace

RedeemCheck CheckRedeem(const YellowbackIndex& index, const CCoinsViewCache& view, const CTransaction& tx, int chainHeight)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(index.cs_yellowback);
    RedeemCheck c;
    const Params& params = index.GetParams();
    State st(index.View());

    // RED-0
    if (!index.IsHealthy()) return Refuse(c, "RED-0", "index unhealthy: " + index.UnhealthyReason());
    std::optional<TipRecord> tip = st.GetTip();
    if (!tip.has_value()) return Refuse(c, "RED-0", "index empty", true);
    c.indexHeight = tip->height;
    if (!index.IsSynced()) return Refuse(c, "RED-0", "index not at the chain tip", true);

    // RED-1
    if (tx.vin.empty()) return Refuse(c, "RED-1", "no inputs");
    c.vault = tx.vin[0].prevout;
    std::optional<VaultRecord> vault = st.GetVault(c.vault);
    if (!vault.has_value() || !vault->IsOpen()) return Refuse(c, "RED-1", "vin[0] does not spend an ACTIVE or VOID vault");
    c.vaultRecord = vault.value();
    for (size_t i = 1; i < tx.vin.size(); i++) {
        if (st.GetVault(tx.vin[i].prevout).has_value()) return Refuse(c, "RED-1", "the transaction spends more than one vault");
    }
    const bool active = vault->Status() == VaultStatus::ACTIVE;

    // D1: every input must exist and be unspent in the caller's view before anything reads it
    // (GetOutputFor and GetValueIn assert on a missing coin). Checked first so a phantom input is
    // always reported as such, never as a short burn.
    for (const CTxIn& txin : tx.vin) {
        const CCoins* coins = view.AccessCoins(txin.prevout.hash);
        if (!coins || !coins->IsAvailable(txin.prevout.n)) return Refuse(c, "RED-4", "input " + txin.prevout.ToString() + " is unknown or spent");
    }

    // RED-2
    if (tx.nLockTime < vault->lockHeight) return Refuse(c, "RED-2", strprintf("nLockTime %u below lockHeight %u", tx.nLockTime, vault->lockHeight));
    if ((int64_t)c.indexHeight < (int64_t)vault->lockHeight) {
        return Refuse(c, "RED-2", strprintf("index height %d below lockHeight %u", c.indexHeight, vault->lockHeight), true);
    }

    // RED-6 (before RED-3, which needs the price for ACTIVE vaults)
    std::optional<Snapshot> snap = st.GetSnapshot((uint32_t)c.indexHeight);
    if (active) {
        if (!snap.has_value() || !snap->priceDefined) return Refuse(c, "RED-6", "no price in effect at the index tip (oracle outage)");
    }

    // RED-3: dry-run §3.7 for the burn
    {
        OverlayStateView overlay(index.View());
        State dry(overlay);
        bool relevant = false;
        TxLogRecord log = ProcessTx(dry, params, tx, c.indexHeight + 1, relevant);
        c.yedIn = log.yedIn;
        c.yedOut = log.yedOut;
        c.burned = log.burned;
    }
    c.requiredBurn = active ? RequiredBurn(vault->mintedCents, snap->errBps) : 0;
    if (c.burned < c.requiredBurn) return Refuse(c, "RED-3", strprintf("burn %d below required %d", c.burned, c.requiredBurn));

    // RED-4
    if (!tx.vJoinSplit.empty() || !tx.vShieldedSpend.empty() || !tx.vShieldedOutput.empty() || tx.valueBalance != 0) {
        return Refuse(c, "RED-4", "transaction is not transparent-only");
    }
    std::string reason;
    if (!IsStandardTx(tx, reason, ::Params(), chainHeight + 1)) return Refuse(c, "RED-4", "non-standard: " + reason);
    c.branchId = SignerBranchId();
    if (!AreInputsStandard(tx, view, c.branchId)) return Refuse(c, "RED-4", "non-standard inputs");
    CAmount valueIn = view.GetValueIn(tx);
    CAmount valueOut = tx.GetValueOut();
    c.fee = valueIn - valueOut;
    if (c.fee < g_yellowbackFee || c.fee > 100 * g_yellowbackFee) {
        return Refuse(c, "RED-4", strprintf("fee %d outside [%d, %d]", c.fee, g_yellowbackFee, 100 * g_yellowbackFee));
    }

    // RED-8 (and the mirror image: a transaction that has already expired, or is about to, is refused too)
    if (tx.nExpiryHeight == 0 || (int64_t)tx.nExpiryHeight > (int64_t)c.indexHeight + MINT_WINDOW + RED_SKEW) {
        return Refuse(c, "RED-8", strprintf("nExpiryHeight %u beyond index height + %d", tx.nExpiryHeight, MINT_WINDOW + RED_SKEW));
    }
    if ((int64_t)tx.nExpiryHeight <= (int64_t)c.indexHeight + (int64_t)TX_EXPIRING_SOON_THRESHOLD) {
        return Refuse(c, "RED-8", strprintf("nExpiryHeight %u has expired or is expiring (index height %d)", tx.nExpiryHeight, c.indexHeight));
    }

    // RED-7: the owner signature over the vault script reconstructed from the index (C5)
    std::vector<RosterRecord> rosters = st.GetRosters();
    if (vault->rosterIndex < 0 || vault->rosterIndex >= (int)rosters.size()) return Refuse(c, "RED-7", "vault references no known roster");
    c.vaultScript = VaultScript(vault->lockHeight, vault->ownerPubKey, rosters[vault->rosterIndex].script);
    if (c.vaultScript.empty()) return Refuse(c, "RED-7", "cannot reconstruct the vault script");
    const CCoins* vaultCoins = view.AccessCoins(c.vault.hash);
    if (vaultCoins->vout[c.vault.n].scriptPubKey != P2SHScript(c.vaultScript)) return Refuse(c, "RED-7", "vault output does not match the reconstructed script");
    std::vector<valtype> quorumSigs;
    valtype ownerSig;
    CScript suppliedScript;
    if (!ParseVaultScriptSig(tx.vin[0].scriptSig, quorumSigs, ownerSig, suppliedScript) || ownerSig.empty()) {
        return Refuse(c, "RED-7", "vin[0] carries no owner signature");
    }
    if (suppliedScript != c.vaultScript) return Refuse(c, "RED-7", "vin[0] carries a redeem script that differs from the index's vault script (C5)");
    c.sighash = SignatureHash(c.vaultScript, tx, 0, SIGHASH_ALL, vault->collateralZat, c.branchId);
    if (ownerSig.back() != SIGHASH_ALL) return Refuse(c, "RED-7", "owner signature is not SIGHASH_ALL");
    valtype der(ownerSig.begin(), ownerSig.end() - 1);
    if (!vault->ownerPubKey.Verify(c.sighash, der)) return Refuse(c, "RED-7", "owner signature does not verify");

    c.ok = true;
    c.rule = "";
    c.reason = "ok";
    return c;
}

} // namespace yellowback
