#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Yellowback lifecycle: mint, coin locking, evalHeight, reorg tolerance, send,
change floor, burn, under-assigned transfer, redeem with RPC co-signing, and
the co-signer / submit refusals (plan §6 Phase 3, §7).

Nodes: 0 user (minter, redeemer), 1 second user, 2-4 federation (2-of-3).
"""

from decimal import Decimal
from io import BytesIO

from test_framework.mininode import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, OP_RETURN
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
    sync_blocks,
    sync_mempools,
)
from test_framework.yellowback_util import (
    MINT_WINDOW,
    TOKEN_VALUE,
    assert_same_statehash,
    assert_yed_synced,
    cosign_and_submit,
    fund_genesis_anchor,
    make_regtest_roster,
    publish_price,
    wait_yed_synced,
    yellowback_node_args,
)


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


class YellowbackLifecycleTest(BitcoinTestFramework):

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

    def mine_to(self, height, price=50000):
        """Mine to `height`, republishing the price every 40 blocks so it never ages out."""
        while self.nodes[0].getblockcount() < height:
            step = min(40, height - self.nodes[0].getblockcount())
            self.price(price)
            self.mine(step)

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
        assert_equal(nodes[0].yed_getinfo()['anchor']['address'], roster['address'])

        print("Price and the first mint (tier 0, $100)")
        self.price(50000)
        self.mine(3)
        assert_rpc_error("bad-mint-amount", nodes[0].yed_mint, 9999, 0)
        assert_rpc_error("bad-mint-tier", nodes[0].yed_mint, 10000, 5)
        est = nodes[0].yed_estimatecollateral(10000, 0)
        assert_equal(est['requiredZat'], 20000 * 100000000)  # $100 at 1000 % at $0.05 = 20,000 YEC
        # Not enough YEC for 20,000 YEC of collateral: refused before signing.
        assert_rpc_error("insufficient YEC", nodes[0].yed_mint, 10000, 0)
        # Raise the price so the collateral fits the wallet: $50/YEC => 20 YEC.
        self.price(50000000)
        self.mine(3)
        est = nodes[0].yed_estimatecollateral(10000, 0)
        assert_equal(est['requiredZat'], 20 * 100000000)
        balance_before = nodes[0].getbalance()
        assert_greater_than(balance_before, Decimal('45'))
        mint = nodes[0].yed_mint(10000, 0)
        vault1 = mint['txid']
        assert_equal(mint['collateralZat'], 20 * 100000000)
        assert_equal(mint['evalHeight'], nodes[0].getblockcount() - 2)
        assert_equal(mint['lockHeight'], mint['evalHeight'] + 48 + MINT_WINDOW)

        print("B4: the fresh YED output is locked before the mint is even confirmed")
        locked = nodes[0].listlockunspent()
        assert {'txid': vault1, 'vout': 1} in [{'txid': l['txid'], 'vout': l['vout']} for l in locked]
        big = nodes[0].sendtoaddress(nodes[1].getnewaddress(), balance_before - Decimal('60'))
        spent = [(v['txid'], v['vout']) for v in nodes[0].decoderawtransaction(nodes[0].gettransaction(big)['hex'])['vin']]
        assert (vault1, 1) not in spent
        assert_equal(nodes[0].yed_getbalance()['unconfirmedCents'], 10000)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 0)
        sync_mempools(nodes)
        assert_equal(nodes[2].yed_gettxinfo(vault1)['verdict'], 'mint-registered')  # mempool dry run

        print("B3: a 20 % lower price before the mint confirms does not void it (evalHeight snapshot)")
        self.price(40000000)
        self.mine(1)
        v = nodes[3].yed_getvault(vault1)
        assert_equal(v['status'], 'ACTIVE')
        assert_equal(v['collateralZat'], 20 * 100000000)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 10000)
        assert_equal(nodes[0].yed_getbalance()['unconfirmedCents'], 0)
        pos = nodes[0].yed_listpositions()
        assert_equal(len(pos), 1)
        assert_equal(pos[0]['vaultTxid'], vault1)
        assert_equal(pos[0]['canRedeem'], False)
        assert_equal(nodes[1].yed_listpositions(), [])
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 10000)
        assert_equal(nodes[4].yed_getstats()['activeVaults'], 1)
        assert_same_statehash(nodes)

        print("yed_send to node 1: conservation and balances on both")
        addr1 = nodes[1].yed_getnewaddress()
        assert addr1.startswith('yr')
        assert_equal(nodes[1].yed_validateaddress(addr1)['ismine'], True)
        assert_equal(nodes[0].yed_validateaddress(addr1)['ismine'], False)
        assert_rpc_error("not a Yellowback address", nodes[0].yed_send, nodes[1].getnewaddress(), 100)
        send = nodes[0].yed_send(addr1, 4000)
        assert_equal(send['changeCents'], 6000)
        sync_mempools(nodes)
        assert_equal(nodes[1].yed_getbalance()['unconfirmedCents'], 4000)
        self.mine(1)
        send_block = nodes[0].getbestblockhash()
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 6000)
        assert_equal(nodes[1].yed_getbalance()['confirmedCents'], 4000)
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 10000)
        assert_equal(nodes[1].yed_gettxinfo(send['txid'])['verdict'], 'transfer-ok')
        hist = nodes[1].yed_listtransactions()
        assert_equal(hist[0]['type'], 'receive')
        assert_equal(hist[0]['amountCents'], 4000)
        hist0 = nodes[0].yed_listtransactions()
        assert_equal([h['type'] for h in hist0], ['send', 'mint'])
        assert_equal(hist0[0]['amountCents'], -4000)

        print("C20: change below $1.00 is refused with the workable amounts")
        assert_rpc_error("C20", nodes[1].yed_send, nodes[0].yed_getnewaddress(), 3950)

        print("C3: disconnecting the block that paid a YED output does not release its lock")
        nodes[0].invalidateblock(send_block)
        wait_yed_synced(nodes[0])
        locked = [(l['txid'], l['vout']) for l in nodes[0].listlockunspent()]
        assert (send['txid'], 1) in locked
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 10000)  # back to the mint output
        nodes[0].reconsiderblock(send_block)
        sync_blocks(nodes)
        assert_yed_synced(nodes)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 6000)

        print("C4: a 2-block reorg carrying a different price leaves a default-lag mint ACTIVE")
        mint2 = nodes[0].yed_mint(10000, 0)
        vault2 = mint2['txid']
        self.mine(2)
        tip_minus_1 = nodes[0].getblockhash(nodes[0].getblockcount() - 1)
        for n in nodes:
            n.invalidateblock(tip_minus_1)
            wait_yed_synced(n)
        self.price(45000000)
        nodes[0].generate(3)
        self.sync_all()
        assert_yed_synced(nodes)
        assert_equal(nodes[3].yed_getvault(vault2)['status'], 'ACTIVE')
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 16000)
        assert_same_statehash(nodes)

        print("D18: a plain spend of a YED output burns it")
        nodes[1].lockunspent(True, [{'txid': send['txid'], 'vout': 0}])  # vout 0: recipient; vout 1: node 0's change
        raw = nodes[1].createrawtransaction([{'txid': send['txid'], 'vout': 0}],
                                            {nodes[1].getnewaddress(): Decimal(TOKEN_VALUE - 1000) / Decimal(100000000)})
        signed = nodes[1].signrawtransaction(raw)
        assert_equal(signed['complete'], True)
        burn = nodes[1].sendrawtransaction(signed['hex'])
        sync_mempools(nodes)
        self.mine(1)
        info = nodes[1].yed_gettxinfo(burn)
        assert_equal(info['verdict'], 'non-yellowback')
        assert_equal(info['burned'], 4000)
        assert_equal(nodes[1].yed_getbalance()['confirmedCents'], 0)
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 16000)
        assert_equal(nodes[1].yed_listtransactions()[0]['type'], 'burn')

        print("A3: a hand-built TRANSFER that under-assigns burns only the remainder")
        token = [c for c in nodes[0].yed_listunspent() if c['cents'] == 6000][0]
        yec = [u for u in nodes[0].listunspent(1) if u['amount'] >= Decimal('0.5') and not u.get('generated', False)][0]
        dest_script = hex_str_to_bytes(nodes[0].validateaddress(nodes[0].getnewaddress())['scriptPubKey'])
        payload = bytes([0x59, 0x42, 0x01, 0x02, 0x01, 0x00]) + (5000).to_bytes(4, 'little')
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int(token['txid'], 16), token['vout'])))
        tx.vin.append(CTxIn(COutPoint(int(yec['txid'], 16), yec['vout'])))
        tx.vout.append(CTxOut(TOKEN_VALUE, CScript(dest_script)))
        tx.vout.append(CTxOut(0, CScript([OP_RETURN, payload])))
        change = int(yec['amount'] * 100000000) + TOKEN_VALUE - TOKEN_VALUE - 1000
        tx.vout.append(CTxOut(change, CScript(hex_str_to_bytes(nodes[0].validateaddress(nodes[0].getnewaddress())['scriptPubKey']))))
        tx.nExpiryHeight = nodes[0].getblockcount() + 40
        signed = nodes[0].signrawtransaction(tx_to_hex(tx))
        assert_equal(signed['complete'], True)
        under = nodes[0].sendrawtransaction(signed['hex'])
        self.mine(1)
        info = nodes[2].yed_gettxinfo(under)
        assert_equal(info['verdict'], 'transfer-ok')
        assert_equal(info['burned'], 1000)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 15000)
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 15000)

        print("A third mint so node 0 can burn for two redemptions later")
        self.mine(2)
        nodes[0].yed_mint(10000, 0)
        self.mine(1)
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 25000)
        assert_equal(nodes[4].yed_getstats()['supplyCents'], 25000)

        print("C22: redemption before lockHeight is refused")
        assert_rpc_error("locked until", nodes[0].yed_redeem, vault1)

        print("Redeem vault 1: owner-signed by yed_redeem, roster-signed by the test (two roster nodes)")
        lock1 = nodes[0].yed_getvault(vault1)['lockHeight']
        self.mine_to(lock1, 50000000)
        assert_equal([p for p in nodes[0].yed_listpositions() if p['vaultTxid'] == vault1][0]['canRedeem'], True)
        red = nodes[0].yed_redeem(vault1)
        assert_equal(red['requiredBurnCents'], 10000)
        decoded = nodes[0].decoderawtransaction(red['hex'])
        yed_outpoints = set((c['txid'], c['vout']) for c in nodes[0].yed_listunspent())
        for i, vin in enumerate(decoded['vin']):
            if i == 0:
                assert_equal(vin['txid'], vault1)
            else:
                assert (vin['txid'], vin['vout']) in yed_outpoints  # C10: no YEC inputs
        assert_equal(len(decoded['vout']), 3)  # collateral, YED change ($50), REDEEM payload
        # D1: the dry run reads only the index; an input that does not exist is no crash.
        phantom = tx_from_hex(red['hex'])
        phantom.vin[1].prevout = COutPoint(int('ab' * 32, 16), 0)
        nodes[2].yed_validaterawtransaction(tx_to_hex(phantom))
        assert_equal(nodes[2].yed_getinfo()['healthy'], True)
        # Phase 0: the co-signer refusals (RED-0..8, SUB-1) went with the federation; the roster
        # signatures are made by the test and the complete transaction is broadcast.
        cosign_and_submit(nodes[0], [nodes[2], nodes[3]], red['hex'])
        self.mine(1)
        v = nodes[4].yed_getvault(vault1)
        assert_equal(v['status'], 'CLOSED')
        assert_equal(v['burnedCents'], 10000)
        assert_equal(v['unbacked'], False)
        stats = nodes[4].yed_getstats()
        assert_equal(stats['supplyCents'], 15000)
        assert_equal(stats['activeVaults'], 2)
        active_collateral = sum(v['collateralZat'] for v in nodes[4].yed_listvaults('ACTIVE')['vaults'])
        assert_equal(stats['collateralZat'], active_collateral)
        assert_greater_than(active_collateral, 40 * 100000000)  # vault 3 was priced at $45
        assert_equal(nodes[0].yed_getbalance()['confirmedCents'], 15000)
        assert_equal(nodes[0].yed_listtransactions()[0]['type'], 'redeem')
        assert_greater_than(nodes[0].getbalance(), Decimal('19'))  # collateral returned
        assert_equal(nodes[0].yed_listpositions('CLOSED')[0]['vaultTxid'], vault1)
        assert_same_statehash(nodes)

        print("A second redemption builds once its vault unlocks (nothing is recorded or broadcast)")
        lock2 = nodes[0].yed_getvault(vault2)['lockHeight']
        self.mine_to(lock2, 50000000)
        red2 = nodes[0].yed_redeem(vault2)
        assert_equal(red2['requiredBurnCents'], 10000)
        assert_equal([p for p in nodes[0].yed_listpositions('ACTIVE') if p['vaultTxid'] == vault2][0]['canRedeem'], True)
        assert_same_statehash(nodes)
        print("Done")


if __name__ == '__main__':
    YellowbackLifecycleTest().main()
