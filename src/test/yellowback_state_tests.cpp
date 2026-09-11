// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/view.h"

#include "key.h"
#include "primitives/block.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

using namespace yellowback;

namespace {

const int START = 200;

struct Fixture
{
    std::vector<CKey> fedKeys;
    Roster roster;
    CScript rosterScript;
    CScript anchorSpk;
    CKey ownerKey;
    CKey userKey;
    Params params;
    MemoryStateView view;
    std::map<int, UndoRecord> undos;
    std::map<int, uint256> hashes;
    CMutableTransaction genesisTx;
    int tipHeight = 0;

    Fixture()
    {
        std::vector<CPubKey> pubs;
        for (int i = 0; i < 3; i++) {
            CKey k;
            k.MakeNewKey(true);
            fedKeys.push_back(k);
            pubs.push_back(k.GetPubKey());
        }
        rosterScript = RosterScript(2, SortKeys(pubs));
        BOOST_REQUIRE(ParseRosterScript(rosterScript, roster));
        anchorSpk = P2SHScript(rosterScript);
        ownerKey.MakeNewKey(true);
        userKey.MakeNewKey(true);

        genesisTx.vout.push_back(CTxOut(COIN, anchorSpk));
        params = RegtestParams(START, COutPoint(genesisTx.GetHash(), 0), rosterScript, 0);
    }

    static uint256 FakeHash(int height)
    {
        uint256 h;
        std::vector<unsigned char> v(32, 0);
        v[0] = height & 0xff;
        v[1] = (height >> 8) & 0xff;
        v[31] = 0x5a;
        return uint256(v);
    }

    /** Apply a block of the given txs at the next height; returns the error (nullopt = ok). */
    std::optional<std::string> Apply(std::vector<CMutableTransaction> txs, int height = -1)
    {
        if (height < 0) height = tipHeight + 1;
        CBlock block;
        for (auto& m : txs) block.vtx.push_back(CTransaction(m));
        UndoRecord undo;
        OverlayStateView overlay(view);
        auto err = ApplyBlock(overlay, params, block, height, FakeHash(height), undo);
        if (err.has_value()) return err;
        overlay.Commit();
        undos[height] = undo;
        hashes[height] = FakeHash(height);
        tipHeight = height;
        return std::nullopt;
    }

    void Undo()
    {
        UndoBlock(view, undos[tipHeight]);
        undos.erase(tipHeight);
        tipHeight--;
    }

    void ApplyGenesis()
    {
        BOOST_REQUIRE(!Apply({ genesisTx }, START).has_value());
    }

    /** Mine empty blocks up to (and including) height h. */
    void MineTo(int h)
    {
        while (tipHeight < h) BOOST_REQUIRE(!Apply({}).has_value());
    }

    CMutableTransaction PriceTx(MicroUsd price, const CScript& newAnchorSpk = CScript(), bool withPayload = true)
    {
        State st(view);
        AnchorRecord a = st.GetAnchor();
        CMutableTransaction m;
        m.vin.push_back(CTxIn(a.outpoint, BuildAnchorSig()));
        m.vout.push_back(CTxOut(a.nValue - DEFAULT_YELLOWBACK_FEE, newAnchorSpk.empty() ? a.scriptPubKey : newAnchorSpk));
        if (withPayload) m.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(price)))));
        return m;
    }

    CScript BuildAnchorSig(const CScript& script = CScript()) const
    {
        const CScript& s = script.empty() ? rosterScript : script;
        return CScript() << OP_0 << valtype(70, 1) << valtype(70, 2) << valtype(s.begin(), s.end());
    }

    /** A well-formed mint of `cents` at `tier` evaluated at `evalHeight` for confirmation at `confirmHeight`. */
    CMutableTransaction MintTx(Cents cents, int tier, int evalHeight, int confirmHeight, CAmount collateralOverride = -1, const CScript& rosterOverride = CScript())
    {
        State st(view);
        uint32_t lockHeight = confirmHeight + params.tierBlocks[tier];
        CScript vault = VaultScript(lockHeight, ownerKey.GetPubKey(), rosterOverride.empty() ? rosterScript : rosterOverride);
        CAmount collateral = collateralOverride;
        if (collateral < 0) {
            auto snap = st.GetSnapshot(evalHeight);
            BOOST_REQUIRE(snap.has_value());
            auto req = RequiredCollateralRounded(cents, params.tierRatioPct[tier], snap->dcaBps, snap->price);
            BOOST_REQUIRE(req.has_value());
            collateral = req.value();
        }
        CMutableTransaction m;
        m.vin.push_back(CTxIn(COutPoint(FakeHash(9999 + confirmHeight), 0)));
        m.vout.push_back(CTxOut(collateral, P2SHScript(vault)));
        m.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(ownerKey.GetPubKey().GetID())));
        m.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Mint(tier, cents, lockHeight, evalHeight, ownerKey.GetPubKey())))));
        return m;
    }

    CMutableTransaction TransferTx(const std::vector<COutPoint>& inputs, const std::vector<Assignment>& as, PayloadType type = PayloadType::TRANSFER)
    {
        CMutableTransaction m;
        for (const COutPoint& o : inputs) m.vin.push_back(CTxIn(o));
        size_t nOut = 0;
        for (const Assignment& a : as) nOut = std::max<size_t>(nOut, a.vout + 1);
        for (size_t i = 0; i < nOut; i++) m.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(userKey.GetPubKey().GetID())));
        Payload p = type == PayloadType::TRANSFER ? Payload::Transfer(as) : Payload::Redeem(as);
        m.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(p))));
        return m;
    }

    Totals GetTotals() { return State(view).GetTotals(); }
    std::optional<MicroUsd> Price(int h) { return State(view).PriceInEffect(h); }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_state_tests, BasicTestingSetup)

