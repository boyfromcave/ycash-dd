#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""Live v2 index smoke test: quote tags, snapshots, activation and restart."""

from test_framework.util import assert_equal
from test_framework.yellowback_util import (
    ACTIVATION_BLOCKS,
    P_FAST_WINDOW,
    P_MID_WINDOW,
    P_SLOW_WINDOW,
    POOLS,
    YellowbackTestFramework,
    assert_best_hash,
    assert_same_statehash,
    set_quote,
    wait_yed_healthy,
)


class YellowbackIndexTest(YellowbackTestFramework):
    """# Rule: TAG-1 TAG-2 TAG-3 PRICE-1 PRICE-2 ACT-1 ACT-2 ACT-3 SNAP UNDO"""

    def run_test(self):
        nodes = self.nodes

        print('initial synchronous index state')
        for node in self.enforcing_nodes():
            info = wait_yed_healthy(node)
            assert_equal(info['rpcversion'], 2)
            assert_equal(info['network'], 'regtest')
            assert_equal(info['height'], node.getblockcount())
            assert_equal(info['startHeight'], 1)
        assert_best_hash(nodes)
        assert_same_statehash(self.enforcing_nodes())

        print('quote tags from the three pool nodes')
        for pool in POOLS:
            result = set_quote(nodes[pool], '2.00')
            assert_equal(result['priceMicroUsd'], 2_000_000)
            assert_equal(result['nextTag']['kind'], 'quote')
            assert_equal(result['nextTag']['signal'], True)

        self.mine_round_robin(POOLS, P_SLOW_WINDOW)
        tip = nodes[0].getblockcount()
        tag = nodes[2].yed_gettag(str(tip))
        assert_equal(tag['found'], True)
        assert_equal(tag['kind'], 'quote')
        assert_equal(tag['priceMicroUsd'], 2_000_000)
        price = nodes[0].yed_getprice()
        assert_equal(price['fill']['fast']['quoteTags'], P_FAST_WINDOW)
        assert_equal(price['fill']['mid']['quoteTags'], P_MID_WINDOW)
        assert_equal(price['fill']['slow']['quoteTags'], P_SLOW_WINDOW)
        assert_equal(price['pFast'], 2_000_000)
        assert_equal(price['pMid'], 2_000_000)
        assert_equal(price['pSlow'], 2_000_000)
        assert_same_statehash(self.enforcing_nodes())

        print('activation after the full signalling window and delay')
        self.mine_round_robin(POOLS, ACTIVATION_BLOCKS - P_SLOW_WINDOW)
        for node in self.enforcing_nodes():
            activation = node.yed_getactivation()
            assert_equal(activation['status'], 'active')
            assert_equal(activation['signalCount'], 64)
            assert_equal(node.yed_getstats()['mintingAllowed'], True)
        self.checkpoint('after activation')

        print('restart preserves the synchronous index')
        before = nodes[2].yed_getstatehash()['statehash']
        self.restart(2)
        self.sync_all(blocks_only=True)
        assert_equal(wait_yed_healthy(nodes[2])['height'], nodes[2].getblockcount())
        assert_equal(nodes[2].yed_getstatehash()['statehash'], before)
        self.checkpoint('after restart')


if __name__ == '__main__':
    YellowbackIndexTest().main()
