// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// ZIP 216 on this line: the R half of every Sapling spendAuthSig and of the bindingSig must be a
// canonical Jubjub encoding, and a coinbase Sapling output's pk_d must decode canonically and not
// be the identity. jubjub 0.10 / reddsa 0.5.2 / sapling-crypto 0.7.0 (the 6.x line) and the
// zakura-* crates (kogany) enforce both at every height; jubjub 0.5.1 here accepts the two
// non-canonical encodings, so ContextualCheckTransaction rejects them by byte compare.

#include "arith_uint256.h"
#include "chainparams.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "key.h"
#include "keystore.h"
#include "main.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/standard.h"
#include "streams.h"
#include "test/test_bitcoin.h"
#include "transaction_builder.h"
#include "random.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "utiltest.h"
#include "zcash/IncrementalMerkleTree.hpp"
#include "zcash/Note.hpp"
#include "zcash/NoteEncryption.hpp"
#include "zcash/address/sapling.hpp"

#include <librustzcash.h>
#include <rust/blake2b.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {

// The rule holds at every height. Every case runs with UPGRADE_VAULT active from VAULT_HEIGHT and
// checks at H, after it; enforced_before_and_at_vault also checks VAULT_HEIGHT - 1.
const int VAULT_HEIGHT = 20;
const int H = VAULT_HEIGHT + 1;

void ActivateVault() { UpdateNetworkUpgradeParameters(Consensus::UPGRADE_VAULT, VAULT_HEIGHT); }
void DeactivateVault()
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_VAULT, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}

const std::string NONCANON_V_ONE = "0100000000000000000000000000000000000000000000000000000000000080";
const std::string NONCANON_V_MINUS_ONE = "00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3";
const std::string CANON_V_ONE = "0100000000000000000000000000000000000000000000000000000000000000";
const std::string CANON_V_MINUS_ONE = "00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73";

struct DecodeRow {
    const char* hex;
    bool accept045; //!< jubjub 0.5.1 AffinePoint::from_bytes (this line)
    bool accept010; //!< jubjub 0.10.0 AffinePoint::from_bytes (the 6.x line; kogany's decoder is identical)
};

