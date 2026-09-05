#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Reorg stress (plan §6 Phase 6, §7): random 1-6 block reorgs over a few
hundred blocks with random Yellowback activity (prices, mints, sends, plain
burns, redemptions) on a four-node network; the state hash must agree on
every -yellowback node after every reorg, after a restart, and after a cold
rebuild (-reindex-yellowback). Seeded so a failure is reproducible.

Nodes 0 and 1 are users, 2-4 the 2-of-3 federation. A reorg is produced by
invalidating the last k blocks on one node and mining k + 1 there; the
others adopt the longer branch.
"""

import random
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes_bi,
    start_node,
    start_nodes,
    stop_node,
    sync_blocks,
    sync_mempools,
)
from test_framework.yellowback_util import (
    assert_same_statehash,
    assert_yed_synced,
    cosign_and_submit,
    fund_genesis_anchor,
    make_regtest_roster,
    publish_price,
    wait_yed_synced,
    yellowback_node_args,
)

TOTAL_BLOCKS = 300
SEED = 20260905


class YellowbackReorgStressTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.genesis = None

    def add_options(self, parser):
        parser.add_option("--seed", dest="seed", type="int", default=SEED)
        parser.add_option("--blocks", dest="blocks", type="int", default=TOTAL_BLOCKS)

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

    def restart(self, i, extra=None):
        stop_node(self.nodes[i], i)
        self.nodes[i] = start_node(i, self.options.tmpdir, self.node_args(i) + (extra or []))
        self.reconnect_all()
        sync_blocks(self.nodes)

    def mine(self, n=1, node=0):
        self.nodes[node].generate(n)
        self.sync_all()
        assert_yed_synced(self.nodes)

    def price(self, p):
        publish_price(self.nodes[2], [self.nodes[2], self.nodes[3]], p)
        sync_mempools(self.nodes)

    # ---- random activity
    def try_mint(self, rng, node):
        try:
            cents = rng.choice([10000, 15000, 20000])
            node.yed_mint(cents, 0)
            return "mint"
        except Exception as e:
            return "mint-refused(%s)" % str(e)[:40]

    def try_send(self, rng, src, dst):
        try:
            coins = [c for c in src.yed_listunspent() if not c["reserved"] and not c["spentUnconfirmed"]]
            if not coins:
                return "send-nocoins"
            c = rng.choice(coins)
            amount = rng.choice([100, 500, c["cents"] // 2, c["cents"]])
            if amount < 100:
                amount = 100
            if 0 < c["cents"] - amount < 100:
                amount = c["cents"]
            src.yed_send(dst.yed_getnewaddress(), amount)
            return "send"
        except Exception as e:
            return "send-refused(%s)" % str(e)[:40]

    def try_burn(self, rng, node):
        try:
            coins = [c for c in node.yed_listunspent() if not c["reserved"] and not c["spentUnconfirmed"]]
            if not coins:
                return "burn-nocoins"
            c = rng.choice(coins)
            node.lockunspent(True, [{"txid": c["txid"], "vout": c["vout"]}])
            raw = node.createrawtransaction([{"txid": c["txid"], "vout": c["vout"]}],
                                            {node.getnewaddress(): Decimal(c["valueZat"] - 1000) / Decimal(100000000)})
            node.sendrawtransaction(node.signrawtransaction(raw)["hex"])
            return "burn"
        except Exception as e:
            return "burn-refused(%s)" % str(e)[:40]

    def try_redeem(self, rng, node):
        try:
            pos = [p for p in node.yed_listpositions("ACTIVE") if p["canRedeem"] and not p["pending"]]
            if not pos:
                return "redeem-none"
            p = rng.choice(pos)
            red = node.yed_redeem(p["vaultTxid"])
            cosign_and_submit(node, [self.nodes[2], self.nodes[3]], red["hex"])
            return "redeem"
        except Exception as e:
            return "redeem-refused(%s)" % str(e)[:40]

    def reorg(self, rng, k):
        """Invalidate the last k blocks on a random node and mine k + 1 there."""
        victim = rng.choice([0, 1, 2])
        node = self.nodes[victim]
        height = node.getblockcount()
        node.invalidateblock(node.getblockhash(height - k + 1))
        wait_yed_synced(node)
        # The node may now sit on an older side branch (stale blocks from an earlier reorg are still
        # valid), so mine whatever it takes to overtake the previous tip.
        newh = node.getblockcount()
        assert newh < height
        node.generate(height - newh + 1)
        sync_blocks(self.nodes)
        assert_yed_synced(self.nodes)
        return victim

    def run_test(self):
        nodes = self.nodes
        rng = random.Random(self.options.seed)
        print("Seed %d, %d blocks" % (self.options.seed, self.options.blocks))
        nodes[0].generate(120)
        self.sync_all()
        nodes[0].sendtoaddress(nodes[1].getnewaddress(), Decimal('100'))
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

        self.price(50000000)
        self.mine(3)
        start = nodes[0].getblockcount()
        stats = {}
        reorgs = 0
        while nodes[0].getblockcount() < start + self.options.blocks:
            # Keep the price fresh and wander it a little (never enough to freeze or trip ERR).
            if rng.random() < 0.15:
                self.price(rng.choice([48000000, 50000000, 52000000]))
            actor = rng.choice([0, 1])
            other = 1 - actor
            r = rng.random()
            if r < 0.35:
                what = self.try_mint(rng, nodes[actor])
            elif r < 0.65:
                what = self.try_send(rng, nodes[actor], nodes[other])
            elif r < 0.75:
                what = self.try_burn(rng, nodes[actor])
            else:
                what = self.try_redeem(rng, nodes[actor])
            stats[what.split("(")[0]] = stats.get(what.split("(")[0], 0) + 1
            sync_mempools(nodes)
            if rng.random() < 0.12:
                k = rng.randint(1, 6)
                victim = self.reorg(rng, k)
                reorgs += 1
                assert_same_statehash(nodes)
            else:
                self.mine(1, node=rng.choice([0, 1, 2]))
            if nodes[0].getblockcount() % 50 == 0:
                assert_same_statehash(nodes)
                s = nodes[4].yed_getstats()
                print("height %d: supply %d cents, %d active / %d void vaults, %d reorgs so far, %s" %
                      (nodes[0].getblockcount(), s["supplyCents"], s["activeVaults"], s["voidVaults"], reorgs, stats))
        print("activity:", stats, "reorgs:", reorgs)
        assert reorgs >= 10, "too few reorgs for a stress test"
        expected = assert_same_statehash(nodes)
        s = nodes[4].yed_getstats()
        assert s["supplyCents"] >= 0
        # Conservation: the confirmed supply equals the sum of every unspent token.
        total_tokens = 0
        for i in range(5):
            for c in nodes[i].yed_listunspent():
                total_tokens += c["cents"]
        # Tokens not owned by any test wallet cannot exist here; every recipient is node 0 or node 1.
        assert_equal(total_tokens, s["supplyCents"])

        print("Restart and cold rebuild give the same state hash")
        self.restart(1)
        wait_yed_synced(nodes[1])
        assert_equal(nodes[1].yed_getstatehash()["statehash"], expected)
        self.restart(2, ["-reindex-yellowback"])
        wait_yed_synced(nodes[2])
        assert_equal(nodes[2].yed_getstatehash()["statehash"], expected)
        assert_same_statehash(nodes)
        print("Done")


if __name__ == '__main__':
    YellowbackReorgStressTest().main()
