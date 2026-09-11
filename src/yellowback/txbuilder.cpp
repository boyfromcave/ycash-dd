// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/txbuilder.h"

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "key_io.h"
#include "main.h"
#include "policy/policy.h"
#include "script/interpreter.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "script/standard.h"
#include "streams.h"
#include "util.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/policy.h"
#include "yellowback/state.h"
#include "zcash/Address.hpp"
#include "zcash/address/zip32.h"

#include <algorithm>
#include <variant>

namespace yellowback {

// ---------------------------------------------------------------- pure shapes (§3.5)

std::vector<CTxOut> MintOutputs(const MintShape& s, int& feeVout)
{
    const CScript vault = VaultScript(s.lockHeight, s.owner, s.claimHeight);
    if (vault.empty()) throw std::runtime_error("mint-bad-lock: cannot build the vault script");
    feeVout = s.payee.has_value() ? 3 : -1;
    std::vector<unsigned char> payload = EncodePayload(Payload::Mint((uint8_t)s.termClass, (uint32_t)s.cents, s.lockHeight, (uint32_t)s.refHeight,
                                                                     s.owner, feeVout < 0 ? FEE_VOUT_NONE : (uint8_t)feeVout));
    if (payload.empty()) throw std::runtime_error("cannot encode the mint payload");
    std::vector<CTxOut> vout;
    vout.push_back(CTxOut(s.collateralZat, P2SHScript(vault)));                        // vout[0] vault
    vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(s.owner.GetID())));      // vout[1] token
    vout.push_back(CTxOut(0, PayloadScript(payload)));                                  // vout[2] payload
    if (s.payee.has_value()) vout.push_back(CTxOut(s.feeZat, GetScriptForDestination(s.payee.value())));   // vout[3] fee
    return vout;
}

VaultSpendPlan PlanVaultSpend(const VaultSpendShape& s)
{
    VaultSpendPlan plan;
    plan.nLockTime = s.ownerPath ? s.lockHeight : s.claimHeight;
    plan.vin.push_back(CTxIn(s.vaultOut, CScript(), 0xFFFFFFFE));
    CAmount yedValue = 0;
    Cents yedIn = 0;
    for (const YedCoin& c : s.yedInputs) {
        plan.vin.push_back(CTxIn(c.outpoint));
        yedValue += c.token.nValue;
        yedIn += c.token.cents;
    }
    const bool change = s.withPayload && s.changeCents > 0;
    const bool fee = s.withPayload && s.payee.has_value();
    plan.collateralOut = s.vaultValue + yedValue - s.networkFee - (fee ? s.feeZat : 0) - (change ? TOKEN_VALUE : 0);
    if (plan.collateralOut <= 0) throw std::runtime_error("vault-value-too-small: the vault does not cover the fees");
    plan.burnCents = s.withPayload ? yedIn - s.changeCents : yedIn;

    std::vector<Assignment> assignments;
    auto addChange = [&]() {
        plan.changeVout = (int)plan.vout.size();
        plan.vout.push_back(CTxOut(TOKEN_VALUE, s.changeScript));
        assignments.push_back(Assignment((uint8_t)plan.changeVout, (uint32_t)s.changeCents));
    };
    auto addFee = [&]() {
        plan.feeVout = (int)plan.vout.size();
        plan.vout.push_back(CTxOut(s.feeZat, GetScriptForDestination(s.payee.value())));
    };
    auto addPayload = [&]() {
        std::vector<unsigned char> payload = EncodePayload(Payload::Redeem((uint32_t)s.refHeight, plan.feeVout < 0 ? FEE_VOUT_NONE : (uint8_t)plan.feeVout, assignments));
        if (payload.empty()) throw std::runtime_error("cannot encode the redeem payload");
        plan.vout.push_back(CTxOut(0, PayloadScript(payload)));
    };

    if (s.collateralScript.has_value()) {
        // Transparent destination: collateral, fee, change, payload.
        plan.vout.push_back(CTxOut(plan.collateralOut, s.collateralScript.value()));
        if (!s.withPayload) return plan;
        if (fee) addFee();
        if (change) addChange();
        addPayload();
        return plan;
    }
    // Sapling destination (M13): change, payload, fee; the collateral is the caller's note.
    if (!s.withPayload) return plan;
    if (change) addChange();
    if (fee) {
        // feeVout must be known when the payload is encoded: the fee lands after the payload.
        plan.feeVout = (int)plan.vout.size() + 1;
        addPayload();
        plan.vout.push_back(CTxOut(s.feeZat, GetScriptForDestination(s.payee.value())));
    } else {
        addPayload();
    }
    return plan;
}