// Generated from both crates (the sweep in the PR body): every v in {0..3, q-4..q+3, 2^255-1,
// a high non-field value} and the v of each of the eight torsion points, each with the sign bit
// clear and set. The two crates disagree exactly where IsNonCanonicalJubjubEncoding is true.
const DecodeRow DECODE_ROWS[] = {
    {"0000000000000000000000000000000000000000000000000000000000000000", true, true},
    {"0000000000000000000000000000000000000000000000000000000000000080", true, true},
    {"0100000000000000000000000000000000000000000000000000000000000000", true, true},
    {"0100000000000000000000000000000000000000000000000000000000000080", true, false},
    {"0200000000000000000000000000000000000000000000000000000000000000", false, false},
    {"0200000000000000000000000000000000000000000000000000000000000080", false, false},
    {"0300000000000000000000000000000000000000000000000000000000000000", true, true},
    {"0300000000000000000000000000000000000000000000000000000000000080", true, true},
    {"fdfffffffefffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", false, false},
    {"fdfffffffefffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", false, false},
    {"fefffffffefffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", true, true},
    {"fefffffffefffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", true, true},
    {"fffffffffefffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", false, false},
    {"fffffffffefffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", false, false},
    {"00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", true, true},
    {"00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", true, false},
    {"01000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", false, false},
    {"01000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", false, false},
    {"02000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", false, false},
    {"02000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", false, false},
    {"03000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", false, false},
    {"03000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", false, false},
    {"04000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", false, false},
    {"04000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", false, false},
    {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", false, false},
    {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", false, false},
    {"fefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefe7e", false, false},
    {"fefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefe", false, false},
    {"0000000000000000000000000000000000000000000000000000000000000000", true, true},
    {"0000000000000000000000000000000000000000000000000000000000000080", true, true},
    {"dd96f4ef68200dffa1a484f390ee069166724dad3530a1162e986619b2bd5849", true, true},
    {"dd96f4ef68200dffa1a484f390ee069166724dad3530a1162e986619b2bd58c9", true, true},
    {"0100000000000000000000000000000000000000000000000000000000000000", true, true},
    {"0100000000000000000000000000000000000000000000000000000000000080", true, false},
    {"00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73", true, true},
    {"00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3", true, false},
    {"0000000000000000000000000000000000000000000000000000000000000000", true, true},
    {"0000000000000000000000000000000000000000000000000000000000000080", true, true},
    {"24690b1096dff2005db7790c72b5b6c29e65545cd2a7981c1ae53610a1e9942a", true, true},
    {"24690b1096dff2005db7790c72b5b6c29e65545cd2a7981c1ae53610a1e994aa", true, true},
    {"24690b1096dff2005db7790c72b5b6c29e65545cd2a7981c1ae53610a1e9942a", true, true},
    {"24690b1096dff2005db7790c72b5b6c29e65545cd2a7981c1ae53610a1e994aa", true, true},
    {"dd96f4ef68200dffa1a484f390ee069166724dad3530a1162e986619b2bd5849", true, true},
    {"dd96f4ef68200dffa1a484f390ee069166724dad3530a1162e986619b2bd58c9", true, true},
};

/** The witness signatures (fixed key, nonce 0, sighash 42..42) that jubjub 0.5.1 + the
 *  cofactored zcash_primitives 0.5 verifier accept and reddsa 0.5.2 rejects when R is
 *  non-canonical. Only R matters here: the bytes are what a transaction would carry. */
struct WitnessSig {
    const char* kind;
    const char* name;
    const char* sig;
    bool nonCanonical;
};
const WitnessSig WITNESS[] = {
    {"spendAuthSig", "canonical (0,1)", "01000000000000000000000000000000000000000000000000000000000000006e6755660ac7cfe74b296697b1450d786da7ff7f2f57874974f09c02cdea030a", false},
    {"spendAuthSig", "NON-canonical (0,1)", "0100000000000000000000000000000000000000000000000000000000000080c23bcf4e02fee2e10f7d4c90f2e5271a7dd2afc8ed98a3aa2966f1c47780350d", true},
    {"spendAuthSig", "canonical (0,-1)", "00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed735ba9e4e49f8115b4cb05d75d4ecb439c2d5a43bcc54e6b48f2d9b3c2fe9ee10c", false},
    {"spendAuthSig", "NON-canonical (0,-1)", "00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf347274d3449f4b475cd0e32d6245f05d90e4074558d94e3b329d364fb3f303305", true},
    {"bindingSig", "canonical (0,1)", "010000000000000000000000000000000000000000000000000000000000000093e886aec6851f980a7ecad4b55408bf60e85e7c4f7ab6d21684990278b25e09", false},
    {"bindingSig", "NON-canonical (0,1)", "01000000000000000000000000000000000000000000000000000000000000808b85959fcddf23c6a97476450e134dabd45ebac589ae553cc10854569f94b501", true},
    {"bindingSig", "canonical (0,-1)", "00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7ed73c1adf666d0f10912e37dd60c5895d4c6318b57305619b7c0d2164ad285ef710a", false},
    {"bindingSig", "NON-canonical (0,-1)", "00000000fffffffffe5bfeff02a4bd5305d8a10908d83933487d9d2953a7edf3eee601e203da14bcbcd18097fada07233e4a19126ef3b92a345db659e116020d", true},
};

std::array<unsigned char, 64> Sig64(const std::string& hex)
{
    std::vector<unsigned char> v = ParseHex(hex);
    BOOST_REQUIRE_EQUAL(v.size(), 64U);
    std::array<unsigned char, 64> out;
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

uint256 Point(const std::string& hex)
{
    std::vector<unsigned char> v = ParseHex(hex);
    BOOST_REQUIRE_EQUAL(v.size(), 32U);
    uint256 out;
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

/** ContextualCheckTransaction's verdict as (accepted, reject reason, DoS score). */
struct Verdict {
    bool ok;
    std::string reason;
    int dos;
};
Verdict Check(const CMutableTransaction& mtx, int nHeight, bool isMined)
{
    CTransaction tx(mtx);
    CValidationState state;
    bool ok = ContextualCheckTransaction(tx, state, Params(), nHeight, isMined,
                                         [](const Consensus::Params&) { return false; });
    int dos = 0;
    state.IsInvalid(dos);
    return {ok, state.GetRejectReason(), dos};
}

/** A valid Sapling-to-Sapling transaction (one spend, two outputs) and a valid t->z one. */
CMutableTransaction ValidZtoZ(const Consensus::Params& consensus, int nHeight)
{
    auto sk = libzcash::SaplingSpendingKey::random();
    auto expsk = sk.expanded_spending_key();
    auto pa = sk.default_address();
    auto testNote = GetTestSaplingNote(pa, 40000);
    auto builder = TransactionBuilder(consensus, nHeight);
    builder.AddSaplingSpend(expsk, testNote.note, testNote.tree.root(), testNote.tree.witness());
    builder.AddSaplingOutput(sk.full_viewing_key().ovk, pa, 25000, {});
    return CMutableTransaction(builder.Build().GetTxOrThrow());
}

CMutableTransaction ValidTtoZ(const Consensus::Params& consensus, int nHeight)
{
    CBasicKeyStore keystore;
    CKey tsk = AddTestCKeyToKeyStore(keystore);
    auto scriptPubKey = GetScriptForDestination(tsk.GetPubKey().GetID());
    auto pa = libzcash::SaplingSpendingKey::random().default_address();
    auto builder = TransactionBuilder(consensus, nHeight, &keystore);
    builder.AddTransparentInput(COutPoint(uint256S("1234"), 0), scriptPubKey, 50000);
    builder.AddSaplingOutput(uint256(), pa, 40000, {});
    return CMutableTransaction(builder.Build().GetTxOrThrow());
}

/** A coinbase with one Sapling output, recoverable with ovk = 0 (ZIP 213). */
CMutableTransaction ShieldedCoinbase(const OutputDescription& odesc)
{
    CMutableTransaction mtx;
    mtx.fOverwintered = true;
    mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    mtx.nVersion = SAPLING_TX_VERSION;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vin[0].scriptSig = CScript() << 1 << OP_0;
    mtx.vShieldedOutput.push_back(odesc);
    mtx.valueBalance = -123456;
    return mtx;
}

/** An output whose note, ciphertexts and cmu are made for pk_d, with cv and proof taken from
 *  a real output (they do not verify for this note, which the checks after recovery report). */
OutputDescription OutputTo(const OutputDescription& real, const uint256& pk_d)
{
    auto addr = libzcash::SaplingSpendingKey::random().default_address();
    uint256 rcm;
    librustzcash_sapling_generate_r(rcm.begin());
    libzcash::SaplingNote note(addr.d, pk_d, 123456, rcm, libzcash::Zip212Enabled::BeforeZip212);
    OutputDescription od = real;
    // a pk_d outside the prime-order subgroup has no note commitment here; keep the real one
    auto cmu = note.cmu();
    if (cmu) od.cmu = *cmu;
    libzcash::SaplingNotePlaintext pt(note, {{0xF6}});
    auto res = pt.encrypt(pk_d);
    BOOST_REQUIRE(res);
    auto enc = res->second;
    od.ephemeralKey = enc.get_epk();
    od.encCiphertext = res->first;
    libzcash::SaplingOutgoingPlaintext outPt(pk_d, enc.get_esk());
    od.outCiphertext = outPt.encrypt(uint256(), od.cv, od.cmu, enc);
    return od;
}

OutputDescription RealOutput()
{
    auto note = libzcash::SaplingNote(
        libzcash::SaplingSpendingKey::random().default_address(), CAmount(123456), libzcash::Zip212Enabled::BeforeZip212);
    auto ctx = librustzcash_sapling_proving_ctx_init();
    auto odesc = OutputDescriptionInfo(uint256(), note, {{0xF6}}).Build(ctx);
    librustzcash_sapling_proving_ctx_free(ctx);
    BOOST_REQUIRE(odesc);
    return *odesc;
}


// ---- A Sapling transaction built here, so the test knows each spend's rsk = ask + alpha ----

const arith_uint256 JUBJUB_R =
    UintToArith256(uint256S("0e7db4ea6533afa906673b0101343b00a6682093ccc81082d0970e5ed6f72cb7"));

arith_uint256 AddModR(const arith_uint256& a, const arith_uint256& b)
{
    arith_uint256 s = a + b; // a, b < r < 2^252: no overflow
    if (s >= JUBJUB_R) s -= JUBJUB_R;
    return s;
}

arith_uint256 MulModR(const arith_uint256& a, const arith_uint256& b)
{
    arith_uint256 acc = 0;
    for (int i = 255; i >= 0; i--) {
        acc = AddModR(acc, acc);
        if (((a >> i).GetLow64() & 1) != 0) acc = AddModR(acc, b);
    }
    return acc;
}

/** c = H*(Rbar || rk || sighash), the challenge zcash_primitives 0.5 and reddsa both compute. */
arith_uint256 Challenge(const unsigned char* rbar, const uint256& rk, const uint256& sighash)
{
    unsigned char personal[BLAKE2bPersonalBytes];
    memcpy(personal, "Zcash_RedJubjubH", BLAKE2bPersonalBytes);
    BLAKE2bState* st = blake2b_init(64, personal);
    blake2b_update(st, rbar, 32);
    blake2b_update(st, rk.begin(), 32);
    blake2b_update(st, sighash.begin(), 32);
    unsigned char h[64];
    blake2b_finalize(st, h, 64);
    blake2b_free(st);
    uint256 c;
    librustzcash_to_scalar(h, c.begin());
    return UintToArith256(c);
}

/** A spendAuthSig with nonce 0: R is the given encoding of (0,1) or (0,-1), S = c * rsk. Valid
 *  under the cofactored check (8R = O), so the 4.5.0 verifier accepts all four encodings. */
std::array<unsigned char, 64> NonceZeroSig(const std::string& rHex, const uint256& rsk,
                                           const uint256& rk, const uint256& sighash)
{
    uint256 rbar = Point(rHex);
    uint256 sbar = ArithToUint256(MulModR(Challenge(rbar.begin(), rk, sighash), UintToArith256(rsk)));
    std::array<unsigned char, 64> sig;
    std::copy(rbar.begin(), rbar.end(), sig.begin());
    std::copy(sbar.begin(), sbar.end(), sig.begin() + 32);
    return sig;
}

struct BuiltTx {
    CMutableTransaction mtx;
    std::vector<uint256> rsk; //!< per spend
    uint256 sighash;
};

/** nSpends honest spends of notes in one tree and one output, all proofs and signatures real. */
BuiltTx BuildSapling(const Consensus::Params& consensus, int nHeight, size_t nSpends)
{
    auto sk = libzcash::SaplingSpendingKey::random();
    auto expsk = sk.expanded_spending_key();
    auto fvk = expsk.full_viewing_key();
    auto pa = sk.default_address();

    std::vector<libzcash::SaplingNote> notes;
    std::vector<SaplingWitness> wits;
    SaplingMerkleTree tree;
    for (size_t i = 0; i < nSpends; i++) {
        notes.emplace_back(pa, 10000, libzcash::Zip212Enabled::BeforeZip212);
        uint256 cm = notes.back().cmu().value();
        for (auto& w : wits) w.append(cm);
        tree.append(cm);
        wits.push_back(tree.witness());
    }
    const uint256 anchor = tree.root();

    BuiltTx out;
    CMutableTransaction& mtx = out.mtx;
    mtx.fOverwintered = true;
    mtx.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    mtx.nVersion = SAPLING_TX_VERSION;
    mtx.nExpiryHeight = 0;

    auto ctx = librustzcash_sapling_proving_ctx_init();
    std::vector<uint256> alphas;
    for (size_t i = 0; i < nSpends; i++) {
        uint256 alpha;
        librustzcash_sapling_generate_r(alpha.begin());
        alphas.push_back(alpha);
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << wits[i].path();
        std::vector<unsigned char> witness(ss.begin(), ss.end());
        SpendDescription sdesc;
        uint256 rcm = notes[i].rcm();
        BOOST_REQUIRE(librustzcash_sapling_spend_proof(
            ctx, fvk.ak.begin(), expsk.nsk.begin(), notes[i].d.data(), rcm.begin(), alpha.begin(),
            notes[i].value(), anchor.begin(), witness.data(),
            sdesc.cv.begin(), sdesc.rk.begin(), sdesc.zkproof.data()));
        sdesc.anchor = anchor;
        sdesc.nullifier = notes[i].nullifier(fvk, wits[i].position()).value();
        mtx.vShieldedSpend.push_back(sdesc);
        out.rsk.push_back(ArithToUint256(AddModR(UintToArith256(expsk.ask), UintToArith256(alpha))));
    }
    const CAmount fee = 1000;
    libzcash::SaplingNote outNote(pa, nSpends * 10000 - fee, libzcash::Zip212Enabled::BeforeZip212);
    auto odesc = OutputDescriptionInfo(fvk.ovk, outNote, {{0xF6}}).Build(ctx);
    BOOST_REQUIRE(odesc);
    mtx.vShieldedOutput.push_back(*odesc);
    mtx.valueBalance = fee;

    out.sighash = SignatureHash(CScript(), mtx, NOT_AN_INPUT, SIGHASH_ALL, 0,
                                CurrentEpochBranchId(nHeight, consensus));
    for (size_t i = 0; i < nSpends; i++) {
        librustzcash_sapling_spend_sig(expsk.ask.begin(), alphas[i].begin(), out.sighash.begin(),
                                       mtx.vShieldedSpend[i].spendAuthSig.data());
    }
    librustzcash_sapling_binding_sig(ctx, mtx.valueBalance, out.sighash.begin(), mtx.bindingSig.data());
    librustzcash_sapling_proving_ctx_free(ctx);
    return out;
}

/** The 4.5.0 Sapling verifier on every description and the binding signature: exactly the calls
 *  ContextualCheckTransaction makes after the ZIP 216 check. */
bool OldVerifierAccepts(const CMutableTransaction& mtx, const uint256& sighash)
{
    auto ctx = librustzcash_sapling_verification_ctx_init();
    bool ok = true;
    for (const SpendDescription& s : mtx.vShieldedSpend) {
        ok = ok && librustzcash_sapling_check_spend(ctx, s.cv.begin(), s.anchor.begin(), s.nullifier.begin(),
                                                    s.rk.begin(), s.zkproof.begin(), s.spendAuthSig.begin(),
                                                    sighash.begin());
    }
    for (const OutputDescription& o : mtx.vShieldedOutput) {
        ok = ok && librustzcash_sapling_check_output(ctx, o.cv.begin(), o.cmu.begin(), o.ephemeralKey.begin(),
                                                     o.zkproof.begin());
    }
    ok = ok && librustzcash_sapling_final_check(ctx, mtx.valueBalance, mtx.bindingSig.begin(), sighash.begin());
    librustzcash_sapling_verification_ctx_free(ctx);
    return ok;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(zip216_tests, JoinSplitTestingSetup)

BOOST_AUTO_TEST_CASE(predicate_is_the_decoder_difference)
{
    BOOST_CHECK(IsNonCanonicalJubjubEncoding(Point(NONCANON_V_ONE).begin()));
    BOOST_CHECK(IsNonCanonicalJubjubEncoding(Point(NONCANON_V_MINUS_ONE).begin()));
    BOOST_CHECK(!IsNonCanonicalJubjubEncoding(Point(CANON_V_ONE).begin()));
    BOOST_CHECK(!IsNonCanonicalJubjubEncoding(Point(CANON_V_MINUS_ONE).begin()));
    int differing = 0;
    for (const DecodeRow& row : DECODE_ROWS) {
        const bool pred = IsNonCanonicalJubjubEncoding(Point(row.hex).begin());
        // the 6.x decoder accepts exactly what this line's decoder accepts and the predicate passes
        BOOST_CHECK_MESSAGE(row.accept010 == (row.accept045 && !pred), row.hex);
        if (row.accept045 != row.accept010) differing++;
    }
    BOOST_CHECK(differing > 0);
}

BOOST_AUTO_TEST_CASE(witness_signature_R)
{
    for (const WitnessSig& w : WITNESS) {
        auto sig = Sig64(w.sig);
        BOOST_CHECK_MESSAGE(IsNonCanonicalJubjubEncoding(sig.data()) == w.nonCanonical,
                            std::string(w.kind) + " " + w.name);
        // the S half never matters to the predicate
        BOOST_CHECK(!IsNonCanonicalJubjubEncoding(sig.data() + 32));
    }
}

BOOST_AUTO_TEST_CASE(spend_auth_sig_R)
{
    RegtestActivateSapling();
    ActivateVault();
    const Consensus::Params& consensus = Params().GetConsensus();
    const CMutableTransaction valid = ValidZtoZ(consensus, H);
    BOOST_REQUIRE_EQUAL(valid.vShieldedSpend.size(), 1U);

    // control: the honest transaction is valid, mined or in the mempool
    for (bool isMined : {true, false}) {
        Verdict v = Check(valid, H, isMined);
        BOOST_CHECK(v.ok);
        BOOST_CHECK_EQUAL(v.reason, "");
    }

    for (const WitnessSig& w : WITNESS) {
        if (std::string(w.kind) != "spendAuthSig") continue;
        CMutableTransaction mtx = valid;
        mtx.vShieldedSpend[0].spendAuthSig = Sig64(w.sig);
        for (bool isMined : {true, false}) {
            Verdict v = Check(mtx, H, isMined);
            BOOST_CHECK(!v.ok);
            if (w.nonCanonical) {
                BOOST_CHECK_EQUAL(v.reason, "bad-sapling-bundle-authorization");
                BOOST_CHECK_EQUAL(v.dos, isMined ? 100 : 0);
            } else {
                // a canonical R passes the new rule and reaches the RedJubjub check, which fails
                // because the witness signs another key and message
                BOOST_CHECK_EQUAL(v.reason, "bad-txns-sapling-spend-description-invalid");
            }
        }
    }
    DeactivateVault();
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_CASE(binding_sig_R)
{
    RegtestActivateSapling();
    ActivateVault();
    const Consensus::Params& consensus = Params().GetConsensus();
    for (const CMutableTransaction& valid : {ValidZtoZ(consensus, H), ValidTtoZ(consensus, H)}) {
        BOOST_CHECK(Check(valid, H, true).ok);
        for (const WitnessSig& w : WITNESS) {
            if (std::string(w.kind) != "bindingSig") continue;
            CMutableTransaction mtx = valid;
            mtx.bindingSig = Sig64(w.sig);
            for (bool isMined : {true, false}) {
                Verdict v = Check(mtx, H, isMined);
                BOOST_CHECK(!v.ok);
                if (w.nonCanonical) {
                    BOOST_CHECK_EQUAL(v.reason, "bad-sapling-bundle-authorization");
                    BOOST_CHECK_EQUAL(v.dos, isMined ? 100 : 0);
                } else {
                    BOOST_CHECK_EQUAL(v.reason, "bad-txns-sapling-binding-signature-invalid");
                }
            }
        }
    }
    DeactivateVault();
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_CASE(coinbase_pk_d)
{
    RegtestActivateHeartwood(false, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    ActivateVault();
    const OutputDescription real = RealOutput();

    // control: an honest shielded coinbase passes recovery and reaches the binding signature
    // (left unset here), as in the gtest HeartwoodAcceptsShieldedCoinbase
    Verdict control = Check(ShieldedCoinbase(real), H, true);
    BOOST_CHECK_EQUAL(control.reason, "bad-txns-sapling-binding-signature-invalid");

    // a non-canonical bindingSig R on a coinbase is caught the same way as on any transaction
    {
        CMutableTransaction mtx = ShieldedCoinbase(real);
        mtx.bindingSig = Sig64(WITNESS[5].sig);
        BOOST_CHECK_EQUAL(Check(mtx, H, true).reason, "bad-sapling-bundle-authorization");
    }

    // a well-formed note to another pk_d: recovery passes, the proof (made for another note) fails
    {
        auto other = libzcash::SaplingSpendingKey::random().default_address();
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, other.pk_d)), H, true);
        BOOST_CHECK_EQUAL(v.reason, "bad-txns-sapling-output-description-invalid");
    }

    // pk_d = identity, canonical and non-canonical: rejected at recovery, as sapling-crypto does
    for (const std::string& pk : {CANON_V_ONE, NONCANON_V_ONE}) {
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, Point(pk))), H, true);
        BOOST_CHECK(!v.ok);
        BOOST_CHECK_EQUAL(v.reason, "bad-cb-output-desc-invalid-ct");
        BOOST_CHECK_EQUAL(v.dos, 100);
    }

    // pk_d = (0,-1), non-canonical: rejected by the new rule; canonical: already rejected here,
    // because the note commitment requires pk_d in the prime-order subgroup (rustzcash.rs)
    {
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, Point(NONCANON_V_MINUS_ONE))), H, true);
        BOOST_CHECK_EQUAL(v.reason, "bad-cb-output-desc-invalid-ct");
    }
    DeactivateVault();
    RegtestDeactivateHeartwood();
}

