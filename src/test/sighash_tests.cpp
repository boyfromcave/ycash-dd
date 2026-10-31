// Copyright (c) 2013 The Bitcoin Core developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "chainparams.h"
#include "chainparamsbase.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "test/data/sighash.json.h"
#include "main.h"
#include "test_random.h"
#include "script/interpreter.h"
#include "script/script.h"
#include "script/sign.h"
#include "script/standard.h"
#include "keystore.h"
#include "serialize.h"
#include "test/test_bitcoin.h"
#include "test/test_util.h"
#include "util.h"
#include "version.h"

#include <iostream>
#include <map>
#include <random>

#include <boost/test/unit_test.hpp>

#include <rust/ed25519.h>

#include <univalue.h>

// Old script.cpp SignatureHash function
uint256 static SignatureHashOld(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    static const uint256 one(uint256S("0000000000000000000000000000000000000000000000000000000000000001"));
    if (nIn >= txTo.vin.size())
    {
        printf("ERROR: SignatureHash(): nIn=%d out of range\n", nIn);
        return one;
    }
    CMutableTransaction txTmp(txTo);

    // Blank out other inputs' signatures
    for (unsigned int i = 0; i < txTmp.vin.size(); i++)
        txTmp.vin[i].scriptSig = CScript();
    txTmp.vin[nIn].scriptSig = scriptCode;

    // Blank out some of the outputs
    if ((nHashType & 0x1f) == SIGHASH_NONE)
    {
        // Wildcard payee
        txTmp.vout.clear();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }
    else if ((nHashType & 0x1f) == SIGHASH_SINGLE)
    {
        // Only lock-in the txout payee at same index as txin
        unsigned int nOut = nIn;
        if (nOut >= txTmp.vout.size())
        {
            printf("ERROR: SignatureHash(): nOut=%d out of range\n", nOut);
            return one;
        }
        txTmp.vout.resize(nOut+1);
        for (unsigned int i = 0; i < nOut; i++)
            txTmp.vout[i].SetNull();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }

    // Blank out other inputs completely, not recommended for open transactions
    if (nHashType & SIGHASH_ANYONECANPAY)
    {
        txTmp.vin[0] = txTmp.vin[nIn];
        txTmp.vin.resize(1);
    }

    // Blank out the joinsplit signature.
    memset(&txTmp.joinSplitSig.bytes, 0, ED25519_SIGNATURE_LEN);

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << nHashType;
    return ss.GetHash();
}

void static RandomScript(CScript &script) {
    static const opcodetype oplist[] = {OP_FALSE, OP_1, OP_2, OP_3, OP_CHECKSIG, OP_IF, OP_VERIF, OP_RETURN};
    script = CScript();
    int ops = (insecure_rand() % 10);
    for (int i=0; i<ops; i++)
        script << oplist[insecure_rand() % (sizeof(oplist)/sizeof(oplist[0]))];
}

// Overwinter tx version numbers are selected randomly from current version range.
// https://en.cppreference.com/w/cpp/numeric/random/uniform_int_distribution
// https://stackoverflow.com/a/19728404
std::random_device rd;
std::mt19937 rng(rd());
std::uniform_int_distribution<int> overwinter_version_dist(
    CTransaction::OVERWINTER_MIN_CURRENT_VERSION,
    CTransaction::OVERWINTER_MAX_CURRENT_VERSION);
std::uniform_int_distribution<int> sapling_version_dist(
    CTransaction::SAPLING_MIN_CURRENT_VERSION,
    CTransaction::SAPLING_MAX_CURRENT_VERSION);