void SignVaultSpend(BuiltTx& out, const CKeyStore& keystore, uint32_t branchId, bool ownerPath)
{
    if (out.builder.has_value()) throw std::runtime_error("FinishSapling must run before SignVaultSpend");
    if (out.tx.vin.size() != out.yedPrevs.size() + 1) throw std::runtime_error("vault spend input count mismatch");
    if (ownerPath) {
        // Owner signature: ZIP-243 over the vault script with the vault's nValue and the epoch
        // branch id (both bound by the digest, mapping §13.1), as rpc/atomicswap.cpp signs by hand.
        CKey ownerKey;
        if (!keystore.GetKey(out.ownerPubKey.GetID(), ownerKey)) throw std::runtime_error("vault-not-owned: owner key not available");
        uint256 hash = SignatureHash(out.vaultScript, CTransaction(out.tx), 0, SIGHASH_ALL, out.vaultValue, branchId);
        valtype ownerSig;
        if (!ownerKey.Sign(hash, ownerSig)) throw std::runtime_error("owner signature failed");
        ownerSig.push_back((unsigned char)SIGHASH_ALL);
        out.tx.vin[0].scriptSig = OwnerScriptSig(ownerSig, out.vaultScript);
    } else {
        out.tx.vin[0].scriptSig = ClaimScriptSig(out.vaultScript);
    }
    for (size_t i = 0; i < out.yedPrevs.size(); i++) {
        if (!SignSignature(keystore, out.yedPrevs[i].first, out.tx, i + 1, out.yedPrevs[i].second, SIGHASH_ALL, branchId)) {
            throw std::runtime_error(strprintf("failed to sign YED input %u", (unsigned)i));
        }
    }
    out.path = ownerPath ? "owner" : "claim";
    out.ownYedOutputs.clear();
    if (out.changeVout >= 0) out.ownYedOutputs.push_back(COutPoint(CTransaction(out.tx).GetHash(), out.changeVout));
}

std::vector<unsigned char> OutPointSelector(const COutPoint& out)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << out;
    return std::vector<unsigned char>(ss.begin(), ss.end());
}

// ---------------------------------------------------------------- wallet builders

namespace {

/** At most this many Sapling notes in one mint (one spend proof each, seconds apiece). */
const size_t MAX_SAPLING_SPENDS = 20;

/** A funding source or collateral destination given as an address string (§4.6). */
struct AddressChoice
{
    enum Kind { NONE, TRANSPARENT, SAPLING } kind;
    CKeyID keyId;                                   //!< TRANSPARENT
    libzcash::SaplingPaymentAddress sapling;        //!< SAPLING
    std::string text;
    AddressChoice() : kind(NONE) {}
};

/** Parse "" / s1… / ys1…; anything else (a Yellowback ye… address, a Sprout zc… address) is refused. */
AddressChoice ParseAddressChoice(const std::string& s, const char* what)
{
    AddressChoice c;
    c.text = s;
    if (s.empty()) return c;
    KeyIO keyIO(::Params());
    CTxDestination dest = keyIO.DecodeDestination(s);
    if (const CKeyID* id = std::get_if<CKeyID>(&dest)) {
        c.kind = AddressChoice::TRANSPARENT;
        c.keyId = *id;
        return c;
    }
    if (keyIO.IsValidPaymentAddressString(s)) {
        libzcash::PaymentAddress pa = keyIO.DecodePaymentAddress(s);
        if (const libzcash::SaplingPaymentAddress* sa = std::get_if<libzcash::SaplingPaymentAddress>(&pa)) {
            c.kind = AddressChoice::SAPLING;
            c.sapling = *sa;
            return c;
        }
        throw std::runtime_error(std::string("bad-address: ") + what + ": Sprout addresses are not supported; use an s1… or ys1… address");
    }
    throw std::runtime_error(std::string("bad-address: ") + what + ": not a transparent (s1…) or Sapling (ys1…) address of this network");
}

struct Context
{
    YellowbackWallet& yw;
    CWallet& wallet;
    YellowbackIndex& index;
    const Params& params;
    State st;
    int chainHeight;
    int indexHeight;
    int refHeight;                          //!< R for a MINT / TRANSFER: indexTip - REF_LAG (§3.5)
    int spendRefHeight;                     //!< R for a vault spend: the index tip (the snapshot yed_listclaimable reads; RED-1's window holds at H = tip + 1)
    uint32_t branchId;