BOOST_AUTO_TEST_CASE(old_final_check_accepts_noncanonical_R)
{
    // The 4.5.0 library itself, no proofs needed: an empty verification context with
    // valueBalance 0 has bvk = identity, so a bindingSig with R of order 1 or 2 and S = 0 satisfies
    // the cofactored equation. librustzcash_sapling_final_check accepts all four encodings;
    // reddsa (the 6.x line) rejects the two non-canonical ones, which is what the predicate flags.
    uint256 sighash = GetRandHash();
    for (const std::string& r : {CANON_V_ONE, NONCANON_V_ONE, CANON_V_MINUS_ONE, NONCANON_V_MINUS_ONE}) {
        std::array<unsigned char, 64> sig{};
        uint256 rbar = Point(r);
        std::copy(rbar.begin(), rbar.end(), sig.begin());
        auto ctx = librustzcash_sapling_verification_ctx_init();
        BOOST_CHECK_MESSAGE(librustzcash_sapling_final_check(ctx, 0, sig.data(), sighash.begin()), r);
        librustzcash_sapling_verification_ctx_free(ctx);
        const bool nonCanonical = (r == NONCANON_V_ONE || r == NONCANON_V_MINUS_ONE);
        BOOST_CHECK_EQUAL(IsNonCanonicalJubjubEncoding(sig.data()), nonCanonical);
    }
}

