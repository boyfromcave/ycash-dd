#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Protection systems (plan D12, §3.6, §6 Phase 5): DCA bands, ERR bands and
the volatility mint freeze, driven by the price on a fixed position.

One $100 vault backed by 20 YEC: health = 20 * price / $100 = price / 5.
Nodes: 0 user, 1 spare, 2-4 federation (2-of-3).
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes_bi,
    start_node,
    start_nodes,
    stop_node,
    sync_mempools,
)
from test_framework.yellowback_util import (
    assert_same_statehash,
    assert_yed_synced,
    build_mint_tx,
    fund_genesis_anchor,
    make_regtest_roster,
    publish_price,
    yellowback_node_args,
)

REGTEST_COOLDOWN = 96


def assert_rpc_error(substr, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        assert substr in str(e), "expected %r in %r" % (substr, str(e))
        return
    raise AssertionError("expected an error containing %r" % substr)


class YellowbackProtectionTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.genesis = None

    def node_args(self, i):
        return yellowback_node_args(genesis=self.genesis, yellowback=(self.genesis is not None))

    def setup_nodes(self):
        return start_nodes(self.num_nodes, self.options.tmpdir,
                           extra_args=[self.node_args(i) for i in range(self.num_nodes)])

    def setup_network(self, split=False):
        self.nodes = self.setup_nodes()
        self.reconnect_all()
        self.is_network_split = False
        self.sync_all()

    def reconnect_all(self):
        for i in range(self.num_nodes - 1):
            connect_nodes_bi(self.nodes, i, i + 1)

    def restart_all(self):
        for i in range(self.num_nodes):
            stop_node(self.nodes[i], i)
        for i in range(self.num_nodes):
            self.nodes[i] = start_node(i, self.options.tmpdir, self.node_args(i))
        self.reconnect_all()

    def mine(self, n=1):
        self.nodes[0].generate(n)
        self.sync_all()
        assert_yed_synced(self.nodes)

    def price(self, p):
        publish_price(self.nodes[2], [self.nodes[2], self.nodes[3]], p)
        sync_mempools(self.nodes)
        self.mine(1)

    def stats(self):
        return self.nodes[4].yed_getstats()

    def prot(self):
        return self.nodes[4].yed_getprotectionstatus()

    def run_test(self):
        nodes = self.nodes
        print("Funding, roster, genesis anchor, restart with -yellowback")
        nodes[0].generate(110)
        self.sync_all()
        for i in (2, 3, 4):
            nodes[0].sendtoaddress(nodes[i].getnewaddress(), Decimal('2'))
        nodes[0].generate(1)
        self.sync_all()
        roster = make_regtest_roster(nodes[2:5], 2)
        self.genesis = fund_genesis_anchor(nodes[0], roster, Decimal('1.0'))
        self.sync_all()
        self.restart_all()
        self.sync_all()
        assert_yed_synced(nodes)

        print("One $100 vault backed by 20 YEC at $50 (health 1000 %)")
        self.price(50000000)
        self.mine(2)
        mint = nodes[0].yed_mint(10000, 0)
        vault = mint["txid"]
        self.mine(1)
        assert_equal(mint["collateralZat"], 20 * 100000000)
        s = self.stats()
        assert_equal(s["healthPct"], 1000)
        assert_equal(s["dcaBps"], 10000)
        assert_equal(s["errBps"], 10000)
        p = self.prot()
        assert_equal(p["dca"]["band"], "healthy")
        assert_equal(p["err"]["active"], False)
        assert_equal(p["mintingAllowed"], True)

        print("DCA bands: health 149 / 119 / 109 % => 1.25x / 1.5x / 2x collateral")
        # health = price_usd / 5 => $7.45 -> 149 %, $5.95 -> 119 %, $5.45 -> 109 %
        for usd_micro, health, bps, band in ((7450000, 149, 12500, "warning"), (5950000, 119, 15000, "critical"), (5450000, 109, 20000, "emergency")):
            self.price(usd_micro)
            s = self.stats()
            assert_equal(s["healthPct"], health)
            assert_equal(s["dcaBps"], bps)
            assert_equal(s["errBps"], 10000)
            assert_equal(self.prot()["dca"]["band"], band)
            # yed_estimatecollateral reads the evaluation-height snapshot (two blocks back): mine past it.
            self.mine(2)
            est = nodes[0].yed_estimatecollateral(10000, 0)
            assert_equal(est["dcaBps"], bps)
            # $100 at 1000 % = $1000 of YEC at the price, times the DCA multiplier.
            expected = (1000 * 1000000 * 100000000 * bps + (usd_micro * 10000) - 1) // (usd_micro * 10000)
            expected = ((expected + 999) // 1000) * 1000
            assert_equal(est["requiredZat"], expected)
        assert_same_statehash(nodes)

        print("ERR bands: health 99 / 94 / 89 / 84 % => burn 10527 / 11112 / 11765 / 12500 cents; mints VOID")
        for usd_micro, health, err_bps, burn in ((4950000, 99, 9500, 10527), (4700000, 94, 9000, 11112),
                                                 (4450000, 89, 8500, 11765), (4200000, 84, 8000, 12500)):
            self.price(usd_micro)
            s = self.stats()
            assert_equal(s["healthPct"], health)
            assert_equal(s["errBps"], err_bps)
            assert_equal(s["dcaBps"], 20000)
            p = self.prot()
            assert_equal(p["err"]["active"], True)
            assert_equal(p["mintingAllowed"], False)
            assert_equal(nodes[4].yed_getvault(vault)["requiredBurnCents"], burn)
            assert_equal(nodes[0].yed_listpositions()[0]["requiredBurnCents"], burn)
            self.mine(2)
            assert_rpc_error("minting-blocked-during-err", nodes[0].yed_mint, 10000, 0)
        # A hand-built mint evaluated under ERR registers VOID.
        eval_h = nodes[0].getblockcount() - 2
        tip = nodes[0].getblockcount()
        hex_, _ = build_mint_tx(nodes[0], 10000, 0, tip + 1 + 48, eval_h, 30 * 100000000)
        void = nodes[0].sendrawtransaction(hex_)
        sync_mempools(nodes)
        self.mine(1)
        assert_equal(nodes[4].yed_getvault(void)["status"], "VOID")
        assert_equal(nodes[4].yed_getvault(void)["voidReason"], "minting-blocked-during-err")
        hist = nodes[4].yed_gethistory(self.genesis["height"], nodes[4].getblockcount())
        assert_equal(hist[-1]["errBps"], 8000)
        assert_equal(hist[-1]["healthPct"], 84)
        assert_same_statehash(nodes)

        print("Volatility: settle at $5, then a +25 % spike in one hour freezes minting; cooldown expires exactly")
        # Bring the price back to $5 (health 100 %). The earlier moves breach only once their echo enters
        # the windows (a move can be judged only against a price 48 / 96 blocks back), so warm both
        # windows at $5 for 140 blocks, then wait out the last echo's cooldown.
        self.price(5000000)
        for _ in range(14):
            self.price(5000000)
            self.mine(9)
        while self.stats()["mintFrozen"]:
            self.price(5000000)
            self.mine(9)
        assert_equal(self.stats()["healthPct"], 100)
        assert_equal(self.prot()["mintingAllowed"], True)
        self.price(5000000)
        self.mine(20)
        self.price(5000000)
        assert_equal(self.stats()["mintFrozen"], False)
        assert_equal(self.prot()["volatility"]["priceShortWindow"], 5000000)
        assert_equal(self.prot()["volatility"]["priceLongWindow"], 5000000)
        # +25 % in one block: |6.25 - 5| / 5 = 25 % >= 20 % => breach.
        self.price(6250000)
        s = self.stats()
        breach = nodes[0].getblockcount()
        assert_equal(s["mintFrozen"], True)
        assert_equal(s["lastBreachHeight"], breach)
        assert_equal(self.prot()["volatility"]["frozenUntil"], breach + REGTEST_COOLDOWN)
        # The breach re-fires every block while price(H - 48) is still $5 (the reference window has to
        # roll past the spike), so the last breach is at breach + 47; then the cooldown runs from there.
        self.price(6250000)
        self.mine(46)
        self.price(6250000)  # keeps the price fresh; height = breach + 48
        last = self.stats()["lastBreachHeight"]
        assert_equal(last, breach + 47)
        self.mine(3)
        assert_equal(self.stats()["lastBreachHeight"], last)
        frozen_until = last + REGTEST_COOLDOWN
        assert_equal(self.prot()["volatility"]["frozenUntil"], frozen_until)
        # Mine to the last frozen block, keeping the price fresh and level (no further breach: the long
        # window compares $6.25 with $5, a 25 % move, below the 30 % threshold).
        while nodes[0].getblockcount() < frozen_until - 1:
            step = min(40, frozen_until - 1 - nodes[0].getblockcount())
            self.price(6250000)
            self.mine(step - 1)
        assert_equal(nodes[0].getblockcount(), frozen_until - 1)
        self.price(6250000)  # height == frozen_until
        assert_equal(nodes[0].getblockcount(), frozen_until)
        assert_equal(self.stats()["mintFrozen"], True)
        assert_equal(self.stats()["lastBreachHeight"], last)
        self.mine(3)
        # evalHeight = tip - 2 = frozen_until + 1 is unfrozen... the wallet reads the snapshot two blocks back:
        # at tip = frozen_until + 3, evalHeight = frozen_until + 1 (not frozen). Check the frozen snapshot instead.
        assert_equal(nodes[4].yed_gethistory(frozen_until, frozen_until + 1)[0]['mintFrozen'], True)
        assert_equal(nodes[4].yed_gethistory(frozen_until, frozen_until + 1)[1]['mintFrozen'], False)
        self.mine(1)
        assert_equal(self.stats()["mintFrozen"], False)
        assert_equal(self.prot()["mintingAllowed"], True)
        self.mine(1)
        # evalHeight = tip - 2 = frozen_until + 2: unfrozen, health 125 % => DCA 1.25x
        m = nodes[0].yed_mint(10000, 0)
        self.mine(1)
        assert_equal(nodes[4].yed_getvault(m["txid"])["status"], "ACTIVE")
        assert_same_statehash(nodes)
        print("Done")


if __name__ == '__main__':
    YellowbackProtectionTest().main()