#if 0 // Phase 0 (plan §6): the anchor-chain price and roster-rotation cases (genesis_and_prices, rotation_and_custody)
      // are disabled, not deleted; the state machine they exercise migrates to v2 in Phase 2.
BOOST_AUTO_TEST_CASE(genesis_and_prices)
{
    Fixture f;
    // Below startHeight: nothing is written.
    BOOST_REQUIRE(!f.Apply({}, START - 1).has_value());
    BOOST_CHECK(f.view.Map().empty());
    // At startHeight without the anchor transaction: unhealthy.
    BOOST_CHECK(f.Apply({}, START).has_value());
    BOOST_CHECK(f.view.Map().empty());
    f.ApplyGenesis();
    State st(f.view);
    BOOST_REQUIRE(st.GetTip().has_value());
    BOOST_CHECK_EQUAL(st.GetTip()->height, START);
    BOOST_CHECK(st.GetAnchor().valid);
    BOOST_CHECK(st.GetAnchor().outpoint == f.params.genesisAnchor);
    BOOST_CHECK_EQUAL(st.GetAnchor().nValue, COIN);
    BOOST_CHECK_EQUAL(st.Rosters().size(), 1u);
    BOOST_CHECK(!st.GetSnapshot(START)->priceDefined);
    BOOST_CHECK_EQUAL(st.GetSnapshot(START)->healthPct, HEALTH_CAP);

    // A PRICE at START+1.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value());
    BOOST_CHECK_EQUAL(f.Price(START + 1).value(), 50000);
    BOOST_CHECK(State(f.view).GetSnapshot(START + 1)->priceDefined);
    BOOST_CHECK(State(f.view).GetAnchor().nValue == COIN - DEFAULT_YELLOWBACK_FEE);
    // Two chained prices in one block: the last wins (B7).
    {
        CMutableTransaction p1 = f.PriceTx(60000);
        // p2 spends p1's new anchor
        CMutableTransaction p2;
        p2.vin.push_back(CTxIn(COutPoint(CTransaction(p1).GetHash(), 0), f.BuildAnchorSig()));
        p2.vout.push_back(CTxOut(p1.vout[0].nValue - DEFAULT_YELLOWBACK_FEE, f.anchorSpk));
        p2.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(70000)))));
        BOOST_REQUIRE(!f.Apply({ p1, p2 }).has_value());
        BOOST_CHECK_EQUAL(f.Price(START + 2).value(), 70000);
        BOOST_CHECK(State(f.view).GetAnchor().outpoint == COutPoint(CTransaction(p2).GetHash(), 0));
    }
    // Age boundary: price at h is in effect through h + 48, not h + 49.
    f.MineTo(START + 2 + PRICE_MAX_AGE);
    BOOST_CHECK_EQUAL(f.Price(START + 2 + PRICE_MAX_AGE).value(), 70000);
    f.MineTo(START + 3 + PRICE_MAX_AGE);
    BOOST_CHECK(!f.Price(START + 3 + PRICE_MAX_AGE).has_value());
    BOOST_CHECK(!State(f.view).GetSnapshot(START + 3 + PRICE_MAX_AGE)->priceDefined);
    // Out-of-range price: anchor moves, no price recorded.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(PRICE_MAX + 1) }).has_value());
    BOOST_CHECK(!f.Price(f.tipHeight).has_value());
    BOOST_CHECK(State(f.view).GetTxLog(CTransaction(f.PriceTx(1)).GetHash()) == std::nullopt);
    // A PRICE payload that does not spend the anchor is non-Yellowback.
    {
        CMutableTransaction m;
        m.vin.push_back(CTxIn(COutPoint(Fixture::FakeHash(1), 0)));
        m.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(50000)))));
        BOOST_REQUIRE(!f.Apply({ m }).has_value());
        BOOST_CHECK(!f.Price(f.tipHeight).has_value());
        auto log = State(f.view).GetTxLog(CTransaction(m).GetHash());
        BOOST_REQUIRE(log.has_value());
        BOOST_CHECK_EQUAL(log->verdict, verdict::PRICE_NOT_ANCHOR);
    }
}

