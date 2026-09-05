// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "ydollar/params.h"
#include "ydollar/payload.h"
#include "ydollar/script.h"

#include "chainparams.h"
#include "coins.h"
#include "consensus/upgrades.h"
#include "key.h"
#include "keystore.h"
#include "main.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"
#include "utiltest.h"

#include <boost/test/unit_test.hpp>

using namespace ydollar;

namespace {

struct Federation
{
    std::vector<CKey> keys;      // private keys in roster (sorted) order
    Roster roster;
    CScript rosterScript;

    Federation(unsigned int k, unsigned int n)
    {
        std::vector<CKey> unsorted;
        std::vector<CPubKey> pubs;
        for (unsigned int i = 0; i < n; i++) {
            CKey key;
            key.MakeNewKey(true);
            unsorted.push_back(key);
            pubs.push_back(key.GetPubKey());
        }
        std::vector<CPubKey> sorted = SortKeys(pubs);
        for (const CPubKey& p : sorted) {
            for (const CKey& key : unsorted) {
                if (key.GetPubKey() == p) keys.push_back(key);
            }
        }
        rosterScript = RosterScript(k, sorted);
        BOOST_REQUIRE(ParseRosterScript(rosterScript, roster));
    }
};

/** A Sapling-format transaction spending vaultValue from a P2SH(vaultScript) output at input 0. */
CMutableTransaction SpendingTx(const CScript& vaultScript, uint32_t lockHeight, CAmount vaultValue, uint32_t nLockTime)
{
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    mtx.nVersion = SAPLING_TX_VERSION;
    mtx.nExpiryHeight = lockHeight + 40;
    mtx.nLockTime = nLockTime;
    mtx.vin.push_back(CTxIn(COutPoint(uint256S("11"), 0), CScript(), 0xFFFFFFFE));
    mtx.vout.push_back(CTxOut(vaultValue - DEFAULT_YD_FEE, GetScriptForDestination(CKeyID(uint160(std::vector<unsigned char>(20, 7))))));
    return mtx;
}

valtype Sign(const CKey& key, const CScript& scriptCode, const CMutableTransaction& mtx, unsigned int nIn, CAmount amount, uint32_t branchId)
{
    uint256 hash = SignatureHash(scriptCode, CTransaction(mtx), nIn, SIGHASH_ALL, amount, branchId);
    valtype sig;
    BOOST_REQUIRE(key.Sign(hash, sig));
    sig.push_back((unsigned char)SIGHASH_ALL);
    return sig;
}

bool Verify(const CMutableTransaction& mtx, const CScript& scriptPubKey, CAmount amount, uint32_t branchId, ScriptError* err = nullptr)
{
    ScriptError e = SCRIPT_ERR_OK;
    bool ok = VerifyScript(mtx.vin[0].scriptSig, scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS,
                           MutableTransactionSignatureChecker(&mtx, 0, amount), branchId, &e);
    if (err) *err = e;
    return ok;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(ydollar_script_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(roster_script_roundtrip)
{
    Federation fed(5, 9);
    BOOST_CHECK_EQUAL(fed.roster.k, 5u);
    BOOST_CHECK_EQUAL(fed.roster.n(), 9u);
    BOOST_CHECK_EQUAL(fed.rosterScript.size(), 1u + 9 * 34 + 1 + 1);
    // Keys sorted ascending.
    for (size_t i = 1; i < fed.roster.keys.size(); i++) {
        BOOST_CHECK(std::lexicographical_compare(fed.roster.keys[i - 1].begin(), fed.roster.keys[i - 1].end(),
                                                 fed.roster.keys[i].begin(), fed.roster.keys[i].end()));
    }
    // It is a standard TX_MULTISIG template (what signrawtransaction's combiner needs).
    txnouttype type;
    std::vector<valtype> solutions;
    BOOST_CHECK(Solver(fed.rosterScript, type, solutions));
    BOOST_CHECK(type == TX_MULTISIG);
    BOOST_CHECK_EQUAL(ScriptSigArgsExpected(type, solutions), 6);
    // GetScriptForMultisig produces the identical script for the same key order.
    BOOST_CHECK(GetScriptForMultisig(5, fed.roster.keys) == fed.rosterScript);

    // Bounds: k >= 1, k <= n, n <= 13, compressed keys only.
    std::vector<CPubKey> pubs = fed.roster.keys;
    BOOST_CHECK(RosterScript(0, pubs).empty());
    BOOST_CHECK(RosterScript(10, pubs).empty());
    Federation big(7, 13);
    BOOST_CHECK(!big.rosterScript.empty());
    {
        std::vector<CPubKey> fourteenSorted;
        for (int i = 0; i < 14; i++) { CKey k; k.MakeNewKey(true); fourteenSorted.push_back(k.GetPubKey()); }
        BOOST_CHECK(RosterScript(7, SortKeys(fourteenSorted)).empty()); // n = 14 exceeds ROSTER_MAX_N
    }
    CKey unc;
    unc.MakeNewKey(false);
    std::vector<CPubKey> withUncompressed = pubs;
    withUncompressed[0] = unc.GetPubKey();
    BOOST_CHECK(RosterScript(5, withUncompressed).empty());
    Roster r;
    BOOST_CHECK(!ParseRosterScript(GetScriptForMultisig(1, { unc.GetPubKey() }), r)); // D7
    // 14-of-14 built by Ycash's own helper is a valid multisig but not a roster.
    std::vector<CPubKey> fourteen;
    for (int i = 0; i < 14; i++) { CKey k; k.MakeNewKey(true); fourteen.push_back(k.GetPubKey()); }
    BOOST_CHECK(!ParseRosterScript(GetScriptForMultisig(14, fourteen), r));
    // Not a multisig at all.
    BOOST_CHECK(!ParseRosterScript(CScript() << OP_TRUE, r));
    BOOST_CHECK(!ParseRosterScript(GetScriptForDestination(pubs[0].GetID()), r));
}

BOOST_AUTO_TEST_CASE(vault_script_sizes_and_sigops)
{
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    CPubKey owner = ownerKey.GetPubKey();

    // n = 13, 3-byte height push (heights < 8,388,608): 44 + 34n = 486.
    Federation f13(13, 13);
    CScript v3 = VaultScript(1000000, owner, f13.roster);
    BOOST_REQUIRE(!v3.empty());
    BOOST_CHECK_EQUAL(v3.size(), 44u + 34 * 13);
    BOOST_CHECK_EQUAL(v3.size(), VaultScriptSize(13, 3));
    BOOST_CHECK(v3.size() <= MAX_SCRIPT_ELEMENT_SIZE);
    BOOST_CHECK_EQUAL(v3.GetSigOpCount(true), 14u);

    // n = 13, 4-byte height push (>= 8,388,608): 45 + 34n = 487.
    CScript v4 = VaultScript(9000000, owner, f13.roster);
    BOOST_REQUIRE(!v4.empty());
    BOOST_CHECK_EQUAL(v4.size(), 45u + 34 * 13);
    BOOST_CHECK(v4.size() <= MAX_SCRIPT_ELEMENT_SIZE);
    BOOST_CHECK_EQUAL(v4.GetSigOpCount(true), 14u);
    BOOST_CHECK(v4.GetSigOpCount(true) <= MAX_P2SH_SIGOPS);

    // v1 roster 5-of-9: 350 bytes, 10 sigops (D3).
    Federation f9(5, 9);
    CScript v9 = VaultScript(1000000, owner, f9.roster);
    BOOST_CHECK_EQUAL(v9.size(), 350u);
    BOOST_CHECK_EQUAL(v9.GetSigOpCount(true), 10u);

    // Parse back.
    uint32_t lock;
    CPubKey o;
    Roster r;
    BOOST_REQUIRE(ParseVaultScript(v4, lock, o, r));
    BOOST_CHECK_EQUAL(lock, 9000000u);
    BOOST_CHECK(o == owner);
    BOOST_CHECK_EQUAL(r.k, 13u);
    BOOST_CHECK(r.keys == f13.roster.keys);
    BOOST_CHECK(VaultScript(lock, o, r) == v4);
    BOOST_CHECK(VaultScript(9000000, owner, f13.rosterScript) == v4);

    // Small heights encode as OP_N and still parse.
    CScript v16 = VaultScript(16, owner, f9.roster);
    BOOST_REQUIRE(ParseVaultScript(v16, lock, o, r));
    BOOST_CHECK_EQUAL(lock, 16u);
    CScript v17 = VaultScript(17, owner, f9.roster);
    BOOST_REQUIRE(ParseVaultScript(v17, lock, o, r));
    BOOST_CHECK_EQUAL(lock, 17u);

    // Invalid arguments.
    BOOST_CHECK(VaultScript(0, owner, f9.roster).empty());
    BOOST_CHECK(VaultScript(LOCKTIME_THRESHOLD, owner, f9.roster).empty());
    BOOST_CHECK(VaultScript(1000, CPubKey(), f9.roster).empty());
    CKey unc;
    unc.MakeNewKey(false);
    BOOST_CHECK(VaultScript(1000, unc.GetPubKey(), f9.roster).empty());
    // Not a vault script.
    BOOST_CHECK(!ParseVaultScript(f9.rosterScript, lock, o, r));
    BOOST_CHECK(!ParseVaultScript(CScript() << OP_TRUE, lock, o, r));
    // Non-minimal height push is rejected.
    {
        CScript bad;
        bad << std::vector<unsigned char>{ 0x10, 0x27, 0x00, 0x00 } << OP_CHECKLOCKTIMEVERIFY << OP_DROP
            << valtype(owner.begin(), owner.end()) << OP_CHECKSIGVERIFY;
        bad += f9.rosterScript;
        BOOST_CHECK(!ParseVaultScript(bad, lock, o, r));
    }
}

BOOST_AUTO_TEST_CASE(vault_scriptsig_roundtrip)
{
    Federation fed(2, 3);
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    CScript vault = VaultScript(500, ownerKey.GetPubKey(), fed.roster);
    valtype a(70, 0xAA), b(71, 0xBB), o(72, 0xCC);

    CScript sig = BuildVaultScriptSig({ a, b }, o, vault);
    std::vector<valtype> q;
    valtype os;
    CScript vs;
    BOOST_REQUIRE(ParseVaultScriptSig(sig, q, os, vs));
    BOOST_CHECK_EQUAL(q.size(), 2u);
    BOOST_CHECK(q[0] == a);
    BOOST_CHECK(q[1] == b);
    BOOST_CHECK(os == o);
    BOOST_CHECK(vs == vault);
    BOOST_CHECK(sig.IsPushOnly());

    // Owner-only (what yd_redeem produces before co-signing).
    sig = BuildVaultScriptSig({}, o, vault);
    BOOST_REQUIRE(ParseVaultScriptSig(sig, q, os, vs));
    BOOST_CHECK(q.empty());
    BOOST_CHECK(os == o);
    // Nothing signed.
    sig = BuildVaultScriptSig({}, valtype(), vault);
    BOOST_REQUIRE(ParseVaultScriptSig(sig, q, os, vs));
    BOOST_CHECK(q.empty());
    BOOST_CHECK(os.empty());
    BOOST_CHECK(vs == vault);

    CScript redeem;
    BOOST_REQUIRE(ExtractRedeemScript(sig, redeem));
    BOOST_CHECK(redeem == vault);
    BOOST_CHECK(!ExtractRedeemScript(CScript(), redeem));
    BOOST_CHECK(!ExtractRedeemScript(CScript() << OP_DUP, redeem));
    // Missing leading OP_0 / too few pushes.
    BOOST_CHECK(!ParseVaultScriptSig(CScript() << o << valtype(vault.begin(), vault.end()), q, os, vs));
    BOOST_CHECK(!ParseVaultScriptSig(CScript() << OP_0, q, os, vs));
}

BOOST_AUTO_TEST_CASE(vault_spend_verifies)
{
    RegtestActivateSapling();
    const uint32_t branchId = NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId;
    const uint32_t lockHeight = 120000;
    const CAmount vaultValue = 5 * COIN;

    Federation fed(5, 9);
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    CScript vault = VaultScript(lockHeight, ownerKey.GetPubKey(), fed.roster);
    CScript spk = P2SHScript(vault);

    auto build = [&](uint32_t nLockTime, const std::vector<int>& signerIdx, const CKey& ownerSigner, const CScript& scriptInSig) {
        CMutableTransaction mtx = SpendingTx(vault, lockHeight, vaultValue, nLockTime);
        valtype ownerSig = Sign(ownerSigner, vault, mtx, 0, vaultValue, branchId);
        std::vector<valtype> qsigs;
        for (int i : signerIdx) qsigs.push_back(Sign(fed.keys[i], vault, mtx, 0, vaultValue, branchId));
        mtx.vin[0].scriptSig = BuildVaultScriptSig(qsigs, ownerSig, scriptInSig);
        return mtx;
    };

    // Success: at lockHeight, owner + 5 roster keys in roster order.
    {
        CMutableTransaction ok = build(lockHeight, { 0, 2, 4, 6, 8 }, ownerKey, vault);
        ScriptError err;
        BOOST_CHECK_MESSAGE(Verify(ok, spk, vaultValue, branchId, &err), ScriptErrorString(err));
        BOOST_CHECK(ok.vin[0].scriptSig.size() < 1650);
        BOOST_CHECK(ok.vin[0].scriptSig.IsPushOnly());
    }
    // Success with a different subset and after lockHeight.
    {
        CMutableTransaction ok = build(lockHeight + 1000, { 1, 2, 3, 4, 5 }, ownerKey, vault);
        BOOST_CHECK(Verify(ok, spk, vaultValue, branchId));
    }
    // Failure 1: early (nLockTime below lockHeight) => CLTV.
    {
        CMutableTransaction early = build(lockHeight - 1, { 0, 2, 4, 6, 8 }, ownerKey, vault);
        ScriptError err;
        BOOST_CHECK(!Verify(early, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
    }
    // Failure 2: owner signature from the wrong key => CHECKSIGVERIFY.
    {
        CKey other;
        other.MakeNewKey(true);
        CMutableTransaction bad = build(lockHeight, { 0, 2, 4, 6, 8 }, other, vault);
        ScriptError err;
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CHECKSIGVERIFY);
    }
    // Failure 3: only k-1 quorum signatures.
    {
        CMutableTransaction bad = build(lockHeight, { 0, 2, 4, 6 }, ownerKey, vault);
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId));
    }
    // Failure 4: wrong roster (5 signatures from keys that are not in the roster).
    {
        Federation other(5, 9);
        CMutableTransaction mtx = SpendingTx(vault, lockHeight, vaultValue, lockHeight);
        valtype ownerSig = Sign(ownerKey, vault, mtx, 0, vaultValue, branchId);
        std::vector<valtype> qsigs;
        for (int i = 0; i < 5; i++) qsigs.push_back(Sign(other.keys[i], vault, mtx, 0, vaultValue, branchId));
        mtx.vin[0].scriptSig = BuildVaultScriptSig(qsigs, ownerSig, vault);
        ScriptError err;
        BOOST_CHECK(!Verify(mtx, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_EVAL_FALSE);
    }
    // Failure 5: signatures out of roster order (CHECKMULTISIG requires order).
    {
        CMutableTransaction bad = build(lockHeight, { 8, 6, 4, 2, 0 }, ownerKey, vault);
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId));
    }
    // Failure 6: a different redeem script in the scriptSig does not match the P2SH hash.
    {
        CScript otherVault = VaultScript(lockHeight + 1, ownerKey.GetPubKey(), fed.roster);
        CMutableTransaction bad = build(lockHeight, { 0, 2, 4, 6, 8 }, ownerKey, otherVault);
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId));
    }
    // Failure 7: final sequence number disables CLTV.
    {
        CMutableTransaction bad = build(lockHeight, { 0, 2, 4, 6, 8 }, ownerKey, vault);
        bad.vin[0].nSequence = 0xFFFFFFFF;
        // Re-sign, since nSequence is committed.
        valtype ownerSig = Sign(ownerKey, vault, bad, 0, vaultValue, branchId);
        std::vector<valtype> qsigs;
        for (int i : { 0, 2, 4, 6, 8 }) qsigs.push_back(Sign(fed.keys[i], vault, bad, 0, vaultValue, branchId));
        bad.vin[0].scriptSig = BuildVaultScriptSig(qsigs, ownerSig, vault);
        ScriptError err;
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
    }
    RegtestDeactivateSapling();
}