    explicit Context(YellowbackWallet& yw_)
        : yw(yw_), wallet(*yw_.Wallet()), index(*yw_.Index()), params(yw_.Index()->GetParams()), st(yw_.Index()->View()),
          chainHeight(0), indexHeight(-1), refHeight(-1), spendRefHeight(-1), branchId(0)
    {
        AssertLockHeld(cs_main);
        AssertLockHeld(wallet.cs_wallet);
        AssertLockHeld(index.cs_yellowback);
        if (!index.IsHealthy()) throw std::runtime_error("yellowback-unhealthy: " + index.UnhealthyReason() + "; restart with -reindex-yellowback");
        chainHeight = chainActive.Height();
        std::optional<TipRecord> tip = st.GetTip();
        if (!tip.has_value()) throw std::runtime_error("index-below-start: the index has not reached the start height");
        indexHeight = tip->height;
        refHeight = indexHeight - g_yellowbackMintLag;
        spendRefHeight = indexHeight;
        if (refHeight < params.startHeight) throw std::runtime_error(strprintf("index-below-start: the index must reach height %d first", params.startHeight + g_yellowbackMintLag));
        branchId = SignerBranchId();
        if (wallet.IsLocked()) throw std::runtime_error("wallet-locked: walletpassphrase first");
    }

    /** nExpiryHeight = R + REF_WINDOW (V11; the MP-1 bound for a vault spend, N5). */
    uint32_t Expiry(int r) const { return (uint32_t)(r + REF_WINDOW); }

    void CheckExpiry(uint32_t expiry) const
    {
        if ((int64_t)expiry < (int64_t)chainHeight + 1 + (int64_t)TX_EXPIRING_SOON_THRESHOLD) {
            throw std::runtime_error("expiring-too-soon: the transaction would expire too soon; the index is too far behind the chain");
        }
    }

    CMutableTransaction NewTx(uint32_t expiry) const
    {
        CheckExpiry(expiry);
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(::Params().GetConsensus(), chainHeight + 1);
        mtx.nExpiryHeight = expiry;
        return mtx;
    }

    /** A TransactionBuilder for the Sapling shapes: same version/expiry/fee as NewTx, no keystore. */
    TransactionBuilder NewBuilder(uint32_t expiry) const
    {
        CheckExpiry(expiry);
        TransactionBuilder b(::Params().GetConsensus(), chainHeight + 1);
        b.SetExpiryHeight(expiry);
        b.SetFee(g_yellowbackFee);
        return b;
    }

    CPubKey FreshKey(const std::string& purpose) const
    {
        CPubKey key;
        if (!wallet.GetKeyFromPool(key)) throw std::runtime_error("keypool-empty: keypool ran out; keypoolrefill first");
        wallet.SetAddressBook(key.GetID(), "", purpose);
        return key;
    }

    /** The payee of §3.7 FEE-W (or the configured preference, the index's L6 values) for `selector` at `r`; nullopt under FEE-0. */
    std::optional<CKeyID> Payee(int r, const std::vector<unsigned char>& selector) const
    {
        return DefaultPayee(st.View(), params, r, selector, index.GetPayeePolicy());
    }

    /**
     * Smallest-first confirmed YEC inputs covering `needed`; returns the amount selected.
     * `only`: restrict to outputs paying that script (an s1… funding address).
     */
    CAmount SelectYec(CAmount needed, CMutableTransaction& mtx, std::vector<std::pair<CScript, CAmount>>& prevs,
                      const CScript* only = nullptr, const std::string& onlyText = "") const
    {
        if (needed <= 0) return 0;
        std::vector<COutput> coins;
        wallet.AvailableCoins(coins, true, nullptr, false, true, false, 1);
        std::sort(coins.begin(), coins.end(), [](const COutput& a, const COutput& b) { return a.Value() < b.Value(); });
        CAmount selected = 0;
        for (const COutput& c : coins) {
            if (!c.fSpendable) continue;
            if (only && c.tx->vout[c.i].scriptPubKey != *only) continue;
            // Never spend a YED-bearing output or a vault as plain YEC (belt and braces: locked coins are already excluded).
            COutPoint o(c.tx->GetHash(), c.i);
            if (st.GetToken(o).has_value() || st.GetVault(o).has_value()) continue;
            mtx.vin.push_back(CTxIn(o));
            prevs.push_back(std::make_pair(c.tx->vout[c.i].scriptPubKey, c.Value()));
            selected += c.Value();
            if (selected >= needed) break;
        }
        if (selected < needed) {
            throw std::runtime_error(strprintf("insufficient-yec: need %s, have %s confirmed and unlocked%s",
                                               FormatMoney(needed), FormatMoney(selected), only ? " at " + onlyText : ""));
        }
        return selected;
    }