BOOST_AUTO_TEST_CASE(rotation_and_custody)
{
    Fixture f;
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value());

    // Rotation: anchor spend to a new roster's P2SH, no OP_RETURN (C9).
    std::vector<CPubKey> pubs2;
    for (int i = 0; i < 3; i++) { CKey k; k.MakeNewKey(true); pubs2.push_back(k.GetPubKey()); }
    CScript roster2 = RosterScript(2, SortKeys(pubs2));
    CScript anchor2 = P2SHScript(roster2);
    CMutableTransaction rot = f.PriceTx(0, anchor2, false);
    BOOST_REQUIRE(!f.Apply({ rot }).has_value());
    State st(f.view);
    BOOST_CHECK(st.GetAnchor().scriptPubKey == anchor2);
    BOOST_CHECK_EQUAL(st.Rosters().size(), 1u); // the new script is revealed only when the new anchor is spent
    BOOST_CHECK_EQUAL(st.GetTxLog(CTransaction(rot).GetHash())->verdict, verdict::PRICE_ROTATION);

    // Next PRICE from the new anchor reveals roster 2 and records a price.
    {
        CMutableTransaction p;
        AnchorRecord a = st.GetAnchor();
        p.vin.push_back(CTxIn(a.outpoint, f.BuildAnchorSig(roster2)));
        p.vout.push_back(CTxOut(a.nValue - DEFAULT_YELLOWBACK_FEE, anchor2));
        p.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(55000)))));
        BOOST_REQUIRE(!f.Apply({ p }).has_value());
        State st2(f.view);
        BOOST_CHECK_EQUAL(st2.Rosters().size(), 2u);
        BOOST_CHECK(st2.Rosters()[1].script == roster2);
        BOOST_CHECK_EQUAL(st2.Rosters()[1].revealHeight, f.tipHeight);
        BOOST_CHECK_EQUAL(f.Price(f.tipHeight).value(), 55000);
        // Previous roster is mintable only during the grace period.
        BOOST_CHECK_EQUAL(MintableRosters(st2.Rosters(), f.params, f.tipHeight).size(), 2u);
        BOOST_CHECK_EQUAL(MintableRosters(st2.Rosters(), f.params, f.tipHeight + f.params.rosterGrace).size(), 2u);
        BOOST_CHECK_EQUAL(MintableRosters(st2.Rosters(), f.params, f.tipHeight + f.params.rosterGrace + 1).size(), 1u);
    }
    // A price carried by a spend that changes the script is a rotation, not a price (B8).
    {
        CMutableTransaction p = f.PriceTx(99000, f.anchorSpk, true);
        p.vin[0].scriptSig = f.BuildAnchorSig(roster2);
        BOOST_REQUIRE(!f.Apply({ p }).has_value());
        BOOST_CHECK(!State(f.view).GetPriceAt(f.tipHeight).has_value());
        BOOST_CHECK_EQUAL(State(f.view).GetTxLog(CTransaction(p).GetHash())->verdict, verdict::PRICE_ROTATION);
        BOOST_CHECK(State(f.view).GetAnchor().scriptPubKey == f.anchorSpk);
        BOOST_CHECK_EQUAL(State(f.view).Rosters().size(), 2u); // roster2 already known; original roster differs from back => appended? no: script == rosters[1]? it is roster2 => not appended
    }
    // Custody break: anchor spent to a non-P2SH output.
    {
        CMutableTransaction bad = f.PriceTx(50000, GetScriptForDestination(f.userKey.GetPubKey().GetID()), true);
        BOOST_REQUIRE(!f.Apply({ bad }).has_value());
        BOOST_CHECK(!State(f.view).GetAnchor().valid);
        // Further "prices" are ignored: nothing spends a valid anchor.
        CMutableTransaction after;
        after.vin.push_back(CTxIn(COutPoint(CTransaction(bad).GetHash(), 0)));
        after.vout.push_back(CTxOut(1000, f.anchorSpk));
        after.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(50000)))));
        BOOST_REQUIRE(!f.Apply({ after }).has_value());
        BOOST_CHECK(!State(f.view).GetPriceAt(f.tipHeight).has_value());
        BOOST_CHECK(!State(f.view).GetAnchor().valid);
    }
}

#endif // Phase 0

