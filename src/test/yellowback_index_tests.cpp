// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// The index's exception boundary and start-up rules (plan §4.3, §8.3):
// an exception inside block application sets the unhealthy flag and never
// propagates into the notifier thread; Stop() silences later deliveries.
// Phase 3 reshapes this file around CheckConnect/CommitConnect/UndoDisconnect.

#include "yellowback/index.h"
#include "yellowback/script.h"

#include "chain.h"
#include "key.h"
#include "main.h"
#include "primitives/block.h"
#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

using namespace yellowback;

namespace {

yellowback::Params MakeTestParams()
{
    return RegtestParams(1, 0, 0, 0);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_index_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(exception_boundary)
{
    yellowback::Params params = MakeTestParams();
    YellowbackIndex index(params, pathTemp / "yellowback-test", 1 << 20, true);
    BOOST_CHECK(index.SyncToChain());        // chain at genesis (< startHeight): empty and healthy
    BOOST_CHECK(index.IsHealthy());
    {
        LOCK(cs_main);
        LOCK(index.cs_yellowback);
        BOOST_CHECK(!index.GetTip().has_value());
        BOOST_CHECK(index.IsSynced());
    }

    // A fake block index for height 1 on top of the regtest genesis.
    CBlockIndex* genesis = chainActive.Genesis();
    BOOST_REQUIRE(genesis != nullptr);
    uint256 hash1 = uint256S("01");
    CBlockIndex idx1;
    idx1.nHeight = 1;
    idx1.pprev = genesis;
    idx1.phashBlock = &hash1;
    CBlock block;

    // Fault injection: an exception in CheckConnect is caught, marks the index unhealthy, does not propagate.
    index.testBeforeApply = [] { throw std::runtime_error("injected fault"); };
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index.CheckConnect(block, &idx1, false));
    }
    BOOST_CHECK(!index.IsHealthy());
    BOOST_CHECK(index.UnhealthyReason().find("injected fault") != std::string::npos);
    // Further deliveries are ignored while unhealthy.
    index.testBeforeApply = nullptr;
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index.CheckConnect(block, &idx1, false));
    }
    BOOST_CHECK(!index.IsHealthy());

    // Fresh index: the start block applies (v2 has no genesis anchor; an empty block is a valid start).
    YellowbackIndex index2(params, pathTemp / "yellowback-test2", 1 << 20, true);
    BOOST_CHECK(index2.SyncToChain());
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index2.CheckConnect(block, &idx1, false));
        BOOST_CHECK(index2.CommitConnect(block, &idx1));
    }
    BOOST_CHECK(index2.IsHealthy());
    {
        LOCK(index2.cs_yellowback);
        BOOST_REQUIRE(index2.GetTip().has_value());
        BOOST_CHECK_EQUAL(index2.GetTip()->height, 1);
    }

    // Stop(): later deliveries return at once and change nothing (D2).
    YellowbackIndex index3(params, pathTemp / "yellowback-test3", 1 << 20, true);
    BOOST_CHECK(index3.SyncToChain());
    index3.Stop();
    BOOST_CHECK(index3.IsStopped());
    index3.testBeforeApply = [] { throw std::runtime_error("must not run"); };
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index3.CheckConnect(block, &idx1, false));
    }
    BOOST_CHECK(index3.IsHealthy());

    // A disconnect of a block the index never had is ignored; below startHeight nothing happens.
    YellowbackIndex index4(params, pathTemp / "yellowback-test4", 1 << 20, true);
    BOOST_CHECK(index4.SyncToChain());
    {
        LOCK(cs_main);
        BOOST_CHECK(index4.UndoDisconnect(genesis));
    }
    BOOST_CHECK(index4.IsHealthy());
    {
        LOCK(index4.cs_yellowback);
        BOOST_CHECK(!index4.GetTip().has_value());
    }

    // An unconfigured network refuses to start (mainnet placeholders until the ceremony).
    YellowbackIndex index5(MainParams(), pathTemp / "yellowback-test5", 1 << 20, true);
    BOOST_CHECK(!index5.SyncToChain());
    BOOST_CHECK(!index5.IsHealthy());
}

BOOST_AUTO_TEST_SUITE_END()
