// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/txbuilder.h"

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "main.h"
#include "script/sign.h"
#include "script/standard.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/policy.h"
#include "yellowback/state.h"

#include <algorithm>

namespace yellowback {

namespace {

struct Context
{
    YellowbackWallet& yw;
    CWallet& wallet;
    YellowbackIndex& index;
    const Params& params;
    State st;
    int chainHeight;
    int indexHeight;
    uint32_t branchId;

    explicit Context(YellowbackWallet& yw_)
        : yw(yw_), wallet(*yw_.Wallet()), index(*yw_.Index()), params(yw_.Index()->GetParams()), st(yw_.Index()->View()),
          chainHeight(0), indexHeight(-1), branchId(0)
    {
        AssertLockHeld(cs_main);
        AssertLockHeld(wallet.cs_wallet);
        AssertLockHeld(index.cs_yellowback);
        if (!index.IsHealthy()) throw std::runtime_error("yellowback index unhealthy: " + index.UnhealthyReason() + "; restart with -reindex-yellowback");
        if (!index.IsSynced()) throw std::runtime_error("yellowback index is not synced to the chain tip; retry after the next block");
        chainHeight = chainActive.Height();
        std::optional<TipRecord> tip = st.GetTip();
        if (!tip.has_value()) throw std::runtime_error("yellowback index has not reached the start height");
        indexHeight = tip->height;
        branchId = SignerBranchId();
        if (wallet.IsLocked()) throw std::runtime_error("wallet is locked; walletpassphrase first");
    }

    CMutableTransaction NewTx(uint32_t expiry) const
    {
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(::Params().GetConsensus(), chainHeight + 1);
        mtx.nExpiryHeight = expiry;
        if ((int64_t)expiry < (int64_t)chainHeight + 1 + (int64_t)TX_EXPIRING_SOON_THRESHOLD) {
            throw std::runtime_error("transaction would expire too soon; the index is too far behind the chain");
        }
        return mtx;
    }

    CPubKey FreshKey(const std::string& purpose) const
    {
        CPubKey key;
        if (!wallet.GetKeyFromPool(key)) throw std::runtime_error("keypool ran out; unlock the wallet");
        wallet.SetAddressBook(key.GetID(), "", purpose);
        return key;
    }

    /** Smallest-first confirmed YEC inputs (F3) covering `needed`; returns the amount selected. */
    CAmount SelectYec(CAmount needed, CMutableTransaction& mtx, std::vector<std::pair<CScript, CAmount>>& prevs) const
    {
        if (needed <= 0) return 0;
        std::vector<COutput> coins;
        wallet.AvailableCoins(coins, true, nullptr, false, true, false, 1);
        std::sort(coins.begin(), coins.end(), [](const COutput& a, const COutput& b) { return a.Value() < b.Value(); });
        CAmount selected = 0;
        for (const COutput& c : coins) {
            if (!c.fSpendable) continue;
            // Never spend a YED-bearing or reserved output as plain YEC (belt and braces: locked coins are already excluded).
            COutPoint o(c.tx->GetHash(), c.i);
            if (st.GetToken(o).has_value() || yw.IsReserved(o)) continue;
            mtx.vin.push_back(CTxIn(o));
            prevs.push_back(std::make_pair(c.tx->vout[c.i].scriptPubKey, c.Value()));
            selected += c.Value();
            if (selected >= needed) break;
        }
        if (selected < needed) throw std::runtime_error(strprintf("insufficient YEC: need %s, have %s confirmed and unlocked", FormatMoney(needed), FormatMoney(selected)));
        return selected;
    }

    void SignInputs(CMutableTransaction& mtx, const std::vector<std::pair<CScript, CAmount>>& prevs, unsigned int first) const
    {
        for (unsigned int i = first; i < mtx.vin.size(); i++) {
            const auto& p = prevs[i - first];
            if (!SignSignature(wallet, p.first, mtx, i, p.second, SIGHASH_ALL, branchId)) {
                throw std::runtime_error(strprintf("failed to sign input %u", i));
            }
        }
    }
};

/** Select YED coins smallest-first for `needed` cents; applies the change floor (C20). */
std::vector<YedCoin> SelectYed(const Context& ctx, int64_t needed, int64_t& change)
{
    std::vector<YedCoin> coins = ctx.yw.SpendableCoins();
    std::sort(coins.begin(), coins.end(), [](const YedCoin& a, const YedCoin& b) { return a.token.cents < b.token.cents; });
    std::vector<YedCoin> sel;
    int64_t sum = 0;
    for (const YedCoin& c : coins) {
        sel.push_back(c);
        sum += c.token.cents;
        if (sum >= needed) break;
    }
    if (sum < needed) throw std::runtime_error(strprintf("insufficient YED: need %d cents, have %d confirmed and spendable", needed, sum));
    change = sum - needed;
    if (change > 0 && change < ctx.params.minOutput) {
        throw std::runtime_error(strprintf("change of %d cents is below the minimum output of %d cents (C20); send %d cents (all selected inputs) or at most %d cents",
                                           change, ctx.params.minOutput, sum, sum - ctx.params.minOutput));
    }
    if (sel.size() > 250) throw std::runtime_error("too many YED inputs; consolidate first");
    return sel;
}

} // namespace

BuiltTx BuildMint(YellowbackWallet& yw, int64_t cents, int tier, CReserveKey& reservekey)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    if (!p.IsValidTier(tier)) throw std::runtime_error("bad-mint-tier");
    if (cents < p.minMint || cents > p.maxMint) throw std::runtime_error(strprintf("bad-mint-amount: cents must be between %d and %d", p.minMint, p.maxMint));

