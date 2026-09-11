// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// The v2 vault script (plan §3.4, V7): sizes and sigops, both spend paths
// under VerifyScript with STANDARD_SCRIPT_VERIFY_FLAGS and with the consensus
// flags ConnectBlock uses (P2SH | CLTV, ref/ycash/src/main.cpp:2931), the K4
// selector cases, and the standardness of every §3.5 template through
// IsStandardTx / AreInputsStandard (regtest sets fRequireStandard = false, so
// functional tests never execute them).

#include "yellowback/math.h"
#include "yellowback/params.h"
#include "yellowback/payload.h"
#include "yellowback/script.h"

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
#include "transaction_builder.h"
#include "utiltest.h"

#include <boost/test/unit_test.hpp>

#include <functional>

using namespace yellowback;

namespace {

const unsigned int CONSENSUS_FLAGS = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;   // main.cpp:2931

/** A Sapling-format transaction spending vaultValue from a P2SH(vaultScript) output at input 0. */
CMutableTransaction SpendingTx(CAmount vaultValue, uint32_t nLockTime, uint32_t nExpiryHeight = 1000)
{
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    mtx.nVersion = SAPLING_TX_VERSION;
    mtx.nExpiryHeight = nExpiryHeight;
    mtx.nLockTime = nLockTime;
    mtx.vin.push_back(CTxIn(COutPoint(uint256S("11"), 0), CScript(), 0xFFFFFFFE));
    mtx.vout.push_back(CTxOut(vaultValue - DEFAULT_YELLOWBACK_FEE, GetScriptForDestination(CKeyID(uint160(std::vector<unsigned char>(20, 7))))));
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

bool Verify(const CMutableTransaction& mtx, const CScript& scriptPubKey, CAmount amount, uint32_t branchId, ScriptError* err = nullptr,
            unsigned int flags = STANDARD_SCRIPT_VERIFY_FLAGS)
{
    ScriptError e = SCRIPT_ERR_OK;
    bool ok = VerifyScript(mtx.vin[0].scriptSig, scriptPubKey, flags, MutableTransactionSignatureChecker(&mtx, 0, amount), branchId, &e);
    if (err) *err = e;
    return ok;
}

CKey NewKey(bool compressed = true)
{
    CKey k;
    k.MakeNewKey(compressed);
    return k;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_script_tests, BasicTestingSetup)

// Rule: RED-1
// Rule: MINT-3
BOOST_AUTO_TEST_CASE(mint3_vault_script_sizes_sigops_and_parse)
{
    const CKey owner = NewKey();
    const CPubKey pub = owner.GetPubKey();
    const uint32_t grace = MainParams().grace;   // 34,560

    // 3-byte height pushes: 51 bytes; 4-byte: 53; mixed: 52.
    struct SizeCase { uint32_t lock; uint32_t claim; size_t size; };
    const std::vector<SizeCase> sizes = {
        { 120000, 120000 + grace, 51 },
        { 9000000, 9000000 + grace, 53 },
        { 8388607, 8388607 + grace, 52 },       // 0x7FFFFF is 3 bytes, +grace is 4
        { 17, 18, 47 },                         // 2-byte pushes (0x11): the arithmetic minimum on regtest-sized heights
        { 1, 2, 45 },                           // OP_1 / OP_2
        { LOCKTIME_THRESHOLD - 2, LOCKTIME_THRESHOLD - 1, 53 },
    };
    for (const SizeCase& c : sizes) {
        CScript vault = VaultScript(c.lock, pub, c.claim);
        BOOST_REQUIRE_MESSAGE(!vault.empty(), "lock " + std::to_string(c.lock));
        BOOST_CHECK_EQUAL(vault.size(), c.size);
        BOOST_CHECK_EQUAL(vault.GetSigOpCount(true), 1u);     // one CHECKSIG
        BOOST_CHECK_EQUAL(vault.GetSigOpCount(false), 1u);
        // Counted through P2SH the way AreInputsStandard / the block sigop limit see it.
        CScript spk = P2SHScript(vault);
        BOOST_CHECK_EQUAL(spk.GetSigOpCount(ClaimScriptSig(vault)), 1u);
        BOOST_CHECK(spk.IsPayToScriptHash());
        BOOST_CHECK_EQUAL(spk.size(), 23u);
        // Round trip.
        uint32_t lock = 0, claim = 0;
        CPubKey parsed;
        BOOST_REQUIRE(ParseVaultScript(vault, lock, parsed, claim));
        BOOST_CHECK_EQUAL(lock, c.lock);
        BOOST_CHECK_EQUAL(claim, c.claim);
        BOOST_CHECK(parsed == pub);
        BOOST_CHECK(VaultScript(lock, parsed, claim) == vault);
    }
    // The exact opcode layout of the 51-byte script.
    {
        CScript vault = VaultScript(120000, pub, 120000 + grace);
        CScript expect;
        expect << OP_IF << (int64_t)120000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIG
               << OP_ELSE << (int64_t)(120000 + grace) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
        BOOST_CHECK(vault == expect);
        BOOST_CHECK_EQUAL(vault[0], OP_IF);
        BOOST_CHECK_EQUAL(vault[1], 0x03);
        BOOST_CHECK_EQUAL(vault[vault.size() - 1], OP_ENDIF);
        BOOST_CHECK_EQUAL(vault[vault.size() - 2], OP_TRUE);
        // The height pushes are minimal, so the script passes MINIMALDATA when executed as the P2SH inner script.
        BOOST_CHECK(vault.IsPushOnly() == false);
    }
    // Invalid arguments give an empty script.
    BOOST_CHECK(VaultScript(0, pub, 100).empty());
    BOOST_CHECK(VaultScript(100, pub, 100).empty());              // claim must exceed lock
    BOOST_CHECK(VaultScript(100, pub, 99).empty());
    BOOST_CHECK(VaultScript(LOCKTIME_THRESHOLD, pub, LOCKTIME_THRESHOLD + 1).empty());
    BOOST_CHECK(VaultScript(100, pub, LOCKTIME_THRESHOLD).empty());
    BOOST_CHECK(VaultScript(100, NewKey(false).GetPubKey(), 200).empty());
    BOOST_CHECK(VaultScript(100, CPubKey(), 200).empty());

    // Parser rejections: anything but the exact template with minimal pushes.
    {
        const CScript good = VaultScript(120000, pub, 120000 + grace);
        uint32_t lock, claim;
        CPubKey k;
        auto rejects = [&](CScript s, const char* what) {
            BOOST_CHECK_MESSAGE(!ParseVaultScript(s, lock, k, claim), what);
        };
        rejects(CScript(), "empty");
        rejects(CScript(good.begin(), good.end() - 1), "missing ENDIF");
        rejects(good + CScript(CScript() << OP_DROP), "trailing opcode");
        // Non-minimal lock push (4 bytes for 120000).
        {
            CScript s;
            s << OP_IF;
            s.push_back(0x04); s.push_back(0xc0); s.push_back(0xd4); s.push_back(0x01); s.push_back(0x00);
            s << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIG
              << OP_ELSE << (int64_t)(120000 + grace) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
            rejects(s, "non-minimal lock push");
        }
        // Claim <= lock.
        {
            CScript s;
            s << OP_IF << (int64_t)120000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIG
              << OP_ELSE << (int64_t)120000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
            rejects(s, "claim == lock");
        }
        // CHECKSIGVERIFY instead of CHECKSIG (would leave nothing for CLEANSTACK on the owner path).
        {
            CScript s;
            s << OP_IF << (int64_t)120000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIGVERIFY
              << OP_ELSE << (int64_t)(120000 + grace) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
            rejects(s, "CHECKSIGVERIFY");
        }
        // Uncompressed owner key.
        {
            CPubKey unc = NewKey(false).GetPubKey();
            CScript s;
            s << OP_IF << (int64_t)120000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(unc.begin(), unc.end()) << OP_CHECKSIG
              << OP_ELSE << (int64_t)(120000 + grace) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
            rejects(s, "uncompressed key");
        }
        // A 5-byte height (above LOCKTIME_THRESHOLD) and a zero height.
        {
            CScript s;
            s << OP_IF << (int64_t)LOCKTIME_THRESHOLD << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIG
              << OP_ELSE << (int64_t)(LOCKTIME_THRESHOLD + 1) << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
            rejects(s, "time-locked");
            CScript z;
            z << OP_IF << OP_0 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIG
              << OP_ELSE << (int64_t)100 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE << OP_ENDIF;
            rejects(z, "zero lock");
        }
        // The prototype's multisig-shaped vault is not a v2 vault.
        rejects(CScript() << (int64_t)120000 << OP_CHECKLOCKTIMEVERIFY << OP_DROP << valtype(pub.begin(), pub.end()) << OP_CHECKSIGVERIFY << OP_1 << valtype(pub.begin(), pub.end()) << OP_1 << OP_CHECKMULTISIG, "v1 vault");
        rejects(GetScriptForDestination(pub.GetID()), "P2PKH");
    }
}

// Rule: RED-1
// Rule: RED-4
BOOST_AUTO_TEST_CASE(red1_scriptsig_shapes_and_path_detection)
{
    const CKey owner = NewKey();
    const CScript vault = VaultScript(120000, owner.GetPubKey(), 120000 + 34560);
    const valtype sig(72, 0x30);
    const valtype vaultBytes(vault.begin(), vault.end());

    // Owner: <sig> OP_1 <script>; claim: OP_0 <script>. Both push-only.
    const CScript ownerSig = OwnerScriptSig(sig, vault);
    const CScript claimSig = ClaimScriptSig(vault);
    BOOST_CHECK_EQUAL(ownerSig.size(), 1 + sig.size() + 1 + 1 + vault.size());
    BOOST_CHECK_EQUAL(claimSig.size(), 1 + 1 + vault.size());
    BOOST_CHECK(ownerSig.IsPushOnly());
    BOOST_CHECK(claimSig.IsPushOnly());
    BOOST_CHECK(ownerSig == (CScript() << sig << OP_1 << vaultBytes));
    BOOST_CHECK(claimSig == (CScript() << OP_0 << vaultBytes));
    BOOST_CHECK_EQUAL(ownerSig[1 + sig.size()], OP_1);
    BOOST_CHECK_EQUAL(claimSig[0], OP_0);
    CScript redeem;
    BOOST_CHECK(ExtractRedeemScript(ownerSig, redeem) && redeem == vault);
    BOOST_CHECK(ExtractRedeemScript(claimSig, redeem) && redeem == vault);

    // Path detection = CastToBool(selector), exactly as OP_IF evaluates it (K4).
    struct Case { const char* name; CScript scriptSig; bool present; bool ownerPath; size_t pushes; bool hasSig; };
    const std::vector<Case> cases = {
        { "owner", ownerSig, true, true, 3, true },
        { "claim", claimSig, true, false, 2, false },
        { "owner_op2", CScript() << sig << OP_2 << vaultBytes, true, true, 3, true },
        { "owner_op16", CScript() << sig << OP_16 << vaultBytes, true, true, 3, true },
        { "owner_1negate", CScript() << sig << OP_1NEGATE << vaultBytes, true, true, 3, true },
        { "owner_nonminimal_1", [&] { CScript s; s << sig; s.push_back(0x01); s.push_back(0x01); s << vaultBytes; return s; }(), true, true, 3, true },
        { "owner_two_byte_true", CScript() << sig << valtype{ 0x00, 0x01 } << vaultBytes, true, true, 3, true },
        { "claim_negative_zero", CScript() << sig << valtype{ 0x80 } << vaultBytes, true, false, 3, true },
        { "claim_zero_byte", CScript() << sig << valtype{ 0x00 } << vaultBytes, true, false, 3, true },
        { "claim_two_zero_bytes", CScript() << valtype{ 0x00, 0x00 } << vaultBytes, true, false, 2, false },
        { "claim_neg_zero_long", CScript() << valtype{ 0x00, 0x80 } << vaultBytes, true, false, 2, false },
        { "claim_with_sig", CScript() << sig << OP_0 << vaultBytes, true, false, 3, true },
        { "claim_extra_op0", CScript() << OP_0 << OP_0 << vaultBytes, true, false, 3, false },
        { "four_pushes", CScript() << sig << sig << OP_1 << vaultBytes, true, true, 4, true },
        { "one_push", CScript() << vaultBytes, false, false, 0, false },
        { "empty", CScript(), false, false, 0, false },
        { "not_push_only", CScript() << OP_1 << vaultBytes << OP_DROP, false, false, 0, false },
        { "opcode_in_middle", CScript() << sig << OP_DUP << vaultBytes, false, false, 0, false },
        { "truncated_push", [&] { CScript s = claimSig; s.resize(s.size() - 5); return s; }(), false, false, 0, false },
    };
    for (const Case& c : cases) {
        auto path = ParseVaultSpendPath(c.scriptSig);
        BOOST_CHECK_MESSAGE(path.has_value() == c.present, std::string(c.name) + ": presence");
        if (!path.has_value() || !c.present) continue;
        BOOST_CHECK_MESSAGE(path->ownerPath == c.ownerPath, std::string(c.name) + ": path");
        BOOST_CHECK_MESSAGE(path->pushes == c.pushes, std::string(c.name) + ": pushes");
        BOOST_CHECK_MESSAGE(path->ownerSig.empty() != c.hasSig, std::string(c.name) + ": sig");
        BOOST_CHECK_MESSAGE(path->vaultScript == vault, std::string(c.name) + ": script");
    }
    // The selector of OP_N is the number itself, of OP_0 the empty vector.
    BOOST_CHECK(ParseVaultSpendPath(ownerSig)->selector == valtype{ 0x01 });
    BOOST_CHECK(ParseVaultSpendPath(CScript() << sig << OP_16 << vaultBytes)->selector == valtype{ 0x10 });
    BOOST_CHECK(ParseVaultSpendPath(claimSig)->selector.empty());
    BOOST_CHECK(ParseVaultSpendPath(ownerSig)->ownerSig == sig);
    // The last push need not be a vault script for the path to be read (RED-1 checks the script separately).
    BOOST_CHECK(ParseVaultSpendPath(CScript() << OP_1 << valtype(3, 9)).has_value());
}

// Rule: RED-1
// Rule: RED-2
// Rule: MINT-3
BOOST_AUTO_TEST_CASE(red1_owner_path_verifies)
{
    RegtestActivateSapling();
    const uint32_t branchId = NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId;
    const uint32_t lockHeight = 120000;
    const uint32_t claimHeight = lockHeight + 34560;
    const CAmount vaultValue = 5 * COIN;
    const CKey ownerKey = NewKey();
    const CScript vault = VaultScript(lockHeight, ownerKey.GetPubKey(), claimHeight);
    const CScript spk = P2SHScript(vault);

    auto build = [&](uint32_t nLockTime, const CKey& signer, const CScript& scriptInSig, std::function<CScript(const valtype&)> shape = nullptr) {
        CMutableTransaction mtx = SpendingTx(vaultValue, nLockTime);
        valtype ownerSig = Sign(signer, vault, mtx, 0, vaultValue, branchId);
        mtx.vin[0].scriptSig = shape ? shape(ownerSig) : OwnerScriptSig(ownerSig, scriptInSig);
        return mtx;
    };

    // Success: at lockHeight and after; the scriptSig is push-only and small.
    {
        CMutableTransaction ok = build(lockHeight, ownerKey, vault);
        ScriptError err;
        BOOST_CHECK_MESSAGE(Verify(ok, spk, vaultValue, branchId, &err), ScriptErrorString(err));
        BOOST_CHECK(Verify(ok, spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK(ok.vin[0].scriptSig.size() < 130);
        BOOST_CHECK(ok.vin[0].scriptSig.IsPushOnly());
        BOOST_CHECK(Verify(build(lockHeight + 1000, ownerKey, vault), spk, vaultValue, branchId));
        BOOST_CHECK(Verify(build(claimHeight + 1, ownerKey, vault), spk, vaultValue, branchId));   // owner can still spend after the grace
    }
    // Early: nLockTime below lockHeight => CLTV.
    {
        ScriptError err;
        BOOST_CHECK(!Verify(build(lockHeight - 1, ownerKey, vault), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        BOOST_CHECK(!Verify(build(0, ownerKey, vault), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        // A time-based nLockTime never satisfies a height CLTV.
        BOOST_CHECK(!Verify(build(LOCKTIME_THRESHOLD + 1, ownerKey, vault), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
    }
    // Wrong key: CHECKSIG leaves false => EVAL_FALSE (not a VERIFY error).
    {
        ScriptError err;
        BOOST_CHECK(!Verify(build(lockHeight, NewKey(), vault), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_EVAL_FALSE);
    }
    // Claim-path selector with a signature: <sig> OP_0 <script>. Before claimHeight CLTV fails;
    // at claimHeight the signature is left on the stack => CLEANSTACK under the standard flags,
    // but consensus (P2SH | CLTV) accepts it: a claim spend that happens to carry a signature.
    {
        auto claimShape = [&](const valtype& s) { return CScript() << s << OP_0 << valtype(vault.begin(), vault.end()); };
        ScriptError err;
        BOOST_CHECK(!Verify(build(lockHeight, ownerKey, vault, claimShape), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        CMutableTransaction atClaim = build(claimHeight, ownerKey, vault, claimShape);
        BOOST_CHECK(!Verify(atClaim, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CLEANSTACK);
        BOOST_CHECK(Verify(atClaim, spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK(!ParseVaultSpendPath(atClaim.vin[0].scriptSig)->ownerPath);
    }
    // K4 selectors: OP_2 is minimal and true => verifies under both flag sets (no MINIMALIF in Ycash);
    // a non-minimal 1 fails MINIMALDATA under the standard flags but is consensus-valid; both are owner spends.
    {
        auto op2 = [&](const valtype& s) { return CScript() << s << OP_2 << valtype(vault.begin(), vault.end()); };
        CMutableTransaction m2 = build(lockHeight, ownerKey, vault, op2);
        ScriptError err;
        BOOST_CHECK_MESSAGE(Verify(m2, spk, vaultValue, branchId, &err), ScriptErrorString(err));
        BOOST_CHECK(Verify(m2, spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK(ParseVaultSpendPath(m2.vin[0].scriptSig)->ownerPath);

        auto nonMinimal = [&](const valtype& s) { CScript c; c << s; c.push_back(0x01); c.push_back(0x01); c << valtype(vault.begin(), vault.end()); return c; };
        CMutableTransaction m1 = build(lockHeight, ownerKey, vault, nonMinimal);
        BOOST_CHECK(!Verify(m1, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_MINIMALDATA);
        BOOST_CHECK(Verify(m1, spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK(ParseVaultSpendPath(m1.vin[0].scriptSig)->ownerPath);
    }
    // Final nSequence disables CLTV.
    {
        CMutableTransaction bad = SpendingTx(vaultValue, lockHeight);
        bad.vin[0].nSequence = 0xFFFFFFFF;
        bad.vin[0].scriptSig = OwnerScriptSig(Sign(ownerKey, vault, bad, 0, vaultValue, branchId), vault);
        ScriptError err;
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
    }
    // A different redeem script does not match the P2SH hash.
    {
        CScript other = VaultScript(lockHeight + 1, ownerKey.GetPubKey(), claimHeight);
        ScriptError err;
        BOOST_CHECK(!Verify(build(lockHeight, ownerKey, other), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_EVAL_FALSE);
    }
    // The signature commits to the amount (ZIP-243): a different amount fails.
    {
        CMutableTransaction ok = build(lockHeight, ownerKey, vault);
        BOOST_CHECK(!Verify(ok, spk, vaultValue + 1, branchId));
        // ... and to the branch id: the Sprout/Overwinter ids fail.
        BOOST_CHECK(!Verify(ok, spk, vaultValue, NetworkUpgradeInfo[Consensus::UPGRADE_OVERWINTER].nBranchId));
    }
    RegtestDeactivateSapling();
}

// Rule: RED-4
BOOST_AUTO_TEST_CASE(red4_claim_path_verifies)
{
    RegtestActivateSapling();
    const uint32_t branchId = NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId;
    const uint32_t lockHeight = 120000;
    const uint32_t claimHeight = lockHeight + 34560;
    const CAmount vaultValue = 5 * COIN;
    const CScript vault = VaultScript(lockHeight, NewKey().GetPubKey(), claimHeight);
    const CScript spk = P2SHScript(vault);

    auto build = [&](uint32_t nLockTime, const CScript& scriptSig = CScript()) {
        CMutableTransaction mtx = SpendingTx(vaultValue, nLockTime);
        mtx.vin[0].scriptSig = scriptSig.empty() ? ClaimScriptSig(vault) : scriptSig;
        return mtx;
    };

    // Success at claimHeight and after: no signature, one element left (CLEANSTACK).
    {
        ScriptError err;
        BOOST_CHECK_MESSAGE(Verify(build(claimHeight), spk, vaultValue, branchId, &err), ScriptErrorString(err));
        BOOST_CHECK(Verify(build(claimHeight + 1), spk, vaultValue, branchId));
        BOOST_CHECK(Verify(build(claimHeight), spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK_EQUAL(build(claimHeight).vin[0].scriptSig.size(), 1u + 1u + vault.size());
    }
    // Early: one below claimHeight, at lockHeight, zero.
    {
        ScriptError err;
        BOOST_CHECK(!Verify(build(claimHeight - 1), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        BOOST_CHECK(!Verify(build(lockHeight), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
        BOOST_CHECK(!Verify(build(0), spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
    }
    // Final nSequence disables CLTV.
    {
        CMutableTransaction bad = build(claimHeight);
        bad.vin[0].nSequence = 0xFFFFFFFF;
        ScriptError err;
        BOOST_CHECK(!Verify(bad, spk, vaultValue, branchId, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
    }
    // K4: negative zero (0x80) is a claim selector under both flag sets.
    {
        CScript negZero = CScript() << valtype{ 0x80 } << valtype(vault.begin(), vault.end());
        ScriptError err;
        BOOST_CHECK_MESSAGE(Verify(build(claimHeight, negZero), spk, vaultValue, branchId, &err), ScriptErrorString(err));
        BOOST_CHECK(Verify(build(claimHeight, negZero), spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK(!ParseVaultSpendPath(negZero)->ownerPath);
        // A two-byte zero is a minimal *push* (CheckMinimalPush only rewrites 1-byte pushes of 1..16 and 0x81),
        // so MINIMALDATA does not police the selector's numeric form: it verifies under both flag sets and is a
        // claim selector (K4). Only the wallet's strict template policy (TPL-2) insists on OP_0.
        CScript twoZero = CScript() << valtype{ 0x00, 0x00 } << valtype(vault.begin(), vault.end());
        BOOST_CHECK_MESSAGE(Verify(build(claimHeight, twoZero), spk, vaultValue, branchId, &err), ScriptErrorString(err));
        BOOST_CHECK(Verify(build(claimHeight, twoZero), spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK(!ParseVaultSpendPath(twoZero)->ownerPath);
    }
    // A scriptSig with fewer than two pushes cannot run OP_IF (RED-1's malformed case is consensus-invalid too).
    {
        ScriptError err;
        BOOST_CHECK(!Verify(build(claimHeight, CScript() << valtype(vault.begin(), vault.end())), spk, vaultValue, branchId, &err, CONSENSUS_FLAGS));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
    }
    RegtestDeactivateSapling();
}

namespace {

// Rule: MINT-3
// Rule: MINT-8
// Rule: RED-3
// Every §3.5 template through IsStandardTx and AreInputsStandard at height 1
// under the given upgrade, with the enforcement-fee output present.
void CheckTemplatesStandard(uint32_t branchId, const char* upgrade)
{
    const int height = 1;
    LOCK(cs_main);
    const yellowback::Params& params = MainParams();

    const CKey ownerKey = NewKey(), userKey = NewKey(), payeeKey = NewKey();
    CBasicKeyStore keystore;
    keystore.AddKey(userKey);
    const CScript userP2PKH = GetScriptForDestination(userKey.GetPubKey().GetID());
    const CScript payeeP2PKH = GetScriptForDestination(payeeKey.GetPubKey().GetID());
    const uint32_t refHeight = 100;
    const uint32_t lockHeight = refHeight + params.classMin[0];
    const uint32_t claimHeight = lockHeight + params.grace;
    const CScript vault = VaultScript(lockHeight, ownerKey.GetPubKey(), claimHeight);
    const CScript vaultSpk = P2SHScript(vault);
    const CAmount vaultValue = 6000 * COIN;
    const CAmount feeZat = FeeZat(vaultValue, params.feeMin, params.feeBps);

    // Funding transaction providing every kind of input.
    CMutableTransaction fund;
    fund.fOverwintered = true;
    fund.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    fund.nVersion = SAPLING_TX_VERSION;
    fund.vout.push_back(CTxOut(10000 * COIN, userP2PKH));   // 0: YEC input for a mint
    fund.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));    // 1: a YED token output
    fund.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));    // 2: another
    fund.vout.push_back(CTxOut(vaultValue, vaultSpk));      // 3: a vault
    CCoinsView dummy;
    CCoinsViewCache view(&dummy);
    view.ModifyCoins(fund.GetHash())->FromTx(fund, height);
    const uint256 fundHash = fund.GetHash();

    auto newTx = [&](uint32_t nLockTime) {
        CMutableTransaction mtx;
        mtx.fOverwintered = true;
        mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
        mtx.nVersion = SAPLING_TX_VERSION;
        mtx.nExpiryHeight = refHeight + REF_WINDOW;
        mtx.nLockTime = nLockTime;
        return mtx;
    };
    auto checkStandard = [&](CMutableTransaction& mtx, const std::string& what) {
        std::string reason;
        BOOST_CHECK_MESSAGE(IsStandardTx(CTransaction(mtx), reason, ::Params(), height), what + " (" + upgrade + "): " + reason);
        BOOST_CHECK_MESSAGE(AreInputsStandard(CTransaction(mtx), view, branchId), what + " (" + upgrade + "): inputs");
        BOOST_CHECK(FindPayload(CTransaction(mtx)).has_value() || what == "SWEEP" || what == "VOID");
    };

    // MINT: YEC input; vault P2SH, token P2PKH, OP_RETURN MINT(feeVout = 3), fee P2PKH, change.
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 0)));
        mtx.vout.push_back(CTxOut(vaultValue, vaultSpk));
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Mint(0, 10000, lockHeight, refHeight, ownerKey.GetPubKey(), 3)))));
        mtx.vout.push_back(CTxOut(feeZat, payeeP2PKH));
        mtx.vout.push_back(CTxOut(10000 * COIN - vaultValue - TOKEN_VALUE - feeZat - DEFAULT_YELLOWBACK_FEE, userP2PKH));
        BOOST_REQUIRE(SignSignature(keystore, userP2PKH, mtx, 0, 10000 * COIN, SIGHASH_ALL, branchId));
        checkStandard(mtx, "MINT");
        BOOST_CHECK_EQUAL(mtx.vout.size(), 5u);
        BOOST_CHECK_EQUAL(FindPayload(CTransaction(mtx))->payload.feeVout, 3);
    }
    // TRANSFER: two token inputs + YEC fee input, 15 assignments (max), 80-byte OP_RETURN, change.
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
    // REDEEM (owner path): vault input + token input, no YEC input; collateral out, fee out, YED change, OP_RETURN REDEEM(feeVout = 1).
    {
        CMutableTransaction mtx = newTx(lockHeight);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 3), CScript(), 0xFFFFFFFE));
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 1)));
        mtx.vout.push_back(CTxOut(vaultValue + TOKEN_VALUE - DEFAULT_YELLOWBACK_FEE - feeZat - TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(feeZat, payeeP2PKH));
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Redeem(refHeight, 1, { Assignment(2, 100) })))));
        mtx.vin[0].scriptSig = OwnerScriptSig(Sign(ownerKey, vault, mtx, 0, vaultValue, branchId), vault);
        BOOST_REQUIRE(SignSignature(keystore, userP2PKH, mtx, 1, TOKEN_VALUE, SIGHASH_ALL, branchId));
        checkStandard(mtx, "REDEEM");
        ScriptError err;
        BOOST_CHECK_MESSAGE(VerifyScript(mtx.vin[0].scriptSig, vaultSpk, STANDARD_SCRIPT_VERIFY_FLAGS,
                                         MutableTransactionSignatureChecker(&mtx, 0, vaultValue), branchId, &err),
                            ScriptErrorString(err));
        BOOST_CHECK(ParseVaultSpendPath(mtx.vin[0].scriptSig)->ownerPath);
    }
    // CLAIM: identical with the claim-path scriptSig and nLockTime = claimHeight.
    {
        CMutableTransaction mtx = newTx(claimHeight);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 3), ClaimScriptSig(vault), 0xFFFFFFFE));
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 1)));
        mtx.vout.push_back(CTxOut(vaultValue + TOKEN_VALUE - DEFAULT_YELLOWBACK_FEE - feeZat - TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(feeZat, payeeP2PKH));
        mtx.vout.push_back(CTxOut(TOKEN_VALUE, userP2PKH));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Redeem(refHeight, 1, { Assignment(2, 100) })))));
        BOOST_REQUIRE(SignSignature(keystore, userP2PKH, mtx, 1, TOKEN_VALUE, SIGHASH_ALL, branchId));
        checkStandard(mtx, "CLAIM");
        ScriptError err;
        BOOST_CHECK_MESSAGE(VerifyScript(mtx.vin[0].scriptSig, vaultSpk, STANDARD_SCRIPT_VERIFY_FLAGS,
                                         MutableTransactionSignatureChecker(&mtx, 0, vaultValue), branchId, &err),
                            ScriptErrorString(err));
        BOOST_CHECK(!ParseVaultSpendPath(mtx.vin[0].scriptSig)->ownerPath);
    }
    // SWEEP / VOID release: the owner path with one output and no payload (L10, L14) — standard, and no fee output.
    {
        CMutableTransaction mtx = newTx(lockHeight);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 3), CScript(), 0xFFFFFFFE));
        mtx.vout.push_back(CTxOut(vaultValue - DEFAULT_YELLOWBACK_FEE, userP2PKH));
        mtx.vin[0].scriptSig = OwnerScriptSig(Sign(ownerKey, vault, mtx, 0, vaultValue, branchId), vault);
        checkStandard(mtx, "SWEEP");
    }
    // Negative control: two OP_RETURN outputs are non-standard ("multi-op-return"), a miner may still include them (§3.3).
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 0)));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Transfer({})))));
        mtx.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Transfer({})))));
        std::string reason;
        BOOST_CHECK(!IsStandardTx(CTransaction(mtx), reason, ::Params(), height));
        BOOST_CHECK_EQUAL(reason, "multi-op-return");
    }
    // Negative control: an 81-byte OP_RETURN push is non-standard ("scriptpubkey").
    {
        CMutableTransaction mtx = newTx(0);
        mtx.vin.push_back(CTxIn(COutPoint(fundHash, 0)));
        mtx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>(81, 0)));
        std::string reason;
        BOOST_CHECK(!IsStandardTx(CTransaction(mtx), reason, ::Params(), height));
        BOOST_CHECK_EQUAL(reason, "scriptpubkey");
    }
}

} // namespace