// B5: regtest never runs IsStandardTx / AreInputsStandard, so every template is
// pinned here. Fixture: regtest with Sapling active (D6), height 1 (C19).
BOOST_AUTO_TEST_CASE(templates_are_standard)
{
    RegtestActivateSapling();
    const uint32_t branchId = NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId;
    const int height = 1;
    LOCK(cs_main);

    Federation fed(13, 13); // worst case for REDEEM: k = n = 13
    CKey ownerKey, userKey, refillKey;
    ownerKey.MakeNewKey(true);
    userKey.MakeNewKey(true);
    refillKey.MakeNewKey(true);
    CBasicKeyStore keystore;
    keystore.AddKey(userKey);
    keystore.AddKey(refillKey);
    const CScript userP2PKH = GetScriptForDestination(userKey.GetPubKey().GetID());
    const CScript refillP2PKH = GetScriptForDestination(refillKey.GetPubKey().GetID());
    const uint32_t lockHeight = 9000000;
    const CScript vault = VaultScript(lockHeight, ownerKey.GetPubKey(), fed.roster);
    const CScript vaultSpk = P2SHScript(vault);
    const CScript anchorSpk = P2SHScript(fed.rosterScript);

    // Funding transaction providing every kind of input.
    CMutableTransaction fund;
    fund.fOverwintered = true;
    fund.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    fund.nVersion = SAPLING_TX_VERSION;
    fund.vout.push_back(CTxOut(10 * COIN, userP2PKH));      // 0: YEC input for a mint
    fund.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));    // 1: a YDollar token output
    fund.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));    // 2: another
    fund.vout.push_back(CTxOut(5 * COIN, vaultSpk));        // 3: a vault
    fund.vout.push_back(CTxOut(COIN, anchorSpk));           // 4: the anchor
    fund.vout.push_back(CTxOut(COIN / 2, refillP2PKH));     // 5: a refill coin
    CCoinsView dummy;
    CCoinsViewCache view(&dummy);
    view.ModifyCoins(fund.GetHash())->FromTx(fund, height);
    const uint256 fundHash = fund.GetHash();

    auto newTx = [&](uint32_t nLockTime) {
        CMutableTransaction mtx;
        mtx.fOverwintered = true;
        mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
        mtx.nVersion = SAPLING_TX_VERSION;
        mtx.nExpiryHeight = 100;
        mtx.nLockTime = nLockTime;
        return mtx;
    };
    auto checkStandard = [&](CMutableTransaction& mtx, const char* what) {
        std::string reason;
        BOOST_CHECK_MESSAGE(IsStandardTx(CTransaction(mtx), reason, ::Params(), height), std::string(what) + ": " + reason);
        BOOST_CHECK_MESSAGE(AreInputsStandard(CTransaction(mtx), view, branchId), std::string(what) + ": inputs");
    };

    // MINT: YEC input, vault P2SH, token P2PKH, OP_RETURN, change.
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 0)));
        mtx.vout.push_back(CTxOut(5 * COIN, vaultSpk));
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Mint(0, 10000, lockHeight, 1, ownerKey.GetPubKey())))));
        mtx.vout.push_back(CTxOut(5 * COIN - TOKEN_VALUE - DEFAULT_YD_FEE, userP2PKH));
        BOOST_REQUIRE(SignSignature(keystore, userP2PKH, mtx, 0, 10 * COIN, SIGHASH_ALL, branchId));
        checkStandard(mtx, "MINT");
    }
    // TRANSFER: two token inputs + YEC fee input, 15 assignments (max), OP_RETURN of 80 bytes, change.
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 1)));
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 2)));
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 0)));
        std::vector<Assignment> as;
        for (uint8_t i = 0; i < 15; i++) {
            mtx.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));
            as.push_back(Assignment(i, 100));
        }
        std::vector<unsigned char> data = EncodePayload(Payload::Transfer(as));
        BOOST_REQUIRE_EQUAL(data.size(), 80u);
        mtx.vout.push_back(CTxOut(0, PayloadScript(data)));
        mtx.vout.push_back(CTxOut(COIN, userP2PKH));
        for (unsigned int i = 0; i < 3; i++) {
            BOOST_REQUIRE(SignSignature(keystore, userP2PKH, mtx, i, fund.vout[mtx.vin[i].prevout.n].nValue, SIGHASH_ALL, branchId));
        }
        checkStandard(mtx, "TRANSFER");
    }
    // REDEEM: vault input (k = n = 13) + token input, no YEC input (C10); collateral out, change token, OP_RETURN.
    {
        CMutableTransaction mtx = newTx(lockHeight);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 3), CScript(), 0xFFFFFFFE));
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 1)));
        mtx.vout.push_back(CTxOut(5 * COIN + TOKEN_VALUE - DEFAULT_YD_FEE - TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Redeem({ Assignment(1, 100) })))));
        valtype ownerSig = Sign(ownerKey, vault, mtx, 0, 5 * COIN, branchId);
        std::vector<valtype> qsigs;
        for (int i = 0; i < 13; i++) qsigs.push_back(Sign(fed.keys[i], vault, mtx, 0, 5 * COIN, branchId));
        mtx.vin[0].scriptSig = BuildVaultScriptSig(qsigs, ownerSig, vault);
        BOOST_REQUIRE(SignSignature(keystore, userP2PKH, mtx, 1, TOKEN_VALUE, SIGHASH_ALL, branchId));
        BOOST_CHECK(mtx.vin[0].scriptSig.size() <= 1650);
        checkStandard(mtx, "REDEEM");
        // And the vault input actually verifies under the standard flags.
        ScriptError err;
        BOOST_CHECK_MESSAGE(VerifyScript(mtx.vin[0].scriptSig, vaultSpk, STANDARD_SCRIPT_VERIFY_FLAGS,
                                         MutableTransactionSignatureChecker(&mtx, 0, 5 * COIN), branchId, &err),
                            ScriptErrorString(err));
    }
    // PRICE with a refill (C6): anchor input + refill input; anchor out (same script) + OP_RETURN. Exactly two outputs.
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 4)));
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 5)));
        mtx.vout.push_back(CTxOut(COIN + COIN / 2 - DEFAULT_YD_FEE, anchorSpk));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(50000)))));
        // Anchor: OP_0 <13 sigs> <rosterScript>, i.e. what signrawtransaction's combiner produces.
        std::vector<valtype> sigs;
        for (int i = 0; i < 13; i++) sigs.push_back(Sign(fed.keys[i], fed.rosterScript, mtx, 0, COIN, branchId));
        CScript anchorSig;
        anchorSig << OP_0;
        for (const valtype& s : sigs) anchorSig << s;
        anchorSig << valtype(fed.rosterScript.begin(), fed.rosterScript.end());
        mtx.vin[0].scriptSig = anchorSig;
        BOOST_REQUIRE(SignSignature(keystore, refillP2PKH, mtx, 1, COIN / 2, SIGHASH_ALL, branchId));
        checkStandard(mtx, "PRICE");
        ScriptError err;
        BOOST_CHECK_MESSAGE(VerifyScript(mtx.vin[0].scriptSig, anchorSpk, STANDARD_SCRIPT_VERIFY_FLAGS,
                                         MutableTransactionSignatureChecker(&mtx, 0, COIN), branchId, &err),
                            ScriptErrorString(err));
    }
    // Negative control: two OP_RETURN outputs are non-standard ("multi-op-return").
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 0)));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(1)))));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Price(2)))));
        std::string reason;
        BOOST_CHECK(!IsStandardTx(CTransaction(mtx), reason, ::Params(), height));
        BOOST_CHECK_EQUAL(reason, "multi-op-return");
    }
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_SUITE_END()
