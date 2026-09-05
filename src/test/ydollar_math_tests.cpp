// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "ydollar/math.h"
#include "ydollar/params.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

using namespace ydollar;

BOOST_FIXTURE_TEST_SUITE(ydollar_math_tests, BasicTestingSetup)

// §3.6 worked example: 1,000 YEC at $0.05 backing $10 => 500 %.
BOOST_AUTO_TEST_CASE(health_worked_example)
{
    BOOST_CHECK_EQUAL(Health(1000, 1000 * COIN, 50000), 500);
    // Empty supply: capped.
    BOOST_CHECK_EQUAL(Health(0, 0, 50000), HEALTH_CAP);
    BOOST_CHECK_EQUAL(Health(0, 1000 * COIN, std::nullopt), HEALTH_CAP);
    // Undefined price: fail closed.
    BOOST_CHECK_EQUAL(Health(1000, 1000 * COIN, std::nullopt), 0);
    // No collateral.
    BOOST_CHECK_EQUAL(Health(1000, 0, 50000), 0);
    // Cap: 1e9 YEC at $100 backing $10 => astronomically high, capped.
    BOOST_CHECK_EQUAL(Health(1000, 1000000000LL * COIN, PRICE_MAX), HEALTH_CAP);
    // Exactly 100 %: 200 YEC at $0.05 = $10 backing $10.
    BOOST_CHECK_EQUAL(Health(1000, 200 * COIN, 50000), 100);
    // Floor: 199.99 YEC => 99 %.
    BOOST_CHECK_EQUAL(Health(1000, 200 * COIN - 1, 50000), 99);
}

// ref/digibyte/src/consensus/dca.cpp:53-58
BOOST_AUTO_TEST_CASE(dca_table)
{
    BOOST_CHECK_EQUAL(DcaBps(HEALTH_CAP), 10000);
    BOOST_CHECK_EQUAL(DcaBps(150), 10000);
    BOOST_CHECK_EQUAL(DcaBps(149), 12500);
    BOOST_CHECK_EQUAL(DcaBps(120), 12500);
    BOOST_CHECK_EQUAL(DcaBps(119), 15000);
    BOOST_CHECK_EQUAL(DcaBps(110), 15000);
    BOOST_CHECK_EQUAL(DcaBps(109), 20000);
    BOOST_CHECK_EQUAL(DcaBps(0), 20000);
}

// ref/digibyte/src/consensus/err.cpp:38-41
BOOST_AUTO_TEST_CASE(err_table)
{
    BOOST_CHECK_EQUAL(ErrBps(HEALTH_CAP), 10000);
    BOOST_CHECK_EQUAL(ErrBps(100), 10000);
    BOOST_CHECK_EQUAL(ErrBps(99), 9500);
    BOOST_CHECK_EQUAL(ErrBps(95), 9500);
    BOOST_CHECK_EQUAL(ErrBps(94), 9000);
    BOOST_CHECK_EQUAL(ErrBps(90), 9000);
    BOOST_CHECK_EQUAL(ErrBps(89), 8500);
    BOOST_CHECK_EQUAL(ErrBps(85), 8500);
    BOOST_CHECK_EQUAL(ErrBps(84), 8000);
    BOOST_CHECK_EQUAL(ErrBps(0), 8000);
}

// ref/digibyte/src/consensus/err.cpp:100 — RequiredDD = ceil(Original * 10000 / ratioBps)
BOOST_AUTO_TEST_CASE(required_burn)
{
    BOOST_CHECK_EQUAL(RequiredBurn(10000, 10000), 10000);
    BOOST_CHECK_EQUAL(RequiredBurn(10000, 8000), 12500);   // 125 %
    BOOST_CHECK_EQUAL(RequiredBurn(10000, 9500), 10527);   // ceil(10526.3)
    BOOST_CHECK_EQUAL(RequiredBurn(1, 9500), 2);           // ceil(1.05)
    BOOST_CHECK_EQUAL(RequiredBurn(0, 8000), 0);
    BOOST_CHECK_EQUAL(RequiredBurn(1000000, 8000), 1250000);
}

