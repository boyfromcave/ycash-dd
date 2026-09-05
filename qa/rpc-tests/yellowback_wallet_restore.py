#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Wallet restore (plan §4.5, §6 Phase 3): positions and balances are derived
from the index by key ownership, never from wallet records (E7). Dumping
the keys of a wallet that minted and received YED and importing them into a
fresh node with an index gives the same balances and positions, and the
fresh node can redeem. Also the experimental encrypted-wallet flow: a locked
wallet refuses to mint, an unlocked one mints.

Nodes: 0 minter, 1 fresh wallet, 2-4 federation (2-of-3).
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
    yellowback_node_args,
)


def assert_rpc_error(substr, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        assert substr in str(e), "expected %r in %r" % (substr, str(e))
        return
    raise AssertionError("expected an error containing %r" % substr)


class YellowbackWalletRestoreTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 5
        self.setup_clean_chain = True
        self.genesis = None
        self.encrypt = False

    def node_args(self, i):
        extra = ['-developerencryptwallet'] if (i == 0 and self.encrypt) else None
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

    def restart(self, i):
        stop_node(self.nodes[i], i)
        self.nodes[i] = start_node(i, self.options.tmpdir, self.node_args(i))
        self.reconnect_all()
        sync_blocks(self.nodes)

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

        print("Node 0 mints twice and receives a send; node 1 holds nothing")
        self.price(50000000)
        self.mine(3)
        m1 = nodes[0].yed_mint(10000, 0)
        self.mine(1)
        m2 = nodes[0].yed_mint(20000, 1)
        self.mine(1)
        recv = nodes[0].yed_getnewaddress()
        nodes[0].yed_send(recv, 5000)
        self.mine(1)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 30000)
        positions0 = sorted(p['vaultTxid'] for p in nodes[0].yed_listpositions())
        assert_equal(len(positions0), 2)
        assert_equal(nodes[1].yed_getbalance()['confirmedCents'], 0)
        assert_equal(nodes[1].yed_listpositions(), [])
        coins0 = sorted((c['txid'], c['vout'], c['cents']) for c in nodes[0].yed_listunspent())

        print("Dump every key node 0 used, import into node 1 without rescan (B23)")
        # Vault owner keys and YED output keys are keypool keys; the address book holds them all.
        keys = nodes[0].getaddressesbyaccount("")  # every keypool key the builder drew is in the address book
        owner_addrs = []
        for p in nodes[0].yed_listpositions():
            # ownerKeyId -> the transparent address of that key via the address book listing
            owner_addrs.append(p['ownerKeyId'])
        yed_addrs = set()
        for c in nodes[0].yed_listunspent():
            yed_addrs.add(c['address'])
        imported = 0
        for addr in set(keys):
            info = nodes[0].validateaddress(addr)
            if not info.get('ismine', False) or info.get('isscript', False):
                continue
            nodes[1].importprivkey(nodes[0].dumpprivkey(addr), "", False)
            imported += 1
        assert_greater_than(imported, 2)
        # The YED addresses decode to the same keys.
        for a in yed_addrs:
            assert_equal(nodes[1].yed_validateaddress(a)['ismine'], True)

        print("Node 1 sees the same balance, coins and positions from the index alone")
        assert_equal(nodes[1].yed_getbalance()['confirmedCents'], 30000)
        assert_equal(sorted((c['txid'], c['vout'], c['cents']) for c in nodes[1].yed_listunspent()), coins0)
        assert_equal(sorted(p['vaultTxid'] for p in nodes[1].yed_listpositions()), positions0)
        for p in nodes[1].yed_listpositions():
            assert p['ownerKeyId'] in owner_addrs
        # yed_lockcoins locks every YED output that is now mine on node 1.
        locked = nodes[1].yed_lockcoins()
        assert_equal(len(locked), len(coins0))
        hist1 = nodes[1].yed_listtransactions()
        assert_equal(sorted(h['type'] for h in hist1), ['mint', 'mint', 'send'])

        print("Node 1 redeems the tier-0 vault with the imported owner key")
        lock1 = nodes[1].yed_getvault(m1['txid'])['lockHeight']
        self.mine_to(lock1, 50000000)
        red = nodes[1].yed_redeem(m1['txid'])
        assert_equal(red['requiredBurnCents'], 10000)
        txid = cosign_and_submit(nodes[1], [nodes[2], nodes[3]], red['hex'])
        sync_mempools(nodes)
        self.mine(1)
        assert_equal(nodes[4].yed_getvault(m1['txid'])['status'], 'CLOSED')
        assert_equal(nodes[1].yed_getbalance()['confirmedCents'], 20000)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 20000)  # same keys, same view
        assert_same_statehash(nodes)

        print("Encrypted wallet (experimental, -developerencryptwallet): locked refuses to mint, unlocked mints")
        self.encrypt = True
        self.restart(0)
        nodes = self.nodes
        assert_yed_synced(nodes)
        nodes[0].encryptwallet("pass")
        # encryptwallet shuts the node down by itself; wait for the process, then restart it.
        from test_framework.util import bitcoind_processes
        bitcoind_processes[0].wait()
        del bitcoind_processes[0]
        nodes[0] = start_node(0, self.options.tmpdir, self.node_args(0))
        self.reconnect_all()
        sync_blocks(nodes)
        assert_yed_synced(nodes)
        assert_rpc_error("walletpassphrase", nodes[0].yed_mint, 10000, 0)
        assert_rpc_error("walletpassphrase", nodes[0].yed_send, nodes[1].yed_getnewaddress(), 100)
        nodes[0].walletpassphrase("pass", 60)
        self.price(50000000)
        self.mine(3)
        m3 = nodes[0].yed_mint(10000, 0)
        self.mine(1)
        assert_equal(nodes[4].yed_getvault(m3['txid'])['status'], 'ACTIVE')
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 30000)
        assert_same_statehash(nodes)
        print("Done")


if __name__ == '__main__':
    YellowbackWalletRestoreTest().main()
