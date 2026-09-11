// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// The version-2 payload codec (plan §3.3, V23): round trips, fixed-width
// little-endian layout, every malformed case, feeVout semantics and the
// transaction-level shape rules FindPayload applies.

#include "yellowback/payload.h"

#include "key.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "test/test_bitcoin.h"
#include "utilstrencodings.h"

#include <boost/test/unit_test.hpp>

using namespace yellowback;

namespace {

CPubKey TestKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key.GetPubKey();
}

std::vector<unsigned char> Hex(const std::string& s) { return ParseHex(s); }

CMutableTransaction TxWithOutputs(size_t n, const CScript& opret, size_t opretIndex)
{
    CMutableTransaction mtx;
    for (size_t i = 0; i < n; i++) {
        mtx.vout.push_back(CTxOut(10000, CScript() << OP_TRUE));
    }
    mtx.vout[opretIndex].scriptPubKey = opret;
    mtx.vout[opretIndex].nValue = 0;
    return mtx;
}

/** A fixed syntactically valid compressed key, so hex vectors are reproducible. */
const std::string KEYHEX = "02cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70";

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_payload_tests, BasicTestingSetup)

// Rule: MINT-1
// Rule: XFER-1
// Rule: RED-1
BOOST_AUTO_TEST_CASE(mint1_roundtrip_table)
{
    CPubKey owner = TestKey();
    std::vector<Assignment> fourteen, fifteen;
    for (uint8_t i = 0; i < 15; i++) {
        if (i < 14) fourteen.push_back(Assignment(i, 100 + i));
        fifteen.push_back(Assignment(i, 100 + i));
    }
    const std::vector<std::pair<Payload, size_t>> cases = {
        { Payload::Mint(0, 10000, 1000, 950, owner, 3), 51 },
        { Payload::Mint(2, 1000000, 0xFFFFFFFF, 0, owner, FEE_VOUT_NONE), 51 },
        { Payload::Mint(0xFF, 0, 0, 0xFFFFFFFF, owner, 0), 51 },      // the codec fixes the shape, MINT-2 the ranges
        { Payload::Transfer({}), 5 },
        { Payload::Transfer({ Assignment(1, 100) }), 10 },
        { Payload::Transfer({ Assignment(0, 100), Assignment(1, 200), Assignment(3, 0xFFFFFFFF) }), 20 },
        { Payload::Transfer(fifteen), 80 },                            // the maximum, 80 bytes exactly
        { Payload::Redeem(950, 2, {}), 10 },                           // count = 0 allowed (§3.5)
        { Payload::Redeem(950, FEE_VOUT_NONE, { Assignment(2, 12345) }), 15 },
        { Payload::Redeem(0xFFFFFFFF, 0, fourteen), 80 },              // the maximum, 80 bytes exactly
    };
    for (const auto& c : cases) {
        std::vector<unsigned char> enc = EncodePayload(c.first);
        BOOST_CHECK_EQUAL(enc.size(), c.second);
        BOOST_REQUIRE(!enc.empty());
        BOOST_CHECK_EQUAL(enc[0], 0x59);
        BOOST_CHECK_EQUAL(enc[1], 0x42);
        BOOST_CHECK_EQUAL(enc[2], PAYLOAD_VERSION);
        BOOST_CHECK_EQUAL(enc[3], (unsigned char)c.first.type);
        Payload dec;
        BOOST_REQUIRE(DecodePayload(enc, dec));
        BOOST_CHECK(dec == c.first);
        BOOST_CHECK_EQUAL(dec.version, PAYLOAD_VERSION);
        BOOST_CHECK_EQUAL(dec.AssignedCents(), c.first.AssignedCents());
        BOOST_CHECK(EncodePayload(dec) == enc);
    }
    // Sums fit int64: 15 x 2^32.
    std::vector<Assignment> big;
    for (uint8_t i = 0; i < 15; i++) big.push_back(Assignment(i, 0xFFFFFFFF));
    BOOST_CHECK_EQUAL(Payload::Transfer(big).AssignedCents(), 15LL * 0xFFFFFFFFLL);
}

