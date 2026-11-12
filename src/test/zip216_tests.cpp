// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// ZIP 216 on this line: the R half of every Sapling spendAuthSig and of the bindingSig must be a
// canonical Jubjub encoding, and a coinbase Sapling output's pk_d must decode canonically and not
// be the identity. jubjub 0.10 / reddsa 0.5.2 / sapling-crypto 0.7.0 (the 6.x line) and the
// zakura-* crates (kogany) enforce both at every height; jubjub 0.5.1 here accepts the two
// non-canonical encodings, so ContextualCheckTransaction rejects them by byte compare.

#include "chainparams.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "key.h"
#include "keystore.h"
#include "main.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"
#include "transaction_builder.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "utiltest.h"
#include "zcash/Note.hpp"
#include "zcash/NoteEncryption.hpp"
#include "zcash/address/sapling.hpp"

#include <librustzcash.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {

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
CMutableTransaction ValidZtoZ(const Consensus::Params& consensus)
{
    auto sk = libzcash::SaplingSpendingKey::random();
    auto expsk = sk.expanded_spending_key();
    auto pa = sk.default_address();
    auto testNote = GetTestSaplingNote(pa, 40000);
    auto builder = TransactionBuilder(consensus, 2);
    builder.AddSaplingSpend(expsk, testNote.note, testNote.tree.root(), testNote.tree.witness());
    builder.AddSaplingOutput(sk.full_viewing_key().ovk, pa, 25000, {});
    return CMutableTransaction(builder.Build().GetTxOrThrow());
}

CMutableTransaction ValidTtoZ(const Consensus::Params& consensus)
{
    CBasicKeyStore keystore;
    CKey tsk = AddTestCKeyToKeyStore(keystore);
    auto scriptPubKey = GetScriptForDestination(tsk.GetPubKey().GetID());
    auto pa = libzcash::SaplingSpendingKey::random().default_address();
    auto builder = TransactionBuilder(consensus, 1, &keystore);
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
    auto consensus = RegtestActivateSapling();
    const CMutableTransaction valid = ValidZtoZ(consensus);
    BOOST_REQUIRE_EQUAL(valid.vShieldedSpend.size(), 1U);

    // control: the honest transaction is valid at every height, mined or in the mempool
    for (bool isMined : {true, false}) {
        Verdict v = Check(valid, 3, isMined);
        BOOST_CHECK(v.ok);
        BOOST_CHECK_EQUAL(v.reason, "");
    }

    for (const WitnessSig& w : WITNESS) {
        if (std::string(w.kind) != "spendAuthSig") continue;
        CMutableTransaction mtx = valid;
        mtx.vShieldedSpend[0].spendAuthSig = Sig64(w.sig);
        for (bool isMined : {true, false}) {
            Verdict v = Check(mtx, 3, isMined);
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
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_CASE(binding_sig_R)
{
    auto consensus = RegtestActivateSapling();
    for (const CMutableTransaction& valid : {ValidZtoZ(consensus), ValidTtoZ(consensus)}) {
        BOOST_CHECK(Check(valid, 3, true).ok);
        for (const WitnessSig& w : WITNESS) {
            if (std::string(w.kind) != "bindingSig") continue;
            CMutableTransaction mtx = valid;
            mtx.bindingSig = Sig64(w.sig);
            for (bool isMined : {true, false}) {
                Verdict v = Check(mtx, 3, isMined);
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
    RegtestDeactivateSapling();
}

BOOST_AUTO_TEST_CASE(coinbase_pk_d)
{
    RegtestActivateHeartwood(false, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    const OutputDescription real = RealOutput();

    // control: an honest shielded coinbase passes recovery and reaches the binding signature
    // (left unset here), as in the gtest HeartwoodAcceptsShieldedCoinbase
    Verdict control = Check(ShieldedCoinbase(real), 10, true);
    BOOST_CHECK_EQUAL(control.reason, "bad-txns-sapling-binding-signature-invalid");

    // a non-canonical bindingSig R on a coinbase is caught the same way as on any transaction
    {
        CMutableTransaction mtx = ShieldedCoinbase(real);
        mtx.bindingSig = Sig64(WITNESS[5].sig);
        BOOST_CHECK_EQUAL(Check(mtx, 10, true).reason, "bad-sapling-bundle-authorization");
    }

    // a well-formed note to another pk_d: recovery passes, the proof (made for another note) fails
    {
        auto other = libzcash::SaplingSpendingKey::random().default_address();
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, other.pk_d)), 10, true);
        BOOST_CHECK_EQUAL(v.reason, "bad-txns-sapling-output-description-invalid");
    }

    // pk_d = identity, canonical and non-canonical: rejected at recovery, as sapling-crypto does
    for (const std::string& pk : {CANON_V_ONE, NONCANON_V_ONE}) {
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, Point(pk))), 10, true);
        BOOST_CHECK(!v.ok);
        BOOST_CHECK_EQUAL(v.reason, "bad-cb-output-desc-invalid-ct");
        BOOST_CHECK_EQUAL(v.dos, 100);
    }

    // pk_d = (0,-1), non-canonical: rejected by the new rule; canonical: already rejected here,
    // because the note commitment requires pk_d in the prime-order subgroup (rustzcash.rs)
    {
        Verdict v = Check(ShieldedCoinbase(OutputTo(real, Point(NONCANON_V_MINUS_ONE))), 10, true);
        BOOST_CHECK_EQUAL(v.reason, "bad-cb-output-desc-invalid-ct");
    }
    RegtestDeactivateHeartwood();
}

BOOST_AUTO_TEST_SUITE_END()