BOOST_AUTO_TEST_CASE(mint_transfer_burn_redeem)
{
    Fixture f;
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value()); // START+1, $0.05
    f.MineTo(START + 3);

    // MINT $100 at tier 0, evaluated at START+1, confirming at START+4.
    const int evalH = START + 1;
    const int mintH = START + 4;
    CMutableTransaction mint = f.MintTx(10000, 0, evalH, mintH);
    BOOST_REQUIRE(!f.Apply({ mint }, mintH).has_value());
    const uint256 mintId = CTransaction(mint).GetHash();
    {
        State st(f.view);
        auto v = st.GetVault(COutPoint(mintId, 0));
        BOOST_REQUIRE(v.has_value());
        BOOST_CHECK(v->Status() == VaultStatus::ACTIVE);
        BOOST_CHECK_EQUAL(v->mintedCents, 10000);
        BOOST_CHECK_EQUAL(v->rosterIndex, 0);
        // $100 at 1000 % = $1,000 of YEC at $0.05 = 20,000 YEC
        BOOST_CHECK_EQUAL(v->collateralZat, 20000 * COIN);
        auto t = st.GetToken(COutPoint(mintId, 1));
        BOOST_REQUIRE(t.has_value());
        BOOST_CHECK_EQUAL(t->cents, 10000);
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, 10000);
        BOOST_CHECK_EQUAL(st.GetTotals().collateralZat, 20000 * COIN);
        BOOST_CHECK_EQUAL(st.GetTotals().activeVaults, 1u);
        BOOST_CHECK_EQUAL(st.GetTxLog(mintId)->verdict, verdict::MINT_OK);
        BOOST_CHECK_EQUAL(st.GetTxLog(mintId)->yedOut, 10000);
        // Snapshot health: 20,000 YEC * $0.05 = $1,000 backing $100 => 1000 %
        BOOST_CHECK_EQUAL(st.GetSnapshot(mintH)->healthPct, 1000);
    }

    // TRANSFER: split into $60 + $40 (exact).
    CMutableTransaction xfer = f.TransferTx({ COutPoint(mintId, 1) }, { Assignment(0, 6000), Assignment(1, 4000) });
    BOOST_REQUIRE(!f.Apply({ xfer }).has_value());
    const uint256 xferId = CTransaction(xfer).GetHash();
    {
        State st(f.view);
        BOOST_CHECK(!st.GetToken(COutPoint(mintId, 1)).has_value());
        BOOST_CHECK_EQUAL(st.GetToken(COutPoint(xferId, 0))->cents, 6000);
        BOOST_CHECK_EQUAL(st.GetToken(COutPoint(xferId, 1))->cents, 4000);
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, 10000);
        BOOST_CHECK_EQUAL(st.GetTxLog(xferId)->burned, 0);
        BOOST_CHECK_EQUAL(st.GetTxLog(xferId)->spentTokens.size(), 1u);
    }

    // Under-assignment burns only the remainder (A3): $40 input, $30 assigned.
    CMutableTransaction under = f.TransferTx({ COutPoint(xferId, 1) }, { Assignment(0, 3000) });
    BOOST_REQUIRE(!f.Apply({ under }).has_value());
    const uint256 underId = CTransaction(under).GetHash();
    BOOST_CHECK_EQUAL(State(f.view).GetTotals().supplyCents, 9000);
    BOOST_CHECK_EQUAL(State(f.view).GetTxLog(underId)->burned, 1000);
    BOOST_CHECK_EQUAL(State(f.view).GetToken(COutPoint(underId, 0))->cents, 3000);

    // Over-assignment assigns nothing (all burned).
    CMutableTransaction over = f.TransferTx({ COutPoint(underId, 0) }, { Assignment(0, 3001) });
    BOOST_REQUIRE(!f.Apply({ over }).has_value());
    BOOST_CHECK_EQUAL(State(f.view).GetTotals().supplyCents, 6000);
    BOOST_CHECK(!State(f.view).GetToken(COutPoint(CTransaction(over).GetHash(), 0)).has_value());
    BOOST_CHECK_EQUAL(State(f.view).GetTxLog(CTransaction(over).GetHash())->verdict, verdict::XFER_OVER_ASSIGNED);

    // Plain spend (no payload) burns.
    CMutableTransaction plain;
    plain.vin.push_back(CTxIn(COutPoint(xferId, 0)));
    plain.vout.push_back(CTxOut(TOKEN_VALUE - 1000, GetScriptForDestination(f.userKey.GetPubKey().GetID())));
    // ... but first, in-block chaining: a transfer of the $60 followed by its spend in the same block.
    CMutableTransaction chain1 = f.TransferTx({ COutPoint(xferId, 0) }, { Assignment(0, 6000) });
    CMutableTransaction chain2 = f.TransferTx({ COutPoint(CTransaction(chain1).GetHash(), 0) }, { Assignment(0, 5500) });
    BOOST_REQUIRE(!f.Apply({ chain1, chain2 }).has_value());
    const uint256 chain2Id = CTransaction(chain2).GetHash();
    BOOST_CHECK_EQUAL(State(f.view).GetToken(COutPoint(chain2Id, 0))->cents, 5500);
    BOOST_CHECK_EQUAL(State(f.view).GetTotals().supplyCents, 5500);
    plain.vin[0].prevout = COutPoint(chain2Id, 0);
    BOOST_REQUIRE(!f.Apply({ plain }).has_value());
    BOOST_CHECK_EQUAL(State(f.view).GetTotals().supplyCents, 0);
    BOOST_CHECK_EQUAL(State(f.view).GetTxLog(CTransaction(plain).GetHash())->burned, 5500);
    BOOST_CHECK_EQUAL(State(f.view).GetTxLog(CTransaction(plain).GetHash())->verdict, verdict::NON_YELLOWBACK);

    // A transfer with no Yellowback input assigns nothing (XFER-3).
    CMutableTransaction noIn = f.TransferTx({ COutPoint(Fixture::FakeHash(77), 0) }, { Assignment(0, 100) });
    BOOST_REQUIRE(!f.Apply({ noIn }).has_value());
    BOOST_CHECK(!State(f.view).GetToken(COutPoint(CTransaction(noIn).GetHash(), 0)).has_value());
    BOOST_CHECK_EQUAL(State(f.view).GetTxLog(CTransaction(noIn).GetHash())->verdict, verdict::XFER_NO_INPUT);

    // Second mint, then a REDEEM that spends the vault and burns the token (IN-2, IN-3).
    const int eval2 = f.tipHeight;
    f.MineTo(eval2 + 2);
    CMutableTransaction mint2 = f.MintTx(20000, 0, eval2, f.tipHeight + 1);
    BOOST_REQUIRE(!f.Apply({ mint2 }).has_value());
    const uint256 mint2Id = CTransaction(mint2).GetHash();
    BOOST_CHECK_EQUAL(State(f.view).GetTotals().supplyCents, 20000);
    // Redeem before lock height is a state-machine non-issue (consensus enforces CLTV); just close it.
    CMutableTransaction redeem = f.TransferTx({ COutPoint(mint2Id, 0), COutPoint(mint2Id, 1) }, {}, PayloadType::REDEEM);
    BOOST_REQUIRE(!f.Apply({ redeem }).has_value());
    {
        State st(f.view);
        auto v = st.GetVault(COutPoint(mint2Id, 0));
        BOOST_REQUIRE(v.has_value());
        BOOST_CHECK(v->Status() == VaultStatus::CLOSED);
        BOOST_CHECK(v->wasActive);
        BOOST_CHECK(v->closingTxid == CTransaction(redeem).GetHash());
        BOOST_CHECK_EQUAL(v->closeHeight, f.tipHeight);
        BOOST_CHECK_EQUAL(v->burnedCents, 20000);
        BOOST_CHECK_EQUAL(v->errBpsAtClose, 10000);
        BOOST_CHECK_EQUAL(v->requiredBurnAtClose, 20000);
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, 0);
        BOOST_CHECK_EQUAL(st.GetTotals().collateralZat, 20000 * COIN); // vault 1 still open
        BOOST_CHECK_EQUAL(st.GetTotals().activeVaults, 1u);
        BOOST_CHECK_EQUAL(st.GetTxLog(CTransaction(redeem).GetHash())->closedVaults.size(), 1u);
        BOOST_CHECK_EQUAL(st.GetTxLog(CTransaction(redeem).GetHash())->verdict, verdict::REDEEM_OK);
    }
}