// Rule: MINT-1
// Rule: RED-1
BOOST_AUTO_TEST_CASE(mint1_fixed_width_little_endian_layout)
{
    CPubKey owner(Hex(KEYHEX));
    std::vector<unsigned char> enc = EncodePayload(Payload::Mint(2, 0x01020304, 0x0A0B0C0D, 0x11223344, owner, 0x03));
    BOOST_REQUIRE_EQUAL(enc.size(), 51u);
    BOOST_CHECK_EQUAL(HexStr(enc), "59420201" "02" "04030201" "0d0c0b0a" "44332211" + KEYHEX + "03");
    enc = EncodePayload(Payload::Transfer({ Assignment(3, 0x0100) }));
    BOOST_CHECK_EQUAL(HexStr(enc), "59420202" "01" "03" "00010000");
    enc = EncodePayload(Payload::Redeem(0x11223344, 0xFF, { Assignment(2, 0x0100) }));
    BOOST_CHECK_EQUAL(HexStr(enc), "59420203" "44332211" "ff" "01" "02" "00010000");
    enc = EncodePayload(Payload::Redeem(7, 1, {}));
    BOOST_CHECK_EQUAL(HexStr(enc), "59420203" "07000000" "01" "00");
}

// Rule: MINT-1
// Rule: XFER-1
// Rule: RED-1
BOOST_AUTO_TEST_CASE(mint1_malformed_table)
{
    const std::string mintBody = "00" "10270000" "e8030000" "b6030000" + KEYHEX + "ff";   // 47 bytes
    struct Case { const char* name; std::string hex; bool ok; };
    const std::vector<Case> cases = {
        { "mint_ok",             "59420201" + mintBody, true },
        { "bad_magic_0",         "58420201" + mintBody, false },
        { "bad_magic_1",         "59430201" + mintBody, false },
        { "version_0",           "59420001" + mintBody, false },
        { "version_3",           "59420301" + mintBody, false },
        { "version_ff",          "5942ff01" + mintBody, false },
        { "unknown_type_04",     "59420204" "01000000", false },
        { "retired_type_10",     "59420210" "50c3000000000000", false },   // the prototype's PRICE, reserved in v2
        { "reserved_type_1f",    "5942021f" "00", false },
        { "reserved_type_20",    "59420220" "00", false },
        { "reserved_type_ff",    "594202ff" "00", false },
        { "type_00",             "59420200" "00", false },
        { "empty",               "", false },
        { "one_byte",            "59", false },
        { "header_only",         "594202", false },
        { "header_no_body",      "59420201", false },
        { "mint_short_by_one",   "59420201" + mintBody.substr(0, mintBody.size() - 2), false },
        { "mint_no_feevout",     "59420201" "00" "10270000" "e8030000" "b6030000" + KEYHEX, false },   // the v1 length
        { "mint_trailing",       "59420201" + mintBody + "00", false },
        // Any 33 bytes decode as the owner key (the codec fixes the shape; MINT-3 gives bad-mint-owner-key
        // and a VOID vault records the bytes verbatim, SERIALISATION.md §3 C).
        { "mint_key_prefix_04",  "59420201" "00" "10270000" "e8030000" "b6030000" "04" + KEYHEX.substr(2) + "ff", true },
        { "mint_key_prefix_01",  "59420201" "00" "10270000" "e8030000" "b6030000" "01" + KEYHEX.substr(2) + "ff", true },
        { "mint_key_prefix_03",  "59420201" "00" "10270000" "e8030000" "b6030000" "03" + KEYHEX.substr(2) + "ff", true },
        { "transfer_empty",      "59420202" "00", true },
        { "transfer_short",      "59420202" "01" "01640000", false },
        { "transfer_long",       "59420202" "01" "0164000000" "00", false },
        { "transfer_count_short","59420202" "02" "0164000000", false },
        { "transfer_zero_cents", "59420202" "01" "0100000000", false },
        { "transfer_dup_vout",   "59420202" "02" "0164000000" "0164000000", false },
        { "transfer_two",        "59420202" "02" "0164000000" "0264000000", true },
        { "transfer_trailing",   "59420202" "00" "00", false },
        { "redeem_empty",        "59420203" "b6030000" "ff" "00", true },
        { "redeem_one",          "59420203" "b6030000" "03" "01" "0139300000", true },
        { "redeem_head_short",   "59420203" "b6030000" "ff", false },
        { "redeem_short",        "59420203" "b6030000" "ff" "01" "01393000", false },
        { "redeem_trailing",     "59420203" "b6030000" "ff" "00" "00", false },
        { "redeem_zero_cents",   "59420203" "b6030000" "ff" "01" "0100000000", false },
        { "redeem_dup_vout",     "59420203" "b6030000" "ff" "02" "0164000000" "0164000000", false },
    };
    for (const Case& c : cases) {
        Payload p;
        BOOST_CHECK_MESSAGE(DecodePayload(Hex(c.hex), p) == c.ok, c.name);
    }
    // TRANSFER count 16 = 85 bytes: over MAX_PAYLOAD and over the count bound; 15 is the maximum.
    {
        std::vector<unsigned char> sixteen = Hex("5942020210");
        for (uint8_t i = 0; i < 16; i++) { sixteen.push_back(i); sixteen.push_back(1); sixteen.push_back(0); sixteen.push_back(0); sixteen.push_back(0); }
        Payload p;
        BOOST_CHECK(!DecodePayload(sixteen, p));
        std::vector<unsigned char> fifteen = Hex("594202020f");
        for (uint8_t i = 0; i < 15; i++) { fifteen.push_back(i); fifteen.push_back(1); fifteen.push_back(0); fifteen.push_back(0); fifteen.push_back(0); }
        BOOST_CHECK_EQUAL(fifteen.size(), 80u);
        BOOST_CHECK(DecodePayload(fifteen, p));
        BOOST_CHECK_EQUAL(p.assignments.size(), 15u);
    }
    // REDEEM count 15 = 85 bytes: rejected; 14 = 80 bytes is the maximum (the refHeight/feeVout head costs one slot).
    {
        std::vector<unsigned char> fifteen = Hex("59420203" "b6030000" "ff" "0f");
        for (uint8_t i = 0; i < 15; i++) { fifteen.push_back(i); fifteen.push_back(1); fifteen.push_back(0); fifteen.push_back(0); fifteen.push_back(0); }
        Payload p;
        BOOST_CHECK(!DecodePayload(fifteen, p));
        std::vector<unsigned char> fourteen = Hex("59420203" "b6030000" "ff" "0e");
        for (uint8_t i = 0; i < 14; i++) { fourteen.push_back(i); fourteen.push_back(1); fourteen.push_back(0); fourteen.push_back(0); fourteen.push_back(0); }
        BOOST_CHECK_EQUAL(fourteen.size(), 80u);
        BOOST_CHECK(DecodePayload(fourteen, p));
        BOOST_CHECK_EQUAL(p.assignments.size(), 14u);
        // The encoder refuses 15 for REDEEM but accepts it for TRANSFER.
        std::vector<Assignment> as;
        for (uint8_t i = 0; i < 15; i++) as.push_back(Assignment(i, 1));
        BOOST_CHECK(EncodePayload(Payload::Redeem(1, 0xFF, as)).empty());
        BOOST_CHECK_EQUAL(EncodePayload(Payload::Transfer(as)).size(), 80u);
    }
    // 81 bytes never decodes, whatever the header.
    {
        std::vector<unsigned char> long81 = Hex("59420202");
        long81.resize(81, 0);
        Payload p;
        BOOST_CHECK(!DecodePayload(long81, p));
    }
    // The encoder refuses what the decoder would refuse.
    BOOST_CHECK(EncodePayload(Payload::Transfer({ Assignment(1, 0) })).empty());
    BOOST_CHECK(EncodePayload(Payload::Transfer({ Assignment(1, 1), Assignment(1, 2) })).empty());
    CPubKey unc;
    { CKey k; k.MakeNewKey(false); unc = k.GetPubKey(); }
    BOOST_CHECK(EncodePayload(Payload::Mint(0, 1, 1, 1, unc, 0xFF)).empty());
    BOOST_CHECK(EncodePayload(Payload::Mint(0, 1, 1, 1, CPubKey(), 0xFF)).empty());
}

