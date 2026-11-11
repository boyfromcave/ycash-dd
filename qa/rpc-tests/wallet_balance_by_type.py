#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Balances and listings by address type (beta2 item 15; address plan W1.5 on the reference line, findings L-2 … L-6).

One wallet (node 0, a fresh wallet file so no coinbase of its own) holds an ECC transparent address, a post-quantum
address of each scheme (FN-DSA-512 = 2, SLH-DSA-SHA2-128s = 1; Falcon is active on this chain) and a Sapling address.
Every view is checked in five states: payments from another wallet unconfirmed, confirmed, this wallet's own spends
unconfirmed, confirmed, the spends' block disconnected (reorg) and reconnected, and the wallet encrypted and locked:

- getwalletinfo: balance = balance_ecc + balance_pq, unconfirmed_balance = unconfirmed_balance_ecc +
  unconfirmed_balance_pq, pq_by_scheme (both schemes, number and name);
- z_gettotalbalance (minconf 1 and 0): transparent = transparent_ecc + transparent_pq, pq_by_scheme;
- z_getbalance of every address, a post-quantum one included (L-3), at minconf 1 and 0;
- listreceivedbyaddress 0 true lists every transparent address, a PQ one with pqscheme / pqschemename (L-4);
  z_listaddresses the Sapling one; validateaddress names the scheme;
- the rule for unconfirmed funds (L-6): getwalletinfo.balance counts own unconfirmed outputs (change),
  unconfirmed_balance counts others' unconfirmed payments, z_gettotalbalance / z_getbalance count by depth only, so
  z_gettotalbalance(0).transparent == balance + unconfirmed_balance always, and the PQ figures follow the same rule.

No address text or prefix is hard-coded: addresses come from the node and their scheme from validateaddress.

    BITCOIND=<ycashd> ../.venv/bin/python -u qa/rpc-tests/wallet_balance_by_type.py --srcdir=<src> --tmpdir=<dir> --portseed=<n>