    // MINTPOL-1
    const int evalHeight = ctx.indexHeight - g_yellowbackMintLag;
    if (evalHeight < p.startHeight) throw std::runtime_error(strprintf("index must reach height %d before minting (MINTPOL-1)", p.startHeight + g_yellowbackMintLag));
    std::optional<Snapshot> E = ctx.st.GetSnapshot((uint32_t)evalHeight);
    if (!E.has_value()) throw std::runtime_error("no snapshot at the evaluation height");
    if (!E->priceDefined) throw std::runtime_error(std::string(verdict::BAD_ORACLE_PRICE) + ": no price in effect at the evaluation height");
    if (E->healthPct < 100) throw std::runtime_error(std::string(verdict::MINT_BLOCKED_ERR) + strprintf(": system health %d%% is below 100%%", E->healthPct));
    if (E->mintFrozen) throw std::runtime_error(std::string(verdict::MINT_FROZEN) + ": minting is paused after a volatility breach");
    Totals totals = ctx.st.GetTotals();
    if (p.supplyCap != 0 && totals.supplyCents + cents > p.supplyCap) {
        throw std::runtime_error(std::string(verdict::MINT_SUPPLY_CAP) + strprintf(": supply cap headroom is %d cents", p.supplyCap - totals.supplyCents));
    }
    BuiltTx out;
    if (p.supplyCap != 0 && p.supplyCap - totals.supplyCents - cents < 10 * p.maxMint) {
        out.warning = "supply cap headroom is low; a competing mint may void this one (MINT-6)";
    }
    std::optional<CAmount> required = RequiredCollateralRounded(cents, p.tierRatioPct[tier], E->dcaBps, E->price);
    if (!required.has_value()) throw std::runtime_error("collateral requirement out of range");

    std::vector<RosterRecord> rosters = ctx.st.GetRosters();
    if (rosters.empty()) throw std::runtime_error("no roster");
    const uint32_t lockHeight = (uint32_t)evalHeight + p.tierBlocks[tier] + MINT_WINDOW;
    const uint32_t expiry = (uint32_t)evalHeight + MINT_WINDOW;

    CMutableTransaction mtx = ctx.NewTx(expiry);
    CPubKey owner = ctx.FreshKey("yellowback-vault");
    CScript vault = VaultScript(lockHeight, owner, rosters.back().script);
    if (vault.empty()) throw std::runtime_error("cannot build the vault script");
    const CScript tokenScript = GetScriptForDestination(owner.GetID());
    mtx.vout.push_back(CTxOut(required.value(), P2SHScript(vault)));
    mtx.vout.push_back(CTxOut(TOKEN_VALUE, tokenScript));
    std::vector<unsigned char> payload = EncodePayload(Payload::Mint((uint8_t)tier, (uint32_t)cents, lockHeight, (uint32_t)evalHeight, owner));
    if (payload.empty()) throw std::runtime_error("cannot encode the mint payload");
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));

    std::vector<std::pair<CScript, CAmount>> prevs;
    const CAmount needed = required.value() + TOKEN_VALUE + g_yellowbackFee;
    CAmount selected = ctx.SelectYec(needed, mtx, prevs);
    CAmount change = selected - needed;
    if (change > 0) {
        CPubKey changeKey;
        if (!reservekey.GetReservedKey(changeKey)) throw std::runtime_error("keypool ran out");
        mtx.vout.push_back(CTxOut(change, GetScriptForDestination(changeKey.GetID())));
    }
    ctx.SignInputs(mtx, prevs, 0);

    out.tx = mtx;
    out.freshKey = owner;
    out.evalHeight = evalHeight;
    out.lockHeight = lockHeight;
    out.collateralZat = required.value();
    out.ownYedOutputs.push_back(COutPoint(CTransaction(mtx).GetHash(), 1));
    return out;
}