// Revision 14 (I1): TX-0 covers the coinbase only. The YED accounting reads transparent
// inputs, vout indexes and the OP_RETURN; Sapling spends/outputs and valueBalance on the same
// transaction are the YEC side's business and change no verdict.
BOOST_AUTO_TEST_CASE(shielded_components_are_ignored)
{
    Fixture f;
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value()); // START+1, $0.05
    f.MineTo(START + 3);

    // A mint whose collateral is unshielded in the same transaction (ys1 -> vault): no
    // transparent inputs at all, one Sapling spend, positive valueBalance.
    const int evalH = START + 1;
    const int mintH = START + 4;
    CMutableTransaction mint = f.MintTx(10000, 0, evalH, mintH);
    mint.vin.clear();
    mint.vShieldedSpend.push_back(SpendDescription());
    mint.valueBalance = mint.vout[0].nValue + TOKEN_VALUE + DEFAULT_YELLOWBACK_FEE;
    BOOST_REQUIRE(!f.Apply({ mint }, mintH).has_value());
    const uint256 mintId = CTransaction(mint).GetHash();
    {
        State st(f.view);
        BOOST_CHECK_EQUAL(st.GetTxLog(mintId)->verdict, verdict::MINT_OK);
        BOOST_REQUIRE(st.GetVault(COutPoint(mintId, 0)).has_value());
        BOOST_CHECK(st.GetVault(COutPoint(mintId, 0))->Status() == VaultStatus::ACTIVE);
        BOOST_CHECK_EQUAL(st.GetToken(COutPoint(mintId, 1))->cents, 10000);
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, 10000);
    }

    // A transfer that shields its YEC change alongside the YED assignment.
    CMutableTransaction xfer = f.TransferTx({ COutPoint(mintId, 1) }, { Assignment(0, 10000) });
    xfer.vShieldedOutput.push_back(OutputDescription());
    xfer.valueBalance = -COIN;
    BOOST_REQUIRE(!f.Apply({ xfer }).has_value());
    const uint256 xferId = CTransaction(xfer).GetHash();
    {
        State st(f.view);
        BOOST_CHECK_EQUAL(st.GetTxLog(xferId)->verdict, verdict::TRANSFER_OK);
        BOOST_CHECK_EQUAL(st.GetToken(COutPoint(xferId, 0))->cents, 10000);
        BOOST_CHECK_EQUAL(st.GetTxLog(xferId)->burned, 0);
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, 10000);
    }

    // A PRICE transaction with a shielded component still records the price: the roster's
    // k-of-n signatures gate that shape (yed_createpricetx peers refuse it), not the state.
    CMutableTransaction price = f.PriceTx(60000);
    price.vShieldedOutput.push_back(OutputDescription());
    price.valueBalance = -1;
    BOOST_REQUIRE(!f.Apply({ price }).has_value());
    BOOST_REQUIRE(f.Price(f.tipHeight).has_value());
    BOOST_CHECK_EQUAL(f.Price(f.tipHeight).value(), (MicroUsd)60000);

    // TX-0 proper: a coinbase carrying a MINT payload creates nothing, not even a VOID vault.
    CMutableTransaction cb = f.MintTx(10000, 0, f.tipHeight - 1, f.tipHeight + 1);
    cb.vin.clear();
    cb.vin.push_back(CTxIn(COutPoint(), CScript() << OP_0));
    BOOST_REQUIRE(CTransaction(cb).IsCoinBase());
    BOOST_REQUIRE(!f.Apply({ cb }).has_value());
    const uint256 cbId = CTransaction(cb).GetHash();
    {
        State st(f.view);
        BOOST_CHECK_EQUAL(st.GetTxLog(cbId)->verdict, verdict::COINBASE);
        BOOST_CHECK(!st.GetVault(COutPoint(cbId, 0)).has_value());
        BOOST_CHECK(!st.GetToken(COutPoint(cbId, 1)).has_value());
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, 10000);
    }
}