// $10 at 500 % with DCA 1.0x at $0.05/YEC => 1,000 YEC.
BOOST_AUTO_TEST_CASE(required_collateral_worked_example)
{
    auto r = RequiredCollateral(1000, 500, 10000, 50000);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 1000 * COIN);

    // DCA 2.0x doubles it.
    r = RequiredCollateral(1000, 500, 20000, 50000);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 2000 * COIN);

    // Ceiling: $1 at 300 % at $0.07 => 3/0.07 = 42.857142857.. YEC
    r = RequiredCollateral(100, 300, 10000, 70000);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 4285714286LL);

    // Rounded up to 1,000 zat.
    r = RequiredCollateralRounded(100, 300, 10000, 70000);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 4285715000LL);
    BOOST_CHECK_EQUAL(r.value() % 1000, 0);

    // Already a multiple: unchanged.
    r = RequiredCollateralRounded(1000, 500, 10000, 50000);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 1000 * COIN);

    // Invalid inputs.
    BOOST_CHECK(!RequiredCollateral(0, 500, 10000, 50000).has_value());
    BOOST_CHECK(!RequiredCollateral(1000, 500, 10000, 0).has_value());
}

// Worst case magnitude: MAX_MINT at tier 0 (1000 %), DCA 2.0x, PRICE_MIN.
// 1e6 * 1000 * 20000 * 1e8 / (100 * 100) = 2e17 zat => exceeds MAX_MONEY, must
// not overflow or abort; and the numerator (2e21) exceeds 64 bits.
BOOST_AUTO_TEST_CASE(required_collateral_overflow)
{
    const Params& p = MainParams();
    auto r = RequiredCollateral(p.maxMint, p.tierRatioPct[0], 20000, PRICE_MIN);
    BOOST_CHECK(!r.has_value()); // 2e17 zat > MAX_MONEY (2.1e15): refused, not wrapped

    // Same at a realistic price of $0.05 => 1e6*1000*20000*1e8/(100*50000) = 4e14 zat
    r = RequiredCollateral(p.maxMint, p.tierRatioPct[0], 20000, 50000);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 400000000000000LL);

    // MAX_OUTPUT-sized cents at PRICE_MAX still fine.
    r = RequiredCollateral(p.maxOutput, 300, 10000, PRICE_MAX);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value(), 3000 * COIN); // $100,000 * 300 % / $100 per YEC = 3,000 YEC
}

BOOST_AUTO_TEST_CASE(volatility_breach)
{
    // 20 % move in the short window breaches at exactly the threshold.
    BOOST_CHECK(VolatilityBreach(120000, 100000, VOL_1H_BPS));
    BOOST_CHECK(VolatilityBreach(80000, 100000, VOL_1H_BPS));
    BOOST_CHECK(!VolatilityBreach(119999, 100000, VOL_1H_BPS));
    BOOST_CHECK(!VolatilityBreach(80001, 100000, VOL_1H_BPS));
    // 30 % in the long window.
    BOOST_CHECK(VolatilityBreach(130000, 100000, VOL_24H_BPS));
    BOOST_CHECK(!VolatilityBreach(129999, 100000, VOL_24H_BPS));
    // Undefined reference: not evaluated.
    BOOST_CHECK(!VolatilityBreach(130000, std::nullopt, VOL_24H_BPS));
    BOOST_CHECK(!VolatilityBreach(std::nullopt, 100000, VOL_24H_BPS));
}

BOOST_AUTO_TEST_CASE(params_tables)
{
    const Params& m = MainParams();
    BOOST_CHECK_EQUAL(m.network, "main");
    BOOST_CHECK_EQUAL(m.tierBlocks[0], 48);
    BOOST_CHECK_EQUAL(m.tierBlocks[4], 420480);
    BOOST_CHECK_EQUAL(m.tierRatioPct[0], 1000);
    BOOST_CHECK_EQUAL(m.tierRatioPct[4], 300);
    BOOST_CHECK_EQUAL(m.supplyCap, 100000000);
    BOOST_CHECK_EQUAL(m.rosterGrace, 1152);
    BOOST_CHECK(!m.IsConfigured()); // genesis anchor awaits the key ceremony

    Params r = RegtestParams(150, COutPoint(uint256S("01"), 0), CScript() << OP_1, 0);
    BOOST_CHECK(r.IsConfigured());
    BOOST_CHECK_EQUAL(r.tierBlocks[4], 240);
    BOOST_CHECK_EQUAL(r.tierRatioPct[4], 300);
    BOOST_CHECK_EQUAL(r.rosterGrace, 48);
    BOOST_CHECK_EQUAL(r.volCooldown, 96);
    BOOST_CHECK_EQUAL(r.volWindowLong, 96);
    BOOST_CHECK_EQUAL(r.supplyCap, 0);
    BOOST_CHECK(!ParamsForNetwork("regtest").IsConfigured());
    BOOST_CHECK_THROW(ParamsForNetwork("nope"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