// Rule: MINT-8
// Rule: RED-3
// feeVout: 0xFF = no fee output; any other value is carried as is. The codec
// does not range-check it (K11): MINT-8/RED-3 compare it with vout.size()
// and the reserved indices, so FindPayload keeps a payload whose feeVout is
// out of range.
BOOST_AUTO_TEST_CASE(mint8_feevout_semantics)
{
    CPubKey owner = TestKey();
    for (uint8_t fv : { (uint8_t)0, (uint8_t)3, (uint8_t)0xFE, FEE_VOUT_NONE }) {
        Payload p;
        BOOST_REQUIRE(DecodePayload(EncodePayload(Payload::Mint(0, 10000, 1000, 950, owner, fv)), p));
        BOOST_CHECK_EQUAL(p.feeVout, fv);
        BOOST_REQUIRE(DecodePayload(EncodePayload(Payload::Redeem(950, fv, {})), p));
        BOOST_CHECK_EQUAL(p.feeVout, fv);
    }
    BOOST_CHECK_EQUAL(FEE_VOUT_NONE, 0xFF);
    BOOST_CHECK_EQUAL(Payload().feeVout, FEE_VOUT_NONE);
    // A MINT with feeVout = 9 in a 3-output transaction still parses (the rule, not the codec, rejects it).
    CMutableTransaction mtx = TxWithOutputs(3, PayloadScript(EncodePayload(Payload::Mint(0, 10000, 1000, 950, owner, 9))), 2);
    auto fp = FindPayload(CTransaction(mtx));
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL(fp->payload.feeVout, 9);
    BOOST_CHECK_EQUAL(fp->opReturnIndex, 2u);
}

