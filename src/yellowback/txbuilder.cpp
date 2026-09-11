// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/txbuilder.h"

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "key_io.h"
#include "main.h"
#include "script/sign.h"
#include "script/standard.h"
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

namespace {

/** At most this many Sapling notes in one mint (one spend proof each, seconds apiece). */
const size_t MAX_SAPLING_SPENDS = 20;

/** A funding source or collateral destination given as an address string (I2). */
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
        throw std::runtime_error(std::string(what) + ": Sprout addresses are not supported; use an s1… or ys1… address");
    }
    throw std::runtime_error(std::string(what) + ": not a transparent (s1…) or Sapling (ys1…) address of this network");
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

    void CheckExpiry(uint32_t expiry) const
    {
        if ((int64_t)expiry < (int64_t)chainHeight + 1 + (int64_t)TX_EXPIRING_SOON_THRESHOLD) {
            throw std::runtime_error("transaction would expire too soon; the index is too far behind the chain");
        }
    }

    CMutableTransaction NewTx(uint32_t expiry) const
    {
        CheckExpiry(expiry);
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(::Params().GetConsensus(), chainHeight + 1);
        mtx.nExpiryHeight = expiry;
        return mtx;
    }

    /** A TransactionBuilder for the Sapling shapes (I2): same version/expiry/fee as NewTx, no keystore. */
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
        if (!wallet.GetKeyFromPool(key)) throw std::runtime_error("keypool ran out; unlock the wallet");
        wallet.SetAddressBook(key.GetID(), "", purpose);
        return key;
    }

    /**
     * Smallest-first confirmed YEC inputs (F3) covering `needed`; returns the amount selected.
     * `only` (I2): restrict to outputs paying that script (an s1… funding address).
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
            // Never spend a YED-bearing output as plain YEC (belt and braces: locked coins are already excluded).
            COutPoint o(c.tx->GetHash(), c.i);
            if (st.GetToken(o).has_value()) continue;
            mtx.vin.push_back(CTxIn(o));
            prevs.push_back(std::make_pair(c.tx->vout[c.i].scriptPubKey, c.Value()));
            selected += c.Value();
            if (selected >= needed) break;
        }
        if (selected < needed) {
            throw std::runtime_error(strprintf("insufficient YEC%s: need %s, have %s confirmed and unlocked",
                                               only ? " at " + onlyText : "", FormatMoney(needed), FormatMoney(selected)));
        }
        return selected;
    }

    /**
     * Sapling notes of `addr` (I2), largest-first as z_sendmany, covering `needed`. Fills the
     * spending key, the anchor and one witness per note; requires cs_main and cs_wallet.
     */
    void SelectSapling(const AddressChoice& addr, CAmount needed, libzcash::SaplingExtendedSpendingKey& extsk,
                       std::vector<SaplingNoteEntry>& sel, std::vector<SaplingWitness>& witnesses, uint256& anchor) const
    {
        libzcash::PaymentAddress pa = addr.sapling;
        if (!std::visit(HaveSpendingKeyForPaymentAddress(&wallet), pa)) {
            throw std::runtime_error("no spending key for " + addr.text + " in this wallet");
        }
        std::optional<libzcash::SpendingKey> sk = std::visit(GetSpendingKeyForPaymentAddress(&wallet), pa);
        if (!sk.has_value()) throw std::runtime_error("no spending key for " + addr.text + " in this wallet");
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
            throw std::runtime_error(strprintf("insufficient YEC at %s: need %s, have %s in confirmed Sapling notes", addr.text, FormatMoney(needed), FormatMoney(available)));
        }
        if (sel.size() > MAX_SAPLING_SPENDS) {
            throw std::runtime_error(strprintf("the mint would spend %u Sapling notes (limit %u); consolidate with z_mergetoaddress first", (unsigned)sel.size(), (unsigned)MAX_SAPLING_SPENDS));
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

BuiltTx BuildMint(YellowbackWallet& yw, int64_t cents, int lockBlocks, CReserveKey& reservekey, const std::string& from)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    if (cents < p.minMint || cents > p.maxMint) {
        throw std::runtime_error(strprintf("bad-mint-amount: cents must be between %d and %d", p.minMint, p.maxMint));
    }
    const int termClass = p.ClassForLockBlocks(lockBlocks);
    if (termClass < 0) throw std::runtime_error(strprintf("mint-bad-lock: lockBlocks %d is in no term class", lockBlocks));
    const int64_t refHeight = (int64_t)ctx.indexHeight - g_yellowbackMintLag;
    const int64_t lockHeight = refHeight + lockBlocks;
    if (lockHeight <= refHeight || lockHeight + p.grace >= LOCKTIME_THRESHOLD) {
        throw std::runtime_error("mint-bad-lock: lockHeight + GRACE exceeds LOCKTIME_THRESHOLD");
    }
    std::optional<Snapshot> snapshot = SnapshotAt(ctx.st, p, refHeight);
    if (!snapshot.has_value() || !snapshot->activation.IsActive() || snapshot->haltMask != 0) {
        throw std::runtime_error("mintpol-not-active: minting is not allowed at the reference snapshot");
    }
    std::optional<MicroUsd> pMint = snapshot->PMint();
    if (!pMint.has_value()) throw std::runtime_error("mintpol-no-price: no mint price at the reference snapshot");
    std::optional<CAmount> required = RequiredCollateralRounded((Cents)cents,
        MinRatioBps(p.baseRatioBps[termClass], snapshot->sigmaMultBps), pMint.value());
    if (!required.has_value()) throw std::runtime_error("mint-unsatisfiable: the required collateral exceeds MAX_MONEY");

    AddressChoice funding = ParseAddressChoice(from, "from");
    if (funding.kind == AddressChoice::SAPLING) throw std::runtime_error("from: Sapling funding is not supported by yed_mint");
    const CPubKey owner = ctx.FreshKey("yellowback-vault");
    std::vector<unsigned char> selector(owner.begin(), owner.end());
    std::optional<CKeyID> payee = DefaultPayee(ctx.index.View(), p, (int)refHeight, selector, ctx.index.GetPayeePolicy());
    const CAmount fee = payee.has_value() ? FeeZat(required.value(), p.feeMin, p.feeBps) : 0;

    CMutableTransaction mtx = ctx.NewTx((uint32_t)(refHeight + p.refWindow));
    const CScript vaultScript = VaultScript((uint32_t)lockHeight, owner, (uint32_t)(lockHeight + p.grace));
    if (vaultScript.empty()) throw std::runtime_error("mint-bad-lock: cannot construct vault script");
    mtx.vout.push_back(CTxOut(required.value(), P2SHScript(vaultScript)));
    mtx.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(owner.GetID())));
    const uint8_t feeVout = payee.has_value() ? 3 : FEE_VOUT_NONE;
    std::vector<unsigned char> payload = EncodePayload(Payload::Mint((uint8_t)termClass, (uint32_t)cents,
        (uint32_t)lockHeight, (uint32_t)refHeight, owner, feeVout));
    if (payload.empty()) throw std::runtime_error("cannot encode the mint payload");
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));
    if (payee.has_value()) mtx.vout.push_back(CTxOut(fee, GetScriptForDestination(payee.value())));

    std::vector<std::pair<CScript, CAmount>> prevs;
    const CAmount needed = required.value() + TOKEN_VALUE + fee + g_yellowbackFee;
    const CScript onlyScript = funding.kind == AddressChoice::TRANSPARENT ? GetScriptForDestination(funding.keyId) : CScript();
    CAmount selected = ctx.SelectYec(needed, mtx, prevs, funding.kind == AddressChoice::TRANSPARENT ? &onlyScript : nullptr, funding.text);
    const CAmount change = selected - needed;
    if (change > 0) {
        CPubKey changeKey;
        if (!reservekey.GetReservedKey(changeKey)) throw std::runtime_error("keypool ran out");
        mtx.vout.push_back(CTxOut(change, GetScriptForDestination(changeKey.GetID())));
    }
    ctx.SignInputs(mtx, prevs, 0);

    BuiltTx out;
    out.tx = mtx;
    out.ownYedOutputs.push_back(COutPoint(CTransaction(out.tx).GetHash(), 1));
    out.freshKey = owner;
    out.fundedFrom = "transparent";
    out.evalHeight = refHeight;
    out.lockHeight = (uint32_t)lockHeight;
    out.collateralZat = required.value();
    out.termClass = termClass;
    out.feeZat = fee;
    out.payee = payee;
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

    CMutableTransaction mtx = ctx.NewTx((uint32_t)ctx.indexHeight + REF_WINDOW);
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

BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to)
{
    Context ctx(yw);
    const COutPoint vaultOut(vaultTxid, 0);
    std::optional<VaultRecord> vault = ctx.st.GetVault(vaultOut);
    if (!vault.has_value()) throw std::runtime_error("vault-not-found: no vault at " + vaultTxid.GetHex() + ":0");
    if (vault->Status() != VaultStatus::ACTIVE) throw std::runtime_error("vault-not-active: vault is not active");
    if (!yw.IsMineVault(vault.value())) throw std::runtime_error("vault-not-owned: this wallet does not hold the vault owner key");
    if (ctx.indexHeight < vault->lockHeight) throw std::runtime_error(strprintf("vault-locked: vault is locked until height %d", vault->lockHeight));

    AddressChoice destination = ParseAddressChoice(to, "to");
    if (destination.kind == AddressChoice::SAPLING) throw std::runtime_error("to: Sapling destinations are not supported by yed_redeem");
    CScript collateralScript;
    std::string collateralTo = to;
    if (destination.kind == AddressChoice::TRANSPARENT) collateralScript = GetScriptForDestination(destination.keyId);
    else {
        CPubKey key = ctx.FreshKey("yellowback-redeem");
        collateralScript = GetScriptForDestination(key.GetID());
        collateralTo = KeyIO(::Params()).EncodeDestination(key.GetID());
    }

    int64_t changeCents = 0;
    std::vector<YedCoin> yed = SelectYed(ctx, vault->mintedCents, changeCents);
    const int64_t refHeight = ctx.indexHeight;
    std::vector<unsigned char> selector(vaultTxid.begin(), vaultTxid.end());
    selector.insert(selector.end(), 4, 0);
    std::optional<CKeyID> payee = DefaultPayee(ctx.index.View(), ctx.params, (int)refHeight, selector, ctx.index.GetPayeePolicy());
    const CAmount fee = payee.has_value() ? FeeZat(vault->collateralZat, ctx.params.feeMin, ctx.params.feeBps) : 0;
    const CAmount yedChangeValue = changeCents > 0 ? TOKEN_VALUE : 0;
    const CAmount collateralOut = vault->collateralZat + (CAmount)yed.size() * TOKEN_VALUE - g_yellowbackFee - fee - yedChangeValue;
    if (collateralOut <= 0) throw std::runtime_error("redeem collateral is insufficient to pay fees");

    CMutableTransaction mtx = ctx.NewTx((uint32_t)(refHeight + ctx.params.refWindow));
    mtx.nLockTime = vault->lockHeight;
    mtx.vin.push_back(CTxIn(vaultOut, CScript(), 0xfffffffe));
    for (const YedCoin& c : yed) mtx.vin.push_back(CTxIn(c.outpoint));
    mtx.vout.push_back(CTxOut(collateralOut, collateralScript));
    const uint8_t feeVout = payee.has_value() ? 1 : FEE_VOUT_NONE;
    if (payee.has_value()) mtx.vout.push_back(CTxOut(fee, GetScriptForDestination(payee.value())));
    int changeVout = -1;
    if (changeCents > 0) {
        CPubKey changeKey = ctx.FreshKey("yellowback-change");
        changeVout = mtx.vout.size();
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(changeKey.GetID())));
    }
    std::vector<unsigned char> payload = EncodePayload(Payload::Redeem((uint32_t)refHeight, feeVout, {}));
    if (payload.empty()) throw std::runtime_error("cannot encode the redemption payload");
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));

    BuiltTx out;
    out.tx = mtx;
    for (const YedCoin& c : yed) {
        out.yedInputs.insert(c.outpoint);
        out.yedPrevs.push_back(std::make_pair(c.token.scriptPubKey, c.token.nValue));
    }
    out.requiredBurn = vault->mintedCents;
    out.burnCents = vault->mintedCents;
    out.changeCents = changeCents;
    out.changeVout = changeVout;
    out.vaultScript = VaultScript(vault->lockHeight, vault->OwnerKey(), vault->claimHeight);
    out.vaultValue = vault->collateralZat;
    out.ownerPubKey = vault->OwnerKey();
    out.feeZat = fee;
    out.payee = payee;
    out.collateralOut = collateralOut;
    out.collateralTo = collateralTo;
    return out;
}