    /**
     * Sapling notes of `addr` (§4.6), largest-first as z_sendmany, covering `needed`. Fills the
     * spending key, the anchor and one witness per note; requires cs_main and cs_wallet.
     */
    void SelectSapling(const AddressChoice& addr, CAmount needed, libzcash::SaplingExtendedSpendingKey& extsk,
                       std::vector<SaplingNoteEntry>& sel, std::vector<SaplingWitness>& witnesses, uint256& anchor) const
    {
        libzcash::PaymentAddress pa = addr.sapling;
        if (!std::visit(HaveSpendingKeyForPaymentAddress(&wallet), pa)) {
            throw std::runtime_error("bad-address: no spending key for " + addr.text + " in this wallet");
        }
        std::optional<libzcash::SpendingKey> sk = std::visit(GetSpendingKeyForPaymentAddress(&wallet), pa);
        if (!sk.has_value()) throw std::runtime_error("bad-address: no spending key for " + addr.text + " in this wallet");
        extsk = std::get<libzcash::SaplingExtendedSpendingKey>(sk.value());

        std::vector<SproutNoteEntry> sprout;
        std::vector<SaplingNoteEntry> notes;
        std::set<libzcash::PaymentAddress> filter;
        filter.insert(pa);
        wallet.GetFilteredNotes(sprout, notes, filter, 1, INT_MAX, true, true, true);
        std::sort(notes.begin(), notes.end(), [](const SaplingNoteEntry& a, const SaplingNoteEntry& b) { return a.note.value() > b.note.value(); });
        CAmount sum = 0;
        CAmount available = 0;
        for (const SaplingNoteEntry& e : notes) available += e.note.value();
        for (const SaplingNoteEntry& e : notes) {
            if (sum >= needed) break;
            sel.push_back(e);
            sum += e.note.value();
        }
        if (sum < needed) {
            throw std::runtime_error(strprintf("insufficient-yec: need %s, have %s in confirmed Sapling notes at %s", FormatMoney(needed), FormatMoney(available), addr.text));
        }
        if (sel.size() > MAX_SAPLING_SPENDS) {
            throw std::runtime_error(strprintf("too-many-notes: the mint would spend %u Sapling notes (limit %u); consolidate with z_mergetoaddress first", (unsigned)sel.size(), (unsigned)MAX_SAPLING_SPENDS));
        }
        std::vector<SaplingOutPoint> ops;
        for (const SaplingNoteEntry& e : sel) ops.push_back(e.op);
        std::vector<std::optional<SaplingWitness>> maybe;
        wallet.GetSaplingNoteWitnesses(ops, maybe, anchor);
        for (size_t i = 0; i < maybe.size(); i++) {
            if (!maybe[i].has_value()) throw std::runtime_error("missing witness for a Sapling note; retry after the next block");
            witnesses.push_back(maybe[i].value());
        }
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

    /** The vault `vaultTxid:0`, or `vault-not-found`. */
    VaultRecord GetVault(const COutPoint& vaultOut) const
    {
        std::optional<VaultRecord> vault = st.GetVault(vaultOut);
        if (!vault.has_value()) throw std::runtime_error("vault-not-found: no vault at " + vaultOut.ToString());
        return vault.value();
    }

    /** The keypool-low nag of §4.6 (the wallet.dat backup rule). */
    std::string KeypoolWarning() const
    {
        const unsigned int n = wallet.GetKeyPoolSize();
        if (n >= 10) return "";
        return strprintf("keypool low (%u keys left): keypoolrefill and back up wallet.dat — the vault owner key lives only there", n);
    }
};

/** Select YED coins smallest-first for `needed` cents; applies the change floor (§4.6). */
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
    if (sum < needed) throw std::runtime_error(strprintf("insufficient-yed: need %d cents, have %d confirmed and spendable", needed, sum));
    change = sum - needed;
    if (change > 0 && change < ctx.params.minOutput) {
        throw std::runtime_error(strprintf("change-floor: change of %d cents is below the minimum output of %d cents; send %d cents (all selected inputs) or at most %d cents",
                                           change, ctx.params.minOutput, sum, sum - ctx.params.minOutput));
    }
    if (sel.size() > MAX_YED_INPUTS) throw std::runtime_error("too-many-inputs: too many YED inputs; consolidate first");
    return sel;
}

/**
 * The shared body of BuildRedeem (ACTIVE and VOID), BuildClaim and BuildSweep: a vault spend of
 * `vault` with `burn` = the debt (REDEEM/CLAIM) or none (release/sweep), to `to`.
 */
BuiltTx BuildVaultSpend(Context& ctx, BuiltKind kind, const COutPoint& vaultOut, const VaultRecord& vault, const std::string& to)
{
    const AddressChoice dest = ParseAddressChoice(to, "to");
    const bool ownerPath = kind != BuiltKind::CLAIM;
    const bool withPayload = kind == BuiltKind::REDEEM || kind == BuiltKind::CLAIM;

    const CPubKey owner = vault.OwnerKey();
    VaultSpendShape shape;
    shape.vaultOut = vaultOut;
    shape.vaultScript = VaultScript((uint32_t)vault.lockHeight, owner, (uint32_t)vault.claimHeight);
    if (shape.vaultScript.empty()) throw std::runtime_error("vault-not-found: cannot reconstruct the vault script");
    shape.vaultValue = vault.collateralZat;
    shape.lockHeight = (uint32_t)vault.lockHeight;
    shape.claimHeight = (uint32_t)vault.claimHeight;
    shape.ownerPath = ownerPath;
    shape.withPayload = withPayload;
    shape.refHeight = ctx.spendRefHeight;
    shape.networkFee = g_yellowbackFee;

    BuiltTx out;
    out.kind = kind;
    out.refHeight = ctx.spendRefHeight;
    out.termClass = vault.termClass;
    out.lockHeight = shape.lockHeight;
    out.claimHeight = shape.claimHeight;
    out.vaultScript = shape.vaultScript;
    out.vaultValue = vault.collateralZat;
    out.ownerPubKey = owner;
    out.path = ownerPath ? "owner" : "claim";

    if (withPayload) {
        // RED-2: the burn is exactly the debt (V20); RED-3: the fee from the collateral.
        int64_t change = 0;
        std::vector<YedCoin> sel = SelectYed(ctx, vault.mintedCents, change);
        shape.yedInputs = sel;
        shape.changeCents = change;
        if (change > 0) {
            CPubKey changeKey = ctx.FreshKey("yellowback-change");
            out.freshKey = changeKey;
            shape.changeScript = GetScriptForDestination(changeKey.GetID());
        }
        shape.payee = ctx.Payee(ctx.spendRefHeight, OutPointSelector(vaultOut));
        shape.feeZat = shape.payee.has_value() ? FeeZat(vault.collateralZat, ctx.params.feeMin, ctx.params.feeBps) : 0;
        for (const YedCoin& c : sel) {
            out.yedInputs.insert(c.outpoint);
            out.yedPrevs.push_back(std::make_pair(c.token.scriptPubKey, c.token.nValue));
        }
        out.changeCents = change;
        out.payee = shape.payee;
        out.feeZat = shape.feeZat;
    }

    const uint32_t expiry = ctx.Expiry(ctx.spendRefHeight);
    if (dest.kind == AddressChoice::SAPLING) {
        // Sapling shape (§4.6): the collateral is one Sapling note; vault and YED inputs go in
        // unsigned and are signed by SignVaultSpend() after FinishSapling().
        shape.collateralScript = std::nullopt;
        VaultSpendPlan plan = PlanVaultSpend(shape);
        TransactionBuilder b = ctx.NewBuilder(expiry);
        b.SetLockTime(plan.nLockTime);
        b.AddTransparentInputUnsigned(vaultOut, vault.collateralZat, 0xFFFFFFFE);
        for (const YedCoin& c : shape.yedInputs) b.AddTransparentInputUnsigned(c.outpoint, c.token.nValue);
        for (const CTxOut& o : plan.vout) b.AddTransparentOutput(o.scriptPubKey, o.nValue);
        // Encrypt the note under the seed-derived key z_sendmany uses for t->z, so it is recoverable
        // from the seed whether or not the destination belongs to this wallet.
        HDSeed seed = ctx.wallet.GetHDSeedForRPC();
        b.AddSaplingOutput(ovkForShieldingFromTaddr(seed), dest.sapling, plan.collateralOut);
        out.builder = b;
        out.collateralTo = dest.text;
        out.collateralOut = plan.collateralOut;
        out.burnCents = plan.burnCents;
        out.feeVout = plan.feeVout;
        out.changeVout = plan.changeVout;
        return out;
    }

    if (dest.kind == AddressChoice::TRANSPARENT) {
        shape.collateralScript = GetScriptForDestination(dest.keyId);
        out.collateralTo = dest.text;
    } else {
        CPubKey fresh = ctx.FreshKey("yellowback-collateral");
        if (!out.freshKey.IsValid()) out.freshKey = fresh;
        shape.collateralScript = GetScriptForDestination(fresh.GetID());
        out.collateralTo = KeyIO(::Params()).EncodeDestination(CTxDestination(fresh.GetID()));
    }
    VaultSpendPlan plan = PlanVaultSpend(shape);
    CMutableTransaction mtx = ctx.NewTx(expiry);
    mtx.nLockTime = plan.nLockTime;
    mtx.vin = plan.vin;
    mtx.vout = plan.vout;
    out.tx = mtx;
    out.collateralOut = plan.collateralOut;
    out.burnCents = plan.burnCents;
    out.feeVout = plan.feeVout;
    out.changeVout = plan.changeVout;
    return out;
}

} // namespace