void static RandomTransaction(CMutableTransaction &tx, bool fSingle, uint32_t consensusBranchId) {
    tx.fOverwintered = insecure_rand() % 2;
    if (tx.fOverwintered) {
        if (insecure_rand() % 2) {
            tx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
            tx.nVersion = sapling_version_dist(rng);
        } else {
            tx.nVersionGroupId = OVERWINTER_VERSION_GROUP_ID;
            tx.nVersion = overwinter_version_dist(rng);
        }
        tx.nExpiryHeight = (insecure_rand() % 2) ? insecure_rand() : 0;
    } else {
        tx.nVersion = insecure_rand() & 0x7FFFFFFF;
    }
    tx.vin.clear();
    tx.vout.clear();
    tx.vShieldedSpend.clear();
    tx.vShieldedOutput.clear();
    tx.vJoinSplit.clear();
    tx.nLockTime = (insecure_rand() % 2) ? insecure_rand() : 0;
    int ins = (insecure_rand() % 4) + 1;
    int outs = fSingle ? ins : (insecure_rand() % 4) + 1;
    int shielded_spends = (insecure_rand() % 4) + 1;
    int shielded_outs = (insecure_rand() % 4) + 1;
    int joinsplits = (insecure_rand() % 4);
    for (int in = 0; in < ins; in++) {
        tx.vin.push_back(CTxIn());
        CTxIn &txin = tx.vin.back();
        txin.prevout.hash = GetRandHash();
        txin.prevout.n = insecure_rand() % 4;
        RandomScript(txin.scriptSig);
        txin.nSequence = (insecure_rand() % 2) ? insecure_rand() : (unsigned int)-1;
    }
    for (int out = 0; out < outs; out++) {
        tx.vout.push_back(CTxOut());
        CTxOut &txout = tx.vout.back();
        txout.nValue = insecure_rand() % 100000000;
        RandomScript(txout.scriptPubKey);
    }
    if (tx.nVersionGroupId == SAPLING_VERSION_GROUP_ID) {
        tx.valueBalance = insecure_rand() % 100000000;
        for (int spend = 0; spend < shielded_spends; spend++) {
            SpendDescription sdesc;
            sdesc.cv = GetRandHash();
            sdesc.anchor = GetRandHash();
            sdesc.nullifier = GetRandHash();
            sdesc.rk = GetRandHash();
            GetRandBytes(sdesc.zkproof.begin(), sdesc.zkproof.size());
            tx.vShieldedSpend.push_back(sdesc);
        }
        for (int out = 0; out < shielded_outs; out++) {
            OutputDescription odesc;
            odesc.cv = GetRandHash();
            odesc.cmu = GetRandHash();
            odesc.ephemeralKey = GetRandHash();
            GetRandBytes(odesc.encCiphertext.begin(), odesc.encCiphertext.size());
            GetRandBytes(odesc.outCiphertext.begin(), odesc.outCiphertext.size());
            GetRandBytes(odesc.zkproof.begin(), odesc.zkproof.size());
            tx.vShieldedOutput.push_back(odesc);
        }
    }
    // We have removed pre-Sapling Sprout support.
    if (tx.fOverwintered && tx.nVersion >= SAPLING_TX_VERSION) {
        for (int js = 0; js < joinsplits; js++) {
            JSDescription jsdesc;
            if (insecure_rand() % 2 == 0) {
                jsdesc.vpub_old = insecure_rand() % 100000000;
            } else {
                jsdesc.vpub_new = insecure_rand() % 100000000;
            }

            jsdesc.anchor = GetRandHash();
            jsdesc.nullifiers[0] = GetRandHash();
            jsdesc.nullifiers[1] = GetRandHash();
            jsdesc.ephemeralKey = GetRandHash();
            jsdesc.randomSeed = GetRandHash();
            GetRandBytes(jsdesc.ciphertexts[0].begin(), jsdesc.ciphertexts[0].size());
            GetRandBytes(jsdesc.ciphertexts[1].begin(), jsdesc.ciphertexts[1].size());
            {
                libzcash::GrothProof zkproof;
                GetRandBytes(zkproof.begin(), zkproof.size());
                jsdesc.proof = zkproof;
            }
            jsdesc.macs[0] = GetRandHash();
            jsdesc.macs[1] = GetRandHash();

            tx.vJoinSplit.push_back(jsdesc);
        }

        Ed25519SigningKey joinSplitPrivKey;
        ed25519_generate_keypair(&joinSplitPrivKey, &tx.joinSplitPubKey);

        // Empty output script.
        CScript scriptCode;
        CTransaction signTx(tx);
        uint256 dataToBeSigned = SignatureHash(scriptCode, signTx, NOT_AN_INPUT, SIGHASH_ALL, 0, consensusBranchId);

        assert(ed25519_sign(
            &joinSplitPrivKey,
            dataToBeSigned.begin(), 32,
            &tx.joinSplitSig));
    }
}