BOOST_AUTO_TEST_CASE(void_mints)
{
    Fixture f;
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value()); // START+1
    f.MineTo(START + 3);

    auto expectVoid = [&](CMutableTransaction m, const char* reason, int height) {
        const Cents supplyBefore = f.GetTotals().supplyCents;
        BOOST_REQUIRE(!f.Apply({ m }, height).has_value());
        State st(f.view);
        const uint256 id = CTransaction(m).GetHash();
        auto v = st.GetVault(COutPoint(id, 0));
        BOOST_REQUIRE_MESSAGE(v.has_value(), reason);
        BOOST_CHECK(v->Status() == VaultStatus::VOID);
        BOOST_CHECK_EQUAL(v->voidReason, reason);
        BOOST_CHECK_EQUAL(st.GetTxLog(id)->verdict, reason);
        BOOST_CHECK(!st.GetToken(COutPoint(id, 1)).has_value());
        BOOST_CHECK_EQUAL(st.GetTotals().supplyCents, supplyBefore);
    };

    // Under-collateralised (eval START+1, confirm START+4).
    expectVoid(f.MintTx(10000, 0, START + 1, f.tipHeight + 1, 20000 * COIN - 1), verdict::BAD_MINT_COLLATERAL, f.tipHeight + 1);
    // evalHeight = START has a snapshot but no price.
    expectVoid(f.MintTx(10000, 0, START, f.tipHeight + 1, 1000 * COIN), verdict::BAD_ORACLE_PRICE, f.tipHeight + 1);
    // Bad tier / amount / lock duration / roster.
    {
        CMutableTransaction m = f.MintTx(10000, 0, f.tipHeight, f.tipHeight + 1);
        m.vout[2].scriptPubKey = PayloadScript(EncodePayload(Payload::Mint(7, 10000, f.tipHeight + 49, f.tipHeight, f.ownerKey.GetPubKey())));
        expectVoid(m, verdict::BAD_MINT_TIER, f.tipHeight + 1);
    }
    expectVoid(f.MintTx(9999, 0, f.tipHeight, f.tipHeight + 1, 1000 * COIN), verdict::BAD_MINT_AMOUNT, f.tipHeight + 1);
    {
        // Lock height one block short of the tier: built for +2, confirmed at +3.
        CMutableTransaction m = f.MintTx(10000, 0, f.tipHeight, f.tipHeight + 2);
        expectVoid(m, verdict::BAD_MINT_LOCK_TIER_DURATION, f.tipHeight + 3);
    }
    {
        std::vector<CPubKey> pubs2;
        for (int i = 0; i < 3; i++) { CKey k; k.MakeNewKey(true); pubs2.push_back(k.GetPubKey()); }
        CMutableTransaction m = f.MintTx(10000, 0, f.tipHeight, f.tipHeight + 1, -1, RosterScript(2, SortKeys(pubs2)));
        expectVoid(m, verdict::BAD_MINT_VAULT_SCRIPT, f.tipHeight + 1);
    }
    // evalHeight older than MINT_WINDOW.
    {
        const int evalH = f.tipHeight;
        f.MineTo(evalH + MINT_WINDOW);
        expectVoid(f.MintTx(10000, 0, evalH, f.tipHeight + 1, 1000000 * COIN), verdict::BAD_MINT_EVAL_HEIGHT, f.tipHeight + 1);
    }
    // Fresh price, then the supply cap.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value());
    const int eval2 = f.tipHeight;
    f.MineTo(eval2 + 2);
    f.params.supplyCap = 15000;
    BOOST_REQUIRE(!f.Apply({ f.MintTx(10000, 0, eval2, f.tipHeight + 1) }).has_value());
    BOOST_CHECK_EQUAL(f.GetTotals().supplyCents, 10000);
    expectVoid(f.MintTx(10000, 0, eval2, f.tipHeight + 1), verdict::MINT_SUPPLY_CAP, f.tipHeight + 1);
    BOOST_CHECK_EQUAL(f.GetTotals().supplyCents, 10000);
    BOOST_CHECK_EQUAL(f.GetTotals().voidVaults, 8u);
    BOOST_CHECK_EQUAL(f.GetTotals().activeVaults, 1u);

    // Releasing a VOID vault: closes with zero required burn and no collateral change.
    {
        State st(f.view);
        CAmount before = st.GetTotals().collateralZat;
        COutPoint voidOut;
        st.View().Iterate("V", [&](const std::string& k, const std::string& v) {
            VaultRecord r;
            DeserializeRecord(v, r);
            if (r.Status() == VaultStatus::VOID) {
                voidOut = COutPoint(uint256(std::vector<unsigned char>(k.begin() + 1, k.begin() + 33)), 0);
                return false;
            }
            return true;
        });
        CMutableTransaction rel;
        rel.vin.push_back(CTxIn(voidOut));
        rel.vout.push_back(CTxOut(1000, GetScriptForDestination(f.ownerKey.GetPubKey().GetID())));
        BOOST_REQUIRE(!f.Apply({ rel }).has_value());
        State st2(f.view);
        auto v = st2.GetVault(voidOut);
        BOOST_CHECK(v->Status() == VaultStatus::CLOSED);
        BOOST_CHECK(!v->wasActive);
        BOOST_CHECK_EQUAL(v->requiredBurnAtClose, 0);
        BOOST_CHECK_EQUAL(st2.GetTotals().collateralZat, before);
        BOOST_CHECK_EQUAL(st2.GetTotals().voidVaults, 7u);
    }
}