// Rule: TX-0
BOOST_AUTO_TEST_CASE(tx0_opreturn_shape)
{
    std::vector<unsigned char> data = EncodePayload(Payload::Transfer({ Assignment(1, 100) }));
    // Exactly OP_RETURN <push>.
    BOOST_CHECK(ExtractOpReturnData(PayloadScript(data)).value() == data);
    // Not OP_RETURN.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << data).has_value());
    BOOST_CHECK(!ExtractOpReturnData(CScript()).has_value());
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN).has_value());
    // Two pushes.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << data << data).has_value());
    // OP_N is not a data push.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << OP_1).has_value());
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << OP_0).has_value());
    // Trailing opcode.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << data << OP_DROP).has_value());
    // Length bounds on the push: 3 bytes and 81 bytes are out; 4 and 80 are in (decoding is separate).
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(3, 1)).has_value());
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(81, 1)).has_value());
    BOOST_CHECK(ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(4, 1)).has_value());
    BOOST_CHECK(ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(80, 1)).has_value());
    // A non-minimal push (OP_PUSHDATA1 for 10 bytes) is still one push of the bytes.
    {
        CScript s;
        s << OP_RETURN;
        s.push_back(OP_PUSHDATA1);
        s.push_back((unsigned char)data.size());
        s.insert(s.end(), data.begin(), data.end());
        BOOST_CHECK(ExtractOpReturnData(s).value() == data);
    }
    // Truncated PUSHDATA.
    {
        CScript s;
        s << OP_RETURN;
        s.push_back(OP_PUSHDATA1);
        s.push_back(50);
        s.push_back(1);
        BOOST_CHECK(!ExtractOpReturnData(s).has_value());
    }
}