BuiltTx BuildMint(YellowbackWallet& yw, Cents cents, int lockBlocks, CReserveKey& reservekey, const std::string& from)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    const AddressChoice source = ParseAddressChoice(from, "from");
    if (cents < p.minMint || cents > p.maxMint) throw std::runtime_error(strprintf("bad-mint-amount: cents must be between %d and %d", p.minMint, p.maxMint));
    const int termClass = p.ClassForLockBlocks(lockBlocks);
    if (termClass < 0) throw std::runtime_error(strprintf("mint-bad-lock: %d blocks is outside every term class (A %d-%d, B %d-%d, C %d-%d)", lockBlocks,
                                                          p.classMin[0], p.classMax[0], p.classMin[1], p.classMax[1], p.classMin[2], p.classMax[2]));
    const int64_t lockHeight = (int64_t)ctx.refHeight + lockBlocks;
    const int64_t claimHeight = lockHeight + p.grace;
    if (claimHeight >= (int64_t)LOCKTIME_THRESHOLD) throw std::runtime_error("mint-bad-lock: lockHeight + GRACE reaches LOCKTIME_THRESHOLD");

    // MINTPOL-1 (§4.6): Snapshots[R].activation == ACTIVE, haltMask == 0, the cap has room.
    std::optional<Snapshot> S = SnapshotAt(ctx.st, p, ctx.refHeight);
    if (!S.has_value() || !S->activation.IsActive() || (S->haltMask & HALT_NOT_ACTIVE)) throw std::runtime_error("mintpol-not-active: Yellowback is not active at the reference height");
    if (S->haltMask & HALT_NO_PRICE) throw std::runtime_error("mintpol-no-price: no defined price at the reference height (PRICE-1 fill)");
    if (S->haltMask & (HALT_PARTICIPATION | HALT_ENFORCEMENT)) throw std::runtime_error("mintpol-participation: minting is halted while miner participation is low (ACT-4)");
    if (S->haltMask & HALT_GLOBAL_RATIO) throw std::runtime_error("mintpol-global-ratio: minting is halted while the global collateral ratio is low (HALT-2)");
    if (S->haltMask & HALT_DIVERGENCE) throw std::runtime_error("mintpol-divergence: minting is halted while the price windows diverge (HALT-3)");
    if (S->haltMask != 0) throw std::runtime_error("mintpol-not-active: an unknown halt bit is set at the reference height");
    std::optional<MicroUsd> pMint = S->PMint();
    if (!pMint.has_value()) throw std::runtime_error("mintpol-no-price: pMint is undefined at the reference height");
    const Totals totals = ctx.st.GetTotals();
    std::optional<Cents> cap = SupplyCapCents(S->issuedZat, pMint, p.supplyCapBps);
    if (cap.has_value() && totals.supplyCents + cents > cap.value()) {
        throw std::runtime_error(strprintf("mintpol-cap: supply cap headroom is %d cents", std::max<Cents>(0, cap.value() - totals.supplyCents)));
    }
    std::optional<CAmount> required = RequiredCollateralRounded(cents, MinRatioBps(p.baseRatioBps[termClass], S->sigmaMultBps), pMint.value());
    if (!required.has_value()) throw std::runtime_error("mint-unsatisfiable: the collateral requirement exceeds MAX_MONEY (K14)");
    CAmount collateral = std::max(required.value(), 4 * p.feeMin);   // MINT-5, K14
    if (collateral % 1000 != 0) collateral += 1000 - collateral % 1000;

    CPubKey owner = ctx.FreshKey("yellowback-vault");
    MintShape shape;
    shape.cents = cents;
    shape.termClass = termClass;
    shape.lockHeight = (uint32_t)lockHeight;
    shape.claimHeight = (uint32_t)claimHeight;
    shape.refHeight = ctx.refHeight;
    shape.owner = owner;
    shape.collateralZat = collateral;
    shape.payee = ctx.Payee(ctx.refHeight, std::vector<unsigned char>(owner.begin(), owner.end()));
    shape.feeZat = shape.payee.has_value() ? FeeZat(collateral, p.feeMin, p.feeBps) : 0;
    int feeVout = -1;
    std::vector<CTxOut> vout = MintOutputs(shape, feeVout);
    const CAmount needed = collateral + TOKEN_VALUE + shape.feeZat + g_yellowbackFee;

    BuiltTx out;
    out.kind = BuiltKind::MINT;
    out.freshKey = owner;
    out.refHeight = ctx.refHeight;
    out.termClass = termClass;
    out.lockHeight = shape.lockHeight;
    out.claimHeight = shape.claimHeight;
    out.collateralZat = collateral;
    out.feeZat = shape.feeZat;
    out.payee = shape.payee;
    out.feeVout = feeVout;
    out.ownerPubKey = owner;
    out.warning = ctx.KeypoolWarning();
    const uint32_t expiry = ctx.Expiry(ctx.refHeight);

    if (source.kind == AddressChoice::SAPLING) {
        // Sapling shape (§4.6): notes of `from` fund the outputs in the same transaction; change is
        // a Sapling output back to `from`. Proved later by FinishSapling() with no lock held.
        libzcash::SaplingExtendedSpendingKey extsk;
        std::vector<SaplingNoteEntry> notes;
        std::vector<SaplingWitness> witnesses;
        uint256 anchor;
        ctx.SelectSapling(source, needed, extsk, notes, witnesses, anchor);
        TransactionBuilder b = ctx.NewBuilder(expiry);
        for (size_t i = 0; i < notes.size(); i++) b.AddSaplingSpend(extsk.expsk, notes[i].note, anchor, witnesses[i]);
        for (const CTxOut& o : vout) b.AddTransparentOutput(o.scriptPubKey, o.nValue);
        b.SendChangeTo(source.sapling, extsk.expsk.full_viewing_key().ovk);
        out.builder = b;
        out.fundedFrom = "sapling";
        return out;
    }

    CMutableTransaction mtx = ctx.NewTx(expiry);
    mtx.vout = vout;
    std::vector<std::pair<CScript, CAmount>> prevs;
    CScript onlyScript;
    if (source.kind == AddressChoice::TRANSPARENT) onlyScript = GetScriptForDestination(source.keyId);
    CAmount selected = ctx.SelectYec(needed, mtx, prevs, onlyScript.empty() ? nullptr : &onlyScript, source.text);
    CAmount change = selected - needed;
    if (change > 0) {
        CPubKey changeKey;
        if (!reservekey.GetReservedKey(changeKey)) throw std::runtime_error("keypool-empty: keypool ran out");
        mtx.vout.push_back(CTxOut(change, GetScriptForDestination(changeKey.GetID())));
    }
    ctx.SignInputs(mtx, prevs, 0);

    out.tx = mtx;
    out.fundedFrom = "transparent";
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

    CMutableTransaction mtx = ctx.NewTx(ctx.Expiry(ctx.refHeight));
    BuiltTx out;
    out.kind = BuiltKind::TRANSFER;
    out.refHeight = ctx.refHeight;
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
        if (!reservekey.GetReservedKey(changeKey)) throw std::runtime_error("keypool-empty: keypool ran out");
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
    if (change > 0) {
        out.changeVout = (int)recipients.size();
        out.ownYedOutputs.push_back(COutPoint(txid, recipients.size()));
    }
    out.changeCents = change;
    return out;
}

BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to)
{
    Context ctx(yw);
    const COutPoint vaultOut(vaultTxid, 0);
    const VaultRecord vault = ctx.GetVault(vaultOut);
    if (!vault.IsOpen()) throw std::runtime_error(strprintf("vault-not-active: the vault is %s", VaultStatusName(vault.Status())));
    if (!yw.IsMineVault(vault)) throw std::runtime_error("vault-not-owned: the vault owner key is not in this wallet");
    if ((int64_t)ctx.indexHeight < (int64_t)vault.lockHeight) {
        throw std::runtime_error(strprintf("vault-locked: the vault is locked until height %d (tip %d)", vault.lockHeight, ctx.indexHeight));
    }
    // ACTIVE: the owner-path REDEEM; VOID: the release (L14, K3): an ordinary spend no rule polices.
    const BuiltKind kind = vault.Status() == VaultStatus::ACTIVE ? BuiltKind::REDEEM : BuiltKind::RELEASE;
    return BuildVaultSpend(ctx, kind, vaultOut, vault, to);
}

BuiltTx BuildClaim(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    const COutPoint vaultOut(vaultTxid, 0);
    const VaultRecord vault = ctx.GetVault(vaultOut);
    if (vault.Status() != VaultStatus::ACTIVE) throw std::runtime_error(strprintf("vault-not-active: the vault is %s", VaultStatusName(vault.Status())));
    if ((int64_t)ctx.indexHeight < (int64_t)vault.claimHeight) {
        throw std::runtime_error(strprintf("claim-not-yet: the claim path opens at height %d (tip %d)", vault.claimHeight, ctx.indexHeight));
    }
    // RED-4 at Snapshots[R]: the vault must be underwater there (an undefined pClaim is "not underwater", M1).
    std::optional<Snapshot> S = SnapshotAt(ctx.st, p, ctx.spendRefHeight);
    std::optional<MicroUsd> pClaim = S.has_value() ? S->PClaim() : std::nullopt;
    if (!IsUnderwater(vault.collateralZat, pClaim, vault.mintedCents, p.claimThresholdBps)) {
        throw std::runtime_error(strprintf("claim-not-underwater: the vault is not underwater at the reference height %d (pClaim %s)",
                                           ctx.spendRefHeight, pClaim.has_value() ? std::to_string(pClaim.value()) : std::string("undefined")));
    }
    return BuildVaultSpend(ctx, BuiltKind::CLAIM, vaultOut, vault, to);
}