// Rule: MINT-3
// Rule: MINT-8
// Rule: RED-3
BOOST_AUTO_TEST_CASE(mint3_templates_are_standard_under_sapling)
{
    RegtestActivateSapling();
    CheckTemplatesStandard(NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId, "sapling");
    RegtestDeactivateSapling();
}

// Rule: MINT-3
// Rule: MINT-8
// Rule: RED-3
BOOST_AUTO_TEST_CASE(mint3_templates_are_standard_under_canopy)
{
    RegtestActivateCanopy();
    CheckTemplatesStandard(NetworkUpgradeInfo[Consensus::UPGRADE_CANOPY].nBranchId, "canopy");
    RegtestDeactivateCanopy();
}

// ---------------------------------------------------------------------------
// The fork's TransactionBuilder extension (unsigned transparent inputs, script
// outputs, lock time) that the wallet's REDEEM/CLAIM builders rely on (§3.5).
BOOST_AUTO_TEST_CASE(transaction_builder_extension)
{
    RegtestActivateSapling();
    const int height = 200;
    const uint32_t branchId = NetworkUpgradeInfo[Consensus::UPGRADE_SAPLING].nBranchId;
    CBasicKeyStore keystore;
    CKey key;
    key.MakeNewKey(true);
    keystore.AddKey(key);
    const CScript p2pkh = GetScriptForDestination(key.GetPubKey().GetID());
    const CScript payload = CScript() << OP_RETURN << valtype(4, 0x42);

    TransactionBuilder b(::Params().GetConsensus(), height, &keystore);
    b.SetExpiryHeight(height + 20);
    b.SetFee(1000);
    b.SetLockTime(123);
    b.AddTransparentInput(COutPoint(uint256S("01"), 0), p2pkh, 2 * COIN);
    b.AddTransparentInputUnsigned(COutPoint(uint256S("02"), 3), 5 * COIN, 0xFFFFFFFE);
    b.AddTransparentOutput(payload, 0);
    CTxDestination dest = key.GetPubKey().GetID();
    b.AddTransparentOutput(dest, 7 * COIN - 1000);
    TransactionBuilderResult r = b.Build();
    const std::string buildError = r.IsError() ? r.GetError() : std::string();
    BOOST_REQUIRE_MESSAGE(!r.IsError(), buildError);
    CTransaction tx = r.GetTxOrThrow();

    BOOST_CHECK_EQUAL(tx.nLockTime, 123u);
    BOOST_CHECK_EQUAL(tx.nExpiryHeight, (uint32_t)(height + 20));
    BOOST_REQUIRE_EQUAL(tx.vin.size(), 2u);
    BOOST_CHECK(!tx.vin[0].scriptSig.empty());
    BOOST_CHECK(tx.vin[1].scriptSig.empty());
    BOOST_CHECK_EQUAL(tx.vin[1].nSequence, 0xFFFFFFFEu);
    BOOST_CHECK(tx.vin[1].prevout == COutPoint(uint256S("02"), 3));
    BOOST_REQUIRE_EQUAL(tx.vout.size(), 2u);
    BOOST_CHECK(tx.vout[0].scriptPubKey == payload);
    BOOST_CHECK_EQUAL(tx.vout[0].nValue, 0);
    BOOST_CHECK(tx.vout[1].scriptPubKey == p2pkh);
    BOOST_CHECK(tx.vShieldedSpend.empty() && tx.vShieldedOutput.empty());
    BOOST_CHECK_EQUAL(tx.valueBalance, 0);
    ScriptError err;
    BOOST_CHECK_MESSAGE(Verify(CMutableTransaction(tx), p2pkh, 2 * COIN, branchId, &err), ScriptErrorString(err));
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_SUITE_END()
