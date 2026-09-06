#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .
"""
Yellowback Sapling funding and destinations (plan revision 15, I2):
  - a mint funded from a ys1... address in one transaction (no transparent inputs, vShieldedSpend,
    positive valueBalance) registers like any other and debits the shielded balance;
  - a mint funded from one s1... address spends only that address's outputs, and is refused when
    that address cannot cover the collateral;
  - a redemption pays its collateral straight to a ys1... address (vShieldedOutput, negative
    valueBalance) through the federation's co-signers, and to a chosen s1... address;
  - a co-signer refuses a redemption that carries a Sapling spend (RED-4);
  - bad `from` / `to` values are refused before anything is signed.
Nodes: 0 user, 1 second user, 2-4 federation (2-of-3).
"""
from decimal import Decimal
from io import BytesIO

from test_framework.mininode import CTransaction, SpendDescription
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    bytes_to_hex_str,
    connect_nodes_bi,
    hex_str_to_bytes,
    start_node,
    start_nodes,
    stop_node,
    sync_mempools,
    wait_and_assert_operationid_status,
)
from test_framework.yellowback_util import (
    MINT_WINDOW,
    TOKEN_VALUE,
    assert_yed_synced,
    cosign_and_submit,
    fund_genesis_anchor,
    make_regtest_roster,
    publish_price,
    yellowback_node_args,
)

FEE = 1000  # DEFAULT_FEE = YELLOWBACK_FEE, in zat
COIN = 100000000


