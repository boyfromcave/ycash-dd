// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/state.h"

#include "crypto/sha256.h"
#include "script/script.h"
#include "yellowback/math.h"
#include "yellowback/script.h"

#include <algorithm>
#include <map>

namespace yellowback {

namespace verdict {
const char* const OK = "ok";
const char* const BURNED = "burned";
const char* const NON_YELLOWBACK = "non-yellowback";
const char* const BAD_MINT_AMOUNT = "bad-mint-amount";
const char* const BAD_MINT_CLASS = "bad-mint-class";
const char* const BAD_MINT_LOCK_HEIGHT = "bad-mint-lock-height";
const char* const BAD_MINT_REF_HEIGHT = "bad-mint-ref-height";
const char* const BAD_MINT_OUTPUTS = "bad-mint-outputs";
const char* const BAD_MINT_OWNER_KEY = "bad-mint-owner-key";
const char* const BAD_MINT_VAULT_SCRIPT = "bad-mint-vault-script";
const char* const MINT_NOT_ACTIVE = "mint-not-active";
const char* const MINT_HALTED_NO_PRICE = "mint-halted-no-price";
const char* const MINT_HALTED_PARTICIPATION = "mint-halted-participation";
const char* const MINT_HALTED_GLOBAL_RATIO = "mint-halted-global-ratio";
const char* const MINT_HALTED_DIVERGENCE = "mint-halted-divergence";
const char* const BAD_MINT_COLLATERAL = "bad-mint-collateral";
const char* const MINT_UNSATISFIABLE = "mint-unsatisfiable";
const char* const MINT_SUPPLY_CAP = "mint-supply-cap";
const char* const BAD_MINT_TOKEN_OUTPUT = "bad-mint-token-output";
const char* const BAD_MINT_FEE = "bad-mint-fee";
const char* const BAD_TRANSFER_ASSIGNMENT = "bad-transfer-assignment";
const char* const TRANSFER_OVER_ASSIGNED = "transfer-over-assigned";
const char* const TRANSFER_NO_YED_INPUT = "transfer-no-yed-input";
const char* const VAULT_SPEND_MALFORMED = "vault-spend-malformed";
const char* const VAULT_SPEND_MISSING_BURN = "vault-spend-missing-burn";
const char* const VAULT_SPEND_SHORT_BURN = "vault-spend-short-burn";
const char* const VAULT_SPEND_BAD_FEE = "vault-spend-bad-fee";
const char* const VAULT_SPEND_BAD_PAYEE = "vault-spend-bad-payee";
const char* const VAULT_CLAIM_NOT_UNDERWATER = "vault-claim-not-underwater";
} // namespace verdict

// ---------------------------------------------------------------------------
// Shared helpers (§3.7 lookups)

namespace {

/** The key hash of a P2PKH scriptPubKey (OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG). */
std::optional<CKeyID> P2PKHKey(const CScript& s)
{
    if (s.size() != 25 || s[0] != OP_DUP || s[1] != OP_HASH160 || s[2] != 20 || s[23] != OP_EQUALVERIFY || s[24] != OP_CHECKSIG) {
        return std::nullopt;
    }
    return CKeyID(uint160(std::vector<unsigned char>(s.begin() + 3, s.begin() + 23)));
}

/** Tags[h] for h >= START_HEIGHT (nothing below it is read). */
std::optional<TagRecord> TagAt(const StateView& view, const Params& p, int64_t h)
{
    if (h < p.startHeight || h < 0 || h > 0xFFFFFFFFLL) return std::nullopt;
    return State(const_cast<StateView&>(view)).GetTag((uint32_t)h);
}

std::optional<Judgement> JudgementAt(const StateView& view, const Params& p, int64_t h)
{
    if (h < p.startHeight || h < 0 || h > 0xFFFFFFFFLL) return std::nullopt;
    return State(const_cast<StateView&>(view)).GetJudgement((uint32_t)h);
}

/** Quote-tag prices with lo < h <= hi (heights >= START_HEIGHT), ascending height. */
std::vector<std::pair<int64_t, MicroUsd>> QuotePrices(const StateView& view, const Params& p, int64_t lo, int64_t hi)
{
    std::vector<std::pair<int64_t, MicroUsd>> out;
    for (int64_t h = std::max<int64_t>(lo + 1, p.startHeight); h <= hi; h++) {
        std::optional<TagRecord> t = TagAt(view, p, h);
        if (t.has_value() && t->IsQuote()) out.push_back(std::make_pair(h, (MicroUsd)t->priceMicroUsd));
    }
    return out;
}

/** median(W, H) (PRICE-1): lowerMedian of the quote prices in (H - W, H], undefined below the window's fill. */
std::optional<MicroUsd> WindowMedian(const std::vector<std::pair<int64_t, MicroUsd>>& prices, int64_t height, int window, int minFill)
{
    std::vector<MicroUsd> v;
    for (const auto& hp : prices) {
        if (hp.first > height - window && hp.first <= height) v.push_back(hp.second);
    }
    if ((int64_t)v.size() < (int64_t)minFill) return std::nullopt;
    return LowerMedian(v);
}

/** Per-evaluation memo: E(R) and Snapshots[h] are read repeatedly by a block's transactions. */
struct EvalContext
{
    State& st;
    const Params& params;
    const int height;
    std::map<int, std::vector<CKeyID>> eligible;
    std::map<int64_t, std::optional<Snapshot>> snapshots;