BOOST_FIXTURE_TEST_SUITE(sighash_tests, JoinSplitTestingSetup)

BOOST_AUTO_TEST_CASE(sighash_test)
{
    uint32_t overwinterBranchId = NetworkUpgradeInfo[Consensus::UPGRADE_OVERWINTER].nBranchId;
    seed_insecure_rand(false);

    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "[\n";
    std::cout << "\t[\"raw_transaction, script, input_index, hashType, branchId, signature_hash (result)\"],\n";
    #endif
    int nRandomTests = 50000;

    #if defined(PRINT_SIGHASH_JSON)
    nRandomTests = 500;
    #endif
    for (int i=0; i<nRandomTests; i++) {
        int nHashType = insecure_rand();
        uint32_t consensusBranchId = NetworkUpgradeInfo[insecure_rand() % Consensus::MAX_NETWORK_UPGRADES].nBranchId;
        CMutableTransaction txTo;
        RandomTransaction(txTo, (nHashType & 0x1f) == SIGHASH_SINGLE, consensusBranchId);
        CScript scriptCode;
        RandomScript(scriptCode);
        int nIn = insecure_rand() % txTo.vin.size();

        uint256 sh, sho;
        sho = SignatureHashOld(scriptCode, txTo, nIn, nHashType);
        sh = SignatureHash(scriptCode, txTo, nIn, nHashType, 0, consensusBranchId);
        #if defined(PRINT_SIGHASH_JSON)
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << txTo;

        std::cout << "\t[\"" ;
        std::cout << HexStr(ss.begin(), ss.end()) << "\", \"";
        std::cout << HexStr(scriptCode) << "\", ";
        std::cout << nIn << ", ";
        std::cout << nHashType << ", ";
        std::cout << consensusBranchId << ", \"";
        std::cout << (txTo.fOverwintered ? sh.GetHex() : sho.GetHex()) << "\"]";
        if (i+1 != nRandomTests) {
          std::cout << ",";
        }
        std::cout << "\n";
        #endif
        if (!txTo.fOverwintered) {
            BOOST_CHECK(sh == sho);
        }
    }
    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "]\n";
    #endif
}