void FinishSapling(BuiltTx& out)
{
    if (!out.builder.has_value()) return;
    TransactionBuilderResult r = out.builder->Build();
    if (r.IsError()) throw std::runtime_error("Sapling build failed: " + r.GetError());
    out.tx = CMutableTransaction(r.GetTxOrThrow());
    out.builder.reset();
    if (out.isMint) out.ownYedOutputs.push_back(COutPoint(CTransaction(out.tx).GetHash(), 1));
}

void SignRedeem(BuiltTx& out, CWallet& wallet, uint32_t branchId)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(wallet.cs_wallet);
    if (out.builder.has_value()) throw std::runtime_error("FinishSapling must run before SignRedeem");
    if (out.tx.vin.size() != out.yedPrevs.size() + 1) throw std::runtime_error("redeem input count mismatch");
    // Owner signature over the vault script (manual sighash, as rpc/atomicswap.cpp does).
    CKey ownerKey;
    if (!wallet.GetKey(out.ownerPubKey.GetID(), ownerKey)) throw std::runtime_error("owner key not available");
    uint256 hash = SignatureHash(out.vaultScript, CTransaction(out.tx), 0, SIGHASH_ALL, out.vaultValue, branchId);
    valtype ownerSig;
    if (!ownerKey.Sign(hash, ownerSig)) throw std::runtime_error("owner signature failed");
    ownerSig.push_back((unsigned char)SIGHASH_ALL);
    out.tx.vin[0].scriptSig = OwnerScriptSig(ownerSig, out.vaultScript);
    for (size_t i = 0; i < out.yedPrevs.size(); i++) {
        if (!SignSignature(wallet, out.yedPrevs[i].first, out.tx, i + 1, out.yedPrevs[i].second, SIGHASH_ALL, branchId)) {
            throw std::runtime_error(strprintf("failed to sign YED input %u", (unsigned)i));
        }
    }
    out.ownYedOutputs.clear();
    if (out.changeVout >= 0) out.ownYedOutputs.push_back(COutPoint(CTransaction(out.tx).GetHash(), out.changeVout));
}

} // namespace yellowback
