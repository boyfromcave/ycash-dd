#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
YDollar index: prices, anchor custody, rotation, reorg, restart, rebuild
(plan §6 Phase 2, §7).

Four regtest nodes, all with every upgrade active from height 1. They start
WITHOUT -ydollar; the test creates a 2-of-3 anchor from the wallets of nodes
0-2 (node 3 holds no roster key), mines it, then restarts every node with
-ydollar and the three regtest genesis arguments (C2).
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_start_raises_init_error,
    connect_nodes_bi,
    start_node,
    start_nodes,
    stop_node,
    sync_blocks,
    sync_mempools,
    wait_bitcoinds,
)
from test_framework.ydollar_util import (
    PRICE_MAX_AGE,
    YD_FEE,
    assert_same_statehash,
    assert_yd_synced,
    fund_genesis_anchor,
    genesis_args,
    make_regtest_roster,
    publish_price,
    ydollar_node_args,
)


class YDollarIndexTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.genesis = None

    def node_args(self, i):
        return ydollar_node_args(genesis=self.genesis, ydollar=(self.genesis is not None))

    def setup_nodes(self):
        return start_nodes(self.num_nodes, self.options.tmpdir,
                           extra_args=[self.node_args(i) for i in range(self.num_nodes)])

    def restart(self, i, extra=None):
        stop_node(self.nodes[i], i)  # stop_node waits for that process; wait_bitcoinds() would wait for all
        self.nodes[i] = start_node(i, self.options.tmpdir, self.node_args(i) + (extra or []))

    def sync_all(self):
        # Across the reorg join only blocks can be synced: the undone price transaction is
        # resurrected into the mempools of nodes 0/1 only, and mempools never resync on reconnect.
        if getattr(self, 'blocks_only', False):
            if self.is_network_split:
                sync_blocks(self.nodes[:2])
                sync_blocks(self.nodes[2:])
            else:
                sync_blocks(self.nodes)
        else:
            super().sync_all()

    def reconnect_all(self):
        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 1, 2)
        connect_nodes_bi(self.nodes, 2, 3)

    def find_utxo(self, node, amount):
        for u in node.listunspent(1):
            if u['amount'] == amount:
                return '%s:%d' % (u['txid'], u['vout'])
        raise AssertionError('no utxo of %s' % amount)

    def run_test(self):
        nodes = self.nodes
        print("Mining a mature coinbase on node 0")
        nodes[0].generate(101)
        self.sync_all()
        assert_greater_than(nodes[0].getbalance(), Decimal('1'))

        print("Building the 2-of-3 roster on nodes 0-2 and funding the genesis anchor")
        roster = make_regtest_roster(nodes[0:3], 2)
        self.genesis = fund_genesis_anchor(nodes[0], roster, Decimal('1.0'))
        self.sync_all()
        start_height = self.genesis['height']
        # A YDollar option without -ydollar is refused.
        stop_node(nodes[3], 3)
        assert_start_raises_init_error(3, self.options.tmpdir,
                                       ydollar_node_args(ydollar=False) + genesis_args(self.genesis),
                                       "YDollar options require -ydollar")
        # -ydollar without the regtest genesis arguments is refused.
        assert_start_raises_init_error(3, self.options.tmpdir,
                                       ydollar_node_args(ydollar=False) + ['-experimentalfeatures', '-ydollar'],
                                       "requires -ydollarstartheight")
        # -ydollar with -prune is refused (D7).
        assert_start_raises_init_error(3, self.options.tmpdir,
                                       ydollar_node_args(genesis=self.genesis) + ['-prune=550'],
                                       "-ydollar is incompatible with -prune")
        # -ydollarfee below DEFAULT_FEE is refused (C16).
        assert_start_raises_init_error(3, self.options.tmpdir,
                                       ydollar_node_args(genesis=self.genesis) + ['-ydollarfee=999'],
                                       "-ydollarfee must be at least")
        nodes[3] = start_node(3, self.options.tmpdir, self.node_args(3))

        print("Restarting every node with -ydollar and the genesis arguments")
        for i in range(3):
            self.restart(i)
        self.reconnect_all()
        self.sync_all()
        assert_yd_synced(nodes)
        for n in nodes:
            info = n.yd_getinfo()
            assert_equal(info['enabled'], True)
            assert_equal(info['rpcversion'], 1)
            assert_equal(info['network'], 'regtest')
            assert_equal(info['startHeight'], start_height)
            assert_equal(info['height'], n.getblockcount())
            assert_equal(info['rosterIndex'], 0)
            assert_equal(info['anchor']['valid'], True)
            assert_equal(info['anchor']['txid'], self.genesis['txid'])
            assert_equal(info['anchor']['address'], roster['address'])
            assert_equal(info['anchor']['valueZat'], 100000000)
            r = n.yd_getroster()
            assert_equal(r['k'], 2)
            assert_equal(r['n'], 3)
            assert_equal(r['address'], roster['address'])
            assert_equal(r['pubkeys'], roster['pubkeys'])
            assert_equal(r['scriptHex'], roster['script'])
            assert_equal(n.yd_getprice()['priceMicroUsd'], None)
            stats = n.yd_getstats()
            assert_equal(stats['supplyCents'], 0)
            assert_equal(stats['healthPct'], 30000)
        assert_same_statehash(nodes)

        print("Publishing a price (2 of 3 signatures merged in one signrawtransaction call)")
        txid1 = publish_price(nodes[0], [nodes[0], nodes[1]], 50000)
        sync_mempools(nodes)
        assert_equal(nodes[2].yd_gettxinfo(txid1)['dryRun'], True)
        assert_equal(nodes[2].yd_gettxinfo(txid1)['verdict'], 'price-recorded')
        nodes[0].generate(1)
        self.sync_all()
        assert_yd_synced(nodes)
        h1 = nodes[0].getblockcount()
        for n in nodes:
            p = n.yd_getprice()
            assert_equal(p['priceMicroUsd'], 50000)
            assert_equal(p['sourceHeight'], h1)
            assert_equal(p['age'], 0)
            assert_equal(n.yd_getinfo()['anchor']['txid'], txid1)
            assert_equal(n.yd_getinfo()['anchor']['valueZat'], 100000000 - YD_FEE)
            log = n.yd_gettxinfo(txid1)
            assert_equal(log['verdict'], 'price-recorded')
            assert_equal(log['priceRecorded'], True)
            assert_equal(log['anchorSpend'], True)
            assert_equal(log['dryRun'], False)
        assert_same_statehash(nodes)

        print("Refill: a confirmed UTXO of the exact amount is absorbed whole into the anchor (C6)")
        nodes[0].sendtoaddress(nodes[0].getnewaddress(), Decimal('0.5'))
        nodes[0].generate(1)
        self.sync_all()
        refill = self.find_utxo(nodes[0], Decimal('0.5'))
        built = nodes[0].yd_createpricetx(51000, refill)
        decoded = nodes[0].decoderawtransaction(built['hex'])
        assert_equal(len(decoded['vin']), 2)
        assert_equal(len(decoded['vout']), 2)
        assert_equal(built['newAnchorValueZat'], 100000000 - YD_FEE + 50000000 - YD_FEE)
        txid2 = publish_price(nodes[0], [nodes[0], nodes[1]], 51000, refill=refill)  # node 0 also signs its refill
        sync_mempools(nodes)
        nodes[0].generate(1)
        self.sync_all()
        assert_yd_synced(nodes)
        assert_equal(nodes[3].yd_getprice()['priceMicroUsd'], 51000)
        assert_equal(nodes[3].yd_getinfo()['anchor']['valueZat'], 100000000 - YD_FEE + 50000000 - YD_FEE)

        print("Chaining: a price spending an anchor that is still in the mempool, no prevtxs (C11)")
        txid3 = publish_price(nodes[1], [nodes[1], nodes[2]], 52000)
        sync_mempools(nodes)
        txid4 = publish_price(nodes[1], [nodes[1], nodes[2]], 53000)
        sync_mempools(nodes)
        nodes[1].generate(1)
        self.sync_all()
        assert_yd_synced(nodes)
        h4 = nodes[1].getblockcount()
        for n in nodes:
            assert_equal(n.yd_getprice()['priceMicroUsd'], 53000)  # last in block order wins (B7)
            assert_equal(n.yd_getinfo()['anchor']['txid'], txid4)
            assert_equal(n.yd_gettxinfo(txid3)['verdict'], 'price-recorded')
        assert_same_statehash(nodes)

        print("Aging: the price is in effect for %d blocks and not for %d" % (PRICE_MAX_AGE, PRICE_MAX_AGE + 1))
        nodes[0].generate(PRICE_MAX_AGE)
        self.sync_all()
        assert_equal(nodes[0].yd_getprice()['priceMicroUsd'], 53000)
        assert_equal(nodes[0].yd_getprice()['age'], PRICE_MAX_AGE)
        nodes[0].generate(1)
        self.sync_all()
        assert_equal(nodes[0].yd_getprice()['priceMicroUsd'], None)
        assert_equal(nodes[0].yd_getprice(h4)['priceMicroUsd'], 53000)
        hist = nodes[0].yd_gethistory(h4, h4 + PRICE_MAX_AGE + 1)
        assert_equal(len(hist), PRICE_MAX_AGE + 2)
        assert_equal(hist[0]['priceMicroUsd'], 53000)
        assert_equal(hist[-2]['priceMicroUsd'], 53000)
        assert_equal(hist[-1]['priceMicroUsd'], None)
        assert_equal(nodes[0].yd_getstats()['priceMicroUsd'], None)

        print("Rotation: anchor spend to a new 2-of-3 script with no OP_RETURN, then a PRICE reveals it (C9)")
        roster2 = make_regtest_roster(nodes[0:3], 2)
        assert roster2['script'] != roster['script']
        rot = publish_price(nodes[0], [nodes[0], nodes[2]], 0, rotate_script=roster2['script'])
        sync_mempools(nodes)
        nodes[0].generate(1)
        self.sync_all()
        assert_yd_synced(nodes)
        for n in nodes:
            info = n.yd_getinfo()
            assert_equal(info['anchor']['txid'], rot)
            assert_equal(info['anchor']['address'], roster2['address'])
            assert_equal(info['rosterIndex'], 0)  # not revealed until the new anchor is spent
            assert_equal(n.yd_gettxinfo(rot)['verdict'], 'price-rotation')
            assert_equal(n.yd_getprice()['priceMicroUsd'], None)
        # yd_createpricetx refuses while the new roster is unrevealed (the signers still know it).
        try:
            nodes[0].yd_createpricetx(54000)
            raise AssertionError("yd_createpricetx should refuse an unrevealed roster")
        except Exception as e:
            assert 'not been revealed' in str(e)
        # Build the reveal transaction by hand: the same shape, signed with the new roster.
        anchor = nodes[0].yd_getinfo()['anchor']
        raw = nodes[0].createrawtransaction([{'txid': anchor['txid'], 'vout': anchor['vout']}],
                                            {roster2['address']: Decimal(anchor['valueZat'] - YD_FEE) / Decimal(100000000)})
        # createrawtransaction cannot add the OP_RETURN (B1); this reveal is a second rotation to the same script,
        # which records no price but reveals the roster (PRICE-2). Then the RPC can build prices again.
        signed = nodes[0].signrawtransaction(raw)
        signed = nodes[2].signrawtransaction(signed['hex'])
        assert_equal(signed['complete'], True)
        reveal = nodes[0].sendrawtransaction(signed['hex'])
        sync_mempools(nodes)
        nodes[0].generate(1)
        self.sync_all()
        assert_yd_synced(nodes)
        for n in nodes:
            r = n.yd_getroster()
            assert_equal(r['index'], 1)
            assert_equal(r['address'], roster2['address'])
            assert_equal(r['pubkeys'], roster2['pubkeys'])
            assert_equal(r['previous']['address'], roster['address'])
            assert_equal(n.yd_getinfo()['anchor']['txid'], reveal)
        txid5 = publish_price(nodes[0], [nodes[1], nodes[2]], 54000)
        sync_mempools(nodes)
        nodes[0].generate(1)
        self.sync_all()
        assert_yd_synced(nodes)
        for n in nodes:
            assert_equal(n.yd_getprice()['priceMicroUsd'], 54000)
            assert_equal(n.yd_getroster()['index'], 1)
        assert_same_statehash(nodes)

        print("Reorg across a price change: the losing branch is undone and the winning one applied")
        self.split_network()  # nodes 0,1 | 2,3
        nodes = self.nodes
        a = publish_price(nodes[0], [nodes[0], nodes[1]], 55000)
        nodes[0].generate(1)
        sync_blocks(nodes[:2])
        assert_yd_synced(nodes[:2])
        assert_equal(nodes[0].yd_getprice()['priceMicroUsd'], 55000)
        # The other side has only node 2's key, so it publishes nothing and mines a longer empty branch.
        nodes[2].generate(2)
        sync_blocks(nodes[2:])
        assert_equal(nodes[2].yd_getprice()['priceMicroUsd'], 54000)
        self.blocks_only = True
        self.join_network()
        nodes = self.nodes
        assert_yd_synced(nodes)
        for n in nodes:
            assert_equal(n.getblockcount(), nodes[2].getblockcount())
            assert_equal(n.yd_getprice()['priceMicroUsd'], 54000)  # the 55000 block was undone on nodes 0,1
        assert_same_statehash(nodes)
        # The undone price transaction returns to the mempool of nodes 0/1 (mempools do not resync across a
        # reconnect, so no sync_mempools here); node 0 mines it and it confirms on the new branch.
        nodes[0].generate(1)
        self.sync_all()
        self.blocks_only = False
        assert_yd_synced(nodes)
        for n in nodes:
            assert_equal(n.yd_getprice()['priceMicroUsd'], 55000)
            assert_equal(n.yd_gettxinfo(a)['verdict'], 'price-recorded')
        expected = assert_same_statehash(nodes)

        print("Restart: a plain restart, -reindex-ydollar and -reindex all rebuild to the same state hash")
        self.restart(1)
        connect_nodes_bi(nodes, 0, 1)
        assert_yd_synced([nodes[1]])
        assert_equal(nodes[1].yd_getstatehash()['statehash'], expected)
        self.restart(2, ['-reindex-ydollar'])
        connect_nodes_bi(nodes, 1, 2)
        assert_yd_synced([nodes[2]])
        assert_equal(nodes[2].yd_getstatehash()['statehash'], expected)
        self.restart(3, ['-reindex'])
        connect_nodes_bi(nodes, 2, 3)
        sync_blocks(nodes)
        assert_yd_synced([nodes[3]])
        assert_equal(nodes[3].yd_getstatehash()['statehash'], expected)
        assert_same_statehash(nodes)

        print("Reorg while offline: node 3 is stopped, the others mine, node 3 rejoins and catches up")
        stop_node(nodes[3], 3)
        nodes[0].generate(3)
        sync_blocks(nodes[:3])
        nodes[3] = start_node(3, self.options.tmpdir, self.node_args(3))
        connect_nodes_bi(nodes, 2, 3)
        sync_blocks(nodes)
        assert_yd_synced(nodes)
        assert_same_statehash(nodes)
        print("Done")


if __name__ == '__main__':
    YDollarIndexTest().main()