// Rule: TX-0
// Rule: XFER-1
BOOST_AUTO_TEST_CASE(tx0_find_payload_in_transaction)
{
    CPubKey owner = TestKey();
    const CScript mint = PayloadScript(EncodePayload(Payload::Mint(0, 10000, 1000, 950, owner, 3)));
    const CScript xfer = PayloadScript(EncodePayload(Payload::Transfer({ Assignment(0, 100), Assignment(2, 200) })));

    // Found, with its index.
    {
        CMutableTransaction mtx = TxWithOutputs(4, mint, 2);
        auto fp = FindPayload(CTransaction(mtx));
        BOOST_REQUIRE(fp.has_value());
        BOOST_CHECK_EQUAL(fp->opReturnIndex, 2u);
        BOOST_CHECK(fp->payload.type == PayloadType::MINT);
        BOOST_CHECK_EQUAL(fp->payload.refHeight, 950u);
    }
    // No OP_RETURN.
    {
        CMutableTransaction mtx = TxWithOutputs(2, CScript() << OP_TRUE, 0);
        BOOST_CHECK(!FindPayload(CTransaction(mtx)).has_value());
        BOOST_CHECK(!FindOpReturn(CTransaction(mtx)).has_value());
    }
    // Two OP_RETURNs: non-Yellowback even if one is a good payload.
    {
        CMutableTransaction mtx = TxWithOutputs(4, mint, 2);
        mtx.vout[3].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>(4, 0);
        BOOST_CHECK(!FindPayload(CTransaction(mtx)).has_value());
        BOOST_CHECK(!FindOpReturn(CTransaction(mtx)).has_value());
    }
    // A bare OP_RETURN (no push) counts as the OP_RETURN output but carries no payload.
    {
        CMutableTransaction mtx = TxWithOutputs(2, CScript() << OP_RETURN, 1);
        BOOST_CHECK_EQUAL(FindOpReturn(CTransaction(mtx)).value(), 1u);
        BOOST_CHECK(!FindPayload(CTransaction(mtx)).has_value());
    }
    // Assigned vout beyond the outputs.
    {
        CMutableTransaction mtx = TxWithOutputs(2, xfer, 1);   // assigns vout 2, which does not exist
        BOOST_CHECK(!FindPayload(CTransaction(mtx)).has_value());
    }
    // Assigned vout is the OP_RETURN itself.
    {
        CMutableTransaction mtx = TxWithOutputs(3, xfer, 2);   // assigns vout 2 = the OP_RETURN
        BOOST_CHECK(!FindPayload(CTransaction(mtx)).has_value());
        CMutableTransaction ok = TxWithOutputs(4, xfer, 3);
        BOOST_CHECK(FindPayload(CTransaction(ok)).has_value());
    }
    // A REDEEM with count = 0 has no assignments to check.
    {
        CMutableTransaction mtx = TxWithOutputs(2, PayloadScript(EncodePayload(Payload::Redeem(1, 0xFF, {}))), 1);
        auto fp = FindPayload(CTransaction(mtx));
        BOOST_REQUIRE(fp.has_value());
        BOOST_CHECK(fp->payload.type == PayloadType::REDEEM);
        BOOST_CHECK(fp->payload.assignments.empty());
    }
    // A version-1 payload of another family is ignored at the transaction level too (V23; see the retained case below).
    {
        CMutableTransaction mtx = TxWithOutputs(2, PayloadScript(Hex("59420304" "00000000")), 1);
        BOOST_CHECK(!FindPayload(CTransaction(mtx)).has_value());
    }
    BOOST_CHECK_EQUAL(std::string(PayloadTypeName(PayloadType::MINT)), "mint");
    BOOST_CHECK_EQUAL(std::string(PayloadTypeName(PayloadType::TRANSFER)), "transfer");
    BOOST_CHECK_EQUAL(std::string(PayloadTypeName(PayloadType::REDEEM)), "redeem");
}

// ---------------------------------------------------------------------------
// V23: version 1 (the prototype's layout, PRICE 0x10 included) is non-Yellowback.
// Rule: MINT-1
BOOST_AUTO_TEST_CASE(version1_is_non_yellowback)
{
    Payload p;
    BOOST_CHECK(!DecodePayload(Hex("59420101" "00" "10270000" "e8030000" "b6030000" + KEYHEX), p));
    BOOST_CHECK(!DecodePayload(Hex("59420110" "50c3000000000000"), p));
    BOOST_CHECK(!DecodePayload(Hex("59420103" "01" "0139300000"), p));
    BOOST_CHECK(!DecodePayload(Hex("59420210" "50c3000000000000"), p));   // 0x10 under version 2: reserved
}

BOOST_AUTO_TEST_SUITE_END()
