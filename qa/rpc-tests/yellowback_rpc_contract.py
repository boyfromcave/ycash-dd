#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
The RPC contract (doc/yellowback-rpc.md, P7): every command's live result is checked against the
example shape in doc/yellowback-rpc-contract.json (generated from the document by `make spec`):
every documented key present with its JSON type, nothing undocumented added, `null` accepted
only where the text marks the field *null when …*, absence accepted only for fields the text
marks **optional**. The wallet context is exercised in full (Phase 6): `yed_mint`, `yed_send`,
`yed_sendmany`, `yed_redeem` on an ACTIVE vault and on a VOID vault (the release, L14),
`yed_claim`, and `yed_sweep` under a forced abandonment (L10), plus every wallet error identifier
of the *Error identifiers* table by its documented provocation. The node context is checked the
same way wherever the scenario passes it; the node-context error provocations that need a
storage fault or a rejected block belong to the Phase 3 script.

Nodes: 0 user, 1 stock, 2-4 pools, 5 observer (claimant; sacrificed to the storage fault at the end).
"""

import json
import os
from decimal import Decimal

from test_framework.util import assert_equal, assert_greater_than, bytes_to_hex_str
from test_framework.yellowback_util import (
    ABANDON_BLOCKS,
    COIN,
    ENFORCEMENT_FLOOR,
    POOLS,
    REF_LAG,
    STOCK,
    YellowbackTestFramework,
    build_mint_tx,
    mine_block_raw,
    set_quote,
)

CONTRACT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'doc', 'yellowback-rpc-contract.json')
SWEEP_ACK = 'I understand this leaves YED unbacked'

# Fields the document marks **optional**, per command (absent unless the state the text names holds).
OPTIONAL = {
    'yed_getvault': {'sweepBefore'},
    'yed_listpositions': {'sweepBefore'},
    'yed_listvaults': {'sweepBefore'},
    'yed_getfeepayee': {'preferred'},
    'yed_gettag': {'version', 'signal', 'priceMicroUsd', 'sourceMask', 'payoutAddress'},
    'yed_validateaddress': {'address', 'keyid', 'ismine', 'transparentAddress'},
    'yed_decodepayload': {'termClass', 'cents', 'lockHeight', 'refHeight', 'ownerPubKey', 'feeVout', 'assignments', 'assignedCents'},
}


# Fields the text marks *null when …* although the example shows a value (by path suffix).
NULLABLE = {'yed_getinfo': {'miner.payoutAddress', 'miner.quoteAgeSeconds', 'params.policy.preferredPayee'},
            'yed_listminers': {'accuracyBps'}, 'yed_getvault': {'closeHeight', 'underwaterAt'},
            'yed_listpositions': {'closeHeight', 'underwaterAt'}, 'yed_listvaults': {'closeHeight', 'underwaterAt'},
            'yed_listclaimable': {'underwaterAt'}, 'yed_gettxinfo': {'payee'}, 'yed_validaterawtransaction': {'payee'},
            'yed_listtransactions': {'payee'}, 'yed_mint': {'payee'}, 'yed_redeem': {'payee'}, 'yed_claim': {'payee'},
            'yed_estimatecollateral': {'requiredZat', 'pMint'},
            'yed_getstats': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim', 'globalRatioBps', 'supplyCapCents'},
            'yed_getprice': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim'},
            'yed_gethistory': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim', 'globalRatioBps'},
            'yed_listclaimable': {'underwaterAt', 'pClaim'}}


def assert_rpc_error(substr, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        assert substr in str(e), 'expected %r in %r' % (substr, str(e))
        return
    raise AssertionError('expected an error containing %r' % substr)


def json_type(v):
    if isinstance(v, bool):
        return 'boolean'
    if isinstance(v, (int, float, Decimal)):
        return 'number'
    if v is None:
        return 'null'
    if isinstance(v, str):
        return 'string'
    if isinstance(v, list):
        return 'array'
    if isinstance(v, dict):
        return 'object'
    raise AssertionError('unexpected value %r' % (v,))


class Contract(object):

    def __init__(self, path):
        with open(path) as f:
            self.doc = json.load(f)
        self.checked = set()

    def check(self, cmd, result, where=''):
        """Compare ``result`` with the example under ``cmd`` in the contract."""
        shape = self.doc[cmd]['returns']
        self._match(cmd, shape, result, cmd + where)
        self.checked.add(cmd)
        return result

    def _match(self, cmd, example, value, path):
        if isinstance(example, dict):
            assert isinstance(value, dict), '%s: expected an object, got %s' % (path, json_type(value))
            optional = OPTIONAL.get(cmd, set())
            for key, ex in example.items():
                if key not in value:
                    assert key in optional, '%s: documented field %r is absent' % (path, key)
                    continue
                self._match(cmd, ex, value[key], path + '.' + key)
            extra = set(value) - set(example)
            assert not extra, '%s: undocumented field(s) %s' % (path, sorted(extra))
            return
        if isinstance(example, list):
            assert isinstance(value, list), '%s: expected an array, got %s' % (path, json_type(value))
            if not example:
                return                     # the example is an empty array: element shape unspecified
            for i, row in enumerate(value):
                self._match(cmd, example[0], row, '%s[%d]' % (path, i))
            return
        if example is None:
            # "null when …": null or the documented type (the checker accepts either)
            return
        got = json_type(value)
        want = json_type(example)
        if got == 'null':
            suffix = path.split('.', 1)[1] if '.' in path else ''
            suffix = suffix.replace('[0]', '').replace('[1]', '')
            assert any(suffix.endswith(n) for n in NULLABLE.get(cmd, ())), '%s: null where the contract has a %s' % (path, want)
            return
        assert got == want, '%s: expected %s, got %s (%r)' % (path, want, got, value)


class YellowbackRpcContractTest(YellowbackTestFramework):

    def run_test(self):
        nodes = self.nodes
        user, stock, claimant = nodes[0], nodes[STOCK], nodes[5]
        c = Contract(CONTRACT)
        assert_equal(c.doc['rpcversion'], 2)

        print('before activation: mintpol-not-active')
        assert_equal(user.yed_getinfo()['rpcversion'], 2)
        assert_rpc_error('mintpol-not-active', user.yed_mint, 10000, 48)
        assert_rpc_error('fee-no-eligible-payee', user.yed_getfeepayee, 1, 10 * COIN)

        print('activate at $50; fund the claimant')
        self.activate(POOLS, quote_usd=50)
        self.mine_round_robin(POOLS, REF_LAG + 1)
        user.sendtoaddress(claimant.getnewaddress(), 5)
        self.sync_all()
        self.mine(POOLS[0])

        print('node context at the tip')
        c.check('yed_getinfo', user.yed_getinfo())
        c.check('yed_getstatehash', user.yed_getstatehash())
        c.check('yed_getstats', user.yed_getstats())
        c.check('yed_getprice', user.yed_getprice())
        c.check('yed_getactivation', user.yed_getactivation())
        rows = c.check('yed_listminers', user.yed_listminers())
        assert_greater_than(len(rows), 0)
        c.check('yed_gettag', user.yed_gettag(str(user.getblockcount())))
        c.check('yed_gettag', user.yed_gettag(user.getblockhash(1)))
        c.check('yed_setquote', nodes[POOLS[0]].yed_setquote(50_000_000, 1))
        assert_rpc_error('quote-out-of-range', nodes[POOLS[0]].yed_setquote, 1, 1)
        assert_rpc_error('no-payout-address', user.yed_setquote, 50_000_000, 1)
        r = user.yed_getinfo()['height'] - REF_LAG
        c.check('yed_getfeepayee', user.yed_getfeepayee(r, 10 * COIN))
        c.check('yed_estimatecollateral', user.yed_estimatecollateral(10000, 48))
        c.check('yed_estimatefee', user.yed_estimatefee(10 * COIN))
        c.check('yed_gethistory', user.yed_gethistory(r - 3, r))
        assert_rpc_error('verdict-parent-not-tip', user.yed_getblockverdict, user.getblockhash(user.getblockcount() - 2))
        assert_rpc_error('mint-unsatisfiable', user.yed_estimatecollateral, 1_000_000, 48, 100)
        assert_rpc_error('mint-bad-lock', user.yed_estimatecollateral, 10000, 10)

        print('wallet context: addresses, balances')
        addr = c.check('yed_getnewaddress', user.yed_getnewaddress())
        assert addr.startswith('yr')
        c.check('yed_validateaddress', user.yed_validateaddress(addr))
        bad = c.check('yed_validateaddress', user.yed_validateaddress(user.getnewaddress()))
        assert_equal((bad['isvalid'], bad['reason']), (False, 'not-a-yellowback-address'))
        c.check('yed_getbalance', user.yed_getbalance())
        assert_equal(c.check('yed_listunspent', user.yed_listunspent()), [])
        assert_equal(c.check('yed_lockcoins', user.yed_lockcoins()), [])
        assert_equal(c.check('yed_listpositions', user.yed_listpositions()), [])
        assert_equal(c.check('yed_listtransactions', user.yed_listtransactions()), [])

        print('yed_mint (twice: one to keep, one to release as VOID) and a raw VOID mint')
        assert_rpc_error('mint-bad-lock', user.yed_mint, 10000, 10)
        assert_rpc_error('mint-unsatisfiable', user.yed_estimatecollateral, 1_000_000, 48, 100)
        mint_a = c.check('yed_mint', user.yed_mint(10000, 48))
        mint_b = c.check('yed_mint', user.yed_mint(10000, 48))
        mint_x = c.check('yed_mint', user.yed_mint(10000, 48))     # its YED funds the redemption of B
        r = user.yed_getinfo()['height'] - REF_LAG
        void_hex, _ = build_mint_tx(user, 10000, 48, r, user.yed_estimatecollateral(10000, 48)['requiredZat'] - 1000)
        void_txid = user.decoderawtransaction(void_hex)['txid']
        raw = user.getrawtransaction(mint_a['txid'], 1)
        c.check('yed_decodepayload', user.yed_decodepayload(raw['vout'][2]['scriptPubKey']['hex'][4:]))
        c.check('yed_validaterawtransaction', user.yed_validaterawtransaction(raw['hex']))
        self.sync_all()
        self.mine(POOLS[1])
        # TPL-2 (strict, the default) skips a MINT whose verdict would be VOID, so no pool template
        # will ever carry the short-collateral mint: its block is assembled in Python (6.0 item 4).
        result, _ = mine_block_raw(nodes[POOLS[1]], [void_hex])
        assert result is None, result
        self.sync_all(blocks_only=True)
        vault_a = c.check('yed_getvault', user.yed_getvault(mint_a['txid']))
        assert_equal(vault_a['status'], 'ACTIVE')
        assert 'sweepBefore' not in vault_a
        void_vault = c.check('yed_getvault', user.yed_getvault(void_txid))
        assert_equal((void_vault['status'], void_vault['sweepBefore']), ('VOID', void_vault['claimHeight']))
        c.check('yed_listvaults', user.yed_listvaults())
        c.check('yed_listvaults', user.yed_listvaults('VOID', 10, 0))
        c.check('yed_gettxinfo', user.yed_gettxinfo(mint_a['txid']))
        assert_rpc_error('vault-not-found', user.yed_getvault, '22' * 32)
        rows = c.check('yed_listunspent', user.yed_listunspent())
        assert_equal(len(rows), 3)
        c.check('yed_lockcoins', user.yed_lockcoins())
        positions = c.check('yed_listpositions', user.yed_listpositions())
        assert_equal(len(positions), 4)
        assert_equal(len(c.check('yed_listpositions', user.yed_listpositions('VOID'))), 1)

        print('yed_send, yed_sendmany, their refusals')
        assert_rpc_error('not-a-yellowback-address', user.yed_send, user.getnewaddress(), 100)
        assert_rpc_error('not-a-yellowback-address', user.yed_sendmany, {user.getnewaddress(): 100})
        assert_rpc_error('insufficient-yed', user.yed_send, claimant.yed_getnewaddress(), 40000)
        assert_rpc_error('insufficient-yed', user.yed_sendmany, {claimant.yed_getnewaddress(): 40000})
        assert_rpc_error('change-floor', user.yed_send, claimant.yed_getnewaddress(), 9950)
        assert_rpc_error('change-floor', user.yed_sendmany, {claimant.yed_getnewaddress(): 9950})
        sent = c.check('yed_send', user.yed_send(claimant.yed_getnewaddress(), 10000))
        self.sync_all()
        self.mine(POOLS[2])
        many = c.check('yed_sendmany', user.yed_sendmany({claimant.yed_getnewaddress(): 100, user.yed_getnewaddress(): 200}))
        self.sync_all()
        self.mine(POOLS[0])
        assert_equal(claimant.yed_getbalance()['confirmedCents'], 10100)
        rows = c.check('yed_listtransactions', user.yed_listtransactions())
        assert_equal({x['txid'] for x in rows} >= {mint_a['txid'], sent['txid'], many['txid']}, True)
        c.check('yed_listtransactions', user.yed_listtransactions(1, 1))

        print('yed_redeem refusals, then the VOID release (L14) and the ACTIVE redemption')
        assert_rpc_error('vault-locked', user.yed_redeem, mint_a['txid'])
        assert_rpc_error('vault-locked', user.yed_redeem, void_txid)
        assert_rpc_error('vault-not-owned', claimant.yed_redeem, mint_a['txid'])
        assert_rpc_error('vault-not-found', user.yed_redeem, '33' * 32)
        assert_rpc_error('claim-not-yet', claimant.yed_claim, mint_a['txid'])
        assert_rpc_error('sweep-not-abandoned', user.yed_sweep, mint_a['txid'], SWEEP_ACK)
        assert_rpc_error('sweep-acknowledgement-missing', user.yed_sweep, mint_a['txid'], 'sure')
        lock = user.yed_getvault(void_txid)['lockHeight']
        self.mine_round_robin(POOLS, lock - user.getblockcount())
        released = c.check('yed_redeem', user.yed_redeem(void_txid))
        assert_equal((released['burnedCents'], released['feeZat'], released['payee']), (0, 0, None))
        redeemed = c.check('yed_redeem', user.yed_redeem(mint_b['txid']))
        assert_equal(redeemed['burnedCents'], 10000)
        self.sync_all()
        self.mine(POOLS[1])
        assert_equal(user.yed_getvault(void_txid)['status'], 'CLOSED')
        assert_equal(user.yed_getvault(mint_b['txid'])['status'], 'CLOSED')
        assert_rpc_error('vault-not-active', user.yed_redeem, mint_b['txid'])
        assert_rpc_error('vault-not-active', claimant.yed_claim, mint_b['txid'])
        assert_rpc_error('insufficient-yed', user.yed_redeem, mint_a['txid'])      # the user's YED went to the claimant
        c.check('yed_gettxinfo', user.yed_gettxinfo(redeemed['txid']))

        print('yed_claim: a price crash, then the claimant claims vault A')
        claim_height = user.yed_getvault(mint_a['txid'])['claimHeight']
        self.mine_round_robin(POOLS, max(0, claim_height - user.getblockcount()))
        assert_rpc_error('claim-not-underwater', claimant.yed_claim, mint_a['txid'])
        for i in POOLS:
            set_quote(nodes[i], '0.01')
        self.mine_round_robin(POOLS, 64)
        claimable = c.check('yed_listclaimable', user.yed_listclaimable())
        assert mint_a['txid'] + ':0' in [x['vault'] for x in claimable]
        claimed = c.check('yed_claim', claimant.yed_claim(mint_a['txid']))       # 100 + 10000 in, 1 YED change
        assert_equal(claimed['burnedCents'], 10000)
        self.sync_all()
        self.mine(POOLS[0])
        assert_equal(user.yed_getvault(mint_a['txid'])['status'], 'CLAIMED')
        c.check('yed_listtransactions', claimant.yed_listtransactions())

        print('yed_sweep under a forced abandonment (L10): mint first, then the pools stop signalling')
        for i in POOLS:
            set_quote(nodes[i], 50)
        self.mine_round_robin(POOLS, 64 + REF_LAG)
        assert_equal(user.yed_getstats()['mintingAllowed'], True)
        mint_c = c.check('yed_mint', user.yed_mint(10000, 48))
        self.sync_all()
        self.mine(POOLS[1])
        for i in POOLS:
            self.restart(i, ['-yellowbacksignal=0'])
            set_quote(nodes[i], 50)
        self.mine_round_robin(POOLS, 64 - ENFORCEMENT_FLOOR + 1 + ABANDON_BLOCKS)
        info = c.check('yed_getinfo', user.yed_getinfo())
        assert_equal(info['abandoned'], True)
        vault_c = c.check('yed_getvault', user.yed_getvault(mint_c['txid']))
        assert_equal(vault_c['sweepBefore'], vault_c['claimHeight'])
        pos = [p for p in c.check('yed_listpositions', user.yed_listpositions()) if p['txid'] == mint_c['txid']][0]
        assert_equal((pos['canSweep'], pos['sweepBefore']), (True, vault_c['claimHeight']))
        assert_rpc_error('sweep-acknowledgement-missing', user.yed_sweep, mint_c['txid'], 'I understand')
        assert_rpc_error('vault-not-owned', claimant.yed_sweep, mint_c['txid'], SWEEP_ACK)
        swept = c.check('yed_sweep', user.yed_sweep(mint_c['txid'], SWEEP_ACK))
        assert_equal(swept['unbackedCents'], 10000)
        c.check('yed_validaterawtransaction', user.yed_validaterawtransaction(swept['hex']))
        self.sync_all()
        self.mine(STOCK)
        assert_equal(user.yed_getvault(mint_c['txid'])['status'], 'CLOSED')
        rows = c.check('yed_listtransactions', user.yed_listtransactions())
        assert_equal([x['type'] for x in rows if x['txid'] == swept['txid']], ['sweep'])
        c.check('yed_getactivation', user.yed_getactivation())
        c.check('yed_getstats', user.yed_getstats())

        print('yellowback-unhealthy on the observer after a storage fault; the allow-list still answers')
        self.restart(5, ['-yellowbacktestfault=storage:commit'])
        self.mine(POOLS[0])
        assert_equal(nodes[5].yed_getinfo()['healthy'], False)
        assert_rpc_error('yellowback-unhealthy', nodes[5].yed_getbalance)
        assert_rpc_error('yellowback-unhealthy', nodes[5].yed_listpositions)
        assert_rpc_error('yellowback-unhealthy', nodes[5].yed_getstats)
        c.check('yed_getinfo', nodes[5].yed_getinfo())
        c.check('yed_gettag', nodes[5].yed_gettag(str(nodes[5].getblockcount())))

        documented = sorted(k for k in c.doc if k.startswith('yed_'))
        unchecked = sorted(set(documented) - c.checked - {'yed_estimatesend', 'yed_unlockcoin', 'yed_getblockverdict'})
        assert_equal(unchecked, [])
        print('checked: %s' % ', '.join(sorted(c.checked)))


if __name__ == '__main__':
    YellowbackRpcContractTest().main()
