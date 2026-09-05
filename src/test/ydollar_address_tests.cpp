// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "ydollar/address.h"
#include "ydollar/params.h"

#include "base58.h"
#include "key.h"
#include "test/test_bitcoin.h"
#include "utilstrencodings.h"

#include <boost/test/unit_test.hpp>

using namespace ydollar;

BOOST_FIXTURE_TEST_SUITE(ydollar_address_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(prefixes_and_roundtrip)
{
    const Params& main = MainParams();
    const Params& test = TestParams();
    Params regtest = RegtestParams(0, COutPoint(), CScript());

    std::vector<CKeyID> ids;
    ids.push_back(CKeyID(uint160()));
    ids.push_back(CKeyID(uint160(std::vector<unsigned char>(20, 0xff))));
    for (int i = 0; i < 20; i++) {
        CKey key;
        key.MakeNewKey(true);
        ids.push_back(key.GetPubKey().GetID());
    }

    for (const CKeyID& id : ids) {
        std::string m = EncodeAddress(id, main);
        std::string t = EncodeAddress(id, test);
        std::string r = EncodeAddress(id, regtest);
        BOOST_CHECK_EQUAL(m.substr(0, 2), "yd");
        BOOST_CHECK_EQUAL(t.substr(0, 2), "yt");
        BOOST_CHECK_EQUAL(r.substr(0, 2), "yr");
        BOOST_CHECK_EQUAL(m.size(), 35u);
        BOOST_CHECK_EQUAL(t.size(), 35u);
        BOOST_CHECK_EQUAL(r.size(), 35u);

        CKeyID back;
        BOOST_CHECK(DecodeAddress(m, main, back));
        BOOST_CHECK(back == id);
        BOOST_CHECK(DecodeAddress(t, test, back));
        BOOST_CHECK(back == id);
        BOOST_CHECK(DecodeAddress(r, regtest, back));
        BOOST_CHECK(back == id);

        // Cross-network rejection.
        BOOST_CHECK(!DecodeAddress(m, test, back));
        BOOST_CHECK(!DecodeAddress(m, regtest, back));
        BOOST_CHECK(!DecodeAddress(t, main, back));
        BOOST_CHECK(!DecodeAddress(r, main, back));
        BOOST_CHECK(!IsValidAddress(t, regtest));
    }
}

BOOST_AUTO_TEST_CASE(rejects_garbage)
{
    const Params& main = MainParams();
    CKeyID id;
    BOOST_CHECK(!DecodeAddress("", main, id));
    BOOST_CHECK(!DecodeAddress("yd", main, id));
    // A Ycash transparent address (s1…) is not a YDollar address.
    BOOST_CHECK(!DecodeAddress("s1RyNzGjPzkgc7jP6uvjJx8tmv7gY9dvRbP", main, id));
    // Checksum damage.
    CKey key;
    key.MakeNewKey(true);
    std::string a = EncodeAddress(key.GetPubKey().GetID(), main);
    std::string damaged = a;
    damaged[damaged.size() - 1] = (damaged.back() == '1') ? '2' : '1';
    BOOST_CHECK(!DecodeAddress(damaged, main, id));
    // Version bytes right but payload wrong length.
    std::vector<unsigned char> data = main.addressVersion;
    data.insert(data.end(), 19, 0);
    BOOST_CHECK(!DecodeAddress(EncodeBase58Check(data), main, id));
}

BOOST_AUTO_TEST_SUITE_END()