BOOST_AUTO_TEST_CASE(err_snapshots)
{
    Fixture f;
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value());
    f.MineTo(START + 3);
    BOOST_REQUIRE(!f.Apply({ f.MintTx(10000, 0, START + 1, f.tipHeight + 1) }).has_value()); // 20,000 YEC back $100
    // $0.0072: health = 20000 * 0.0072 / 100 = 144 % => DCA 1.25x, ERR inactive.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(7200) }).has_value());
    BOOST_CHECK_EQUAL(State(f.view).GetSnapshot(f.tipHeight)->healthPct, 144);
    BOOST_CHECK_EQUAL(State(f.view).GetSnapshot(f.tipHeight)->dcaBps, 12500);
    BOOST_CHECK_EQUAL(State(f.view).GetSnapshot(f.tipHeight)->errBps, 10000);
    // $0.0004: health 8 % => ERR floor.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(400) }).has_value());
    {
        Snapshot s = State(f.view).GetSnapshot(f.tipHeight).value();
        BOOST_CHECK_EQUAL(s.healthPct, 8);
        BOOST_CHECK_EQUAL(s.dcaBps, 20000);
        BOOST_CHECK_EQUAL(s.errBps, 8000);
        BOOST_CHECK(!s.mintFrozen); // no price 48 blocks back yet: the window is not evaluated
    }
    // A mint evaluated at that height is blocked by ERR (even with huge collateral).
    {
        const int evalH = f.tipHeight;
        f.MineTo(evalH + 2);
        CMutableTransaction m = f.MintTx(10000, 0, evalH, f.tipHeight + 1, 1000000 * COIN);
        BOOST_REQUIRE(!f.Apply({ m }).has_value());
        BOOST_CHECK_EQUAL(State(f.view).GetVault(COutPoint(CTransaction(m).GetHash(), 0))->voidReason, verdict::MINT_BLOCKED_ERR);
    }
    // A redemption now needs a 125 % burn: vault closed with errBpsAtClose from the previous block.
    {
        State st(f.view);
        COutPoint vaultOut;
        st.View().Iterate("V", [&](const std::string& k, const std::string& v) {
            VaultRecord r;
            DeserializeRecord(v, r);
            if (r.Status() == VaultStatus::ACTIVE) { vaultOut = COutPoint(uint256(std::vector<unsigned char>(k.begin() + 1, k.begin() + 33)), 0); return false; }
            return true;
        });
        CMutableTransaction redeem = f.TransferTx({ vaultOut }, {}, PayloadType::REDEEM);
        BOOST_REQUIRE(!f.Apply({ redeem }).has_value());
        auto v = State(f.view).GetVault(vaultOut);
        BOOST_CHECK_EQUAL(v->errBpsAtClose, 8000);
        BOOST_CHECK_EQUAL(v->requiredBurnAtClose, 12500);
        BOOST_CHECK_EQUAL(v->burnedCents, 0); // unbacked: burnedCents < requiredBurnAtClose (F1)
    }
}