"""

import os
from decimal import Decimal

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    VAULT_BRANCH_ID,
    assert_equal,
    bitcoind_processes,
    connect_nodes_bi,
    nuparams,
    start_node,
    start_nodes,
    sync_blocks,
    wait_and_assert_operationid_status,
)

ACTIVATION = 210
SLH, FN = 1, 2
NAMES = {SLH: 'SLH-DSA-SHA2-128s', FN: 'FN-DSA-512'}
ZFEE = Decimal('0.0001')
PASSPHRASE = 'balance-by-type'


def D(x):
    return Decimal(str(x))


def assert_raises_rpc(substr, fn, *args):
    try:
        fn(*args)
    except JSONRPCException as e:
        assert substr in e.error['message'], 'expected %r in %r' % (substr, e.error['message'])
        return e.error['message']
    raise AssertionError('expected an RPC error containing %r' % substr)


class WalletBalanceByTypeTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 2
        self.setup_clean_chain = False

    def node_args(self, i):
        a = [nuparams(VAULT_BRANCH_ID, ACTIVATION), '-pqfalcon=1']
        if i == 0:
            # a wallet of its own: the cached chain's coinbase of node 0 stays in wallet.dat
            a += ['-wallet=bytype.dat', '-experimentalfeatures', '-developerencryptwallet']
        return a

    def setup_network(self, split=False):
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir,
                                 extra_args=[self.node_args(i) for i in range(self.num_nodes)])
        connect_nodes_bi(self.nodes, 0, 1)
        self.is_network_split = False
        self.sync_all()

    def mine(self, n=1):
        self.sync_all()
        self.nodes[1].generate(n)
        self.sync_all()

    # ---- the views ----------------------------------------------------------------------------------------------

    def view(self):
        """Every balance figure of node 0, as Decimals, with the invariants checked."""
        n0 = self.nodes[0]
        w = n0.getwalletinfo()
        v = {'balance': D(w['balance']), 'unconfirmed': D(w['unconfirmed_balance']),
             'balance_ecc': D(w['balance_ecc']), 'balance_pq': D(w['balance_pq']),
             'unconfirmed_ecc': D(w['unconfirmed_balance_ecc']), 'unconfirmed_pq': D(w['unconfirmed_balance_pq']),
             'immature': D(w['immature_balance'])}
        assert_equal(v['balance'], v['balance_ecc'] + v['balance_pq'])
        assert_equal(v['unconfirmed'], v['unconfirmed_ecc'] + v['unconfirmed_pq'])
        assert_equal([(s['pqscheme'], s['pqschemename']) for s in w['pq_by_scheme']], [(SLH, NAMES[SLH]), (FN, NAMES[FN])])
        for s in w['pq_by_scheme']:
            v['balance_pq_%d' % s['pqscheme']] = D(s['balance_pq'])
            v['unconfirmed_pq_%d' % s['pqscheme']] = D(s['unconfirmed_balance_pq'])
        assert_equal(v['balance_pq'], v['balance_pq_1'] + v['balance_pq_2'])
        assert_equal(v['unconfirmed_pq'], v['unconfirmed_pq_1'] + v['unconfirmed_pq_2'])
        assert_equal(D(n0.getbalance()), v['balance'])
        for minconf in (1, 0):
            t = n0.z_gettotalbalance(minconf)
            k = 'z%d_' % minconf
            for f in ('transparent', 'private', 'total', 'transparent_ecc', 'transparent_pq'):
                assert isinstance(t[f], str), (f, t[f])     # strings, as the existing three are
                v[k + f] = D(t[f])
            assert_equal(v[k + 'transparent'], v[k + 'transparent_ecc'] + v[k + 'transparent_pq'])
            assert_equal(v[k + 'total'], v[k + 'transparent'] + v[k + 'private'])
            assert_equal([(s['pqscheme'], s['pqschemename']) for s in t['pq_by_scheme']], [(SLH, NAMES[SLH]), (FN, NAMES[FN])])
            for s in t['pq_by_scheme']:
                v[k + 'pq_%d' % s['pqscheme']] = D(s['transparent_pq'])
            assert_equal(v[k + 'transparent_pq'], v[k + 'pq_1'] + v[k + 'pq_2'])
        # L-6: minconf 0 counts every unconfirmed output; balance + unconfirmed_balance counts them too (no immature
        # or locked coins in this wallet), so the two agree, for the ECC and the PQ part alike
        assert_equal(v['immature'], 0)
        assert_equal(v['z0_transparent'], v['balance'] + v['unconfirmed'])
        assert_equal(v['z0_transparent_ecc'], v['balance_ecc'] + v['unconfirmed_ecc'])
        assert_equal(v['z0_transparent_pq'], v['balance_pq'] + v['unconfirmed_pq'])
        for s in (SLH, FN):
            assert_equal(v['z0_pq_%d' % s], v['balance_pq_%d' % s] + v['unconfirmed_pq_%d' % s])
        for name, addr in self.addrs.items():
            for minconf in (1, 0):
                v['%s@%d' % (name, minconf)] = D(n0.z_getbalance(addr, minconf))
        # per address adds up to the totals
        for minconf in (1, 0):
            k = 'z%d_' % minconf
            assert_equal(v[k + 'pq_%d' % FN], v['fn@%d' % minconf])
            assert_equal(v[k + 'pq_%d' % SLH], v['slh@%d' % minconf])
            assert_equal(v[k + 'private'], v['z@%d' % minconf])
        return v

    def check_listing(self):
        n0 = self.nodes[0]
        rcv = {r['address']: r for r in n0.listreceivedbyaddress(0, True)}
        for name, scheme in (('fn', FN), ('slh', SLH)):
            r = rcv[self.addrs[name]]
            assert_equal((r['pqscheme'], r['pqschemename']), (scheme, NAMES[scheme]))
            va = n0.validateaddress(self.addrs[name])
            assert_equal((va['ismine'], va['pqscheme'], va['pqschemename']), (True, scheme, NAMES[scheme]))
        r = rcv[self.addrs['ecc']]
        assert 'pqscheme' not in r and 'pqschemename' not in r
        assert 'pqschemename' not in n0.validateaddress(self.addrs['ecc'])
        assert_equal(n0.z_listaddresses(), [self.addrs['z']])

    # ---- the test ----------------------------------------------------------------------------------------------

    def run_test(self):
        n0, n1 = self.nodes
        self.mine(ACTIVATION - n0.getblockcount())

        print('a fresh wallet: an ECC, an FN-DSA-512, an SLH-DSA and a Sapling address, all zero')
        self.addrs = {'ecc': n0.getnewaddress(), 'fn': n0.getnewaddress('', 'pq'),
                      'slh': n0.getnewaddress('', 'pqslh'), 'z': n0.z_getnewaddress('sapling')}
        assert_equal(n0.validateaddress(self.addrs['fn'])['pqscheme'], FN)     # Falcon active: "pq" is FN-DSA-512
        assert_equal(n0.validateaddress(self.addrs['slh'])['pqscheme'], SLH)
        v = self.view()
        assert all(x == 0 for x in v.values()), v
        self.check_listing()

        print('payments from another wallet, unconfirmed: unconfirmed_balance by type; z_* only at minconf 0')
        t1 = n1.getnewaddress()
        n1.sendtoaddress(t1, 10)
        self.mine(1)
        wait_and_assert_operationid_status(n1, n1.z_sendmany(t1, [{'address': self.addrs['z'], 'amount': 1}], 1, ZFEE))
        n1.sendtoaddress(self.addrs['ecc'], 3)
        n1.sendtoaddress(self.addrs['fn'], 2)
        n1.sendtoaddress(self.addrs['slh'], Decimal('1.5'))
        self.sync_all()
        v = self.view()
        assert_equal((v['balance'], v['balance_ecc'], v['balance_pq']), (0, 0, 0))
        assert_equal((v['unconfirmed'], v['unconfirmed_ecc'], v['unconfirmed_pq']), (Decimal('6.5'), 3, Decimal('3.5')))
        assert_equal((v['unconfirmed_pq_1'], v['unconfirmed_pq_2']), (Decimal('1.5'), 2))
        assert_equal((v['z1_transparent'], v['z1_private'], v['z1_total']), (0, 0, 0))
        assert_equal((v['z0_transparent'], v['z0_transparent_ecc'], v['z0_transparent_pq']), (Decimal('6.5'), 3, Decimal('3.5')))
        assert_equal((v['z0_private'], v['z0_total']), (1, Decimal('7.5')))
        assert_equal((v['ecc@1'], v['fn@1'], v['slh@1'], v['z@1']), (0, 0, 0, 0))
        assert_equal((v['ecc@0'], v['fn@0'], v['slh@0'], v['z@0']), (3, 2, Decimal('1.5'), 1))
        self.check_listing()

        print('confirmed: balance by type and scheme, every address')
        self.mine(1)
        v = self.view()
        assert_equal((v['balance'], v['balance_ecc'], v['balance_pq']), (Decimal('6.5'), 3, Decimal('3.5')))
        assert_equal((v['balance_pq_1'], v['balance_pq_2'], v['unconfirmed']), (Decimal('1.5'), 2, 0))
        for k in ('z1_', 'z0_'):
            assert_equal((v[k + 'transparent'], v[k + 'transparent_ecc'], v[k + 'transparent_pq'], v[k + 'private'],
                          v[k + 'total'], v[k + 'pq_1'], v[k + 'pq_2']),
                         (Decimal('6.5'), 3, Decimal('3.5'), 1, Decimal('7.5'), Decimal('1.5'), 2))
        assert_equal((v['ecc@1'], v['fn@1'], v['slh@1'], v['z@1']), (3, 2, Decimal('1.5'), 1))
        self.check_listing()
        confirmed = v

        print('own spends, unconfirmed (L-6): balance counts own change, z_gettotalbalance at minconf 1 does not')
        out = n1.getnewaddress()
        # SLH-DSA: the only coin left unlocked, so the wallet must spend it; its change goes to a new ECC key
        others = [{'txid': u['txid'], 'vout': u['vout']} for u in n0.listunspent(0, 9999999, [self.addrs['ecc'], self.addrs['fn']])]
        assert_equal(len(others), 2)
        assert n0.lockunspent(False, others)
        tx_s = n0.sendtoaddress(out, 1)
        assert n0.lockunspent(True, others)
        fee_s = -D(n0.gettransaction(tx_s)['fee'])
        assert_equal(len(n0.getrawtransaction(tx_s, 1)['vin']), 1)
        assert_equal(n0.listunspent(0, 9999999, [self.addrs['slh']]), [])    # the SLH coin is spent
        change_s = Decimal('1.5') - 1 - fee_s
        # ECC: z_sendmany spends every coin of the ECC address; its change goes to a new ECC key of the wallet
        tx_e = wait_and_assert_operationid_status(n0, n0.z_sendmany(self.addrs['ecc'], [{'address': out, 'amount': 1}], 1, ZFEE))
        self.sync_all()
        v = self.view()
        assert_equal(v['unconfirmed'], 0)                               # nothing from others
        assert_equal(v['balance'], Decimal('6.5') - 2 - ZFEE - fee_s)
        assert_equal(v['balance_pq'], 2)                                # the FN coin; the SLH one is spent
        assert_equal((v['balance_pq_1'], v['balance_pq_2']), (0, 2))
        assert_equal(v['balance_ecc'], 3 - 1 - ZFEE + change_s)         # own unconfirmed change counted
        assert_equal(v['z1_transparent_ecc'], 0)                        # depth only: the change is unconfirmed,
        assert_equal(v['z1_transparent_pq'], 2)                         # the spent coins are gone
        assert_equal(v['z0_transparent'], v['balance'])
        assert_equal((v['ecc@1'], v['ecc@0'], v['slh@1'], v['slh@0'], v['fn@1']), (0, 0, 0, 0, 2))
        self.check_listing()
        unconfirmed_spends = v

        print('own spends, confirmed')
        self.mine(1)
        blk = n0.getbestblockhash()
        v = self.view()
        assert_equal(v['balance'], unconfirmed_spends['balance'])
        assert_equal(v['z1_transparent'], v['balance'])
        assert_equal((v['z1_transparent_ecc'], v['z1_transparent_pq'], v['z1_pq_1'], v['z1_pq_2']),
                     (v['balance_ecc'], 2, 0, 2))
        assert_equal((v['ecc@1'], v['slh@1'], v['fn@1'], v['z@1']), (0, 0, 2, 1))
        spent_confirmed = v

        print('reorg: the spends\' block disconnected, the spends back in the mempool, then reconnected')
        n0.invalidateblock(blk)
        assert tx_e in n0.getrawmempool() and tx_s in n0.getrawmempool()
        assert_equal(self.view(), unconfirmed_spends)
        n0.reconsiderblock(blk)
        sync_blocks(self.nodes)
        assert_equal(self.view(), spent_confirmed)

        print('reorg below the payments: they go back to unconfirmed, and so do the views')
        pay_blk = n0.getblockhash(n0.getblockcount() - 1)
        n0.invalidateblock(pay_blk)
        v = self.view()
        assert_equal(v['z1_transparent'], 0)
        assert_equal(v['z0_transparent'], v['balance'] + v['unconfirmed'])
        n0.reconsiderblock(pay_blk)
        sync_blocks(self.nodes)
        assert_equal(self.view(), spent_confirmed)
        self.check_listing()

        print('an encrypted, locked wallet shows the same balances and listings')
        n0.encryptwallet(PASSPHRASE)
        bitcoind_processes[0].wait()
        self.nodes[0] = start_node(0, self.options.tmpdir, self.node_args(0))
        connect_nodes_bi(self.nodes, 0, 1)
        sync_blocks(self.nodes)
        n0 = self.nodes[0]
        assert_equal(n0.getwalletinfo()['unlocked_until'], 0)
        assert_equal(self.view(), spent_confirmed)
        self.check_listing()
        assert_raises_rpc('walletpassphrase', n0.getnewaddress, '', 'pq')
        n0.walletpassphrase(PASSPHRASE, 60)
        assert_equal(self.view(), spent_confirmed)
        n0.walletlock()


if __name__ == '__main__':
    WalletBalanceByTypeTest().main()
