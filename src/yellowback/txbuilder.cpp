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
            // Never spend a YED-bearing or reserved output as plain YEC (belt and braces: locked coins are already excluded).
            COutPoint o(c.tx->GetHash(), c.i);
            if (st.GetToken(o).has_value() || yw.IsReserved(o)) continue;
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

BuiltTx BuildMint(YellowbackWallet& yw, int64_t cents, int tier, CReserveKey& reservekey, const std::string& from)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    const AddressChoice source = ParseAddressChoice(from, "from");
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

    CPubKey owner = ctx.FreshKey("yellowback-vault");
    CScript vault = VaultScript(lockHeight, owner, rosters.back().script);
    if (vault.empty()) throw std::runtime_error("cannot build the vault script");
    const CScript tokenScript = GetScriptForDestination(owner.GetID());
    std::vector<unsigned char> payload = EncodePayload(Payload::Mint((uint8_t)tier, (uint32_t)cents, lockHeight, (uint32_t)evalHeight, owner));
    if (payload.empty()) throw std::runtime_error("cannot encode the mint payload");
    const CAmount needed = required.value() + TOKEN_VALUE + g_yellowbackFee;

    out.isMint = true;
    out.freshKey = owner;
    out.evalHeight = evalHeight;
    out.lockHeight = lockHeight;
    out.collateralZat = required.value();

    if (source.kind == AddressChoice::SAPLING) {
        // Sapling shape (I2): notes of `from` fund vout[0..2] in the same transaction; change is a
        // Sapling output back to `from`. Proved later by FinishSapling() with no lock held.
        libzcash::SaplingExtendedSpendingKey extsk;
        std::vector<SaplingNoteEntry> notes;
        std::vector<SaplingWitness> witnesses;
        uint256 anchor;
        ctx.SelectSapling(source, needed, extsk, notes, witnesses, anchor);
        TransactionBuilder b = ctx.NewBuilder(expiry);
        for (size_t i = 0; i < notes.size(); i++) b.AddSaplingSpend(extsk.expsk, notes[i].note, anchor, witnesses[i]);
        b.AddTransparentOutput(P2SHScript(vault), required.value());
        b.AddTransparentOutput(tokenScript, TOKEN_VALUE);
        b.AddTransparentOutput(PayloadScript(payload), 0);
        b.SendChangeTo(source.sapling, extsk.expsk.full_viewing_key().ovk);
        out.builder = b;
        out.fundedFrom = "sapling";
        return out;
    }

    CMutableTransaction mtx = ctx.NewTx(expiry);
    mtx.vout.push_back(CTxOut(required.value(), P2SHScript(vault)));
    mtx.vout.push_back(CTxOut(TOKEN_VALUE, tokenScript));
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));

    std::vector<std::pair<CScript, CAmount>> prevs;
    CScript onlyScript;
    if (source.kind == AddressChoice::TRANSPARENT) onlyScript = GetScriptForDestination(source.keyId);
    CAmount selected = ctx.SelectYec(needed, mtx, prevs, onlyScript.empty() ? nullptr : &onlyScript, source.text);
    CAmount change = selected - needed;
    if (change > 0) {
        CPubKey changeKey;
        if (!reservekey.GetReservedKey(changeKey)) throw std::runtime_error("keypool ran out");
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

BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to)
{
    Context ctx(yw);
    const Params& p = ctx.params;
    const AddressChoice dest = ParseAddressChoice(to, "to");
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

    const uint32_t expiry = (uint32_t)ctx.indexHeight + MINT_WINDOW;
    BuiltTx out;
    out.requiredBurn = requiredBurn;
    out.burnCents = requiredBurn;
    out.changeCents = change;
    out.vaultScript = vaultScript;
    out.vaultValue = vault->collateralZat;
    out.ownerPubKey = vault->ownerPubKey;
    for (const YedCoin& c : sel) {
        out.yedInputs.insert(c.outpoint);
        out.yedPrevs.push_back(std::make_pair(c.token.scriptPubKey, c.token.nValue));
    }
    // Outputs: collateral (+ surplus token value - fee - change token value), optional YED change, REDEEM payload (C10).
    const CAmount tokenIn = (CAmount)sel.size() * TOKEN_VALUE;
    const CAmount changeTokens = change > 0 ? TOKEN_VALUE : 0;
    const CAmount collateralOut = vault->collateralZat + tokenIn - g_yellowbackFee - changeTokens;
    if (collateralOut <= 0) throw std::runtime_error("vault value does not cover the fee");
    CScript changeScript;
    if (change > 0) {
        CPubKey changeKey = ctx.FreshKey("yellowback-change");
        changeScript = GetScriptForDestination(changeKey.GetID());
    }

    if (dest.kind == AddressChoice::SAPLING) {
        // Sapling shape (I2): the collateral is a single Sapling output; YED change (if any) is
        // vout[0] and the payload vout[1]. Vault and YED inputs go in unsigned and are signed by
        // SignRedeem() after FinishSapling() — ZIP-243 never covers a scriptSig.
        TransactionBuilder b = ctx.NewBuilder(expiry);
        b.SetLockTime(vault->lockHeight);
        b.AddTransparentInputUnsigned(vaultOut, vault->collateralZat, 0xFFFFFFFE);
        for (const YedCoin& c : sel) b.AddTransparentInputUnsigned(c.outpoint, c.token.nValue);
        std::vector<Assignment> assignments;
        if (change > 0) {
            b.AddTransparentOutput(changeScript, TOKEN_VALUE);
            assignments.push_back(Assignment(0, (uint32_t)change));
            out.changeVout = 0;
        }
        std::vector<unsigned char> payload = EncodePayload(Payload::Redeem(assignments));
        if (payload.empty()) throw std::runtime_error("cannot encode the redeem payload");
        b.AddTransparentOutput(PayloadScript(payload), 0);
        // Encrypt the note under the seed-derived key z_sendmany uses for t->z, so it is recoverable
        // from the seed whether or not the destination belongs to this wallet.
        HDSeed seed = ctx.wallet.GetHDSeedForRPC();
        b.AddSaplingOutput(ovkForShieldingFromTaddr(seed), dest.sapling, collateralOut);
        out.builder = b;
        out.collateralTo = dest.text;
        return out;
    }

    CMutableTransaction mtx = ctx.NewTx(expiry);
    mtx.nLockTime = vault->lockHeight;
    mtx.vin.push_back(CTxIn(vaultOut, CScript(), 0xFFFFFFFE));
    for (const YedCoin& c : sel) mtx.vin.push_back(CTxIn(c.outpoint));
    CScript collateralScript;
    if (dest.kind == AddressChoice::TRANSPARENT) {
        collateralScript = GetScriptForDestination(dest.keyId);
        out.collateralTo = dest.text;
    } else {
        CPubKey fresh = ctx.FreshKey("yellowback-collateral");
        out.freshKey = fresh;
        collateralScript = GetScriptForDestination(fresh.GetID());
        out.collateralTo = KeyIO(::Params()).EncodeDestination(CTxDestination(fresh.GetID()));
    }
    mtx.vout.push_back(CTxOut(collateralOut, collateralScript));
    std::vector<Assignment> assignments;
    if (change > 0) {
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, changeScript));
        assignments.push_back(Assignment(1, (uint32_t)change));
        out.changeVout = 1;
    }
    std::vector<unsigned char> payload = EncodePayload(Payload::Redeem(assignments));
    if (payload.empty()) throw std::runtime_error("cannot encode the redeem payload");
    mtx.vout.push_back(CTxOut(0, PayloadScript(payload)));
    out.tx = mtx;
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
    out.tx.vin[0].scriptSig = BuildVaultScriptSig({}, ownerSig, out.vaultScript);
    for (size_t i = 0; i < out.yedPrevs.size(); i++) {
        if (!SignSignature(wallet, out.yedPrevs[i].first, out.tx, i + 1, out.yedPrevs[i].second, SIGHASH_ALL, branchId)) {
            throw std::runtime_error(strprintf("failed to sign YED input %u", (unsigned)i));
        }
    }
    out.ownYedOutputs.clear();
    if (out.changeVout >= 0) out.ownYedOutputs.push_back(COutPoint(CTransaction(out.tx).GetHash(), out.changeVout));
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
    if (a.valueBalance != b.valueBalance || a.vShieldedSpend != b.vShieldedSpend || a.vShieldedOutput != b.vShieldedOutput ||
        a.vJoinSplit != b.vJoinSplit || a.bindingSig != b.bindingSig) { why = "shielded components differ"; return false; }
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