BuiltTx BuildSweep(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to)
{
    Context ctx(yw);
    const COutPoint vaultOut(vaultTxid, 0);
    const VaultRecord vault = ctx.GetVault(vaultOut);
    if (vault.Status() != VaultStatus::ACTIVE) throw std::runtime_error(strprintf("vault-not-active: the vault is %s", VaultStatusName(vault.Status())));
    if (!yw.IsMineVault(vault)) throw std::runtime_error("vault-not-owned: the vault owner key is not in this wallet");
    if ((int64_t)ctx.indexHeight < (int64_t)vault.lockHeight) {
        throw std::runtime_error(strprintf("vault-locked: the vault is locked until height %d (tip %d)", vault.lockHeight, ctx.indexHeight));
    }
    return BuildVaultSpend(ctx, BuiltKind::SWEEP, vaultOut, vault, to);
}

void FinishSapling(BuiltTx& out)
{
    if (!out.builder.has_value()) return;
    TransactionBuilderResult r = out.builder->Build();
    if (r.IsError()) throw std::runtime_error("Sapling build failed: " + r.GetError());
    out.tx = CMutableTransaction(r.GetTxOrThrow());
    out.builder.reset();
    if (out.kind == BuiltKind::MINT) out.ownYedOutputs.push_back(COutPoint(CTransaction(out.tx).GetHash(), 1));
}

} // namespace yellowback
