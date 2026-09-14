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

v3 (Phase A2): after the wallet flow three attestors are registered raw and armed, the pool is
fed and every v3 node command is checked with every node-side v3 error identifier provoked
(attest-unknown-seq, attest-not-eligible, attest-stale, attest-bad-sig, attest-range,
attest-malformed, bundle-insufficient, mint10-diverged). The wallet commands whose v3 shape
Phase A3 delivers (PENDING_A3 below) are checked softly: a mismatch is recorded and printed,
not fatal, until A3 lands; the new wallet commands are not called.

Nodes: 0 user, 1 stock, 2-4 pools, 5 observer (claimant; sacrificed to the storage fault at the end).
"""

import json
import os
from decimal import Decimal

from test_framework.util import assert_equal, assert_greater_than
from test_framework.util import bytes_to_hex_str
from test_framework.yellowback_util import (
    ABANDON_BLOCKS,
    ATTEST_ARM_MIN,
    ATTEST_MAX_AGE,
    COIN,
    ENFORCEMENT_FLOOR,
    POOLS,
    REF_LAG,
    STOCK,
    YellowbackTestFramework,
    build_mint_tx,
    mine_block_raw,
    set_quote,
    usd_to_micro,
)
from test_framework.yellowback_attest import (
    attestor_keys,
    build_carrier_tx,
    encode_bundle,
    equivocation_raw,
    feed,
    feed_all,
    hot_secret_for,
    register_and_arm,
    sign_attestation,
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
    'yed_decodepayload': {'termClass', 'cents', 'lockHeight', 'refHeight', 'ownerPubKey', 'feeVout', 'attestFeeVout', 'assignments',
                          'assignedCents', 'register', 'notice', 'equivocation', 'revive', 'bundle'},
    'yed_getnotice': {'vault', 'txid', 'height', 'refHeight', 'pEmerg', 'emergencyOpenAt', 'expiresAt'},
    'yed_gettxinfo': {'seq'},
}

# Wallet commands whose v3 shape Phase A3 delivers (the node context is complete in A2): a shape
# mismatch is recorded, not fatal, and the new wallet commands are not called at all.
PENDING_A3 = {'yed_mint', 'yed_claim', 'yed_listpositions'}
NEW_WALLET_A3 = {'yed_claimnotice', 'yed_sweepcarriers', 'yed_registerattestor', 'yed_withdrawbond', 'yed_revive',
                 'yed_reportequivocation', 'yed_signattestation'}


# Fields the text marks *null when …* although the example shows a value (by path suffix).
NULLABLE = {'yed_getinfo': {'miner.payoutAddress', 'miner.quoteAgeSeconds', 'params.policy.preferredPayee', 'params.policy.preferredAttestor'},
            'yed_listminers': {'accuracyBps'}, 'yed_getvault': {'closeHeight', 'underwaterAt'},
            'yed_listpositions': {'closeHeight', 'underwaterAt'}, 'yed_listvaults': {'closeHeight', 'underwaterAt'},
            'yed_listclaimable': {'underwaterAt'}, 'yed_gettxinfo': {'payee'}, 'yed_validaterawtransaction': {'payee'},
            'yed_listtransactions': {'payee'}, 'yed_mint': {'payee'}, 'yed_redeem': {'payee'}, 'yed_claim': {'payee'},
            'yed_estimatecollateral': {'requiredZat', 'pMint'},
            'yed_getstats': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim', 'globalRatioBps', 'supplyCapCents'},
            'yed_getprice': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim'},
            'yed_gethistory': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim', 'globalRatioBps'}}
# v3 fields the text marks null when ... (the example shows a value)
for _cmd, _fields in {
    'yed_getprice': {'xMint', 'xClaim'},
    'yed_getvault': {'noticeHeight', 'emergencyOpenAt'}, 'yed_listvaults': {'noticeHeight', 'emergencyOpenAt'},
    'yed_listpositions': {'noticeHeight', 'emergencyOpenAt'}, 'yed_listclaimable': {'noticeHeight', 'emergencyOpenAt'},
    'yed_gettxinfo': {'xMint', 'xClaim', 'aMint', 'aClaim', 'pMint', 'pClaim', 'attestPayee'},
    'yed_estimatecollateral': {'xMint', 'aMint', 'divergenceBps'},
    'yed_listattestors': {'bondSpentHeight', 'seatedSince', 'lastBundleHeight'},
    'yed_buildbundle': {'aMint', 'aClaim'},
}.items():
    NULLABLE.setdefault(_cmd, set()).update(_fields)


def assert_rpc_error(substr, fn, *args):
    try:
        fn(*args)
    except Exception as e:
        assert substr in str(e), 'expected %r in %r' % (substr, str(e))
        return str(e)
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
        self.pending = {}        # PENDING_A3 command -> the first mismatch message

    def check(self, cmd, result, where=''):
        """Compare ``result`` with the example under ``cmd`` in the contract."""
        shape = self.doc[cmd]['returns']
        if cmd in PENDING_A3:
            try:
                self._match(cmd, shape, result, cmd + where)
            except AssertionError as e:
                self.pending.setdefault(cmd, str(e))
                return result
        else:
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
            extra = set(value) - set(example) - optional      # an optional field may be present when its state holds
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
        user, claimant = nodes[0], nodes[5]
        c = Contract(CONTRACT)
        assert_equal(c.doc['rpcversion'], 3)

        print('before activation: mintpol-not-active')
        assert_equal(user.yed_getinfo()['rpcversion'], c.doc['rpcversion'])
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
        c.check('yed_mint', user.yed_mint(10000, 48))               # its YED funds the redemption of B
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
        # H1: the floor-aware selector makes most amounts workable; what is left unworkable is
        # 50 cents under the wallet's whole spendable balance (no subset sums to it, and the
        # only larger selection is everything, leaving 50 cents of change).
        band_amount = user.yed_getbalance()['confirmedCents'] - 50
        assert_rpc_error('change-floor', user.yed_send, claimant.yed_getnewaddress(), band_amount)
        assert_rpc_error('change-floor', user.yed_sendmany, {claimant.yed_getnewaddress(): band_amount})
# Rule: H2
        # H2: the change-floor message is structured — the GUI reads the two amounts out of it.
        msg = assert_rpc_error('change-floor', user.yed_send, claimant.yed_getnewaddress(), band_amount)
        assert 'nearest workable amounts: below ' in msg and ', above ' in msg, msg

# Rule: H3
        print('yed_estimatesend (H3): the dry run, workable and unworkable')
        est = c.check('yed_estimatesend', user.yed_estimatesend(10000))
        assert_equal(est['workable'], True)
        assert_equal(est['alternatives'], None)
        assert_greater_than(len(est['inputs']), 0)
        band = c.check('yed_estimatesend', user.yed_estimatesend(band_amount))
        assert_equal(band['workable'], False)
        assert_equal(band['error'], 'change-floor')
        assert_equal(band['stage'], 'none')
        assert band['alternatives'] is not None
        c.check('yed_estimatesend', user.yed_estimatesend({claimant.yed_getnewaddress(): 10000}))

# Rule: H5
        print('yed_unlockcoin (H5) and the lockunspent refusal')
        held = user.yed_listunspent()[0]
        point = {'txid': held['txid'], 'vout': held['vout']}
        assert_rpc_error('yed-locked-outpoint', user.lockunspent, True, [point])
        assert_rpc_error('unlock-acknowledgement-missing', user.yed_unlockcoin, held['txid'], held['vout'], 'nope')
        c.check('yed_unlockcoin', user.yed_unlockcoin(held['txid'], held['vout'], 'I understand this burns YED'))
        user.yed_lockcoins()

# Rule: H10
        info_h10 = user.yed_getinfo()
        assert_equal(info_h10['protectedByIndex'], True)
        assert_equal(info_h10['lockedOutputs'], len(user.yed_listunspent()))

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

        self.v3_node_context(c, mint_a['txid'])

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
        unchecked = sorted(set(documented) - c.checked - {'yed_estimatesend', 'yed_unlockcoin', 'yed_getblockverdict'}
                           - PENDING_A3 - NEW_WALLET_A3)
        assert_equal(unchecked, [])
        print('checked: %s' % ', '.join(sorted(c.checked)))
        for cmd in sorted(PENDING_A3):
            print('pending A3 (%s): %s' % (cmd, c.pending.get(cmd, 'v3 shape already satisfied')))
        print('not called (A3 wallet commands): %s' % ', '.join(sorted(NEW_WALLET_A3)))

    def v3_node_context(self, c, some_vault_txid):
        """Phase A2: the six v3 node commands, the v3 fields of the v2 ones, and every node-side
        v3 error identifier by its documented provocation."""
        nodes = self.nodes
        user = nodes[0]
        print('v3: register %d attestors raw, mature, arm (ARM-1/2)' % ATTEST_ARM_MIN)
        for i in POOLS:
            set_quote(nodes[i], 50)
        seqs = register_and_arm(self, ATTEST_ARM_MIN)
        assert_equal(len(seqs), ATTEST_ARM_MIN)
        self.mine_round_robin(POOLS, REF_LAG)           # a transaction at tip + 1 reads Snapshots[tip - REF_LAG]: ARMED there too
        info = c.check('yed_getinfo', user.yed_getinfo())
        assert_equal((info['attest']['status'], info['attest']['armed'], info['attest']['seatedCount']), ('ARMED', True, ATTEST_ARM_MIN))
        assert_equal(info['rebuilt'], False)
        rows = c.check('yed_listattestors', user.yed_listattestors())
        assert_equal(sorted(int(r['seq']) for r in rows), seqs)
        assert_equal([r['status'] for r in rows], ['ELIGIBLE'] * ATTEST_ARM_MIN)
        assert all(r['seated'] and int(r['weight']) > 0 and r['founding'] for r in rows), rows
        c.check('yed_listattestors', user.yed_listattestors(user.yed_getinfo()['height'] - 1))
        price = c.check('yed_getprice', user.yed_getprice())
        assert_equal((price['armed'], price['attestStatus'], price['seated'], price['pinnedKeys'], price['pinnedSeqs']),
                     (True, 'ARMED', seqs, [], []))
        assert_equal((price['xMint'], price['xClaim']), (price['pMint'], price['pClaim']))
        reg_txid = rows[0]['bondOutpoint']['txid']
        reg = c.check('yed_gettxinfo', user.yed_gettxinfo(reg_txid))
        assert_equal((reg['type'], reg['seq']), ('register', int(rows[0]['seq'])))
        raw = user.getrawtransaction(reg_txid, 1)
        dec = c.check('yed_decodepayload', user.yed_decodepayload(raw['vout'][1]['scriptPubKey']['hex'][4:]))
        assert_equal((dec['type'], dec['register']['bondKeyAddress']), ('register', rows[0]['bondKeyAddress']))

        print('v3: the empty pool, then feed_all; yed_getattestations / yed_addattestation / yed_buildbundle / yed_getselection')
        assert_equal(c.check('yed_getattestations', user.yed_getattestations()), [])
        r = user.yed_getinfo()['height'] - REF_LAG
        sel = c.check('yed_getselection', user.yed_getselection(r, ''))
        assert_equal((sel['reachable'], sel['armed'], sorted(sel['pool'])), (0, True, seqs))
        assert_rpc_error('bundle-insufficient', user.yed_buildbundle, r, '')
        assert_rpc_error('bundle-insufficient', user.yed_estimatecollateral, 10000, 48)
        fed = feed_all(user, {seq: 50 for seq in seqs})
        added = c.check('yed_addattestation', fed[seqs[0]][1])
        assert_equal((added['accepted'], added['replaced'], added['seq']), (True, False, seqs[0]))
        again = c.check('yed_addattestation', user.yed_addattestation(fed[seqs[0]][0]))
        assert_equal((again['accepted'], again['replaced']), (True, False))
        pool = c.check('yed_getattestations', user.yed_getattestations())
        assert_equal(sorted(int(a['seq']) for a in pool), seqs)
        assert all(a['fresh'] and a['seated'] for a in pool), pool
        assert_equal(user.yed_getinfo()['attest']['poolFresh'], ATTEST_ARM_MIN)
        bundle = c.check('yed_buildbundle', user.yed_buildbundle(r, ''))
        assert_equal((bundle['missing'], bundle['count'], bundle['aMint'], bundle['armed']), ([], len(bundle['seqs']), usd_to_micro(50), True))
        sel = c.check('yed_getselection', user.yed_getselection(r, ''))
        assert_equal(sel['reachable'], len(sel['selected']))
        c.check('yed_getselection', user.yed_getselection(r, '11' * 36))
        est = c.check('yed_estimatecollateral', user.yed_estimatecollateral(10000, 48))
        assert_equal((est['armed'], est['aMint'], est['source'], sorted(est['bundleSeqs']), est['divergenceBps']),
                     (True, usd_to_micro(50), 'x', sorted(bundle['seqs']), 0))
        assert_greater_than(est['attestFeeZat'], 0)
        notice = c.check('yed_getnotice', user.yed_getnotice(some_vault_txid))
        assert_equal(notice, {'found': False})
        assert_rpc_error('vault-not-found', user.yed_getnotice, '22' * 32)
        c.check('yed_listclaimable', user.yed_listclaimable())
        c.check('yed_getvault', user.yed_getvault(some_vault_txid))

        print('v3: the node-side error identifiers')
        secret0 = hot_secret_for(user, seqs[0])
        tip = user.getblockcount()
        blockhash = user.getblockhash
        bad_seq = sign_attestation(secret0, 999, usd_to_micro(50), tip - REF_LAG, blockhash(tip - REF_LAG))
        assert_rpc_error('attest-unknown-seq', user.yed_addattestation, bytes_to_hex_str(bad_seq))
        stale = sign_attestation(secret0, seqs[0], usd_to_micro(50), tip - 20, blockhash(tip - 20))
        assert_rpc_error('attest-stale', user.yed_addattestation, bytes_to_hex_str(stale))
        good = bytearray(sign_attestation(secret0, seqs[0], usd_to_micro(50), tip - REF_LAG, blockhash(tip - REF_LAG)))
        good[20] ^= 0x01
        assert_rpc_error('attest-bad-sig', user.yed_addattestation, bytes_to_hex_str(bytes(good)))
        low = sign_attestation(secret0, seqs[0], 50, tip - REF_LAG, blockhash(tip - REF_LAG))       # 50 micro-USD < PRICE_MIN
        assert_rpc_error('attest-range', user.yed_addattestation, bytes_to_hex_str(low))
        assert_rpc_error('attest-malformed', user.yed_addattestation, bytes_to_hex_str(bytes(good[:73])))
        assert_rpc_error('attest-malformed', user.yed_addattestation, 'zz')
        assert_rpc_error('selectorHex', user.yed_buildbundle, r, '1122')
        print('v3: mint10-diverged (attestors at 2x the pools), then bundle-insufficient after the window lapses')
        feed_all(user, {seq: 100 for seq in seqs})
        assert_rpc_error('mint10-diverged', user.yed_estimatecollateral, 10000, 48)
        c.check('yed_estimatecollateral', user.yed_estimatecollateral(10000, 48, usd_to_micro(50)))   # an override bypasses both
        self.mine_round_robin(POOLS, ATTEST_MAX_AGE + 1)
        r = user.yed_getinfo()['height'] - REF_LAG
        msg = assert_rpc_error('bundle-insufficient', user.yed_buildbundle, r, '')
        assert 'of %d selected attestors have a fresh attestation; missing seq' % len(user.yed_getselection(r, '')['selected']) in msg, msg
        feed(user, seqs[0], 50)
        assert_rpc_error('bundle-insufficient', user.yed_estimatecollateral, 10000, 48)
        print('v3: an equivocation ejects seq %d (EQV-1); attest-not-eligible' % seqs[1])
        secret1 = hot_secret_for(user, seqs[1])
        cited = user.getblockcount() - REF_LAG
        a = sign_attestation(secret1, seqs[1], usd_to_micro(50), cited, blockhash(cited))
        b = sign_attestation(secret1, seqs[1], usd_to_micro(51), cited, blockhash(cited))
        carrier = build_carrier_tx(user, encode_bundle([a, b]))
        self.sync_all()
        self.mine(POOLS[0])
        eqv_txid = user.sendrawtransaction(equivocation_raw(user, carrier))
        self.sync_all()
        self.mine(POOLS[1])
        for node in self.enforcing_nodes():
            rec = [x for x in node.yed_listattestors() if int(x['seq']) == seqs[1]][0]
            assert_equal(rec['status'], 'EJECTED')
        eqv = c.check('yed_gettxinfo', user.yed_gettxinfo(eqv_txid))
        assert_equal((eqv['type'], eqv['bundleSeqs'], eqv['carrierVin'] >= 0, eqv['bundleSource']), ('equivocation', [seqs[1]], True, 'scriptsig'))
        assert_rpc_error('attest-not-eligible', feed, user, seqs[1], 50)
        c.check('yed_listattestors', user.yed_listattestors())
        c.check('yed_getinfo', user.yed_getinfo())


if __name__ == '__main__':
    YellowbackRpcContractTest().main()