// Goal: check that SignatureHash generates correct hash
BOOST_AUTO_TEST_CASE(sighash_from_data)
{
    UniValue tests = read_json(std::string(json_tests::sighash, json_tests::sighash + sizeof(json_tests::sighash)));

    for (size_t idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 1) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        if (test.size() == 1) continue; // comment

        std::string raw_tx, raw_script, sigHashHex;
        int nIn, nHashType;
        uint32_t consensusBranchId;
        uint256 sh;
        CTransaction tx;
        CScript scriptCode = CScript();

        try {
          // deserialize test data
          raw_tx = test[0].get_str();
          raw_script = test[1].get_str();
          nIn = test[2].get_int();
          nHashType = test[3].get_int();
          consensusBranchId = test[4].get_int();
          sigHashHex = test[5].get_str();

          uint256 sh;
          CDataStream stream(ParseHex(raw_tx), SER_NETWORK, PROTOCOL_VERSION);
          stream >> tx;

          CValidationState state;
          if (tx.fOverwintered) {
              // Note that OVERWINTER_MIN_CURRENT_VERSION and OVERWINTER_MAX_CURRENT_VERSION
              // are checked in IsStandardTx(), not in CheckTransactionWithoutProofVerification()
              if (tx.nVersion < OVERWINTER_MIN_TX_VERSION ||
                  tx.nExpiryHeight >= TX_EXPIRY_HEIGHT_THRESHOLD)
              {
                  // Transaction must be invalid
                  BOOST_CHECK_MESSAGE(!CheckTransactionWithoutProofVerification(tx, state), strTest);
                  BOOST_CHECK(!state.IsValid());
              } else {
                  BOOST_CHECK_MESSAGE(CheckTransactionWithoutProofVerification(tx, state), strTest);
                  BOOST_CHECK(state.IsValid());
              }
          } else if (tx.nVersion < SPROUT_MIN_TX_VERSION) {
              // Transaction must be invalid
              BOOST_CHECK_MESSAGE(!CheckTransactionWithoutProofVerification(tx, state), strTest);
              BOOST_CHECK(!state.IsValid());
          } else {
              BOOST_CHECK_MESSAGE(CheckTransactionWithoutProofVerification(tx, state), strTest);
              BOOST_CHECK(state.IsValid());
          }

          std::vector<unsigned char> raw = ParseHex(raw_script);
          scriptCode.insert(scriptCode.end(), raw.begin(), raw.end());
        } catch (...) {
          BOOST_ERROR("Bad test, couldn't deserialize data: " << strTest);
          continue;
        }

        sh = SignatureHash(scriptCode, tx, nIn, nHashType, 0, consensusBranchId);
        BOOST_CHECK_MESSAGE(sh.GetHex() == sigHashHex, strTest);
    }
}
static bool SignV4(const CKeyStore& keystore, const CMutableTransaction& txFrom, CMutableTransaction& txTo, uint32_t branch)
{
    return SignSignature(keystore, txFrom, txTo, 0, SIGHASH_ALL, branch);
}

static bool VerifyV4(const CMutableTransaction& txFrom, const CMutableTransaction& txTo, uint32_t branch)
{
    ScriptError err;
    return VerifyScript(txTo.vin[0].scriptSig, txFrom.vout[0].scriptPubKey, SCRIPT_VERIFY_P2SH,
                        MutableTransactionSignatureChecker(&txTo, 0, txFrom.vout[0].nValue), branch, &err);
}