BOOST_AUTO_TEST_CASE(valid_transaction_with_noncanonical_R_on_any_spend)
{
    // End to end: a transaction whose proofs and signatures are all real, three spends of notes in
    // one tree. One spend at a time is re-signed with nonce 0 under each of the four encodings of
    // (0,1) and (0,-1), the others keep their honest signatures. The 4.5.0 verifier accepts every
    // variant; the rule rejects exactly the non-canonical ones, on whichever spend they sit (a check
    // of the first spend only fails here, at spends 1 and 2).
    RegtestActivateSapling();
    ActivateVault();
    const Consensus::Params& consensus = Params().GetConsensus();
    BuiltTx built = BuildSapling(consensus, H, 3);
    BOOST_REQUIRE(OldVerifierAccepts(built.mtx, built.sighash));
    for (bool isMined : {true, false}) {
        Verdict v = Check(built.mtx, H, isMined);
        BOOST_CHECK(v.ok);
        BOOST_CHECK_EQUAL(v.reason, "");
    }
    for (size_t i = 0; i < built.mtx.vShieldedSpend.size(); i++) {
        for (const std::string& r : {CANON_V_ONE, NONCANON_V_ONE, CANON_V_MINUS_ONE, NONCANON_V_MINUS_ONE}) {
            CMutableTransaction mtx = built.mtx;
            mtx.vShieldedSpend[i].spendAuthSig =
                NonceZeroSig(r, built.rsk[i], mtx.vShieldedSpend[i].rk, built.sighash);
            const bool nonCanonical = (r == NONCANON_V_ONE || r == NONCANON_V_MINUS_ONE);
            const std::string what = "spend " + std::to_string(i) + " R " + r;
            BOOST_CHECK_MESSAGE(OldVerifierAccepts(mtx, built.sighash), what);
            for (bool isMined : {true, false}) {
                Verdict v = Check(mtx, H, isMined);
                if (nonCanonical) {
                    BOOST_CHECK_MESSAGE(!v.ok, what);
                    BOOST_CHECK_EQUAL(v.reason, "bad-sapling-bundle-authorization");
                    BOOST_CHECK_EQUAL(v.dos, isMined ? 100 : 0);
                } else {
                    BOOST_CHECK_MESSAGE(v.ok, what + " " + v.reason);
                }
            }
        }
    }
    // the witness signatures on the last spend only: rejected for a non-canonical R
    for (const WitnessSig& w : WITNESS) {
        if (std::string(w.kind) != "spendAuthSig" || !w.nonCanonical) continue;
        CMutableTransaction mtx = built.mtx;
        mtx.vShieldedSpend[2].spendAuthSig = Sig64(w.sig);
        BOOST_CHECK_EQUAL(Check(mtx, H, true).reason, "bad-sapling-bundle-authorization");
    }
    DeactivateVault();
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_CASE(enforced_before_and_at_vault)
{
    // The rule does not depend on the epoch: before Vault and at its activation height alike.
    RegtestActivateSapling();
    ActivateVault();
    const Consensus::Params& consensus = Params().GetConsensus();
    for (int h : {VAULT_HEIGHT - 1, VAULT_HEIGHT}) {
        BuiltTx built = BuildSapling(consensus, h, 2);
        CMutableTransaction mtx = built.mtx;
        mtx.vShieldedSpend[1].spendAuthSig =
            NonceZeroSig(NONCANON_V_ONE, built.rsk[1], mtx.vShieldedSpend[1].rk, built.sighash);
        BOOST_CHECK(OldVerifierAccepts(mtx, built.sighash));
        Verdict v = Check(mtx, h, true);
        BOOST_CHECK(!v.ok);
        BOOST_CHECK_EQUAL(v.reason, "bad-sapling-bundle-authorization");
    }
    DeactivateVault();
    RegtestDeactivateSapling();

    RegtestActivateHeartwood(false, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    ActivateVault();
    const OutputDescription real = RealOutput();
    for (int h : {VAULT_HEIGHT - 1, VAULT_HEIGHT}) {
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, Point(CANON_V_ONE))), h, true);
        BOOST_CHECK_EQUAL(v.reason, "bad-cb-output-desc-invalid-ct");
    }
    DeactivateVault();
    RegtestDeactivateHeartwood();
}

BOOST_AUTO_TEST_SUITE_END()