BuiltTx BuildTransfer(YellowbackWallet& yw, const std::vector<std::pair<CScript, int64_t>>& recipients, CReserveKey& reservekey)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    if (recipients.empty()) throw std::runtime_error("no recipients");
    if (recipients.size() > MAX_ASSIGNMENTS - 1) throw std::runtime_error(strprintf("at most %u recipients per transaction", (unsigned)(MAX_ASSIGNMENTS - 1)));
    int64_t needed = 0;
    for (const auto& r : recipients) {
        if (r.second < p.minOutput || r.second > p.maxOutput) {
            throw std::runtime_error(strprintf("bad-xfer-amount: each amount must be between %d and %d cents", p.minOutput, p.maxOutput));
        }
        needed += r.second;
    }
    int64_t change = 0;
    std::vector<YedCoin> sel = SelectYed(ctx, needed, change);

    CMutableTransaction mtx = ctx.NewTx((uint32_t)ctx.indexHeight + MINT_WINDOW);
    BuiltTx out;
    for (const YedCoin& c : sel) {
        mtx.vin.push_back(CTxIn(c.outpoint));
        out.yedInputs.insert(c.outpoint);
    }
    std::vector<Assignment> assignments;
    for (size_t i = 0; i < recipients.size(); i++) {
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, recipients[i].first));
        assignments.push_back(Assignment((uint8_t)i, (uint32_t)recipients[i].second));
    }
    if (change > 0) {
        CPubKey changeKey = ctx.FreshKey("yellowback-change");
        out.freshKey = changeKey;
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(changeKey.GetID())));
        assignments.push_back(Assignment((uint8_t)recipients.size(), (uint32_t)change));
    }
    std::vector<unsigned char> payload = EncodePayload(Payload::Transfer(assignments));
    if (payload.empty()) throw std::runtime_error("cannot encode the transfer payload");
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));

    // YEC accounting: token inputs carry TOKEN_VALUE each; outputs need TOKEN_VALUE each plus the fee.
    const CAmount tokenIn = (CAmount)sel.size() * TOKEN_VALUE;
    const CAmount tokenOut = (CAmount)(recipients.size() + (change > 0 ? 1 : 0)) * TOKEN_VALUE;
    const CAmount yecNeeded = tokenOut + g_yellowbackFee - tokenIn;
    std::vector<std::pair<CScript, CAmount>> prevs;
    const unsigned int firstYec = mtx.vin.size();
    CAmount selectedYec = ctx.SelectYec(yecNeeded, mtx, prevs);
    CAmount yecChange = selectedYec - yecNeeded;
    if (yecChange > 0) {
        CPubKey changeKey;
        if (!reservekey.GetReservedKey(changeKey)) throw std::runtime_error("keypool ran out");
        mtx.vout.push_back(CTxOut(yecChange, GetScriptForDestination(changeKey.GetID())));
    }
    // Sign YED inputs (P2PKH, mine) then YEC inputs.
    for (size_t i = 0; i < sel.size(); i++) {
        if (!SignSignature(ctx.wallet, sel[i].token.scriptPubKey, mtx, i, sel[i].token.nValue, SIGHASH_ALL, ctx.branchId)) {
            throw std::runtime_error(strprintf("failed to sign YED input %u", (unsigned)i));
        }
    }
    ctx.SignInputs(mtx, prevs, firstYec);

    out.tx = mtx;
    const uint256 txid = CTransaction(mtx).GetHash();
    for (size_t i = 0; i < recipients.size(); i++) {
        if (yw.IsMineScript(recipients[i].first)) out.ownYedOutputs.push_back(COutPoint(txid, i));
    }
    if (change > 0) out.ownYedOutputs.push_back(COutPoint(txid, recipients.size()));
    out.changeCents = change;
    return out;
}

BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    const COutPoint vaultOut(vaultTxid, 0);
    std::optional<VaultRecord> vault = ctx.st.GetVault(vaultOut);
    if (!vault.has_value()) throw std::runtime_error("vault not found");
    if (!vault->IsOpen()) throw std::runtime_error("vault is already CLOSED");
    if (!yw.IsMineVault(vault.value())) throw std::runtime_error("vault owner key is not in this wallet");
    if (yw.GetPending(vaultOut).has_value()) throw std::runtime_error("a redemption of this vault is already pending; yed_abortredeem to start over");
    if ((int64_t)ctx.indexHeight < (int64_t)vault->lockHeight) {
        throw std::runtime_error(strprintf("vault is locked until height %u (index at %d)", vault->lockHeight, ctx.indexHeight));
    }
    const bool active = vault->Status() == VaultStatus::ACTIVE;
    std::optional<Snapshot> snap = ctx.st.GetSnapshot((uint32_t)ctx.indexHeight);
    if (active && (!snap.has_value() || !snap->priceDefined)) throw std::runtime_error("no price in effect; co-signers would refuse (RED-6)");
    const int64_t requiredBurn = active ? RequiredBurn(vault->mintedCents, snap->errBps) : 0;

    int64_t change = 0;
    std::vector<YedCoin> sel = requiredBurn > 0 ? SelectYed(ctx, requiredBurn, change) : std::vector<YedCoin>();

    std::vector<RosterRecord> rosters = ctx.st.GetRosters();
    if (vault->rosterIndex < 0 || vault->rosterIndex >= (int)rosters.size()) throw std::runtime_error("vault references no known roster");
    CScript vaultScript = VaultScript(vault->lockHeight, vault->ownerPubKey, rosters[vault->rosterIndex].script);
    if (vaultScript.empty()) throw std::runtime_error("cannot reconstruct the vault script");

    CMutableTransaction mtx = ctx.NewTx((uint32_t)ctx.indexHeight + MINT_WINDOW);
    mtx.nLockTime = vault->lockHeight;
    mtx.vin.push_back(CTxIn(vaultOut, CScript(), 0xFFFFFFFE));
    BuiltTx out;
    for (const YedCoin& c : sel) {
        mtx.vin.push_back(CTxIn(c.outpoint));
        out.yedInputs.insert(c.outpoint);
    }
    // Outputs: collateral (+ surplus token value - fee - change token value), optional YED change, REDEEM payload (C10).
    const CAmount tokenIn = (CAmount)sel.size() * TOKEN_VALUE;
    const CAmount changeTokens = change > 0 ? TOKEN_VALUE : 0;
    const CAmount collateralOut = vault->collateralZat + tokenIn - g_yellowbackFee - changeTokens;
    if (collateralOut <= 0) throw std::runtime_error("vault value does not cover the fee");
    CPubKey dest = ctx.FreshKey("yellowback-collateral");
    out.freshKey = dest;
    mtx.vout.push_back(CTxOut(collateralOut, GetScriptForDestination(dest.GetID())));
    std::vector<Assignment> assignments;
    if (change > 0) {
        CPubKey changeKey = ctx.FreshKey("yellowback-change");
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(changeKey.GetID())));
        assignments.push_back(Assignment(1, (uint32_t)change));
    }
    std::vector<unsigned char> payload = EncodePayload(Payload::Redeem(assignments));
    if (payload.empty()) throw std::runtime_error("cannot encode the redeem payload");
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));

    // Owner signature over the vault script (manual sighash, as rpc/atomicswap.cpp does).
    CKey ownerKey;
    if (!ctx.wallet.GetKey(vault->ownerPubKey.GetID(), ownerKey)) throw std::runtime_error("owner key not available");
    uint256 hash = SignatureHash(vaultScript, CTransaction(mtx), 0, SIGHASH_ALL, vault->collateralZat, ctx.branchId);
    valtype ownerSig;
    if (!ownerKey.Sign(hash, ownerSig)) throw std::runtime_error("owner signature failed");
    ownerSig.push_back((unsigned char)SIGHASH_ALL);
    mtx.vin[0].scriptSig = BuildVaultScriptSig({}, ownerSig, vaultScript);
    for (size_t i = 0; i < sel.size(); i++) {
        if (!SignSignature(ctx.wallet, sel[i].token.scriptPubKey, mtx, i + 1, sel[i].token.nValue, SIGHASH_ALL, ctx.branchId)) {
            throw std::runtime_error(strprintf("failed to sign YED input %u", (unsigned)i));
        }
    }

    out.tx = mtx;
    out.requiredBurn = requiredBurn;
    out.burnCents = requiredBurn;
    out.changeCents = change;
    if (change > 0) out.ownYedOutputs.push_back(COutPoint(CTransaction(mtx).GetHash(), 1));
    return out;
}

unsigned int CountQuorumSignatures(const CTransaction& tx)
{
    if (tx.vin.empty()) return 0;
    std::vector<valtype> q;
    valtype o;
    CScript s;
    if (!ParseVaultScriptSig(tx.vin[0].scriptSig, q, o, s)) return 0;
    return q.size();
}