def assert_rpc_error(substr, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        assert substr in str(e), "expected %r in %r" % (substr, str(e))
        return
    raise AssertionError("expected an error containing %r" % substr)


def tx_from_hex(h):
    tx = CTransaction()
    tx.deserialize(BytesIO(hex_str_to_bytes(h)))
    return tx


def tx_to_hex(tx):
    return bytes_to_hex_str(tx.serialize())


def zat(d):
    return int(Decimal(str(d)) * COIN)


class YellowbackSaplingTest(BitcoinTestFramework):

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

    def mine(self, n=1, node=0):
        self.nodes[node].generate(n)
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

    def vault(self, node, txid):
        for p in node.yed_listpositions():
            if p['vaultTxid'] == txid:
                return p
        raise AssertionError("vault %s not listed" % txid)

    def run_test(self):
        nodes = self.nodes
        print("Funding")
        nodes[0].generate(105)
        self.sync_all()
        nodes[0].sendtoaddress(nodes[1].getnewaddress(), Decimal('5'))
        for i in (2, 3, 4):
            nodes[0].sendtoaddress(nodes[i].getnewaddress(), Decimal('2'))
        nodes[0].generate(1)
        self.sync_all()

        print("Roster 2-of-3 on nodes 2-4, genesis anchor, restart with -yellowback")
        roster = make_regtest_roster(nodes[2:5], 2)
        self.genesis = fund_genesis_anchor(nodes[0], roster, Decimal('1.0'))
        self.sync_all()
        self.restart_all()
        self.sync_all()
        assert_yed_synced(nodes)

        print("Price $50/YEC, so $100 at 1000 % is 20 YEC of collateral")
        self.price(50000000)
        self.mine(3)
        assert_equal(nodes[0].yed_estimatecollateral(10000, 0)['requiredZat'], 20 * COIN)

        print("Shield 30 YEC into a ys1... address of node 0")
        ys = nodes[0].z_getnewaddress('sapling')
        assert ys.startswith('yregtestsapling')
        taddr = nodes[0].getnewaddress()
        nodes[0].sendtoaddress(taddr, Decimal('30.001'))
        self.mine(1)
        opid = nodes[0].z_sendmany(taddr, [{'address': ys, 'amount': Decimal('30')}], 1, Decimal('0.0001'))
        wait_and_assert_operationid_status(nodes[0], opid)
        self.mine(1)
        assert_equal(nodes[0].z_getbalance(ys), Decimal('30'))

        print("I2: mint from the ys1... address in one transaction")
        assert_rpc_error("not a transparent (s1", nodes[0].yed_mint, 10000, 0, "nonsense")
        assert_rpc_error("not a transparent (s1", nodes[0].yed_mint, 10000, 0, nodes[0].yed_getnewaddress())
        assert_rpc_error("no spending key", nodes[1].yed_mint, 10000, 0, ys)
        mint1 = nodes[0].yed_mint(10000, 0, ys)
        vault1 = mint1['txid']
        assert_equal(mint1['fundedFrom'], 'sapling')
        assert_equal(mint1['from'], ys)
        assert_equal(mint1['collateralZat'], 20 * COIN)
        raw = nodes[0].getrawtransaction(vault1, 1)
        assert_equal(raw['vin'], [])
        assert_greater_than(len(raw['vShieldedSpend']), 0)
        assert_equal(len(raw['vShieldedOutput']), 1)          # change back to ys
        assert_equal(zat(raw['valueBalance']), 20 * COIN + TOKEN_VALUE + FEE)
        assert_equal(len(raw['vout']), 3)
        # B4 still holds for the Sapling shape: the token output is locked before confirmation.
        assert {'txid': vault1, 'vout': 1} in [{'txid': l['txid'], 'vout': l['vout']} for l in nodes[0].listlockunspent()]
        sync_mempools(nodes)
        assert_equal(nodes[2].yed_gettxinfo(vault1)['verdict'], 'mint-registered')
        self.mine(1)
        assert_equal(nodes[2].yed_gettxinfo(vault1)['verdict'], 'mint-registered')
        assert_equal(self.vault(nodes[0], vault1)['status'], 'ACTIVE')
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 10000)
        assert_equal(nodes[0].z_getbalance(ys), Decimal('30') - Decimal(20 * COIN + TOKEN_VALUE + FEE) / COIN)

        print("I2: mint from one s1... address only")
        t2 = nodes[0].getnewaddress()
        t_empty = nodes[0].getnewaddress()
        fund = nodes[0].sendtoaddress(t2, Decimal('21'))
        self.mine(1)
        assert_rpc_error("insufficient YEC at " + t_empty, nodes[0].yed_mint, 10000, 0, t_empty)
        mint2 = nodes[0].yed_mint(10000, 0, t2)
        vault2 = mint2['txid']
        assert_equal(mint2['fundedFrom'], 'transparent')
        raw2 = nodes[0].getrawtransaction(vault2, 1)
        assert_equal(len(raw2['vin']), 1)
        assert_equal(raw2['vin'][0]['txid'], fund)
        funded_vout = raw2['vin'][0]['vout']
        assert_equal(nodes[0].getrawtransaction(fund, 1)['vout'][funded_vout]['scriptPubKey']['addresses'], [t2])
        assert_equal(raw2['vShieldedSpend'], [])
        self.mine(1)
        assert_equal(self.vault(nodes[0], vault2)['status'], 'ACTIVE')
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 20000)

        print("Wait for both vaults to unlock")
        lock1 = mint1['lockHeight']
        lock2 = mint2['lockHeight']
        self.mine_to(max(lock1, lock2), 50000000)
        assert_equal(self.vault(nodes[0], vault1)['canRedeem'], True)

        print("I2: bad destinations are refused before signing")
        assert_rpc_error("not a transparent (s1", nodes[0].yed_redeem, vault1, "nonsense")
        assert_rpc_error("not a transparent (s1", nodes[0].yed_redeem, vault1, nodes[0].yed_getnewaddress())
        assert_equal(nodes[0].yed_abortredeem(vault1)['aborted'], False)  # nothing was recorded

        print("I2: redeem vault 1 straight to the ys1... address")
        z_before = nodes[0].z_getbalance(ys)
        red1 = nodes[0].yed_redeem(vault1, ys)
        assert_equal(red1['collateralTo'], ys)
        assert_equal(red1['shielded'], True)
        assert_equal(red1['requiredBurnCents'], 10000)
        assert_equal(red1['changeCents'], 0)
        rraw = nodes[0].decoderawtransaction(red1['hex'])
        assert_equal(len(rraw['vShieldedOutput']), 1)
        assert_equal(rraw['vShieldedSpend'], [])
        assert_equal(rraw['vin'][0]['txid'], vault1)
        assert_equal(len(rraw['vin']), 2)                    # vault + one 10,000-cent token
        assert_equal(len(rraw['vout']), 1)                   # the payload only: no YED change
        collateral_out = 20 * COIN + TOKEN_VALUE - FEE
        assert_equal(zat(rraw['valueBalance']), -collateral_out)
        assert_equal(rraw['locktime'], lock1)

        print("RED-4: a co-signer refuses a redemption carrying a Sapling spend")
        spend = tx_from_hex(red1['hex'])
        dummy = SpendDescription()
        dummy.cv = dummy.anchor = dummy.nullifier = dummy.rk = 1
        dummy.zkproof = b'\x00' * 192
        dummy.spendAuthSig = b'\x00' * 64
        spend.shieldedSpends.append(dummy)
        assert_rpc_error("RED-4", nodes[2].yed_cosignredeem, tx_to_hex(spend))

        txid1 = cosign_and_submit(nodes[0], [nodes[2], nodes[3]], red1['hex'])
        sync_mempools(nodes)
        self.mine(1)
        assert_equal(self.vault(nodes[0], vault1)['status'], 'CLOSED')
        assert_equal(nodes[0].z_getbalance(ys), z_before + Decimal(collateral_out) / COIN)
        assert_equal(nodes[0].yed_gettxinfo(txid1)['verdict'], 'redeem-ok')
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 10000)

        print("I2: redeem vault 2 to a chosen s1... address")
        t3 = nodes[0].getnewaddress()
        red2 = nodes[0].yed_redeem(vault2, t3)
        assert_equal(red2['collateralTo'], t3)
        assert_equal(red2['shielded'], False)
        rraw2 = nodes[0].decoderawtransaction(red2['hex'])
        assert_equal(rraw2['vShieldedOutput'], [])
        assert_equal(rraw2['vout'][0]['scriptPubKey']['addresses'], [t3])
        txid2 = cosign_and_submit(nodes[0], [nodes[2], nodes[3]], red2['hex'])
        sync_mempools(nodes)
        self.mine(1)
        assert_equal(self.vault(nodes[0], vault2)['status'], 'CLOSED')
        got = [u for u in nodes[0].listunspent(1) if u['address'] == t3]
        assert_equal(len(got), 1)
        assert_equal(zat(got[0]['amount']), collateral_out)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 0)
        print("Done")


if __name__ == '__main__':
    YellowbackSaplingTest().main()