BOOST_AUTO_TEST_CASE(volatility_freeze_and_cooldown)
{
    Fixture f;
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value()); // START+1
    f.MineTo(START + 40);
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value()); // START+41 keeps it fresh
    f.MineTo(START + 52);
    // START+53: -22 % => breaches the 1-hour window (p1 = price(START+5) = 50000), not the 24-hour one.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(39000) }).has_value());
    const int B = f.tipHeight;
    BOOST_CHECK_EQUAL(State(f.view).GetVolatility().lastBreachHeight, B);
    BOOST_CHECK(State(f.view).GetSnapshot(B)->mintFrozen);
    // SNAP re-evaluates breach(H) every block: while price(H - 48) still reads the old level the
    // breach repeats, so the last breach is at B + 47; from B + 48 the reference is 39000.
    const int LB = B + PRICE_MAX_AGE - 1;
    f.MineTo(B + 39);
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(39000) }).has_value()); // keeps the price fresh, same level
    f.MineTo(LB + 1);
    BOOST_CHECK_EQUAL(State(f.view).GetVolatility().lastBreachHeight, LB);
    f.MineTo(B + 79);
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(39000) }).has_value()); // p24 = 50000: -22 % < 30 %, no breach
    BOOST_CHECK_EQUAL(State(f.view).GetVolatility().lastBreachHeight, LB);
    f.MineTo(LB + f.params.volCooldown);
    BOOST_CHECK(State(f.view).GetSnapshot(f.tipHeight)->mintFrozen);
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(39000) }).has_value()); // LB + cooldown + 1
    BOOST_CHECK(!State(f.view).GetSnapshot(f.tipHeight)->mintFrozen);
    BOOST_CHECK_EQUAL(State(f.view).GetVolatility().lastBreachHeight, LB);
    // A -19.9 % move does not breach; a -20 % move does.
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(31240) }).has_value()); // 39000 * 0.801 = 31239 => -19.9 %
    BOOST_CHECK(!State(f.view).GetSnapshot(f.tipHeight)->mintFrozen);
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(31200) }).has_value()); // exactly -20 % of 39000
    BOOST_CHECK(State(f.view).GetSnapshot(f.tipHeight)->mintFrozen);
}

BOOST_AUTO_TEST_CASE(apply_undo_identity)
{
    Fixture f;
    std::vector<MemoryStateView> snapshots;
    auto step = [&](std::vector<CMutableTransaction> txs, int height = -1) {
        BOOST_REQUIRE(!f.Apply(txs, height).has_value());
        snapshots.push_back(f.view);
    };

    snapshots.push_back(f.view);
    step({ f.genesisTx }, START);
    step({ f.PriceTx(50000) });
    step({});
    step({});
    CMutableTransaction mint = f.MintTx(10000, 0, START + 1, f.tipHeight + 1);
    step({ mint });
    const uint256 mintId = CTransaction(mint).GetHash();
    step({ f.TransferTx({ COutPoint(mintId, 1) }, { Assignment(0, 6000), Assignment(1, 4000) }), f.PriceTx(400) });
    step({ f.TransferTx({ COutPoint(mintId, 0) }, {}, PayloadType::REDEEM) });
    std::vector<CPubKey> pubs2;
    for (int i = 0; i < 3; i++) { CKey k; k.MakeNewKey(true); pubs2.push_back(k.GetPubKey()); }
    step({ f.PriceTx(0, P2SHScript(RosterScript(2, SortKeys(pubs2))), false) });
    step({ f.PriceTx(1, GetScriptForDestination(f.userKey.GetPubKey().GetID())) });

    // Undo everything, checking byte identity at every step, including back to empty.
    for (size_t i = snapshots.size() - 1; i > 0; i--) {
        BOOST_CHECK(f.view == snapshots[i]);
        BOOST_CHECK(StateHash(f.view) == StateHash(snapshots[i]));
        f.Undo();
        BOOST_CHECK_MESSAGE(f.view == snapshots[i - 1], "undo to step " << (i - 1));
    }
    BOOST_CHECK(f.view.Map().empty());
    // Re-applying gives the same hash as the original.
    f.ApplyGenesis();
    BOOST_REQUIRE(!f.Apply({ f.PriceTx(50000) }).has_value());
    BOOST_CHECK(StateHash(f.view) == StateHash(snapshots[2]));
}

BOOST_AUTO_TEST_CASE(overlay_and_hash)
{
    MemoryStateView base;
    base.Write("Ka", "1");
    base.Write("Kb", "2");
    base.Write("Ua", "undo");
    uint256 h1 = StateHash(base);
    OverlayStateView ov(base);
    ov.Write("Kc", "3");
    ov.Erase("Ka");
    std::string v;
    BOOST_CHECK(!ov.Read("Ka", v));
    BOOST_CHECK(ov.Read("Kc", v) && v == "3");
    BOOST_CHECK(base.Read("Ka", v));
    std::vector<std::string> seen;
    ov.Iterate("K", [&](const std::string& k, const std::string&) { seen.push_back(k); return true; });
    BOOST_CHECK_EQUAL(seen.size(), 2u);
    BOOST_CHECK_EQUAL(seen[0], "Kb");
    BOOST_CHECK_EQUAL(seen[1], "Kc");
    BOOST_CHECK(StateHash(base) == h1);
    ov.Commit();
    BOOST_CHECK(StateHash(base) != h1);
    BOOST_CHECK(!base.Read("Ka", v));
    // Undo records do not enter the hash.
    MemoryStateView other;
    other.Write("Kb", "2");
    other.Write("Kc", "3");
    BOOST_CHECK(StateHash(base) == StateHash(other));
}

BOOST_AUTO_TEST_SUITE_END()