unsigned int AddCosignature(CMutableTransaction& tx, const CScript& vaultScript, CAmount vaultValue, const Roster& roster,
                            const CKeyStore& keystore, uint32_t branchId)
{
    if (tx.vin.empty()) throw std::runtime_error("no inputs");
    std::vector<valtype> existing;
    valtype ownerSig;
    CScript supplied;
    if (!ParseVaultScriptSig(tx.vin[0].scriptSig, existing, ownerSig, supplied) || ownerSig.empty()) {
        throw std::runtime_error("vin[0] carries no owner signature");
    }
    const uint256 hash = SignatureHash(vaultScript, CTransaction(tx), 0, SIGHASH_ALL, vaultValue, branchId);

    // Place existing signatures by the roster key they verify against.
    std::map<size_t, valtype> byKey;
    for (const valtype& sig : existing) {
        if (sig.empty() || sig.back() != SIGHASH_ALL) throw std::runtime_error("a quorum signature is not SIGHASH_ALL");
        valtype der(sig.begin(), sig.end() - 1);
        bool matched = false;
        for (size_t k = 0; k < roster.keys.size(); k++) {
            if (roster.keys[k].Verify(hash, der)) {
                byKey[k] = sig;
                matched = true;
                break;
            }
        }
        if (!matched) throw std::runtime_error("a quorum signature matches no roster key");
    }
    // Our key: the first roster key we hold that has not signed yet.
    bool signedNow = false;
    for (size_t k = 0; k < roster.keys.size(); k++) {
        if (byKey.count(k)) continue;
        CKey key;
        if (!keystore.GetKey(roster.keys[k].GetID(), key)) continue;
        valtype sig;
        if (!key.Sign(hash, sig)) throw std::runtime_error("signing failed");
        sig.push_back((unsigned char)SIGHASH_ALL);
        byKey[k] = sig;
        signedNow = true;
        break;
    }
    if (!signedNow) {
        for (size_t k = 0; k < roster.keys.size(); k++) {
            if (byKey.count(k) && keystore.HaveKey(roster.keys[k].GetID())) throw std::runtime_error("this node has already signed");
        }
        throw std::runtime_error("this wallet holds no roster key");
    }
    std::vector<valtype> ordered;
    for (const auto& kv : byKey) ordered.push_back(kv.second);
    tx.vin[0].scriptSig = BuildVaultScriptSig(ordered, ownerSig, vaultScript);
    return ordered.size();
}

bool SameExceptSignatures(const CTransaction& a, const CTransaction& b, std::string& why)
{
    if (a.fOverwintered != b.fOverwintered || a.nVersion != b.nVersion || a.nVersionGroupId != b.nVersionGroupId) { why = "version differs"; return false; }
    if (a.nLockTime != b.nLockTime || a.nExpiryHeight != b.nExpiryHeight) { why = "locktime or expiry differs"; return false; }
    if (a.valueBalance != b.valueBalance || !b.vShieldedSpend.empty() || !b.vShieldedOutput.empty() || !b.vJoinSplit.empty()) { why = "shielded components differ"; return false; }
    if (a.vin.size() != b.vin.size() || a.vout.size() != b.vout.size()) { why = "input or output count differs"; return false; }
    for (size_t i = 0; i < a.vout.size(); i++) {
        if (a.vout[i].nValue != b.vout[i].nValue || a.vout[i].scriptPubKey != b.vout[i].scriptPubKey) { why = strprintf("output %u differs", (unsigned)i); return false; }
    }
    for (size_t i = 0; i < a.vin.size(); i++) {
        if (a.vin[i].prevout != b.vin[i].prevout || a.vin[i].nSequence != b.vin[i].nSequence) { why = strprintf("input %u differs", (unsigned)i); return false; }
        if (i > 0 && a.vin[i].scriptSig != b.vin[i].scriptSig) { why = strprintf("input %u signature differs", (unsigned)i); return false; }
    }
    std::vector<valtype> qa, qb;
    valtype oa, ob;
    CScript sa, sb;
    if (!ParseVaultScriptSig(a.vin[0].scriptSig, qa, oa, sa) || !ParseVaultScriptSig(b.vin[0].scriptSig, qb, ob, sb)) { why = "vin[0] scriptSig is malformed"; return false; }
    if (sa != sb) { why = "vault script differs"; return false; }
    if (oa != ob) { why = "owner signature differs"; return false; }
    if (qb.size() < qa.size()) { why = "quorum signatures were removed"; return false; }
    for (const valtype& s : qa) {
        if (std::find(qb.begin(), qb.end(), s) == qb.end()) { why = "an original quorum signature is missing"; return false; }
    }
    why.clear();
    return true;
}

} // namespace yellowback