// The testnet shadow fork (docs/plans/yellowback-testnet-shadow-fork-plan.md §2): testnet-yb-slow and
// testnet-legacy copy mainnet through H_S and sign under their own branch IDs after it. A v4 signature
// commits to the branch ID (ZIP 243), so a transaction signed on either is invalid under mainnet's
// parameters at the same height, and the reverse; slow and legacy refuse each other's too. H_S is not
// fixed yet, so the test gives each profile's parameters a split height of its own.
BOOST_AUTO_TEST_CASE(shadow_fork_replay)
{
    const int HS = 3000000;          // a split height for the test; the release fixes the real one
    const int VAULT = HS + 10080;    // slow's (and, for the test, mainnet's) Vault height
    Consensus::Params main = Params(CBaseChainParams::MAIN).GetConsensus();
    std::map<std::string, Consensus::Params> shadow;
    for (const std::string profile : {CBaseChainParams::TESTNET_SLOW, CBaseChainParams::TESTNET_LEGACY}) {
        SelectTestnetProfile(profile);
        shadow[profile] = Params(CBaseChainParams::TESTNET).GetConsensus();
        SelectTestnetProfile(CBaseChainParams::TESTNET_FAST);
        shadow[profile].vUpgrades[Consensus::UPGRADE_SHADOW].nActivationHeight = HS + 1;
    }
    Consensus::Params& slow = shadow[CBaseChainParams::TESTNET_SLOW];
    Consensus::Params& legacy = shadow[CBaseChainParams::TESTNET_LEGACY];
    slow.vUpgrades[Consensus::UPGRADE_VAULT].nActivationHeight = VAULT;

    // Through H_S the history is mainnet's, branch IDs included; from H_S + 1 each network has its own.
    for (const Consensus::Params* p : {&slow, &legacy}) {
        BOOST_CHECK_EQUAL(CurrentEpochBranchId(HS, *p), CurrentEpochBranchId(HS, main));
        BOOST_CHECK(CurrentEpochBranchId(HS + 1, *p) != CurrentEpochBranchId(HS + 1, main));
    }
    BOOST_CHECK_EQUAL(CurrentEpochBranchId(HS + 1, slow), SLOW_SHADOW_BRANCH_ID);
    BOOST_CHECK_EQUAL(CurrentEpochBranchId(HS + 1, legacy), LEGACY_SHADOW_BRANCH_ID);
    BOOST_CHECK_EQUAL(CurrentEpochBranchId(VAULT, slow), SLOW_VAULT_BRANCH_ID);
    BOOST_CHECK_EQUAL(CurrentEpochBranchId(VAULT, legacy), LEGACY_SHADOW_BRANCH_ID);

    // Height pairs to sign and verify at: after the split before mainnet's Vault, and after mainnet
    // activates Vault (0x6d5b7a31) at the same height as slow does (the worst case for slow).
    Consensus::Params mainVault = main;
    mainVault.vUpgrades[Consensus::UPGRADE_VAULT].nActivationHeight = VAULT;
    BOOST_CHECK_EQUAL(CurrentEpochBranchId(VAULT, mainVault), NetworkUpgradeInfo[Consensus::UPGRADE_VAULT].nBranchId);
    BOOST_CHECK(SLOW_VAULT_BRANCH_ID != NetworkUpgradeInfo[Consensus::UPGRADE_VAULT].nBranchId);
    struct Net { const char* name; const Consensus::Params* params; int height; };
    const std::vector<Net> nets = {
        {"main", &main, HS + 1}, {"slow", &slow, HS + 1}, {"legacy", &legacy, HS + 1},
        {"main-vault", &mainVault, VAULT}, {"slow-vault", &slow, VAULT}, {"legacy-vault", &legacy, VAULT},
    };

    CBasicKeyStore keystore;
    CKey key;
    key.MakeNewKey(true);
    keystore.AddKey(key);
    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
    txFrom.vout[0].nValue = COIN;

    for (const Net& signer : nets) {
        const uint32_t signBranch = CurrentEpochBranchId(signer.height, *signer.params);
        CMutableTransaction txTo;
        txTo.fOverwintered = true;
        txTo.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
        txTo.nVersion = SAPLING_TX_VERSION;
        txTo.vin.resize(1);
        txTo.vin[0].prevout = COutPoint(txFrom.GetHash(), 0);
        txTo.vout.resize(1);
        txTo.vout[0].nValue = COIN / 2;
        txTo.vout[0].scriptPubKey = txFrom.vout[0].scriptPubKey;
        BOOST_REQUIRE(SignV4(keystore, txFrom, txTo, signBranch));
        for (const Net& verifier : nets) {
            const uint32_t verifyBranch = CurrentEpochBranchId(verifier.height, *verifier.params);
            // Valid exactly where the verifier's branch ID is the signer's.
            BOOST_CHECK_MESSAGE(VerifyV4(txFrom, txTo, verifyBranch) == (verifyBranch == signBranch),
                                strprintf("signed on %s, verified on %s", signer.name, verifier.name));
            const bool sameNetwork = std::string(signer.name).substr(0, 4) == std::string(verifier.name).substr(0, 4);
            if (!sameNetwork) BOOST_CHECK(verifyBranch != signBranch);
        }
    }
}
BOOST_AUTO_TEST_SUITE_END()