    EvalContext(State& st, const Params& p, int h) : st(st), params(p), height(h) {}

    const std::vector<CKeyID>& Eligible(int refHeight)
    {
        auto it = eligible.find(refHeight);
        if (it != eligible.end()) return it->second;
        return eligible[refHeight] = EligiblePayees(st.View(), params, refHeight);
    }

    const std::optional<Snapshot>& Snap(int64_t h)
    {
        auto it = snapshots.find(h);
        if (it != snapshots.end()) return it->second;
        return snapshots[h] = SnapshotAt(st, params, h);
    }
};

/** A vault being spent by the transaction under evaluation. */
struct SpentVault
{
    COutPoint outpoint;
    VaultRecord record;
};

bool Contains(const std::vector<SpentVault>& v, const COutPoint& o)
{
    for (const SpentVault& s : v) {
        if (s.outpoint == o) return true;
    }
    return false;
}

bool ContainsKey(const std::vector<CKeyID>& keys, const CKeyID& k)
{
    return std::find(keys.begin(), keys.end(), k) != keys.end();
}

void SetVault(VaultRecord& v, const Payload& p, const Params& params, const CTransaction& tx, int height)
{
    v.ownerPubKey = p.ownerKeyBytes;
    v.termClass = p.termClass;
    v.lockHeight = (int32_t)p.lockHeight;
    v.claimHeight = (int32_t)((int64_t)p.lockHeight + params.grace);
    v.collateralZat = tx.vout.empty() ? 0 : tx.vout[0].nValue;
    v.mintedCents = p.cents;
    v.mintHeight = height;
    v.refHeight = (int32_t)p.refHeight;
}

// ---------------------------------------------------------------------------
// MINT-2..8 (§3.8), in the plan's clause order (SERIALISATION.md §3 D)

const char* MintVerdict(EvalContext& ctx, const CTransaction& tx, const Payload& p, unsigned int opReturnIndex, const Totals& totals)
{
    const Params& P = ctx.params;
    const int64_t H = ctx.height;
    // MINT-2 (signed 64-bit arithmetic, M1)
    if (!P.IsValidClass(p.termClass)) return verdict::BAD_MINT_CLASS;
    if ((Cents)p.cents < P.minMint || (Cents)p.cents > P.maxMint) return verdict::BAD_MINT_AMOUNT;
    const int64_t lock = p.lockHeight, ref = p.refHeight;
    if (!(lock + (int64_t)P.grace < (int64_t)LOCKTIME_THRESHOLD)) return verdict::BAD_MINT_LOCK_HEIGHT;
    if (!(H - P.refWindow <= ref && ref <= H - 1) || ref < P.startHeight) return verdict::BAD_MINT_REF_HEIGHT;
    if (!(lock > ref) || lock - ref < P.classMin[p.termClass] || lock - ref > P.classMax[p.termClass]) return verdict::BAD_MINT_LOCK_HEIGHT;
    // MINT-3
    if (tx.vout.size() < 3) return verdict::BAD_MINT_OUTPUTS;
    if (!p.ownerPubKey.IsValid() || !p.ownerPubKey.IsCompressed() || !p.ownerPubKey.IsFullyValid()) return verdict::BAD_MINT_OWNER_KEY;
    const CScript expected = P2SHScript(VaultScript(p.lockHeight, p.ownerPubKey, (uint32_t)(lock + P.grace)));
    if (!tx.vout[0].scriptPubKey.IsPayToScriptHash() || tx.vout[0].scriptPubKey != expected) return verdict::BAD_MINT_VAULT_SCRIPT;
    // MINT-4
    const std::optional<Snapshot>& S = ctx.Snap(ref);
    if (!S.has_value() || !S->activation.IsActive()) return verdict::MINT_NOT_ACTIVE;
    if (S->haltMask & HALT_NOT_ACTIVE) return verdict::MINT_NOT_ACTIVE;
    if (S->haltMask & HALT_NO_PRICE) return verdict::MINT_HALTED_NO_PRICE;
    if (S->haltMask & (HALT_PARTICIPATION | HALT_ENFORCEMENT)) return verdict::MINT_HALTED_PARTICIPATION;
    if (S->haltMask & HALT_GLOBAL_RATIO) return verdict::MINT_HALTED_GLOBAL_RATIO;
    if (S->haltMask & HALT_DIVERGENCE) return verdict::MINT_HALTED_DIVERGENCE;
    if (S->haltMask != 0) return verdict::MINT_NOT_ACTIVE; // an unknown bit: MINT-4 needs haltMask == 0
    // MINT-5
    std::optional<MicroUsd> pMint = S->PMint();
    if (!pMint.has_value()) return verdict::MINT_HALTED_NO_PRICE;
    std::optional<CAmount> required = RequiredCollateral((Cents)p.cents, MinRatioBps(P.baseRatioBps[p.termClass], S->sigmaMultBps), pMint.value());
    if (!required.has_value()) return verdict::MINT_UNSATISFIABLE;
    if (tx.vout[0].nValue < required.value() || tx.vout[0].nValue < 4 * P.feeMin) return verdict::BAD_MINT_COLLATERAL;
    // MINT-6
    std::optional<Cents> cap = SupplyCapCents(S->issuedZat, pMint, P.supplyCapBps);
    if (cap.has_value() && totals.supplyCents + (Cents)p.cents > cap.value()) return verdict::MINT_SUPPLY_CAP;
    // MINT-7
    if (opReturnIndex == 1) return verdict::BAD_MINT_TOKEN_OUTPUT;
    // MINT-8 (FEE-0 when E(R) is empty, K11)
    const std::vector<CKeyID>& eligible = ctx.Eligible((int)ref);
    if (!eligible.empty()) {
        const uint8_t fv = p.feeVout;
        if (fv == FEE_VOUT_NONE || fv >= tx.vout.size() || fv == 0 || fv == 1 || fv == opReturnIndex) return verdict::BAD_MINT_FEE;
        std::optional<CKeyID> key = P2PKHKey(tx.vout[fv].scriptPubKey);
        if (!key.has_value() || !ContainsKey(eligible, key.value())) return verdict::BAD_MINT_FEE;
        if (tx.vout[fv].nValue < FeeZat(tx.vout[0].nValue, P.feeMin, P.feeBps)) return verdict::BAD_MINT_FEE;
    }
    return verdict::OK;
}

/** MINT-1..8. Returns true when a Vaults entry (ACTIVE or VOID) was created. */
bool ApplyMint(EvalContext& ctx, const CTransaction& tx, const uint256& txid, const Payload& p, unsigned int opReturnIndex,
               TxLogRecord& log, Totals& totals)
{
    const Params& P = ctx.params;
    const char* v = MintVerdict(ctx, tx, p, opReturnIndex, totals);
    VaultRecord vault;
    SetVault(vault, p, P, tx, ctx.height);
    const COutPoint vaultOut(txid, 0);
    if (v == verdict::OK) {
        vault.status = (uint8_t)VaultStatus::ACTIVE;
        if (!ctx.Eligible((int)p.refHeight).empty()) {
            vault.feePaidZat = tx.vout[p.feeVout].nValue;
            log.feeZat = vault.feePaidZat;
            std::optional<CKeyID> key = P2PKHKey(tx.vout[p.feeVout].scriptPubKey);
            log.hasPayee = key.has_value();
            if (key.has_value()) log.payee = key.value();
        }
        ctx.st.Put(keys::Vault(vaultOut), vault);
        TokenRecord tok;
        tok.cents = p.cents;
        tok.nValue = tx.vout[1].nValue;
        tok.scriptPubKey = tx.vout[1].scriptPubKey;
        tok.height = ctx.height;
        const COutPoint tokOut(txid, 1);
        ctx.st.Put(keys::Token(tokOut), tok);
        AssignedOutput a;
        a.outpoint = tokOut;
        a.cents = p.cents;
        a.scriptPubKey = tok.scriptPubKey;
        log.assigned.push_back(a);
        totals.supplyCents += (Cents)p.cents;
        totals.collateralZat += vault.collateralZat;
        totals.activeVaults++;
        log.yedOut = p.cents;
        log.verdict = verdict::OK;
        return true;
    }
    log.verdict = v;
    log.yedOut = 0;
    if (!tx.vout.empty() && tx.vout[0].scriptPubKey.IsPayToScriptHash()) {
        vault.status = (uint8_t)VaultStatus::VOID;
        vault.voidReason = v;
        ctx.st.Put(keys::Vault(vaultOut), vault);
        totals.voidVaults++;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// XFER-1..3 for a TRANSFER, or a REDEEM payload that spends no ACTIVE vault

void ApplyTransfer(EvalContext& ctx, const CTransaction& tx, const uint256& txid, const Payload& p, Cents yedIn, TxLogRecord& log)
{
    const Params& P = ctx.params;
    const char* v = verdict::OK;
    for (const Assignment& a : p.assignments) {                                   // XFER-1
        if ((Cents)a.cents < P.minOutput || (Cents)a.cents > P.maxOutput) { v = verdict::BAD_TRANSFER_ASSIGNMENT; break; }
    }
    const int64_t total = p.AssignedCents();
    if (v == verdict::OK && total > yedIn) v = verdict::TRANSFER_OVER_ASSIGNED;    // XFER-2
    if (v == verdict::OK && yedIn <= 0) v = verdict::TRANSFER_NO_YED_INPUT;        // XFER-3
    if (v != verdict::OK) {
        log.verdict = v;                                                            // everything burns (cenotaph)
        log.yedOut = 0;
        return;
    }
    for (const Assignment& a : p.assignments) {
        const COutPoint out(txid, a.vout);
        TokenRecord tok;
        tok.cents = a.cents;
        tok.nValue = tx.vout[a.vout].nValue;
        tok.scriptPubKey = tx.vout[a.vout].scriptPubKey;
        tok.height = ctx.height;
        ctx.st.Put(keys::Token(out), tok);
        AssignedOutput ao;
        ao.outpoint = out;
        ao.cents = a.cents;
        ao.scriptPubKey = tok.scriptPubKey;
        log.assigned.push_back(ao);
    }
    log.yedOut = total;
    log.verdict = total < yedIn ? verdict::BURNED : verdict::OK;
}

// ---------------------------------------------------------------------------
// RED-1..4 over a transaction that spends at least one ACTIVE vault (M3)

const char* RedVerdict(EvalContext& ctx, const CTransaction& tx, const std::optional<FoundPayload>& fp,
                       const std::vector<SpentVault>& active, const std::optional<VaultSpendPath>& path, Cents yedIn)
{
    const Params& P = ctx.params;
    const int64_t H = ctx.height;
    // RED-1
    if (active.size() != 1 || tx.vin.empty() || !(active[0].outpoint == tx.vin[0].prevout)) return verdict::VAULT_SPEND_MALFORMED;
    if (!path.has_value()) return verdict::VAULT_SPEND_MALFORMED;
    if (!fp.has_value() || fp->payload.type != PayloadType::REDEEM) return verdict::VAULT_SPEND_MALFORMED;
    const Payload& p = fp->payload;
    const int64_t ref = p.refHeight;
    if (!(H - P.refWindow <= ref && ref <= H - 1) || ref < P.startHeight) return verdict::VAULT_SPEND_MALFORMED;
    for (const Assignment& a : p.assignments) {
        if ((Cents)a.cents < P.minOutput || (Cents)a.cents > P.maxOutput) return verdict::VAULT_SPEND_MALFORMED;
    }
    const VaultRecord& vault = active[0].record;
    // RED-2
    const int64_t burn = yedIn - p.AssignedCents();
    if (burn < vault.mintedCents) return burn <= 0 ? verdict::VAULT_SPEND_MISSING_BURN : verdict::VAULT_SPEND_SHORT_BURN;
    // RED-3 (FEE-0 when E(R) is empty, K11)
    const std::vector<CKeyID>& eligible = ctx.Eligible((int)ref);
    if (!eligible.empty()) {
        const uint8_t fv = p.feeVout;
        bool assignedVout = false;
        for (const Assignment& a : p.assignments) {
            if (a.vout == fv) assignedVout = true;
        }
        if (fv == FEE_VOUT_NONE || fv >= tx.vout.size() || fv == fp->opReturnIndex || assignedVout) return verdict::VAULT_SPEND_BAD_FEE;
        std::optional<CKeyID> key = P2PKHKey(tx.vout[fv].scriptPubKey);
        if (!key.has_value() || !ContainsKey(eligible, key.value())) return verdict::VAULT_SPEND_BAD_PAYEE;
        if (tx.vout[fv].nValue < FeeZat(vault.collateralZat, P.feeMin, P.feeBps)) return verdict::VAULT_SPEND_BAD_FEE;
    }
    // RED-4
    if (!path->ownerPath) {
        const std::optional<Snapshot>& S = ctx.Snap(ref);
        std::optional<MicroUsd> pClaim = S.has_value() ? S->PClaim() : std::nullopt;
        if (!IsUnderwater(vault.collateralZat, pClaim, vault.mintedCents, P.claimThresholdBps)) return verdict::VAULT_CLAIM_NOT_UNDERWATER;
    }
    return verdict::OK;
}

/** Applies IN-2 for the ACTIVE vaults either way. Returns true iff RED-1..4 failed. `active` records are updated in place. */
bool ApplyVaultSpend(EvalContext& ctx, const CTransaction& tx, const uint256& txid, const std::optional<FoundPayload>& fp,
                     std::vector<SpentVault>& active, Cents yedIn, TxLogRecord& log, Totals& totals)
{
    std::optional<VaultSpendPath> path = tx.vin.empty() ? std::nullopt : ParseVaultSpendPath(tx.vin[0].scriptSig);
    if (path.has_value()) log.path = path->ownerPath ? "owner" : "claim";
    const char* v = RedVerdict(ctx, tx, fp, active, path, yedIn);
    if (v == verdict::OK) {
        const Payload& p = fp->payload;
        VaultRecord& vault = active[0].record;
        for (const Assignment& a : p.assignments) {
            const COutPoint out(txid, a.vout);
            TokenRecord tok;
            tok.cents = a.cents;
            tok.nValue = tx.vout[a.vout].nValue;
            tok.scriptPubKey = tx.vout[a.vout].scriptPubKey;
            tok.height = ctx.height;
            ctx.st.Put(keys::Token(out), tok);
            AssignedOutput ao;
            ao.outpoint = out;
            ao.cents = a.cents;
            ao.scriptPubKey = tok.scriptPubKey;
            log.assigned.push_back(ao);
        }
        log.yedOut = p.AssignedCents();
        log.verdict = verdict::OK;
        vault.status = (uint8_t)(path->ownerPath ? VaultStatus::CLOSED : VaultStatus::CLAIMED);
        vault.closeHeight = ctx.height;
        vault.closingTxid = txid;
        vault.unbacked = false;
        vault.feePaidZat = 0; // rewritten by the close: the fee this spend paid (0 under FEE-0), SERIALISATION.md §3 J
        if (!ctx.Eligible((int)p.refHeight).empty()) {
            vault.feePaidZat = tx.vout[p.feeVout].nValue;
            log.feeZat = vault.feePaidZat;
            std::optional<CKeyID> key = P2PKHKey(tx.vout[p.feeVout].scriptPubKey);
            log.hasPayee = key.has_value();
            if (key.has_value()) log.payee = key.value();
        }
        totals.collateralZat -= vault.collateralZat;
        if (totals.activeVaults > 0) totals.activeVaults--;
        if (path->ownerPath) totals.closedVaults++;
        else totals.claimedVaults++;
        log.closedVaults.push_back(active[0].outpoint);
        return false;
    }
    log.verdict = v;
    log.yedOut = 0;
    for (SpentVault& s : active) {
        s.record.status = (uint8_t)VaultStatus::CLOSED;
        s.record.closeHeight = ctx.height;
        s.record.closingTxid = txid;
        totals.collateralZat -= s.record.collateralZat;
        if (totals.activeVaults > 0) totals.activeVaults--;
        totals.closedVaults++;
        log.closedVaults.push_back(s.outpoint);
    }
    return true;
}

// ---------------------------------------------------------------------------
// One transaction (IN-1..3, TX-0, MINT / XFER / RED)

TxOutcome ProcessTxImpl(EvalContext& ctx, const CTransaction& tx)
{
    TxOutcome out;
    TxLogRecord& log = out.log;
    log.height = ctx.height;
    log.verdict = verdict::NON_YELLOWBACK;
    if (tx.IsCoinBase()) return out; // TX-0: the coinbase registers nothing; only its scriptSig (the tag) is read

    const uint256 txid = tx.GetHash();
    Totals totals = ctx.st.GetTotals();
    const Totals before = totals;

    // ---- inputs (IN-1, and the vaults IN-2 will close). Shielded components are ignored (TX-0).
    Cents yedIn = 0;
    std::vector<SpentVault> active, voids;
    for (const CTxIn& in : tx.vin) {
        const COutPoint& prev = in.prevout;
        if (std::optional<TokenRecord> tok = ctx.st.GetToken(prev)) {
            yedIn += tok->cents;
            AssignedOutput spent;
            spent.outpoint = prev;
            spent.cents = tok->cents;
            spent.scriptPubKey = tok->scriptPubKey;
            log.spentTokens.push_back(spent);
            ctx.st.EraseKey(keys::Token(prev));
        }
        if (std::optional<VaultRecord> v = ctx.st.GetVault(prev)) {
            if (v->Status() == VaultStatus::ACTIVE && !Contains(active, prev)) active.push_back({ prev, v.value() });
            else if (v->Status() == VaultStatus::VOID && !Contains(voids, prev)) voids.push_back({ prev, v.value() });
        }
    }
    log.yedIn = yedIn;
    log.verdict = verdict::OK;

    // ---- outputs
    std::optional<FoundPayload> fp = FindPayload(tx);
    bool touched = !log.spentTokens.empty() || !active.empty() || !voids.empty();
    if (!active.empty()) {
        // M3: a transaction spending an ACTIVE vault sees RED-1..4 only, whatever its payload.
        log.type = (uint8_t)TxLogType::REDEEM;
        out.vaultSpend = true;
        out.redFailed = ApplyVaultSpend(ctx, tx, txid, fp, active, yedIn, log, totals);
        touched = true;
    } else if (fp.has_value() && fp->payload.type == PayloadType::MINT) {
        log.type = (uint8_t)TxLogType::MINT;
        if (ApplyMint(ctx, tx, txid, fp->payload, fp->opReturnIndex, log, totals)) touched = true;
    } else if (fp.has_value()) {
        log.type = (uint8_t)(fp->payload.type == PayloadType::TRANSFER ? TxLogType::TRANSFER : TxLogType::REDEEM);
        ApplyTransfer(ctx, tx, txid, fp->payload, yedIn, log);
    } else {
        log.type = (uint8_t)TxLogType::NONE;
        if (yedIn > 0) log.verdict = verdict::BURNED;
    }

    // ---- IN-2 for VOID vaults: an ordinary spend that closes them (K3)
    for (SpentVault& s : voids) {
        s.record.status = (uint8_t)VaultStatus::CLOSED;
        s.record.closeHeight = ctx.height;
        s.record.closingTxid = txid;
        s.record.unbacked = false;
        if (totals.voidVaults > 0) totals.voidVaults--;
        totals.closedVaults++;
        log.closedVaults.push_back(s.outpoint);
    }

    // ---- IN-3 (for a MINT the burn formula's yedOut is 0, N19: every YED input of a mint is burned)
    const Cents burnOut = log.Type() == TxLogType::MINT ? 0 : log.yedOut;
    const Cents burned = yedIn - burnOut;
    log.burned = burned;
    totals.supplyCents -= burned;
    for (SpentVault& s : active) {
        s.record.burnedCents = burned;
        if (out.redFailed) {
            s.record.unbacked = burned < s.record.mintedCents;
            totals.unbackedCents += std::max<int64_t>(0, s.record.mintedCents - burned);
        }
        ctx.st.Put(keys::Vault(s.outpoint), s.record);
    }
    for (SpentVault& s : voids) {
        s.record.burnedCents = burned;
        ctx.st.Put(keys::Vault(s.outpoint), s.record);
    }
    if (touched) {
        if (!(SerializeRecord(totals) == SerializeRecord(before)) || !ctx.st.Has(keys::Totals())) ctx.st.Put(keys::Totals(), totals);
        ctx.st.Put(keys::TxLog(txid), log);
        out.relevant = true;
    } else {
        log.verdict = verdict::NON_YELLOWBACK;
    }
    return out;
}

// ---------------------------------------------------------------------------
// REG-4 judgement, performed at H for the quote tag at H - PEER_LAG

void Judge(State& st, const Params& P, int64_t H)
{
    const int64_t lag = P.peerLag;
    const int64_t t = H - lag;
    if (t < P.startHeight) return;
    std::optional<TagRecord> tag = TagAt(st.View(), P, t);
    if (!tag.has_value() || !tag->IsQuote()) return;
    std::vector<MicroUsd> peers;
    for (int64_t h = t - lag; h <= t + lag - 1; h++) {
        if (h == t) continue;
        std::optional<TagRecord> q = TagAt(st.View(), P, h);
        if (q.has_value() && q->IsQuote()) peers.push_back((MicroUsd)q->priceMicroUsd);
    }
    Judgement j;
    if ((int64_t)peers.size() >= (int64_t)P.peerMin) {
        std::optional<MicroUsd> m = LowerMedian(peers);
        if (m.has_value() && m.value() > 0) {
            const uint64_t price = tag->priceMicroUsd, med = (uint64_t)m.value();
            const uint64_t diff = price >= med ? price - med : med - price;
            const uint64_t dev = diff * (uint64_t)BPS / med;          // price, med <= PRICE_MAX: no overflow
            j.evaluated = true;
            j.inBand = dev <= (uint64_t)std::max(0, P.accuracyBandBps);
            j.penalized = dev > (uint64_t)std::max(0, P.deviationBps);
        }
    }
    st.Put(keys::Judgement((uint32_t)t), j);
}

} // namespace

// ---------------------------------------------------------------------------
// Public lookups

std::optional<Snapshot> SnapshotAt(const State& st, const Params& P, int64_t height)
{
    if (height < P.startHeight) return Snapshot::Virtual();
    if (height < 0 || height > 0xFFFFFFFFLL) return std::nullopt;
    return st.GetSnapshot((uint32_t)height);
}

bool EnforcementOn(const State& st, const Params& P, int height)
{
    std::optional<Snapshot> prev = SnapshotAt(st, P, (int64_t)height - 1);
    if (!prev.has_value() || !prev->activation.IsActive()) return false;
    if (prev->haltMask & HALT_ENFORCEMENT) return false;
    if (P.enforceUntilHeight > 0 && height > P.enforceUntilHeight) return false;
    return true;
}

uint32_t SignalCount(const State& st, const Params& P, int height)
{
    uint32_t n = 0;
    for (int64_t h = std::max<int64_t>((int64_t)height - P.signalWindow + 1, P.startHeight); h <= height; h++) {
        std::optional<TagRecord> t = TagAt(st.View(), P, h);
        if (t.has_value() && t->signal) n++;
    }
    return n;
}

std::vector<CKeyID> EligiblePayees(const StateView& view, const Params& P, int refHeight)
{
    std::vector<CKeyID> keys;
    for (int64_t h = std::max<int64_t>((int64_t)refHeight - P.payeeWindow + 1, P.startHeight); h <= refHeight; h++) {
        std::optional<TagRecord> t = TagAt(view, P, h);
        if (!t.has_value() || !t->IsQuote()) continue;
        CKeyID k(t->payoutKey);
        if (!ContainsKey(keys, k)) keys.push_back(k);
    }
    return keys;
}

bool Registered(const StateView& view, const Params& P, const CKeyID& key, int refHeight)
{
    for (int64_t h = std::max<int64_t>((int64_t)refHeight - P.nReg + 1, P.startHeight); h <= refHeight; h++) {
        std::optional<TagRecord> t = TagAt(view, P, h);
        if (t.has_value() && t->IsQuote() && t->payoutKey == key) return true;
    }
    return false;
}

bool Penalized(const StateView& view, const Params& P, const CKeyID& key, int refHeight, int penaltyBlocks)
{
    // t + PEER_LAG < R <= t + PEER_LAG + N_PENALTY  <=>  R - PEER_LAG - N_PENALTY <= t < R - PEER_LAG
    const int64_t lo = (int64_t)refHeight - P.peerLag - std::max(0, penaltyBlocks);
    const int64_t hi = (int64_t)refHeight - P.peerLag - 1;
    for (int64_t t = std::max<int64_t>(lo, P.startHeight); t <= hi; t++) {
        std::optional<TagRecord> tag = TagAt(view, P, t);
        if (!tag.has_value() || !tag->IsQuote() || !(tag->payoutKey == key)) continue;
        std::optional<Judgement> j = JudgementAt(view, P, t);
        if (j.has_value() && j->penalized) return true;
    }
    return false;
}

int AccuracyBps(const StateView& view, const Params& P, const CKeyID& key, int refHeight, int accuracyWindow)
{
    const int64_t hi = (int64_t)refHeight - P.peerLag;
    const int64_t lo = hi - std::max(0, accuracyWindow) + 1;
    int64_t quoted = 0, inBand = 0;
    for (int64_t t = std::max<int64_t>(lo, P.startHeight); t <= hi; t++) {
        std::optional<TagRecord> tag = TagAt(view, P, t);
        if (!tag.has_value() || !tag->IsQuote() || !(tag->payoutKey == key)) continue;
        std::optional<Judgement> j = JudgementAt(view, P, t);
        if (!j.has_value() || !j->evaluated) continue;
        quoted++;
        if (j->inBand) inBand++;
    }
    return quoted > 0 ? (int)(BPS * inBand / quoted) : 0;
}

std::optional<CKeyID> DefaultPayee(const StateView& view, const Params& P, int refHeight,
                                   const std::vector<unsigned char>& selector, const PayeePolicy& policy)
{
    const std::vector<CKeyID> eligible = EligiblePayees(view, P, refHeight);
    if (eligible.empty()) return std::nullopt;
    if (policy.preferred.has_value() && ContainsKey(eligible, policy.preferred.value())) return policy.preferred;

    std::vector<std::pair<CKeyID, int64_t>> candidates;
    for (int64_t h = std::max<int64_t>((int64_t)refHeight - P.payeeWindow + 1, P.startHeight); h <= refHeight; h++) {
        std::optional<TagRecord> t = TagAt(view, P, h);
        if (!t.has_value() || !t->IsQuote()) continue;
        CKeyID k(t->payoutKey);
        if (Penalized(view, P, k, refHeight, policy.penaltyBlocks)) continue;
        const int64_t w = BPS + (int64_t)std::max(0, policy.tiltBps) * (int64_t)AccuracyBps(view, P, k, refHeight, policy.accuracyWindow) / BPS;
        candidates.push_back(std::make_pair(k, w));
    }
    if (candidates.empty()) {
        for (const CKeyID& k : eligible) candidates.push_back(std::make_pair(k, (int64_t)BPS));
    }
    int64_t total = 0;
    for (const auto& c : candidates) total += c.second;
    if (total <= 0) return eligible.front();

    uint256 blockHash;
    if (refHeight >= P.startHeight && refHeight >= 0) {
        std::optional<Snapshot> s = State(const_cast<StateView&>(view)).GetSnapshot((uint32_t)refHeight);
        if (s.has_value()) blockHash = s->blockHash;
    }
    unsigned char digest[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(blockHash.begin(), 32).Write(selector.data(), selector.size()).Finalize(digest);
    uint64_t seed = 0;
    for (int i = 7; i >= 0; i--) seed = (seed << 8) | digest[i];
    const int64_t pick = (int64_t)(seed % (uint64_t)total);
    int64_t cumulative = 0;
    for (const auto& c : candidates) {
        cumulative += c.second;
        if (cumulative > pick) return c.first;
    }
    return candidates.back().first;
}

// ---------------------------------------------------------------------------
// SNAP

Snapshot ComputeSnapshot(State& st, const Params& P, int height, const uint256& blockHash, CAmount subsidyZat,
                         const std::optional<CoinbaseTag>& tag)
{
    const int64_t H = height;
    Judge(st, P, H);                                                                    // REG-4

    // ACT-1..3
    const uint32_t count = SignalCount(st, P, height);
    Activation a = st.GetActivation();
    const Activation aBefore = a;
    if (a.Status() == ActivationStatus::SIGNALING && H >= (int64_t)P.startHeight + P.signalWindow - 1 && count >= (uint32_t)std::max(0, P.activationThreshold)) {
        a.status = (uint8_t)ActivationStatus::LOCKED_IN;
        a.lockInHeight = height;
        a.activateHeight = (int32_t)(H + P.activationDelay);
    }
    if (a.Status() == ActivationStatus::LOCKED_IN && H >= a.activateHeight) a.status = (uint8_t)ActivationStatus::ACTIVE;
    if (!(a == aBefore) || !st.Has(keys::Activation())) st.Put(keys::Activation(), a);

    Snapshot s;
    s.blockHash = blockHash;
    s.tagged = tag.has_value();
    s.quote = tag.has_value() && tag->IsQuote();
    s.signalCount = count;
    s.activation = a;

    // PRICE-1..2
    const int maxWindow = std::max(P.pFastWindow, std::max(P.pMidWindow, P.pSlowWindow));
    const std::vector<std::pair<int64_t, MicroUsd>> prices = QuotePrices(st.View(), P, H - maxWindow, H);
    std::optional<MicroUsd> pFast = WindowMedian(prices, H, P.pFastWindow, P.pFastMinFill);
    std::optional<MicroUsd> pMid = WindowMedian(prices, H, P.pMidWindow, P.pMidMinFill);
    std::optional<MicroUsd> pSlow = WindowMedian(prices, H, P.pSlowWindow, P.pSlowMinFill);
    std::optional<MicroUsd> pMint, pClaim;
    if (pFast.has_value() && pMid.has_value() && pSlow.has_value()) pMint = std::min(pFast.value(), std::min(pMid.value(), pSlow.value()));
    if (pMid.has_value() && pSlow.has_value()) pClaim = std::max(pMid.value(), pSlow.value());
    s.pFast = pFast.value_or(0);
    s.pMid = pMid.value_or(0);
    s.pSlow = pSlow.value_or(0);
    s.pMint = pMint.value_or(0);
    s.pClaim = pClaim.value_or(0);

    // SIGMA-1: s_0 is this SNAP's pFast, s_k = Snapshots[H - k * VOL_STEP].pFast; a virtual or missing sample is undefined (K12)
    std::vector<std::optional<MicroUsd>> samples;
    samples.push_back(pFast);
    if (P.volStep > 0) {
        for (int k = 1; k <= P.volWindow / P.volStep; k++) {
            std::optional<Snapshot> sk = SnapshotAt(st, P, H - (int64_t)k * P.volStep);
            samples.push_back(sk.has_value() ? sk->PFast() : std::nullopt);
        }
    }
    s.sigmaMultBps = SigmaMultBps(samples, P.sigmaRefBps, P.volPeriodsPerYear, P.sigmaMultMaxBps);

    // Issuance, totals, global ratio
    std::optional<Snapshot> prevOpt = SnapshotAt(st, P, H - 1);
    const Snapshot prev = prevOpt.has_value() ? prevOpt.value() : Snapshot();
    s.issuedZat = (CAmount)((uint64_t)prev.issuedZat + (uint64_t)subsidyZat);       // wraps rather than overflows (total)
    const Totals totals = st.GetTotals();
    s.supplyCents = totals.supplyCents;
    s.collateralZat = totals.collateralZat;
    std::optional<int64_t> ratio = GlobalRatioBps(s.collateralZat, pMint, s.supplyCents);
    s.globalRatioBps = ratio.value_or(0);

    // HALT-1..4, ACT-4, ACT-6
    uint32_t mask = 0;
    if (!a.IsActive()) mask |= HALT_NOT_ACTIVE;                                                              // HALT-4
    if (!pMint.has_value()) mask |= HALT_NO_PRICE;                                                          // HALT-1
    if (pMint.has_value() && s.supplyCents > 0 && ratio.has_value() && ratio.value() < P.globalRatioHaltBps) mask |= HALT_GLOBAL_RATIO; // HALT-2
    if (pFast.has_value() && pMid.has_value() && pSlow.has_value()) {                                       // HALT-3
        const int64_t k = BPS - P.divergenceBps;
        if (pFast.value() * BPS < k * pMid.value() || pMid.value() * BPS < k * pSlow.value()) mask |= HALT_DIVERGENCE;
    }
    bool part = (prev.haltMask & HALT_PARTICIPATION) != 0;                                                  // ACT-4
    if (part) part = count < (uint32_t)std::max(0, P.activationThreshold);
    if (a.IsActive() && count < (uint32_t)std::max(0, P.participationFloor)) part = true;
    if (part) mask |= HALT_PARTICIPATION;
    bool enf = (prev.haltMask & HALT_ENFORCEMENT) != 0;                                                     // ACT-6
    if (enf) enf = count < (uint32_t)std::max(0, P.enforcementResume);
    if (a.IsActive() && count < (uint32_t)std::max(0, P.enforcementFloor)) enf = true;
    if (enf) mask |= HALT_ENFORCEMENT;
    s.haltMask = mask;
    return s;
}

// ---------------------------------------------------------------------------
// Blocks

TxOutcome ProcessTx(State& st, const Params& params, const CTransaction& tx, int height)
{
    EvalContext ctx(st, params, height);
    return ProcessTxImpl(ctx, tx);
}

BlockEvaluation EvaluateBlock(OverlayStateView& overlay, const Params& params, const CBlock& block, int height,
                              const uint256& blockHash, CAmount subsidyZat)
{
    BlockEvaluation ev;
    if (height < params.startHeight) return ev;          // ignored completely (§3.8)
    ev.undo.height = height;
    State st(overlay, &ev.undo);
    EvalContext ctx(st, params, height);

    // The records the state hash always carries (Params, Totals, Activation) exist from the first applied block.
    if (!st.Has(keys::Params())) st.Put(keys::Params(), ParamsRecord(params));
    if (!st.Has(keys::Totals())) st.Put(keys::Totals(), Totals());

    ev.enforcementOn = EnforcementOn(st, params, height);                                // ACT-5, from Snapshots[H - 1]

    // TAG-1..5: the coinbase scriptSig, read first
    std::optional<CoinbaseTag> tag;
    if (!block.vtx.empty() && block.vtx[0].IsCoinBase() && !block.vtx[0].vin.empty()) {
        tag = FindTag(block.vtx[0].vin[0].scriptSig, height);
    }
    if (tag.has_value()) {
        TagRecord t;
        t.payoutKey = tag->payoutKey;
        t.priceMicroUsd = tag->priceMicroUsd;
        t.signal = tag->Signal();
        t.sourceMask = tag->sourceMask;
        st.Put(keys::Tag((uint32_t)height), t);
    }

    // Transactions in block order (TX-0 skips every coinbase)
    for (const CTransaction& tx : block.vtx) {
        if (tx.IsCoinBase()) continue;
        TxOutcome o = ProcessTxImpl(ctx, tx);
        if (o.relevant) ev.txlogs.push_back(std::make_pair(tx.GetHash(), o.log));
        if (o.redFailed && !ev.blockInvalid) {                                             // BLK-1
            ev.blockInvalid = true;
            ev.reason = o.log.verdict + ":" + tx.GetHash().GetHex();
        }
    }

    // SNAP
    ev.snapshot = ComputeSnapshot(st, params, height, blockHash, subsidyZat, tag);
    st.Put(keys::Snapshot((uint32_t)height), ev.snapshot);
    TipRecord tip;
    tip.height = height;
    tip.blockHash = blockHash;
    tip.schemaVersion = SCHEMA_VERSION;
    tip.network = params.network;
    st.Put(keys::Tip(), tip);
    return ev;
}

std::optional<std::string> ApplyBlock(StateView& view, const Params& params, const CBlock& block, int height,
                                      const uint256& blockHash, CAmount subsidyZat, UndoRecord& undo)
{
    OverlayStateView overlay(view);
    BlockEvaluation ev = EvaluateBlock(overlay, params, block, height, blockHash, subsidyZat);
    overlay.Commit();
    undo = ev.undo;
    return std::nullopt;
}

void UndoBlock(StateView& view, const UndoRecord& undo)
{
    for (const UndoEntry& e : undo.entries) {
        if (e.hadValue) view.Write(e.key, e.value);
        else view.Erase(e.key);
    }
}

} // namespace yellowback
