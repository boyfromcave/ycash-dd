// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// The v2 transaction builder's pure layer (plan §3.5, §4.2a): every template's
// output layout (vout order, feeVout, payload round-trip, nLockTime /
// nSequence / nExpiryHeight), SignVaultSpend under STANDARD_SCRIPT_VERIFY_FLAGS
// with the right amount and branch id and with the wrong ones (ZIP-243 binds
// both, mapping §13.1), the VOID release and the SWEEP without OP_RETURN or
// fee output, and the pure shapes evaluated by the state machine itself
// (MINT-1..8 and RED-1..4 over a MemoryStateView), so the wallet and the
// validator cannot disagree on a template the wallet emits. The Build*
// functions that select coins from a CWallet are exercised by the functional
// tests (yellowback_lifecycle.py, yellowback_claim.py, yellowback_void_mint.py).

#include "yellowback/math.h"
#include "yellowback/params.h"
#include "yellowback/payload.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/txbuilder.h"
#include "yellowback/view.h"

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "key.h"
#include "keystore.h"
#include "main.h"
#include "policy/policy.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/script_error.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"
#include "utiltest.h"

#include <boost/test/unit_test.hpp>

using namespace yellowback;

namespace {

const unsigned int CONSENSUS_FLAGS = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;   // main.cpp:2931

CKey NewKey()
{
    CKey k;
    k.MakeNewKey(true);
    return k;
}

/** A Sapling-format transaction shell as Context::NewTx makes it. */
CMutableTransaction Shell(uint32_t nExpiryHeight)
{
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    mtx.nVersion = SAPLING_TX_VERSION;
    mtx.nExpiryHeight = nExpiryHeight;
    return mtx;
}

bool Verify(const CMutableTransaction& mtx, unsigned int nIn, const CScript& scriptPubKey, CAmount amount, uint32_t branchId,
            ScriptError* err = nullptr, unsigned int flags = STANDARD_SCRIPT_VERIFY_FLAGS)
{
    const CTransaction tx(mtx);
    PrecomputedTransactionData txdata(tx);
    return VerifyScript(tx.vin[nIn].scriptSig, scriptPubKey, flags, TransactionSignatureChecker(&tx, nIn, amount, txdata), branchId, err);
}

YedCoin Coin(const uint256& txid, uint32_t n, Cents cents, const CKey& key)
{
    YedCoin c;
    c.outpoint = COutPoint(txid, n);
    c.token.cents = cents;
    c.token.nValue = TOKEN_VALUE;
    c.token.scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
    c.token.height = 100;
    return c;
}

/** A vault shape for tests: the ACTIVE vault of `owner`, 5 YEC, class A on the regtest set. */
struct Fixture
{
    yellowback::Params params;
    CKey owner;
    CKey yedKey;
    CKey payeeKey;
    uint32_t lockHeight, claimHeight;
    CAmount vaultValue;
    COutPoint vaultOut;
    CScript vaultScript;
    uint32_t branchId;

    Fixture() : params(RegtestParams(1, 0, 0, 0)), owner(NewKey()), yedKey(NewKey()), payeeKey(NewKey()),
                lockHeight(300), claimHeight(300 + params.grace), vaultValue(5 * COIN), vaultOut(uint256S("aa"), 0),
                branchId(NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId)
    {
        vaultScript = VaultScript(lockHeight, owner.GetPubKey(), claimHeight);
        BOOST_REQUIRE(!vaultScript.empty());
    }

    VaultSpendShape Shape(bool ownerPath, bool withPayload, bool transparentDest, bool withPayee, std::vector<YedCoin> yed, Cents change = 0) const
    {
        VaultSpendShape s;
        s.vaultOut = vaultOut;
        s.vaultScript = vaultScript;
        s.vaultValue = vaultValue;
        s.lockHeight = lockHeight;
        s.claimHeight = claimHeight;
        s.ownerPath = ownerPath;
        s.withPayload = withPayload;
        s.refHeight = 250;
        if (withPayee) s.payee = payeeKey.GetPubKey().GetID();
        s.feeZat = FeeZat(vaultValue, params.feeMin, params.feeBps);
        s.yedInputs = yed;
        s.changeCents = change;
        s.changeScript = GetScriptForDestination(NewKey().GetPubKey().GetID());
        if (transparentDest) s.collateralScript = GetScriptForDestination(NewKey().GetPubKey().GetID());
        s.networkFee = DEFAULT_YELLOWBACK_FEE;
        return s;
    }

