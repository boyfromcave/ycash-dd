#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
The RPC contract (plan P7, §4.5, Phase 3): doc/yellowback-rpc-contract.json is derived from
doc/yellowback-rpc.md by the workspace's `make spec`; this script checks the *binary* against
that document, never the reverse.  Every node-context command is called on a node past
activation with one VOID vault, one ACTIVE vault, one claimable vault, one token and one
rejected block in Rejected, so every optional field has a value, and every documented key must
be present with the documented JSON type — recursively for nested objects, element-wise for
arrays that carry rows.  Then every documented error identifier of the node context is provoked
the documented way and its message must begin with the identifier.

The wallet context (`yed_mint`, `yed_redeem`, …) joins in Phase 6.
"""

import json
import os
from decimal import Decimal

from test_framework.util import assert_equal, assert_greater_than, start_node, stop_node
from test_framework.yellowback_util import (
    MAX_MINT,
    POOLS,
    PRICE_MIN,
    REF_LAG,
    STOCK,
    USER,
    YellowbackTestFramework,
    assert_same_statehash,
    build_mint_tx,
    build_vault_spend_raw,
    mine_block_raw,
    mint_vault_raw,
    wait_for_rejection,
    wait_yed_healthy,
)
from test_framework import yellowback_model as ym

NODE_CONTEXT = [
    'yed_getinfo', 'yed_getstatehash', 'yed_getstats', 'yed_getprice', 'yed_getactivation',
    'yed_listminers', 'yed_gettag', 'yed_setquote', 'yed_getfeepayee', 'yed_getvault',
    'yed_listvaults', 'yed_listclaimable', 'yed_gettxinfo', 'yed_decodepayload',
    'yed_validaterawtransaction', 'yed_getblockverdict', 'yed_estimatecollateral',
    'yed_estimatefee', 'yed_gethistory',
]

# Keys the document says are null in a documented case (the example shows the non-null form).
NULLABLE = {
    'yed_getvault': {'underwaterAt', 'closeHeight'},
    'yed_listvaults': {'underwaterAt', 'closeHeight'},
    'yed_getstats': {'pClaim', 'globalRatioBps', 'supplyCapCents'},
    'yed_getprice': {'pFast', 'pMid', 'pSlow', 'pMint', 'pClaim'},
    'yed_listminers': {'accuracyBps'},
    'yed_gettxinfo': {'payee'},
    'yed_validaterawtransaction': {'payee'},
    'yed_getblockverdict': {'payee'},
    'yed_estimatecollateral': {'requiredZat', 'pMint'},
    'yed_getinfo': {'payoutAddress', 'quoteAgeSeconds', 'preferredPayee'},
}

# Keys the document marks **optional**: each is asserted present by a second, dedicated call.
OPTIONAL = {
    'yed_getvault': {'sweepBefore'},
    'yed_listvaults': {'sweepBefore'},
    'yed_getfeepayee': {'preferred'},
    'yed_gettag': {'version', 'signal', 'priceMicroUsd', 'sourceMask', 'payoutAddress'},
    'yed_getprice': set(),
}

# Node-context error identifiers and how the document says to provoke them (wallet ones skipped).
WALLET_ONLY = {
    'mintpol-not-active', 'mintpol-no-price', 'mintpol-participation', 'mintpol-global-ratio',
    'mintpol-divergence', 'mintpol-cap', 'vault-not-active', 'vault-not-owned', 'vault-locked',
    'claim-not-yet', 'claim-not-underwater', 'sweep-not-abandoned', 'sweep-acknowledgement-missing',
    'change-floor', 'not-a-yellowback-address', 'insufficient-yed', 'mempool-check-failed:<verdict>',
}


def rpc_error_message(fn, *args):
    try:
        fn(*args)
    except Exception as e:  # JSONRPCException
        err = getattr(e, 'error', None)
        if isinstance(err, dict):
            return str(err.get('message', ''))
        return str(e)
    raise AssertionError('expected an RPC error from %r%r' % (fn, args))


def assert_identifier(identifier, fn, *args):
    msg = rpc_error_message(fn, *args)
    assert msg.startswith(identifier), 'expected the message to begin with %r, got %r' % (identifier, msg)
    print('  %-28s ok  (%s)' % (identifier, msg[:70]))


def type_name(v):
    if v is None:
        return 'null'
    if isinstance(v, bool):
        return 'bool'
    if isinstance(v, (int, float, Decimal)):      # the proxy parses JSON numbers with decimals as Decimal
        return 'number'
    if isinstance(v, str):
        return 'string'
    if isinstance(v, dict):
        return 'object'
    if isinstance(v, list):
        return 'array'
    return type(v).__name__


def check_shape(expected, actual, path, optional=(), nullable=()):
    """Every documented key present with the documented JSON type; nested objects recursively;
    arrays element-wise against the documented row when the answer has rows.  A documented
    ``null`` (a field that may be null) accepts any value; a documented number accepts a
    number and, by the document's own rule, ``null`` only where it says so — the fixture is
    built so that every such field carries a value here."""
    if expected is None:
        return
    if isinstance(expected, dict):
        assert isinstance(actual, dict), '%s: expected an object, got %s' % (path, type_name(actual))
        for key, sub in expected.items():
            if key not in actual:
                assert key in optional, '%s: documented key %r is missing (keys: %s)' % (path, key, sorted(actual))
                continue
            if actual[key] is None and key in nullable:
                continue
            check_shape(sub, actual[key], path + '.' + key, optional, nullable)
        return
    if isinstance(expected, list):
        assert isinstance(actual, list), '%s: expected an array, got %s' % (path, type_name(actual))
        if expected and actual:
            for i, row in enumerate(actual):
                check_shape(expected[0], row, '%s[%d]' % (path, i), optional, nullable)
        return
    et, at = type_name(expected), type_name(actual)
    assert et == at, '%s: documented %s (%r), got %s (%r)' % (path, et, expected, at, actual)


# Rule: FEE-0 RED-2 SNAP
class YellowbackRpcContractTest(YellowbackTestFramework):

    def node_args(self, i, extra=None):
        # node 2 configures a preferred payee (its own) so yed_getfeepayee.preferred has a value
        if i == 2:
            extra = ['-yellowbackpreferredpayee=%s' % self.pool_addresses[0]] + list(extra or [])
        return super().node_args(i, extra)

    def run_test(self):
        nodes = self.nodes
        user = nodes[USER]
        contract_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'doc', 'yellowback-rpc-contract.json')
        with open(contract_path) as f:
            contract = json.load(f)
        assert_equal(contract['rpcversion'], 2)
        for cmd in NODE_CONTEXT:
            assert cmd in contract, 'command %s missing from the contract' % cmd

        print('fixture: a VOID vault minted before activation')
        self.mine(USER, 60)                                   # mature enough of node 0's coinbases for 250 YEC of collateral
        for pool in POOLS:
            self.quote(pool, '2.00')
        self.mine_round_robin(POOLS, 8)                       # some quote tags so a price exists
        void_txid = None
        est = user.yed_estimatecollateral(10_000, 48, 2_000_000)
        ref = user.getblockcount() - REF_LAG
        hex_, _ = build_mint_tx(user, 10_000, 48, ref, int(est['requiredZat']))
        void_txid = user.sendrawtransaction(hex_)
        self.sync_all()
        # TPL-2 (strict, the default) skips a MINT whose verdict would be VOID, so no pool
        # template will ever carry it: the block is assembled in Python (section 6.0 item 4).
        result, _ = mine_block_raw(nodes[POOLS[0]], [hex_])
        assert result is None, result
        self.sync_all(blocks_only=True)
        void_vault = user.yed_getvault(void_txid)
        assert_equal(void_vault['status'], 'VOID')
        assert 'sweepBefore' in void_vault

        print('fixture: activation, two ACTIVE vaults, one redeemable token')
        self.activate(POOLS)
        self.mine_round_robin(POOLS, REF_LAG + 1)
        vault_a_txid, vault_a = mint_vault_raw(self, user, POOLS[1])
        vault_b_txid, vault_b = mint_vault_raw(self, user, POOLS[2])

        print('fixture: past the claim height with the price crashed (vault B claimable)')
        target = int(vault_b['claimHeight']) + 1
        while user.getblockcount() < target - 40:
            self.mine_round_robin(POOLS, 1)
        for pool in POOLS:
            self.quote(pool, '0.40')
        while user.getblockcount() < target or nodes[2].yed_getstats()['pClaim'] is None or nodes[2].yed_getstats()['pClaim'] > 500_000:
            self.mine_round_robin(POOLS, 1)
        claimable = nodes[2].yed_listclaimable()
        assert_greater_than(len(claimable), 0)
        assert vault_b_txid in [c['vault'].split(':')[0] if isinstance(c['vault'], str) else c['vault']['txid'] for c in claimable]

        print('fixture: a rejected block in Rejected (a fee-carrying spend without its burn, RED-2)')
        stock = nodes[STOCK]
        ref = stock.getblockcount() - REF_LAG
        payee = nodes[2].yed_getfeepayee(ref, int(vault_a['collateralZat']))
        bad_hex = build_vault_spend_raw(user, vault_a, 'owner', [], payload=ym.encode_redeem(ref, 1, []),
                                        fee=(payee['default']['payoutAddress'], int(payee['feeZat'])),
                                        ref_height=ref, expiry=stock.getblockcount() + 4)
        verdict = nodes[2].yed_validaterawtransaction(bad_hex)
        assert_equal(verdict['blockValid'], False)
        assert_equal(verdict['wouldBeRejected'], True)
        bad_txid = stock.sendrawtransaction(bad_hex)
        rejected = stock.generate(1)[0]
        wait_for_rejection(self.enforcing_nodes(), rejected)
        node = nodes[2]
        assert_equal(node.yed_getinfo()['rejectedBlocks'], 1)

        print('shapes: every node-context command against the document')
        tip = node.getblockcount()
        est = node.yed_estimatecollateral(10_000, 48)
        collateral = int(vault_b['collateralZat'])
        ref = tip - REF_LAG
        good_hex = build_vault_spend_raw(user, vault_b, 'owner', [(vault_b_txid, 1)], payload=ym.encode_redeem(ref, 1, []),
                                         fee=(payee['default']['payoutAddress'], int(payee['feeZat'])), ref_height=ref)
        mint_raw = node.getrawtransaction(vault_b_txid, 1)
        payload_hex = [o['scriptPubKey']['hex'] for o in mint_raw['vout'] if o['scriptPubKey']['type'] == 'nulldata'][0]
        calls = [
            ('yed_getinfo', ()),
            ('yed_getstatehash', ()),
            ('yed_getstats', ()),
            ('yed_getprice', ()),
            ('yed_getactivation', ()),
            ('yed_listminers', ()),
            ('yed_gettag', (str(tip),)),
            ('yed_setquote', (2_000_000, 3)),
            ('yed_getfeepayee', (ref, collateral)),
            ('yed_getvault', (vault_b_txid,)),
            ('yed_listvaults', ()),
            ('yed_listclaimable', ()),
            ('yed_gettxinfo', (vault_b_txid,)),
            ('yed_decodepayload', (payload_hex,)),
            ('yed_validaterawtransaction', (good_hex,)),
            ('yed_getblockverdict', (rejected,)),
            ('yed_estimatecollateral', (10_000, 48)),
            ('yed_estimatefee', (collateral,)),
            ('yed_gethistory', (tip - 7, tip)),
        ]
        for cmd, args in calls:
            result = getattr(node, cmd)(*args)
            check_shape(contract[cmd]['returns'], result, cmd, OPTIONAL.get(cmd, ()), NULLABLE.get(cmd, ()))
            if isinstance(result, list):
                assert_greater_than(len(result), 0)
            print('  %-28s ok' % cmd)
        # the optional keys, each from the call that carries it
        assert 'sweepBefore' in node.yed_getvault(void_txid)
        assert 'preferred' in node.yed_getfeepayee(ref, collateral)
        tag = node.yed_gettag(str(tip))
        for key in OPTIONAL['yed_gettag']:
            assert key in tag, key
        check_shape(contract['yed_gettag']['returns'], node.yed_getprice()['tag'], 'yed_getprice.tag')
        assert_equal(node.yed_getblockverdict(rejected)['blockInvalid'], True)
        assert node.yed_getblockverdict(rejected)['reason'].startswith('vault-spend-missing-burn:' + bad_txid)
        assert_greater_than(len(node.yed_getblockverdict(rejected)['transactions']), 0)
        assert_equal(node.yed_getblockverdict(rejected)['transactions'][0]['verdict'], 'vault-spend-missing-burn')
        assert_equal(node.yed_getstats()['claimedVaults'], 0)
        assert_same_statehash(self.enforcing_nodes())

        print('errors: every documented node-context identifier by its documented provocation')
        provoked = set()

        def provoke(identifier, fn, *args):
            assert_identifier(identifier, fn, *args)
            provoked.add(identifier)

        provoke('mint-unsatisfiable', node.yed_estimatecollateral, MAX_MINT, 48, PRICE_MIN)
        provoke('mint-bad-lock', node.yed_estimatecollateral, 10_000, 1)
        provoke('vault-not-found', node.yed_getvault, '00' * 32)
        provoke('verdict-parent-not-tip', node.yed_getblockverdict, node.getblockhash(tip - 2))
        provoke('fee-no-eligible-payee', node.yed_getfeepayee, 1, collateral)
        provoke('quote-out-of-range', node.yed_setquote, 1, 1)
        provoke('no-payout-address', user.yed_setquote, 2_000_000, 1)

        print('errors: yellowback-unhealthy from a storage fault at CommitConnect, and the allow-list')
        stop_node(nodes[5], 5)
        nodes[5] = start_node(5, self.options.tmpdir, self.node_args(5, ['-yellowbacktestfault=storage:commit']))
        self.reconnect(5)
        self.mine(POOLS[0], 2)        # two blocks: node 1 and 5 sit on the rejected block, one would only tie
        info = nodes[5].yed_getinfo()
        assert_equal(info['healthy'], False)
        assert 'CommitConnect' in info['unhealthyReason']
        provoke('yellowback-unhealthy', nodes[5].yed_getstats)
        assert_identifier('yellowback-unhealthy', nodes[5].yed_getprice)
        assert_identifier('yellowback-unhealthy', nodes[5].yed_gethistory, 1, 2)
        # the diagnostic and operator commands still answer (M8)
        assert_equal(nodes[5].yed_getinfo()['enforcing'], False)
        assert_equal(nodes[5].yed_gettag(str(tip))['found'], True)
        assert_equal(nodes[5].yed_decodepayload(payload_hex)['type'], 'mint')
        assert_identifier('no-payout-address', nodes[5].yed_setquote, 2_000_000, 1)   # refused by MINER-2, not by the gate
        stalled = nodes[5].yed_getinfo()['height']                                          # the index stopped at the faulted commit
        assert_equal(nodes[5].yed_getblockverdict(nodes[5].getblockhash(stalled + 1))['blockInvalid'], False)   # parent == the stalled tip
        stop_node(nodes[5], 5)
        nodes[5] = start_node(5, self.options.tmpdir, self.node_args(5, ['-reindex-yellowback']))
        self.reconnect(5)
        wait_yed_healthy(nodes[5])

        documented = {k for k in contract['errors'] if k not in WALLET_ONLY}
        missing = documented - provoked
        assert not missing, 'node-context identifiers never provoked: %s' % sorted(missing)
        print('all %d node-context identifiers provoked' % len(provoked))


if __name__ == '__main__':
    YellowbackRpcContractTest().main()
