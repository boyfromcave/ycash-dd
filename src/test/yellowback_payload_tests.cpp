// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

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

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_payload_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(roundtrip_table)
{
    CPubKey owner = TestKey();
    std::vector<std::pair<Payload, size_t>> cases = {
        { Payload::Mint(0, 10000, 1000, 950, owner), 50 },
        { Payload::Mint(4, 1000000, 0xFFFFFFFF, 0, owner), 50 },
        { Payload::Transfer({}), 5 },
        { Payload::Transfer({ Assignment(1, 100) }), 10 },
        { Payload::Transfer({ Assignment(0, 100), Assignment(1, 200), Assignment(3, 0xFFFFFFFF) }), 20 },
        { Payload::Redeem({}), 5 },
        { Payload::Redeem({ Assignment(2, 12345) }), 10 },
        { Payload::Price(1), 12 },
        { Payload::Price(0xFFFFFFFFFFFFFFFFULL), 12 },
    };
    // 15 assignments: the maximum, 80 bytes exactly.
    std::vector<Assignment> fifteen;
    for (uint8_t i = 0; i < 15; i++) fifteen.push_back(Assignment(i, 100 + i));
    cases.push_back({ Payload::Transfer(fifteen), 80 });

    for (const auto& c : cases) {
        std::vector<unsigned char> enc = EncodePayload(c.first);
        BOOST_CHECK_EQUAL(enc.size(), c.second);
        BOOST_REQUIRE(!enc.empty());
        BOOST_CHECK_EQUAL(enc[0], 0x59);
        BOOST_CHECK_EQUAL(enc[1], 0x42);
        BOOST_CHECK_EQUAL(enc[2], 0x01);
        Payload dec;
        BOOST_REQUIRE(DecodePayload(enc, dec));
        BOOST_CHECK(dec == c.first);
        BOOST_CHECK_EQUAL(dec.AssignedCents(), c.first.AssignedCents());
    }
}

BOOST_AUTO_TEST_CASE(fixed_width_little_endian)
{
    CPubKey owner = TestKey();
    std::vector<unsigned char> enc = EncodePayload(Payload::Mint(2, 0x01020304, 0x0A0B0C0D, 0x11223344, owner));
    BOOST_REQUIRE_EQUAL(enc.size(), 50u);
    BOOST_CHECK_EQUAL(HexStr(enc.begin(), enc.begin() + 17), "594201010204030201" "0d0c0b0a" "44332211");
    BOOST_CHECK(std::equal(owner.begin(), owner.end(), enc.begin() + 17));

    enc = EncodePayload(Payload::Price(0x0102030405060708ULL));
    BOOST_CHECK_EQUAL(HexStr(enc), "594201100807060504030201");

    enc = EncodePayload(Payload::Transfer({ Assignment(3, 0x00000100) }));
    BOOST_CHECK_EQUAL(HexStr(enc), "5942010201" "03" "00010000");
}

BOOST_AUTO_TEST_CASE(malformed_cases)
{
    CPubKey owner = TestKey();
    std::string keyhex = HexStr(owner.begin(), owner.end());
    Payload p;

    // Too short, too long.
    BOOST_CHECK(!DecodePayload(Hex("594201"), p));
    BOOST_CHECK(!DecodePayload(std::vector<unsigned char>(81, 0), p));
    // Bad magic / version / type.
    BOOST_CHECK(!DecodePayload(Hex("5945011000000000000000000"), p));
    BOOST_CHECK(!DecodePayload(Hex("594202100100000000000000"), p));
    BOOST_CHECK(!DecodePayload(Hex("594201040100000000000000"), p));   // unknown type 0x04 (forward-compat)
    BOOST_CHECK(!DecodePayload(Hex("5942011100000000000000000"), p));  // unknown type 0x11
    // PRICE with a short body / trailing byte.
    BOOST_CHECK(!DecodePayload(Hex("59420110010000000000"), p));
    BOOST_CHECK(!DecodePayload(Hex("59420110010000000000000000"), p));
    // MINT: short, long, uncompressed key prefix, invalid key prefix.
    BOOST_CHECK(!DecodePayload(Hex("59420101" "00" "10270000" "e8030000" "b6030000") , p));
    BOOST_CHECK(!DecodePayload(Hex("59420101" "00" "10270000" "e8030000" "b6030000" + keyhex + "00"), p));
    {
        std::string bad = keyhex; bad[0] = '0'; bad[1] = '4';
        BOOST_CHECK(!DecodePayload(Hex("59420101" "00" "10270000" "e8030000" "b6030000" + bad), p));
        bad[1] = '5';
        BOOST_CHECK(!DecodePayload(Hex("59420101" "00" "10270000" "e8030000" "b6030000" + bad), p));
    }
    BOOST_CHECK(DecodePayload(Hex("59420101" "00" "10270000" "e8030000" "b6030000" + keyhex), p));
    BOOST_CHECK_EQUAL(p.cents, 10000u);
    BOOST_CHECK_EQUAL(p.lockHeight, 1000u);
    BOOST_CHECK_EQUAL(p.evalHeight, 950u);
    // TRANSFER: count/body mismatch, count > 15, cents == 0, duplicate vout.
    BOOST_CHECK(!DecodePayload(Hex("5942010202" "0164000000"), p));
    BOOST_CHECK(!DecodePayload(Hex("5942010201" "0164000000" "00"), p));
    BOOST_CHECK(!DecodePayload(Hex("5942010200" "00"), p));
    {
        std::vector<unsigned char> sixteen = Hex("5942010210");
        for (int i = 0; i < 16; i++) { sixteen.push_back(i); sixteen.push_back(1); sixteen.push_back(0); sixteen.push_back(0); sixteen.push_back(0); }
        BOOST_CHECK_EQUAL(sixteen.size(), 85u);
        BOOST_CHECK(!DecodePayload(sixteen, p));
    }
    BOOST_CHECK(!DecodePayload(Hex("5942010201" "0100000000"), p));
    BOOST_CHECK(!DecodePayload(Hex("5942010202" "0164000000" "0164000000"), p));
    BOOST_CHECK(DecodePayload(Hex("5942010202" "0164000000" "0264000000"), p));
    BOOST_CHECK_EQUAL(p.AssignedCents(), 200);
    // REDEEM with count 0 is valid and self-describing.
    BOOST_CHECK(DecodePayload(Hex("5942010300"), p));
    BOOST_CHECK(p.type == PayloadType::REDEEM);
    BOOST_CHECK(p.assignments.empty());

    // Encoder refuses the same malformed inputs.
    BOOST_CHECK(EncodePayload(Payload::Transfer({ Assignment(1, 0) })).empty());
    BOOST_CHECK(EncodePayload(Payload::Transfer({ Assignment(1, 1), Assignment(1, 2) })).empty());
    std::vector<Assignment> sixteen;
    for (uint8_t i = 0; i < 16; i++) sixteen.push_back(Assignment(i, 1));
    BOOST_CHECK(EncodePayload(Payload::Transfer(sixteen)).empty());
    BOOST_CHECK(EncodePayload(Payload::Mint(0, 1, 1, 1, CPubKey())).empty());
}