    BuiltTx Built(const VaultSpendShape& s, const VaultSpendPlan& plan) const
    {
        BuiltTx out;
        out.tx = Shell((uint32_t)(s.refHeight + REF_WINDOW));
        out.tx.nLockTime = plan.nLockTime;
        out.tx.vin = plan.vin;
        out.tx.vout = plan.vout;
        out.vaultScript = s.vaultScript;
        out.vaultValue = s.vaultValue;
        out.ownerPubKey = owner.GetPubKey();
        out.changeVout = plan.changeVout;
        for (const YedCoin& c : s.yedInputs) out.yedPrevs.push_back(std::make_pair(c.token.scriptPubKey, c.token.nValue));
        return out;
    }

    void AddKeys(CBasicKeyStore& ks) const
    {
        ks.AddKey(owner);
        ks.AddKey(yedKey);
    }
};

std::optional<FoundPayload> PayloadOf(const CMutableTransaction& mtx) { return FindPayload(CTransaction(mtx)); }

int OpReturns(const CMutableTransaction& mtx)
{
    int n = 0;
    for (const CTxOut& o : mtx.vout) if (!o.scriptPubKey.empty() && o.scriptPubKey[0] == OP_RETURN) n++;
    return n;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_txbuilder_tests, BasicTestingSetup)

// Rule: MINT-1 MINT-3 MINT-7 MINT-8
BOOST_AUTO_TEST_CASE(mint_layout_matches_3_5)
{
    Fixture f;
    MintShape s;
    s.cents = 100000;
    s.termClass = 0;
    s.lockHeight = f.lockHeight;
    s.claimHeight = f.claimHeight;
    s.refHeight = 250;
    s.owner = f.owner.GetPubKey();
    s.collateralZat = 251 * COIN;
    s.payee = f.payeeKey.GetPubKey().GetID();
    s.feeZat = FeeZat(s.collateralZat, f.params.feeMin, f.params.feeBps);
    int feeVout = -1;
    std::vector<CTxOut> vout = MintOutputs(s, feeVout);
    BOOST_REQUIRE_EQUAL(vout.size(), 4u);
    BOOST_CHECK_EQUAL(feeVout, 3);
    BOOST_CHECK(vout[0].scriptPubKey == P2SHScript(f.vaultScript));
    BOOST_CHECK_EQUAL(vout[0].nValue, 251 * COIN);
    BOOST_CHECK(vout[1].scriptPubKey == GetScriptForDestination(f.owner.GetPubKey().GetID()));
    BOOST_CHECK_EQUAL(vout[1].nValue, TOKEN_VALUE);
    BOOST_CHECK_EQUAL(vout[2].nValue, 0);
    BOOST_CHECK(vout[3].scriptPubKey == GetScriptForDestination(f.payeeKey.GetPubKey().GetID()));
    BOOST_CHECK_EQUAL(vout[3].nValue, s.feeZat);
    // Payload round-trip.
    CMutableTransaction mtx = Shell(290);
    mtx.vout = vout;
    std::optional<FoundPayload> fp = PayloadOf(mtx);
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL(fp->opReturnIndex, 2u);
    BOOST_CHECK(fp->payload.type == PayloadType::MINT);
    BOOST_CHECK_EQUAL(fp->payload.cents, 100000u);
    BOOST_CHECK_EQUAL(fp->payload.termClass, 0);
    BOOST_CHECK_EQUAL(fp->payload.lockHeight, f.lockHeight);
    BOOST_CHECK_EQUAL(fp->payload.refHeight, 250u);
    BOOST_CHECK_EQUAL((int)fp->payload.feeVout, 3);
    BOOST_CHECK(fp->payload.ownerPubKey == f.owner.GetPubKey());
    // FEE-0: no payee => three outputs and feeVout = 0xFF.
    s.payee = std::nullopt;
    vout = MintOutputs(s, feeVout);
    BOOST_CHECK_EQUAL(vout.size(), 3u);
    BOOST_CHECK_EQUAL(feeVout, -1);
    mtx.vout = vout;
    fp = PayloadOf(mtx);
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL((int)fp->payload.feeVout, (int)FEE_VOUT_NONE);
    // A lock that cannot be scripted is refused with the mint-bad-lock identifier.
    s.lockHeight = LOCKTIME_THRESHOLD;
    BOOST_CHECK_THROW(MintOutputs(s, feeVout), std::runtime_error);
}

// Rule: MINT-2 MINT-4 MINT-5 MINT-6 MINT-8
BOOST_AUTO_TEST_CASE(mint_shape_passes_the_state_machine)
{
    // The wallet's own template, evaluated by ProcessTx over a state whose reference snapshot is
    // ACTIVE with a price and one quote tag in the payee window: the verdict must be `ok`.
    Fixture f;
    MemoryStateView view;
    State st(view);
    const int R = 250;
    const MicroUsd pMint = 2000000;   // $2.00
    Snapshot S;
    S.activation.status = (uint8_t)ActivationStatus::ACTIVE;
    S.pFast = S.pMid = S.pSlow = S.pMint = S.pClaim = pMint;
    S.sigmaMultBps = 10000;
    S.haltMask = 0;
    st.Put(keys::Snapshot(R), S);
    TagRecord tag;
    tag.payoutKey = f.payeeKey.GetPubKey().GetID();
    tag.priceMicroUsd = pMint;
    tag.signal = true;
    st.Put(keys::Tag(R), tag);
    BOOST_REQUIRE_EQUAL(EligiblePayees(view, f.params, R).size(), 1u);

    MintShape s;
    s.cents = 100000;
    s.termClass = f.params.ClassForLockBlocks(48);
    BOOST_REQUIRE_EQUAL(s.termClass, 0);
    s.lockHeight = R + 48;
    s.claimHeight = s.lockHeight + f.params.grace;
    s.refHeight = R;
    s.owner = f.owner.GetPubKey();
    std::optional<CAmount> required = RequiredCollateralRounded(s.cents, MinRatioBps(f.params.baseRatioBps[0], S.sigmaMultBps), pMint);
    BOOST_REQUIRE(required.has_value());
    s.collateralZat = std::max(required.value(), 4 * f.params.feeMin);
    s.payee = DefaultPayee(view, f.params, R, std::vector<unsigned char>(s.owner.begin(), s.owner.end()), PayeePolicy::Defaults(f.params));
    BOOST_REQUIRE(s.payee.has_value());
    s.feeZat = FeeZat(s.collateralZat, f.params.feeMin, f.params.feeBps);
    int feeVout = -1;
    CMutableTransaction mtx = Shell(R + REF_WINDOW);
    mtx.vin.push_back(CTxIn(COutPoint(uint256S("11"), 0)));
    mtx.vout = MintOutputs(s, feeVout);
    mtx.vout.push_back(CTxOut(1 * COIN, GetScriptForDestination(NewKey().GetPubKey().GetID())));   // YEC change at vout[4]
    TxOutcome o = ProcessTx(st, f.params, CTransaction(mtx), R + 3);
    BOOST_CHECK(o.relevant);
    BOOST_CHECK_EQUAL(o.log.verdict, verdict::OK);
    BOOST_CHECK_EQUAL(o.log.yedOut, 100000);
    BOOST_CHECK_EQUAL(o.log.feeZat, s.feeZat);
    BOOST_CHECK(o.log.hasPayee);
    // A collateral one zat short is VOID (bad-mint-collateral) — the wallet rounds up, never down.
    mtx.vout[0].nValue = required.value() - 1;
    MemoryStateView view2 = view;
    State st2(view2);
    o = ProcessTx(st2, f.params, CTransaction(mtx), R + 3);
    BOOST_CHECK_EQUAL(o.log.verdict, verdict::BAD_MINT_COLLATERAL);
}

// Rule: RED-1 RED-2 RED-3 TPL-2
BOOST_AUTO_TEST_CASE(redeem_layout_transparent_destination)
{
    Fixture f;
    // Debt 100000 cents paid from two YED coins of 60000 and 50000 => change 10000.
    std::vector<YedCoin> yed = { Coin(uint256S("b1"), 1, 60000, f.yedKey), Coin(uint256S("b2"), 1, 50000, f.yedKey) };
    VaultSpendShape s = f.Shape(true, true, true, true, yed, 10000);
    VaultSpendPlan plan = PlanVaultSpend(s);
    BOOST_REQUIRE_EQUAL(plan.vin.size(), 3u);
    BOOST_CHECK(plan.vin[0].prevout == f.vaultOut);
    BOOST_CHECK_EQUAL(plan.vin[0].nSequence, 0xFFFFFFFEu);
    BOOST_CHECK_EQUAL(plan.nLockTime, f.lockHeight);
    BOOST_REQUIRE_EQUAL(plan.vout.size(), 4u);           // collateral, fee, change, payload
    BOOST_CHECK_EQUAL(plan.feeVout, 1);
    BOOST_CHECK_EQUAL(plan.changeVout, 2);
    BOOST_CHECK_EQUAL(plan.burnCents, 100000);
    BOOST_CHECK_EQUAL(plan.collateralOut, f.vaultValue + 2 * TOKEN_VALUE - DEFAULT_YELLOWBACK_FEE - s.feeZat - TOKEN_VALUE);
    BOOST_CHECK_EQUAL(plan.vout[0].nValue, plan.collateralOut);
    BOOST_CHECK(plan.vout[0].scriptPubKey == s.collateralScript.value());
    BOOST_CHECK_EQUAL(plan.vout[1].nValue, s.feeZat);
    BOOST_CHECK(plan.vout[1].scriptPubKey == GetScriptForDestination(f.payeeKey.GetPubKey().GetID()));
    BOOST_CHECK_EQUAL(plan.vout[2].nValue, TOKEN_VALUE);
    BOOST_CHECK(plan.vout[2].scriptPubKey == s.changeScript);
    BuiltTx b = f.Built(s, plan);
    BOOST_CHECK_EQUAL(b.tx.nExpiryHeight, (uint32_t)(s.refHeight + REF_WINDOW));
    std::optional<FoundPayload> fp = PayloadOf(b.tx);
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL(fp->opReturnIndex, 3u);
    BOOST_CHECK(fp->payload.type == PayloadType::REDEEM);
    BOOST_CHECK_EQUAL(fp->payload.refHeight, 250u);
    BOOST_CHECK_EQUAL((int)fp->payload.feeVout, 1);
    BOOST_REQUIRE_EQUAL(fp->payload.assignments.size(), 1u);
    BOOST_CHECK_EQUAL((int)fp->payload.assignments[0].vout, 2);
    BOOST_CHECK_EQUAL(fp->payload.assignments[0].cents, 10000u);
    // Without change and under FEE-0: collateral + payload only, feeVout = 0xFF, count = 0.
    VaultSpendShape s0 = f.Shape(true, true, true, false, { Coin(uint256S("b3"), 1, 100000, f.yedKey) });
    VaultSpendPlan p0 = PlanVaultSpend(s0);
    BOOST_REQUIRE_EQUAL(p0.vout.size(), 2u);
    BOOST_CHECK_EQUAL(p0.feeVout, -1);
    BOOST_CHECK_EQUAL(p0.changeVout, -1);
    BOOST_CHECK_EQUAL(p0.collateralOut, f.vaultValue + TOKEN_VALUE - DEFAULT_YELLOWBACK_FEE);
    BuiltTx b0 = f.Built(s0, p0);
    fp = PayloadOf(b0.tx);
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL((int)fp->payload.feeVout, (int)FEE_VOUT_NONE);
    BOOST_CHECK(fp->payload.assignments.empty());
    // A vault that cannot cover the fees is refused.
    VaultSpendShape tiny = f.Shape(true, true, true, true, {});
    tiny.vaultValue = tiny.feeZat;
    BOOST_CHECK_THROW(PlanVaultSpend(tiny), std::runtime_error);
}

// Rule: RED-3
BOOST_AUTO_TEST_CASE(redeem_layout_sapling_destination)
{
    // M13: YED change (if any) at vout[0], then the payload, then the fee output; feeVout names it
    // wherever it lands (vout[2] with change, vout[1] without); the collateral is the caller's note.
    Fixture f;
    std::vector<YedCoin> yed = { Coin(uint256S("b1"), 1, 110000, f.yedKey) };
    VaultSpendShape s = f.Shape(true, true, false, true, yed, 10000);
    VaultSpendPlan plan = PlanVaultSpend(s);
    BOOST_REQUIRE_EQUAL(plan.vout.size(), 3u);
    BOOST_CHECK_EQUAL(plan.changeVout, 0);
    BOOST_CHECK_EQUAL(plan.feeVout, 2);
    BOOST_CHECK_EQUAL(plan.vout[2].nValue, s.feeZat);
    BOOST_CHECK_EQUAL(plan.collateralOut, f.vaultValue + TOKEN_VALUE - DEFAULT_YELLOWBACK_FEE - s.feeZat - TOKEN_VALUE);
    BuiltTx b = f.Built(s, plan);
    std::optional<FoundPayload> fp = PayloadOf(b.tx);
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL(fp->opReturnIndex, 1u);
    BOOST_CHECK_EQUAL((int)fp->payload.feeVout, 2);
    BOOST_CHECK_EQUAL((int)fp->payload.assignments[0].vout, 0);
    // Without change: payload at vout[0], fee at vout[1].
    VaultSpendShape s1 = f.Shape(true, true, false, true, { Coin(uint256S("b2"), 1, 100000, f.yedKey) });
    VaultSpendPlan p1 = PlanVaultSpend(s1);
    BOOST_REQUIRE_EQUAL(p1.vout.size(), 2u);
    BOOST_CHECK_EQUAL(p1.feeVout, 1);
    BOOST_CHECK_EQUAL(p1.changeVout, -1);
    BuiltTx b1 = f.Built(s1, p1);
    fp = PayloadOf(b1.tx);
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL(fp->opReturnIndex, 0u);
    BOOST_CHECK_EQUAL((int)fp->payload.feeVout, 1);
    // A release to a Sapling address has no transparent output at all.
    VaultSpendShape rel = f.Shape(true, false, false, false, {});
    VaultSpendPlan pr = PlanVaultSpend(rel);
    BOOST_CHECK(pr.vout.empty());
    BOOST_CHECK_EQUAL(pr.collateralOut, f.vaultValue - DEFAULT_YELLOWBACK_FEE);
}

// Rule: RED-4
BOOST_AUTO_TEST_CASE(claim_layout)
{
    Fixture f;
    std::vector<YedCoin> yed = { Coin(uint256S("c1"), 1, 100000, f.yedKey) };
    VaultSpendShape s = f.Shape(false, true, true, true, yed);
    VaultSpendPlan plan = PlanVaultSpend(s);
    BOOST_CHECK_EQUAL(plan.nLockTime, f.claimHeight);
    BOOST_REQUIRE_EQUAL(plan.vout.size(), 3u);           // collateral, fee, payload
    BOOST_CHECK_EQUAL(plan.feeVout, 1);
    BOOST_CHECK_EQUAL(plan.burnCents, 100000);
    BuiltTx b = f.Built(s, plan);
    CBasicKeyStore ks;
    f.AddKeys(ks);
    SignVaultSpend(b, ks, f.branchId, false);
    BOOST_CHECK_EQUAL(b.path, "claim");
    BOOST_CHECK(b.tx.vin[0].scriptSig == ClaimScriptSig(f.vaultScript));
    std::optional<VaultSpendPath> path = ParseVaultSpendPath(b.tx.vin[0].scriptSig);
    BOOST_REQUIRE(path.has_value());
    BOOST_CHECK(!path->ownerPath);
    BOOST_CHECK_EQUAL(path->pushes, 2u);
    // The claim path verifies at claimHeight with no signature; the YED input is signed.
    ScriptError err;
    BOOST_CHECK_MESSAGE(Verify(b.tx, 0, P2SHScript(f.vaultScript), f.vaultValue, f.branchId, &err), ScriptErrorString(err));
    BOOST_CHECK(Verify(b.tx, 1, yed[0].token.scriptPubKey, TOKEN_VALUE, f.branchId, &err));
}

// Rule: RED-1 TPL-2
BOOST_AUTO_TEST_CASE(sign_vault_spend_owner_path)
{
    Fixture f;
    std::vector<YedCoin> yed = { Coin(uint256S("d1"), 1, 100000, f.yedKey) };
    VaultSpendShape s = f.Shape(true, true, true, true, yed);
    BuiltTx b = f.Built(s, PlanVaultSpend(s));
    CBasicKeyStore ks;
    f.AddKeys(ks);
    SignVaultSpend(b, ks, f.branchId, true);
    BOOST_CHECK_EQUAL(b.path, "owner");
    // Exactly the wallet's `<sig> OP_1 <script>` shape (TPL-2).
    std::optional<VaultSpendPath> path = ParseVaultSpendPath(b.tx.vin[0].scriptSig);
    BOOST_REQUIRE(path.has_value());
    BOOST_CHECK(path->ownerPath);
    BOOST_CHECK_EQUAL(path->pushes, 3u);
    BOOST_CHECK(path->selector == CScriptNum(1).getvch());
    BOOST_CHECK(path->vaultScript == f.vaultScript);
    BOOST_CHECK(b.tx.vin[0].scriptSig.IsPushOnly());
    // Verifies under the standard and the consensus flags with the right amount and branch id.
    ScriptError err;
    const CScript spk = P2SHScript(f.vaultScript);
    BOOST_CHECK_MESSAGE(Verify(b.tx, 0, spk, f.vaultValue, f.branchId, &err), ScriptErrorString(err));
    BOOST_CHECK(Verify(b.tx, 0, spk, f.vaultValue, f.branchId, &err, CONSENSUS_FLAGS));
    BOOST_CHECK(Verify(b.tx, 1, yed[0].token.scriptPubKey, TOKEN_VALUE, f.branchId, &err));
    // ZIP-243 binds the amount and the branch id (mapping §13.1): either wrong => the signature fails.
    BOOST_CHECK(!Verify(b.tx, 0, spk, f.vaultValue + 1, f.branchId, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_EVAL_FALSE);
    const uint32_t overwinter = NetworkUpgradeInfo[Consensus::UPGRADE_OVERWINTER].nBranchId;
    BOOST_CHECK(!Verify(b.tx, 0, spk, f.vaultValue, overwinter, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_EVAL_FALSE);
    // Signed under Overwinter's id it verifies there and not under Sapling's.
    BuiltTx b2 = f.Built(s, PlanVaultSpend(s));
    SignVaultSpend(b2, ks, overwinter, true);
    BOOST_CHECK(Verify(b2.tx, 0, spk, f.vaultValue, overwinter));
    BOOST_CHECK(!Verify(b2.tx, 0, spk, f.vaultValue, f.branchId));
    // The owner key must be in the keystore; the YED keys too.
    CBasicKeyStore onlyYed;
    onlyYed.AddKey(f.yedKey);
    BuiltTx b3 = f.Built(s, PlanVaultSpend(s));
    BOOST_CHECK_THROW(SignVaultSpend(b3, onlyYed, f.branchId, true), std::runtime_error);
    CBasicKeyStore onlyOwner;
    onlyOwner.AddKey(f.owner);
    BuiltTx b4 = f.Built(s, PlanVaultSpend(s));
    BOOST_CHECK_THROW(SignVaultSpend(b4, onlyOwner, f.branchId, true), std::runtime_error);
}

// Rule: RED-1 RED-2 RED-3 RED-4 MP-1
BOOST_AUTO_TEST_CASE(vault_spend_shapes_pass_the_state_machine)
{
    // The wallet's REDEEM and CLAIM, evaluated by ProcessTx over an index holding the ACTIVE
    // vault, the YED input and a snapshot at R: `ok` for both; and the SWEEP / release shapes
    // seen by the same machine: the sweep fails RED-1 (by design, L10), the release of a VOID
    // vault is an ordinary spend (K3).
    Fixture f;
    const int R = 250, H = 253;
    auto seed = [&](MemoryStateView& view, VaultStatus status, MicroUsd pClaim) {
        State st(view);
        VaultRecord v;
        v.ownerPubKey = std::vector<unsigned char>(f.owner.GetPubKey().begin(), f.owner.GetPubKey().end());
        v.termClass = 0;
        v.lockHeight = f.lockHeight;
        v.claimHeight = f.claimHeight;
        v.collateralZat = f.vaultValue;
        v.mintedCents = 10000;   // $100 against 5 YEC
        v.mintHeight = 200;
        v.refHeight = 197;
        v.status = (uint8_t)status;
        st.Put(keys::Vault(f.vaultOut), v);
        TokenRecord t;
        t.cents = 10000;
        t.nValue = TOKEN_VALUE;
        t.scriptPubKey = GetScriptForDestination(f.yedKey.GetPubKey().GetID());
        t.height = 210;
        st.Put(keys::Token(COutPoint(uint256S("e1"), 1)), t);
        Snapshot S;
        S.activation.status = (uint8_t)ActivationStatus::ACTIVE;
        S.pFast = S.pMid = S.pSlow = S.pMint = S.pClaim = pClaim;
        st.Put(keys::Snapshot(R), S);
        TagRecord tag;
        tag.payoutKey = f.payeeKey.GetPubKey().GetID();
        tag.priceMicroUsd = pClaim;
        st.Put(keys::Tag(R), tag);
        Totals totals;
        totals.supplyCents = 10000;
        totals.collateralZat = f.vaultValue;
        totals.activeVaults = 1;
        st.Put(keys::Totals(), totals);
    };
    std::vector<YedCoin> yed = { Coin(uint256S("e1"), 1, 10000, f.yedKey) };
    CBasicKeyStore ks;
    f.AddKeys(ks);

    // REDEEM (owner path): ok, vault CLOSED, burn = the debt, fee recorded.
    {
        MemoryStateView view;
        seed(view, VaultStatus::ACTIVE, 100000000);   // $100/YEC: 5 YEC = $500 >= $110
        State st(view);
        VaultSpendShape s = f.Shape(true, true, true, true, yed);
        BuiltTx b = f.Built(s, PlanVaultSpend(s));
        SignVaultSpend(b, ks, f.branchId, true);
        TxOutcome o = ProcessTx(st, f.params, CTransaction(b.tx), H);
        BOOST_CHECK(o.vaultSpend);
        BOOST_CHECK(!o.redFailed);
        BOOST_CHECK_EQUAL(o.log.verdict, verdict::OK);
        BOOST_CHECK_EQUAL(o.log.path, "owner");
        BOOST_CHECK_EQUAL(o.log.burned, 10000);
        BOOST_CHECK_EQUAL(o.log.feeZat, s.feeZat);
        BOOST_CHECK(st.GetVault(f.vaultOut)->Status() == VaultStatus::CLOSED);
        BOOST_CHECK(!st.GetVault(f.vaultOut)->unbacked);
    }
    // CLAIM at an underwater price ($2: 5 YEC = $10 < $110): ok, CLAIMED.
    {
        MemoryStateView view;
        seed(view, VaultStatus::ACTIVE, 2000000);
        State st(view);
        VaultSpendShape s = f.Shape(false, true, true, true, yed);
        BuiltTx b = f.Built(s, PlanVaultSpend(s));
        SignVaultSpend(b, ks, f.branchId, false);
        TxOutcome o = ProcessTx(st, f.params, CTransaction(b.tx), H);
        BOOST_CHECK_EQUAL(o.log.verdict, verdict::OK);
        BOOST_CHECK_EQUAL(o.log.path, "claim");
        BOOST_CHECK(st.GetVault(f.vaultOut)->Status() == VaultStatus::CLAIMED);
    }
    // CLAIM at a healthy price: RED-4 fails (what yed_claim's claim-not-underwater gate prevents).
    {
        MemoryStateView view;
        seed(view, VaultStatus::ACTIVE, 100000000);   // $100/YEC: 5 YEC = $500 >= $110
        State st(view);
        VaultSpendShape s = f.Shape(false, true, true, true, yed);
        BuiltTx b = f.Built(s, PlanVaultSpend(s));
        SignVaultSpend(b, ks, f.branchId, false);
        TxOutcome o = ProcessTx(st, f.params, CTransaction(b.tx), H);
        BOOST_CHECK(o.redFailed);
        BOOST_CHECK_EQUAL(o.log.verdict, verdict::VAULT_CLAIM_NOT_UNDERWATER);
    }
    // SWEEP of an ACTIVE vault: no payload => fails RED-1 by design; the vault closes unbacked.
    {
        MemoryStateView view;
        seed(view, VaultStatus::ACTIVE, 100000000);   // $100/YEC: 5 YEC = $500 >= $110
        State st(view);
        VaultSpendShape s = f.Shape(true, false, true, false, {});
        BuiltTx b = f.Built(s, PlanVaultSpend(s));
        SignVaultSpend(b, ks, f.branchId, true);
        TxOutcome o = ProcessTx(st, f.params, CTransaction(b.tx), H);
        BOOST_CHECK(o.redFailed);
        BOOST_CHECK_EQUAL(o.log.verdict, verdict::VAULT_SPEND_MALFORMED);
        BOOST_CHECK(st.GetVault(f.vaultOut)->Status() == VaultStatus::CLOSED);
        BOOST_CHECK(st.GetVault(f.vaultOut)->unbacked);
        BOOST_CHECK_EQUAL(st.GetTotals().unbackedCents, 10000);
    }
    // VOID release: the same shape on a VOID vault is an ordinary spend (K3): closed, not unbacked, nothing burned.
    {
        MemoryStateView view;
        seed(view, VaultStatus::VOID, 100000000);
        State st(view);
        VaultSpendShape s = f.Shape(true, false, true, false, {});
        BuiltTx b = f.Built(s, PlanVaultSpend(s));
        SignVaultSpend(b, ks, f.branchId, true);
        TxOutcome o = ProcessTx(st, f.params, CTransaction(b.tx), H);
        BOOST_CHECK(!o.vaultSpend);
        BOOST_CHECK(!o.redFailed);
        BOOST_CHECK(st.GetVault(f.vaultOut)->Status() == VaultStatus::CLOSED);
        BOOST_CHECK(!st.GetVault(f.vaultOut)->unbacked);
        BOOST_CHECK_EQUAL(st.GetVault(f.vaultOut)->burnedCents, 0);
    }
}

// Rule: RED-1
BOOST_AUTO_TEST_CASE(void_release_has_no_payload_and_no_fee)
{
    // L14: the VOID release is the owner-path spend with one output, no OP_RETURN, no fee output,
    // nLockTime = lockHeight, nSequence non-final, burnedCents = 0.
    Fixture f;
    VaultSpendShape s = f.Shape(true, false, true, true, {});   // a payee is ignored without a payload
    VaultSpendPlan plan = PlanVaultSpend(s);
    BOOST_REQUIRE_EQUAL(plan.vin.size(), 1u);
    BOOST_CHECK_EQUAL(plan.vin[0].nSequence, 0xFFFFFFFEu);
    BOOST_CHECK_EQUAL(plan.nLockTime, f.lockHeight);
    BOOST_REQUIRE_EQUAL(plan.vout.size(), 1u);
    BOOST_CHECK_EQUAL(plan.feeVout, -1);
    BOOST_CHECK_EQUAL(plan.changeVout, -1);
    BOOST_CHECK_EQUAL(plan.burnCents, 0);
    BOOST_CHECK_EQUAL(plan.collateralOut, f.vaultValue - DEFAULT_YELLOWBACK_FEE);
    BuiltTx b = f.Built(s, plan);
    BOOST_CHECK_EQUAL(OpReturns(b.tx), 0);
    BOOST_CHECK(!PayloadOf(b.tx).has_value());
    CBasicKeyStore ks;
    f.AddKeys(ks);
    SignVaultSpend(b, ks, f.branchId, true);
    BOOST_CHECK(b.ownYedOutputs.empty());
    ScriptError err;
    BOOST_CHECK_MESSAGE(Verify(b.tx, 0, P2SHScript(f.vaultScript), f.vaultValue, f.branchId, &err), ScriptErrorString(err));
}

// Rule: RED-1 MP-1
BOOST_AUTO_TEST_CASE(sweep_has_no_burn_no_fee_no_payload)
{
    // L10: the SWEEP is the same shape as the release on an ACTIVE vault: no YED input, no fee,
    // no payload; it fails RED-1 on purpose and is admitted only under abandonment (L13).
    Fixture f;
    VaultSpendShape s = f.Shape(true, false, true, true, {});
    VaultSpendPlan plan = PlanVaultSpend(s);
    BOOST_CHECK_EQUAL(plan.vin.size(), 1u);
    BOOST_CHECK_EQUAL(plan.vout.size(), 1u);
    BOOST_CHECK_EQUAL(plan.feeVout, -1);
    BOOST_CHECK_EQUAL(plan.burnCents, 0);
    BuiltTx b = f.Built(s, plan);
    BOOST_CHECK_EQUAL(OpReturns(b.tx), 0);
    for (const CTxOut& o : b.tx.vout) BOOST_CHECK(o.scriptPubKey != GetScriptForDestination(f.payeeKey.GetPubKey().GetID()));
    CBasicKeyStore ks;
    f.AddKeys(ks);
    SignVaultSpend(b, ks, f.branchId, true);
    BOOST_CHECK(Verify(b.tx, 0, P2SHScript(f.vaultScript), f.vaultValue, f.branchId));
    BOOST_CHECK(Verify(b.tx, 0, P2SHScript(f.vaultScript), f.vaultValue, f.branchId, nullptr, CONSENSUS_FLAGS));
}

// Rule: FEE-W
BOOST_AUTO_TEST_CASE(outpoint_selector_is_the_serialised_outpoint)
{
    // The REDEEM/CLAIM selector is the 36-byte serialised vault outpoint (§3.7): txid bytes then LE index.
    COutPoint o(uint256S("0102"), 7);
    std::vector<unsigned char> sel = OutPointSelector(o);
    BOOST_REQUIRE_EQUAL(sel.size(), 36u);
    BOOST_CHECK(std::vector<unsigned char>(sel.begin(), sel.begin() + 32) == std::vector<unsigned char>(o.hash.begin(), o.hash.end()));
    BOOST_CHECK_EQUAL((int)sel[32], 7);
    BOOST_CHECK_EQUAL((int)sel[33], 0);
}

BOOST_AUTO_TEST_SUITE_END()
