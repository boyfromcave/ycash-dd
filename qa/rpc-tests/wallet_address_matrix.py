#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
The address matrix (beta2 address interoperability plan §6, decisions D-2, D-4, D-6): plain YEC moves between every
address type with the documented call, z_sendmany (and z_mergetoaddress), on one script that runs unchanged on both
C++ node lines.

Address types: ECC (transparent P2PKH), Sapling, and post-quantum (TX_PQPKH) in both schemes, FN-DSA-512 (scheme 2)
and SLH-DSA-SHA2-128s (scheme 1). The scheme of a PQ address is read from validateaddress (pqscheme); no address or
prefix is hard-coded (the prefixes change under D-9).

- sources ECC, Sapling, PQ FN-DSA, PQ SLH-DSA x destinations ECC, Sapling, PQ FN-DSA, PQ SLH-DSA (16 cells), plus a
  multi-recipient send mixing all four types from a PQ source and from a Sapling source. For every transaction:
  accepted to the mempool of a second node, mined, its fee pinned (a PQ input: max(DEFAULT_FEE, -pqfeerate x size,
  the ZIP-317 conventional fee); otherwise the line's default), change from a PQ source back to that address (D-4) and
  from a Sapling source a Sapling note, balances and listings right on both wallets; one reorg across the block, then
  the same checks; every received output spent again;
- z_mergetoaddress: PQ -> PQ, PQ + ECC -> Sapling, Sapling -> PQ, with the same fee rule;
- refusals: before the vault upgrade, an FN-DSA address before Falcon, an amount below the PQ dust threshold, an
  explicit fee below what a PQ spend needs, a coinbase-only source to a PQ address, a locked wallet.

Node profile P0 (no index): every check uses the wallet calls only.

    BITCOIND=<ycashd> ../.venv/bin/python -u qa/rpc-tests/wallet_address_matrix.py --srcdir=<src> --tmpdir=<dir> --portseed=<n>
"""

import time
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    VAULT_BRANCH_ID,
    assert_equal,
    bitcoind_processes,
    connect_nodes_bi,
    get_coinbase_address,
    nuparams,
    start_node,
    start_nodes,
    sync_blocks,
    sync_mempools,
    wait_and_assert_operationid_status,
)
from test_framework.yellowback_util import conventional_fee

ACTIVATION = 210        # the vault upgrade (UPGRADE_VAULT)
FALCON = 220            # -pqfalconheight: FN-DSA-512 admitted from here
DEFAULT_FEE = 1000      # policy/fees.h (ycash-dd)
PQ_FEE_RATE = 100       # -pqfeerate default: -minrelaytxfee, zat per 1,000 bytes
ECC, SAP, FN, SLH = 'ECC', 'Sapling', 'PQ-FN-DSA', 'PQ-SLH-DSA'
KINDS = (ECC, SAP, FN, SLH)

LOCKED = 'Please enter the wallet passphrase with walletpassphrase first'


def zat(x):
    return int((Decimal(x) * 100000000).to_integral_value())


def coins(z):
    return Decimal(z) / 100000000


def assert_raises_rpc(substr, fn, *args):
    try:
        fn(*args)
    except JSONRPCException as e:
        assert substr in e.error['message'], 'expected %r in %r' % (substr, e.error['message'])
        return e.error['message']
    raise AssertionError('expected an RPC error containing %r' % substr)


class WalletAddressMatrixTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 4
        self.setup_clean_chain = True

    def node_args(self, i):
        a = [nuparams(VAULT_BRANCH_ID, ACTIVATION), '-pqfalconheight=%d' % FALCON]
        if i == 3:
            a += ['-experimentalfeatures', '-developerencryptwallet']
        return a

    def setup_network(self, split=False):
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir,
                                 extra_args=[self.node_args(i) for i in range(self.num_nodes)])
        for a, b in ((0, 1), (1, 2), (0, 2), (2, 3)):
            connect_nodes_bi(self.nodes, a, b)
        self.is_network_split = False
        self.sync_all()

    # --- line differences (ycash-dd v4.5.0 / ycash6 6.x), guarded so the file is byte-identical on both lines ---

    def is_v6(self):
        if not hasattr(self, '_v6'):
            self._v6 = self.nodes[0].getnetworkinfo()['version'] >= 6000000
        return self._v6

    def zsend(self, node, src, recipients, fee=None):
        """z_sendmany with the documented arguments; the 6.x privacy policy (D-5) where the line has one."""
        args = [src, [{'address': a, 'amount': v} for a, v in recipients], 1]
        if fee is not None or self.is_v6():
            args.append(fee if fee is not None else (None if self.is_v6() else DEFAULT_FEE))
        if self.is_v6():
            args.append('AllowFullyTransparent')
        return node.z_sendmany(*args)

    def zsend_ok(self, node, src, recipients, fee=None):
        return wait_and_assert_operationid_status(node, self.zsend(node, src, recipients, fee))

    def zsend_fails(self, node, src, recipients, substr, fee=None):
        opid = self.zsend(node, src, recipients, fee)
        res = None
        for _ in range(300):
            r = node.z_getoperationresult([opid])
            if r:
                res = r[0]
                break
            time.sleep(1)
        assert res is not None, 'timeout'
        assert_equal(res['status'], 'failed')
        assert substr in res['error']['message'], (substr, res['error']['message'])

    def default_fee(self, raw):
        return conventional_fee(raw) if self.is_v6() else DEFAULT_FEE

    # --- helpers ---

    def mine(self, n=1):
        self.sync_all()
        blocks = self.nodes[2].generate(n)
        self.sync_all()
        return blocks

    def kind_of(self, node, addr):
        v = node.validateaddress(addr)
        if v.get('isvalid') and 'pqscheme' in v:
            return {2: FN, 1: SLH}[v['pqscheme']]
        if v.get('isvalid'):
            return ECC
        assert node.z_validateaddress(addr)['isvalid'], addr
        return SAP

    def spk(self, node, addr):
        return node.validateaddress(addr)['scriptPubKey']

    def fee_of(self, node, txid):
        """The network fee of a wallet transaction still in the mempool: inputs (from the wallet) - outputs."""
        raw = node.getrawtransaction(txid, 1)
        vin = 0
        for i in raw['vin']:
            prev = node.decoderawtransaction(node.gettransaction(i['txid'])['hex'])
            vin += zat(prev['vout'][i['vout']]['value'])
        vout = sum(zat(o['value']) for o in raw['vout'])
        return vin + zat(raw.get('valueBalance', 0)) - vout, raw

    def pq_rule(self, raw):
        size = len(raw['hex']) // 2
        return max(DEFAULT_FEE, max(PQ_FEE_RATE, PQ_FEE_RATE * size // 1000), conventional_fee(raw))

    def has_pq_input(self, node, raw):
        for i in raw['vin']:
            prev = node.decoderawtransaction(node.gettransaction(i['txid'])['hex'])
            if prev['vout'][i['vout']]['scriptPubKey']['type'] == 'pqpubkeyhash':
                return True
        return False

    def check_sent(self, node, txid, src):
        """Mempool of a second node, the fee rule, and D-4 change."""
        sync_mempools(self.nodes[:3])
        assert txid in self.nodes[2].getrawmempool(), txid
        fee, raw = self.fee_of(node, txid)
        pq = self.has_pq_input(node, raw)
        expected = self.pq_rule(raw) if pq else self.default_fee(raw)
        assert_equal(fee, expected)
        if src is not None and self.kind_of(node, src) in (FN, SLH):
            assert pq, txid
            src_spk = self.spk(node, src)
            mine_spks = [o['scriptPubKey']['hex'] for o in raw['vout'] if node.validateaddress(o['scriptPubKey']['addresses'][0])['ismine']]
            assert all(s == src_spk for s in mine_spks), (txid, mine_spks)
        if src is not None and self.kind_of(node, src) == SAP:
            assert all(not node.validateaddress(o['scriptPubKey']['addresses'][0])['ismine'] for o in raw['vout']), txid
        return fee

    def received(self, node, addr):
        if self.kind_of(node, addr) == SAP:
            return Decimal(node.z_getbalance(addr))
        return node.getreceivedbyaddress(addr)

    def unspent(self, node, addr):
        if self.kind_of(node, addr) == SAP:
            return sum(Decimal(u['amount']) for u in node.z_listunspent(1, 9999999, False, [addr]))
        return sum(u['amount'] for u in node.listunspent(1, 9999999, [addr]))

    def run_test(self):
        n0, n1, n2, n3 = self.nodes
        n0.generate(120)          # node 0 (the sender) holds mature coinbase; node 2 mines from here on
        self.mine(ACTIVATION + 1 - n0.getblockcount())

        print('addresses of every type on the sender (node 0) and the receiver (node 1)')
        src = {ECC: n0.getnewaddress(), SAP: n0.z_getnewaddress('sapling'), SLH: n0.getnewaddress('', 'pqslh')}
        dst = {ECC: n1.getnewaddress(), SAP: n1.z_getnewaddress('sapling'), SLH: n1.getnewaddress('', 'pqslh')}
        locked_pq = n3.getnewaddress('', 'pqslh')

        # Each refusal rewinds node 0 a few blocks with invalidateblock (fewer than six, or safe mode answers).
        print('refusals before the vault upgrade')
        h_vault = n0.getblockhash(ACTIVATION - 1)
        n0.invalidateblock(h_vault)
        assert n0.getblockcount() + 1 < ACTIVATION
        assert_raises_rpc('post-quantum addresses can be paid only once the vault upgrade is active',
                          self.zsend, n0, src[ECC], [(dst[SLH], 1)])
        assert_raises_rpc('post-quantum addresses can be spent from only once the vault upgrade is active',
                          self.zsend, n0, src[SLH], [(dst[ECC], 1)])
        assert_raises_rpc('can be spent from only once the vault upgrade is active', n0.z_mergetoaddress, [src[SLH]], dst[ECC])
        n0.reconsiderblock(h_vault)
        sync_blocks(self.nodes)

        self.mine(FALCON + 2 - n0.getblockcount())
        src[FN] = n0.getnewaddress('', 'pq')      # FN-DSA-512 once Falcon is active at the next block
        dst[FN] = n1.getnewaddress('', 'pq')
        for k in KINDS:
            assert_equal(self.kind_of(n0, src[k]), k)
            assert_equal(self.kind_of(n1, dst[k]), k)

        print('refusals before Falcon')
        h_falcon = n0.getblockhash(FALCON - 2)
        n0.invalidateblock(h_falcon)
        assert n0.getblockcount() + 1 < FALCON
        assert_raises_rpc('FN-DSA-512 (scheme 2) post-quantum address can be paid only once Falcon is active',
                          self.zsend, n0, src[ECC], [(dst[FN], 1)])
        assert_raises_rpc('FN-DSA-512 (scheme 2) post-quantum address can be spent from only once Falcon is active',
                          self.zsend, n0, src[FN], [(dst[ECC], 1)])
        assert_raises_rpc('can be spent from only once Falcon is active', n0.z_mergetoaddress, [src[FN]], dst[ECC])
        n0.reconsiderblock(h_falcon)
        sync_blocks(self.nodes)

        print('fund node 0: five 1-YEC coins at ECC and at each PQ address, five 1-YEC Sapling notes')
        fund = n0.getnewaddress()
        for _ in range(5):
            n0.sendtoaddress(fund, 2)
            for k in (ECC, FN, SLH):
                n0.sendtoaddress(src[k], 1)
        self.mine(1)
        for _ in range(5):
            self.zsend_ok(n0, fund, [(src[SAP], 1)])
        self.mine(1)
        for k in KINDS:
            assert_equal(self.unspent(n0, src[k]), Decimal(5))

        print('refusals: PQ dust, an explicit fee below a PQ spend, a coinbase-only source, a locked wallet')
        assert_raises_rpc('below the dust threshold', self.zsend, n0, src[ECC], [(dst[SLH], Decimal('0.00001'))])
        assert_raises_rpc('below the dust threshold', self.zsend, n0, src[SAP], [(dst[FN], Decimal('0.00000100'))])
        self.zsend_fails(n0, src[SLH], [(dst[ECC], Decimal('0.1'))], 'Fee 0.0001 is below the', Decimal('0.0001'))
        cb = get_coinbase_address(n0)
        self.zsend_fails(n0, cb, [(dst[SLH], 1)], 'Could not find any non-coinbase UTXOs to spend')
        n3.encryptwallet('matrix-pass')
        bitcoind_processes[3].wait()
        self.nodes[3] = n3 = start_node(3, self.options.tmpdir, self.node_args(3))
        connect_nodes_bi(self.nodes, 2, 3)
        assert_raises_rpc(LOCKED, self.zsend, n3, locked_pq, [(dst[ECC], 1)])
        assert_raises_rpc(LOCKED, n3.z_mergetoaddress, [locked_pq], dst[ECC])

        print('the matrix: 16 cells and two mixed sends, all with the documented call (z_sendmany)')
        expect_in = {k: Decimal(0) for k in KINDS}        # receiver totals per destination
        expect_left = {k: Decimal(5) for k in KINDS}      # sender totals per source (PQ and Sapling: change returns)
        sent = []                                         # (txid, source kind)
        amount = Decimal('0.10')
        for s in KINDS:
            for d in KINDS:
                amount += Decimal('0.01')
                txid = self.zsend_ok(n0, src[s], [(dst[d], amount)])
                fee = self.check_sent(n0, txid, src[s])
                expect_in[d] += amount
                expect_left[s] -= amount + coins(fee)
                sent.append((txid, s))
                print('  %-10s -> %-10s %s fee %d' % (s, d, amount, fee))
        for s in (SLH, SAP):
            mixed = [(dst[d], Decimal('0.05')) for d in KINDS]
            txid = self.zsend_ok(n0, src[s], mixed)
            fee = self.check_sent(n0, txid, src[s])
            for d in KINDS:
                expect_in[d] += Decimal('0.05')
            expect_left[s] -= Decimal('0.20') + coins(fee)
            sent.append((txid, s))
            print('  %-10s -> all four  0.05 each, fee %d' % (s, fee))

        def check_balances(label):
            print('  balances and listings (%s)' % label)
            for k in KINDS:
                assert_equal(self.received(n1, dst[k]), expect_in[k])
                assert_equal(self.unspent(n1, dst[k]), expect_in[k])
                if k != ECC:   # ECC change goes to a fresh key (-sendchangeback off)
                    assert_equal(self.unspent(n0, src[k]), expect_left[k])
            for txid, s in sent:
                assert_equal(n0.gettransaction(txid)['confirmations'], 1)
                assert_equal(n1.gettransaction(txid)['confirmations'], 1)
            listed = {t['txid'] for t in n1.listtransactions('*', 1000)}
            for txid, s in sent:
                raw = n0.decoderawtransaction(n0.gettransaction(txid)['hex'])
                if any(o['scriptPubKey'].get('addresses', [None])[0] in (dst[ECC], dst[FN], dst[SLH]) for o in raw['vout']):
                    assert txid in listed, txid
            zlisted = {r['txid'] for r in n1.z_listreceivedbyaddress(dst[SAP])}
            assert_equal(len(zlisted), 6)

        print('mine the matrix')
        block = self.mine(1)[0]
        check_balances('mined')

        print('one reorg across the matrix block: every transaction returns to the mempool and is mined again')
        for n in self.nodes:
            n.invalidateblock(block)
        for txid, _ in sent:
            assert txid in n2.getrawmempool(), txid
        block2 = self.mine(1)[0]
        assert block2 != block
        for txid, _ in sent:
            assert_equal(n0.gettransaction(txid)['blockhash'], block2)
        check_balances('after the reorg')

        print('every received output is spent again (node 1, each type, to node 0)')
        back = n0.getnewaddress()
        for d in KINDS:
            txid = self.zsend_ok(n1, dst[d], [(back, Decimal('0.05'))])
            self.check_sent(n1, txid, dst[d])
        self.mine(1)
        assert_equal(n0.getreceivedbyaddress(back), Decimal('0.20'))

        print('z_mergetoaddress: PQ -> PQ, PQ + ECC -> Sapling, Sapling -> PQ')
        merged_pq = n0.getnewaddress('', 'pqslh')
        r = n0.z_mergetoaddress([src[FN]], merged_pq)
        txid = wait_and_assert_operationid_status(n0, r['opid'])
        fee = self.check_sent(n0, txid, None)
        assert_equal(coins(fee), Decimal(r['mergingTransparentValue']) - n0.getrawtransaction(txid, 1)['vout'][0]['value'])
        self.mine(1)
        assert_equal(self.unspent(n0, src[FN]), 0)
        assert_raises_rpc('Fee 0.0001 is below the', n0.z_mergetoaddress, [merged_pq], dst[ECC], Decimal('0.0001'))
        r = n0.z_mergetoaddress([src[SLH], src[ECC]], src[SAP])
        txid = wait_and_assert_operationid_status(n0, r['opid'])
        self.check_sent(n0, txid, None)
        self.mine(1)
        assert_equal(self.unspent(n0, src[SLH]), 0)
        merged_fn = n0.getnewaddress('', 'pq')
        r = n0.z_mergetoaddress(['ANY_SAPLING'], merged_fn)
        txid = wait_and_assert_operationid_status(n0, r['opid'])
        fee = self.check_sent(n0, txid, None)
        assert_equal(fee, DEFAULT_FEE if not self.is_v6() else fee)
        self.mine(1)
        assert_equal(self.unspent(n0, merged_fn), Decimal(r['mergingShieldedValue']) - coins(fee))
        txid = self.zsend_ok(n0, merged_fn, [(dst[SAP], Decimal('0.5'))])
        self.check_sent(n0, txid, merged_fn)
        self.mine(1)


if __name__ == '__main__':
    WalletAddressMatrixTest().main()
