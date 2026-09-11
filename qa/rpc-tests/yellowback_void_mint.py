#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
VOID mints (plan D18, §6 Phase 3): every failing MINT rule registers a VOID
vault whose collateral is locked and released at lockHeight with zero burn.

Nodes: 0 and 1 users, 2-4 federation (2-of-3). Every node starts with a supply
cap of $200 so the cap race can be exercised (-yellowbacksupplycap is
regtest-only and per node: node 4's index is the one that records the cap
verdict; the others see the mint as valid, which is why every node must run
with the same cap in production — here nodes 0-4 all get the cap).
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    connect_nodes_bi,
    start_node,
    start_nodes,
    stop_node,
    sync_mempools,
)
from test_framework.yellowback_util import (
    MINT_WINDOW,
    assert_same_statehash,
    assert_yed_synced,
    build_mint_tx_v1 as build_mint_tx,
    cosign_and_submit,
    fund_genesis_anchor,
    make_regtest_roster,
    publish_price,
    yellowback_node_args,
)

SUPPLY_CAP = 20000  # $200: room for exactly one more $100 mint after the first


def assert_rpc_error(substr, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        assert substr in str(e), "expected %r in %r" % (substr, str(e))
        return
    raise AssertionError("expected an error containing %r" % substr)


class YellowbackVoidMintTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.genesis = None

    def node_args(self, i):
        extra = ['-yellowbacksupplycap=%d' % SUPPLY_CAP] if self.genesis is not None else None
        return yellowback_node_args(extra=extra, genesis=self.genesis, yellowback=(self.genesis is not None))

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

    def mine_to(self, height, price):
        while self.nodes[0].getblockcount() < height:
            step = min(40, height - self.nodes[0].getblockcount())
            self.price(price)
            self.mine(step)

    def send_mint(self, hex_, node=0):
        txid = self.nodes[node].sendrawtransaction(hex_)
        sync_mempools(self.nodes)
        self.mine(1)
        return txid

    def expect_void(self, txid, reason):
        for n in self.nodes:
            v = n.yed_getvault(txid)
            assert_equal(v['status'], 'VOID')
            assert_equal(v['voidReason'], reason)
            assert_equal(n.yed_gettxinfo(txid)['verdict'], reason)
        assert_same_statehash(self.nodes)

    def run_test(self):
        nodes = self.nodes
        print("Funding, roster, genesis anchor, restart with -yellowback and a $200 supply cap")
        nodes[0].generate(120)
        self.sync_all()
        nodes[0].sendtoaddress(nodes[1].getnewaddress(), Decimal('30'))
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
        start = self.genesis['height']
        assert_equal(nodes[0].yed_getinfo()['params']['supplyCapCents'], SUPPLY_CAP)

        print("No price at evalHeight (= the start block) => bad-oracle-price")
        tip = nodes[0].getblockcount()
        hex_, _ = build_mint_tx(nodes[0], 10000, 0, tip + 1 + 48, start, 20 * 100000000, roster_script_hex=self.genesis['script'])
        void1 = self.send_mint(hex_)
        self.expect_void(void1, 'bad-oracle-price')
        assert_equal(nodes[0].yed_listpositions('VOID')[0]['vaultTxid'], void1)
        assert_equal(nodes[0].yed_listpositions('VOID')[0]['canRedeem'], False)
        assert_equal(nodes[4].yed_getstats()['voidVaults'], 1)
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 0)
        assert_equal(nodes[4].yed_getstats()['collateralZat'], 0)  # VOID collateral is not counted

        print("Under-collateralised => bad-mint-collateral")
        self.price(50000000)  # $50: $100 at 1000 % needs 20 YEC
        self.mine(3)
        eval_h = nodes[0].getblockcount() - 2
        tip = nodes[0].getblockcount()
        hex_, _ = build_mint_tx(nodes[0], 10000, 0, tip + 1 + 48, eval_h, 20 * 100000000 - 1, roster_script_hex=self.genesis['script'])
        void2 = self.send_mint(hex_)
        self.expect_void(void2, 'bad-mint-collateral')
        # The MINTPOL-1 path refuses such a mint before it is built: the wallet never under-collateralises.
        good = nodes[0].yed_mint(10000, 0)
        self.mine(1)
        assert_equal(nodes[0].yed_getvault(good['txid'])['status'], 'ACTIVE')
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 10000)

        print("evalHeight older than MINT_WINDOW => bad-mint-eval-height")
        eval_h = nodes[0].getblockcount() - MINT_WINDOW  # H - evalHeight = 41 at confirmation
        tip = nodes[0].getblockcount()
        hex_, _ = build_mint_tx(nodes[0], 10000, 0, tip + 1 + 48, eval_h, 20 * 100000000, roster_script_hex=self.genesis['script'])
        void3 = self.send_mint(hex_)
        self.expect_void(void3, 'bad-mint-eval-height')

        print("Lock height outside [tier, tier + window] => bad-mint-lock-tier-duration")
        eval_h = nodes[0].getblockcount() - 2
        tip = nodes[0].getblockcount()
        hex_, _ = build_mint_tx(nodes[0], 10000, 0, tip + 1 + 47, eval_h, 20 * 100000000, roster_script_hex=self.genesis['script'])
        void4 = self.send_mint(hex_)
        self.expect_void(void4, 'bad-mint-lock-tier-duration')

        print("ERR at evalHeight => minting-blocked-during-err (the wallet refuses; a hand-built mint voids)")
        # 20 YEC back $100; at $1 health is 20 % => ERR.
        self.price(1000000)
        self.mine(3)
        eval_h = nodes[0].getblockcount() - 2
        assert_rpc_error("minting-blocked-during-err", nodes[0].yed_mint, 10000, 0)
        tip = nodes[0].getblockcount()
        hex_, _ = build_mint_tx(nodes[0], 10000, 0, tip + 1 + 48, eval_h, 30 * 100000000, roster_script_hex=self.genesis['script'])
        void5 = self.send_mint(hex_)
        self.expect_void(void5, 'minting-blocked-during-err')

        print("Supply cap race => mint-supply-cap on the second mint of the block")
        # Recover the price and wait out the volatility freeze: the 24-hour window keeps re-breaching
        # while it passes over the crash blocks, so loop on mintFrozen rather than on one cooldown.
        self.price(50000000)
        self.mine(1)
        while nodes[4].yed_getstats()['mintFrozen']:
            self.price(50000000)
            self.mine(20)
        self.mine(3)
        # Supply is $100, cap $200: two $100 mints at the same evaluation height both pass MINTPOL-1
        # (supply counts confirmed YED only), but only the first one to confirm registers.
        m0 = nodes[0].yed_mint(10000, 0)
        m1 = nodes[1].yed_mint(10000, 0)
        sync_mempools(nodes)
        self.mine(1)
        statuses = sorted([nodes[4].yed_getvault(m0['txid'])['status'], nodes[4].yed_getvault(m1['txid'])['status']])
        assert_equal(statuses, ['ACTIVE', 'VOID'])
        voided = m0['txid'] if nodes[4].yed_getvault(m0['txid'])['status'] == 'VOID' else m1['txid']
        self.expect_void(voided, 'mint-supply-cap')
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 20000)
        # A wallet is warned when the headroom is low and refused once the cap is reached.
        assert_rpc_error("mint-supply-cap", nodes[0].yed_mint, 10000, 0)
        assert_equal(nodes[4].yed_getstats()['voidVaults'], 6)
        assert_equal(nodes[0].yed_listvaults('VOID')['total'], 6)

        print("Release of a VOID vault at lockHeight: zero burn, collateral back, no price needed")
        v2 = nodes[0].yed_getvault(void2)
        assert_equal(nodes[0].yed_listpositions('VOID')[0]['requiredBurnCents'], 0)
        self.mine_to(v2['lockHeight'], 50000000)
        balance_before = nodes[0].getbalance()
        red = nodes[0].yed_redeem(void2)
        assert_equal(red['requiredBurnCents'], 0)
        assert_equal(red['burnCents'], 0)
        decoded = nodes[0].decoderawtransaction(red['hex'])
        assert_equal(len(decoded['vin']), 1)  # the vault only: no YED inputs, no YEC inputs (C10)
        # RED-6 does not apply to a VOID vault: it is released even with no price in effect (E3).
        self.mine(49)  # the price ages out (49 > 48)
        assert_equal(nodes[2].yed_getprice()['priceMicroUsd'], None)
        red = nodes[0].yed_redeem(void2)  # rebuilt: the first one has expired meanwhile (40 blocks)
        cosign_and_submit(nodes[0], [nodes[2], nodes[3]], red['hex'])
        self.mine(1)
        v2 = nodes[4].yed_getvault(void2)
        assert_equal(v2['status'], 'CLOSED')
        assert_equal(v2['wasActive'], False)
        assert_equal(v2['requiredBurnAtClose'], 0)
        assert_equal(v2['burnedCents'], 0)
        assert_equal(v2['unbacked'], False)
        assert_greater_than(nodes[0].getbalance(), balance_before + Decimal('19'))
        assert_equal(nodes[4].yed_getstats()['voidVaults'], 5)
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 20000)
        assert_same_statehash(nodes)
        print("Done")


if __name__ == '__main__':
    YellowbackVoidMintTest().main()