BOOST_AUTO_TEST_CASE(opreturn_shape)
{
    std::vector<unsigned char> data = EncodePayload(Payload::Price(5));
    // Canonical shape: OP_RETURN <push>.
    BOOST_CHECK(ExtractOpReturnData(PayloadScript(data)) == data);
    // PUSHDATA1 encoding of the same data is also a single push.
    {
        CScript s;
        s << OP_RETURN;
        s.push_back(OP_PUSHDATA1);
        s.push_back((unsigned char)data.size());
        s.insert(s.end(), data.begin(), data.end());
        BOOST_CHECK(ExtractOpReturnData(s) == data);
    }
    // Not OP_RETURN first.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << data << OP_RETURN).has_value());
    // Bare OP_RETURN.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN).has_value());
    // Two pushes.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << data << data).has_value());
    // OP_N is not a data push.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << OP_5).has_value());
    // Trailing opcode.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << data << OP_DROP).has_value());
    // Below the minimum size / above the maximum.
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(3, 0x59)).has_value());
    BOOST_CHECK(!ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(81, 0x59)).has_value());
    BOOST_CHECK(ExtractOpReturnData(CScript() << OP_RETURN << std::vector<unsigned char>(80, 0x59)).has_value());
}

BOOST_AUTO_TEST_CASE(find_payload_in_transaction)
{
    CScript opret = PayloadScript(EncodePayload(Payload::Transfer({ Assignment(0, 100), Assignment(1, 200) })));

    // vout: [token, token, OP_RETURN]
    CMutableTransaction mtx = TxWithOutputs(3, opret, 2);
    auto fp = FindPayload(CTransaction(mtx));
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK_EQUAL(fp->opReturnIndex, 2u);
    BOOST_CHECK(fp->payload.type == PayloadType::TRANSFER);
    BOOST_CHECK_EQUAL(fp->payload.AssignedCents(), 300);

    // No OP_RETURN at all.
    CMutableTransaction plain = TxWithOutputs(2, CScript() << OP_TRUE, 0);
    BOOST_CHECK(!FindOpReturn(CTransaction(plain)).has_value());
    BOOST_CHECK(!FindPayload(CTransaction(plain)).has_value());

    // Two OP_RETURN outputs: non-Yellowback regardless of contents (A4).
    CMutableTransaction two = TxWithOutputs(3, opret, 2);
    two.vout[1].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>(4, 1);
    BOOST_CHECK(!FindOpReturn(CTransaction(two)).has_value());
    BOOST_CHECK(!FindPayload(CTransaction(two)).has_value());

    // Assigned vout out of range.
    CMutableTransaction shortTx = TxWithOutputs(2, opret, 1);
    // vout[1] is the OP_RETURN itself here: assignment (1, 200) points at it => non-Yellowback.
    BOOST_CHECK(!FindPayload(CTransaction(shortTx)).has_value());
    CScript opret3 = PayloadScript(EncodePayload(Payload::Transfer({ Assignment(0, 100), Assignment(5, 200) })));
    CMutableTransaction oor = TxWithOutputs(3, opret3, 2);
    BOOST_CHECK(!FindPayload(CTransaction(oor)).has_value());

    // Malformed payload in a well-shaped OP_RETURN: non-Yellowback, but FindOpReturn still sees the output.
    CMutableTransaction bad = TxWithOutputs(3, CScript() << OP_RETURN << std::vector<unsigned char>(10, 0xAA), 2);
    BOOST_CHECK(FindOpReturn(CTransaction(bad)).has_value());
    BOOST_CHECK(!FindPayload(CTransaction(bad)).has_value());

    // MINT payload: no assignments to range-check; found wherever the OP_RETURN sits.
    CPubKey owner = TestKey();
    CScript mint = PayloadScript(EncodePayload(Payload::Mint(1, 20000, 5000, 4960, owner)));
    CMutableTransaction m = TxWithOutputs(4, mint, 2);
    fp = FindPayload(CTransaction(m));
    BOOST_REQUIRE(fp.has_value());
    BOOST_CHECK(fp->payload.type == PayloadType::MINT);
    BOOST_CHECK(fp->payload.ownerPubKey == owner);
    BOOST_CHECK_EQUAL(std::string(PayloadTypeName(fp->payload.type)), "mint");
}

BOOST_AUTO_TEST_SUITE_END()
