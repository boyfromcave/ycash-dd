#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Shared helpers for the Yellowback functional tests (plan §6.0, §7).

- Ycash's own network-upgrade branch IDs (util.py carries Zcash's for
  Blossom/Heartwood/Canopy, plan B6).
- yellowback_node_args(): every upgrade active from height 1 (regtest activates
  none by default, C12; Overwinter and Sapling are already passed by
  start_node) plus -experimentalfeatures -yellowback and the regtest genesis
  arguments once known (C2).
- make_regtest_roster(): k-of-n roster from the operator nodes' own keys,
  registered with addmultisigaddress on every signer (B2, C7).
- fund_genesis_anchor(): creates the anchor UTXO on chain.
- publish_price(): builds with yed_createpricetx, signs on k nodes with
  signrawtransaction (partials merged in one call, F2), broadcasts.
- assert_yed_synced(): sync_blocks already waits for the notifier cycle in
  which the index ran (C13), so this is a sync plus an assertion.
"""

from decimal import Decimal

from .util import (
    assert_equal,
    connect_nodes_bi,
    nuparams,
    start_node,
    stop_node,
    sync_blocks,
    sync_mempools,
    wait_bitcoinds,
)

# ref/ycash/src/consensus/upgrades.cpp
OVERWINTER_BRANCH_ID = 0x5ba81b19
SAPLING_BRANCH_ID = 0x76b809bb
YCASH_BRANCH_ID = 0x374d694f
YCASH_BLOSSOM_BRANCH_ID = 0x8e471bd6
YCASH_HEARTWOOD_BRANCH_ID = 0x66314da3
YCASH_CANOPY_BRANCH_ID = 0x19bd2d2f

# Plan §3.1
MINT_WINDOW = 40
PRICE_MAX_AGE = 48
TOKEN_VALUE = 10000
YELLOWBACK_FEE = 1000
DEFAULT_MINT_EVAL_LAG = 2

# start_node already passes Overwinter and Sapling at height 1.
YCASH_UPGRADE_ARGS = [
    nuparams(YCASH_BRANCH_ID, 1),
    nuparams(YCASH_BLOSSOM_BRANCH_ID, 1),
    nuparams(YCASH_HEARTWOOD_BRANCH_ID, 1),
    nuparams(YCASH_CANOPY_BRANCH_ID, 1),
]


def yellowback_node_args(extra=None, genesis=None, yellowback=True):
    """Arguments for one node. `genesis` is the dict returned by fund_genesis_anchor."""
    args = list(YCASH_UPGRADE_ARGS)
    if yellowback:
        args += ['-experimentalfeatures', '-yellowback']
        if genesis is not None:
            args += genesis_args(genesis)
    if extra:
        args += list(extra)
    return args


def genesis_args(genesis):
    return [
        '-yellowbackstartheight=%d' % genesis['height'],
        '-yellowbackgenesisanchor=%s:%d' % (genesis['txid'], genesis['vout']),
        '-yellowbackgenesisroster=%s' % genesis['script'],
    ]


def assert_yed_synced(nodes):
    """After sync_blocks the index is at the tip on every -yellowback node (C13)."""
    sync_blocks(nodes)
    for node in nodes:
        info = node.yed_getinfo()
        assert_equal(info['healthy'], True, "index unhealthy: " + info['unhealthyReason'])
        assert_equal(info['synced'], True)


def wait_yed_synced(node, timeout=30):
    """Poll until the index tip equals the chain tip (needed after invalidateblock: the framework's
    fullyNotified flag does not cover block disconnects)."""
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        info = node.yed_getinfo()
        assert_equal(info['healthy'], True, "index unhealthy: " + info['unhealthyReason'])
        if info['synced']:
            return
        time.sleep(0.1)
    raise AssertionError("index did not reach the chain tip within %ds" % timeout)


def assert_same_statehash(nodes):
    hashes = [n.yed_getstatehash()['statehash'] for n in nodes]
    assert_equal(hashes, [hashes[0]] * len(hashes))
    return hashes[0]


def node_pubkey(node):
    """A fresh compressed public key from the node's wallet (C7: keys never leave the node)."""
    addr = node.getnewaddress()
    pubkey = node.validateaddress(addr)['pubkey']
    assert_equal(len(pubkey), 66)
    return pubkey


def make_regtest_roster(signer_nodes, k, extra_pubkeys=None):
    """
    Build a k-of-n roster from one key per signer node plus optional extra
    (never-signing) public keys, sorted ascending as §3.3 requires, and run
    addmultisigaddress on every signer so signrawtransaction can solve the
    anchor (B2). Returns {'k', 'pubkeys', 'script', 'address'}.
    """
    pubkeys = [node_pubkey(n) for n in signer_nodes] + list(extra_pubkeys or [])
    pubkeys = sorted(pubkeys, key=lambda h: bytes.fromhex(h))
    address = None
    for n in signer_nodes:
        a = n.addmultisigaddress(k, pubkeys)
        if address is None:
            address = a
        assert_equal(a, address)
    script = signer_nodes[0].validateaddress(address)['hex']
    return {'k': k, 'n': len(pubkeys), 'pubkeys': pubkeys, 'script': script, 'address': address}


def fund_genesis_anchor(funder, roster, amount=Decimal('1.0')):
    """
    Pay `amount` to the roster's P2SH address and mine one block; returns the
    genesis dict for -yellowbackstartheight / -yellowbackgenesisanchor /
    -yellowbackgenesisroster (C2). Caller syncs afterwards.
    """
    txid = funder.sendtoaddress(roster['address'], amount)
    funder.generate(1)
    height = funder.getblockcount()
    raw = funder.decoderawtransaction(funder.gettransaction(txid)['hex'])  # no -txindex needed
    vout = None
    for out in raw['vout']:
        if out['scriptPubKey'].get('addresses') == [roster['address']]:
            vout = out['n']
    assert vout is not None
    return {'txid': txid, 'vout': vout, 'height': height, 'script': roster['script'],
            'address': roster['address'], 'k': roster['k'], 'pubkeys': roster['pubkeys']}


def restart_with_yellowback(test, node_indices, genesis, extra=None, yellowback_indices=None):
    """
    Stop and restart the given nodes with -yellowback and the genesis arguments
    (nodes listed in yellowback_indices=None => all of node_indices get -yellowback;
    others get only the upgrade args), then reconnect them in a chain.
    """
    if yellowback_indices is None:
        yellowback_indices = node_indices
    for i in node_indices:
        stop_node(test.nodes[i], i)
    for i in node_indices:
        per_node_extra = None
        if isinstance(extra, dict):
            per_node_extra = extra.get(i)
        elif extra is not None:
            per_node_extra = extra
        test.nodes[i] = start_node(i, test.options.tmpdir,
                                   yellowback_node_args(per_node_extra, genesis, yellowback=(i in yellowback_indices)))
    for a, b in zip(node_indices, node_indices[1:]):
        connect_nodes_bi(test.nodes, a, b)


def publish_price(builder, signers, price, refill=None, rotate_script=None, prevtxs=None):
    """
    Build the PRICE (or ROTATION) transaction on `builder` with
    yed_createpricetx, collect partial signatures from `signers` in parallel
    style (each signs the unsigned hex), merge them with one
    signrawtransaction call on the first signer, and broadcast. Returns the
    txid. Does not mine or sync.
    """
    if rotate_script is not None:
        built = builder.yed_createpricetx('rotate', refill or '', rotate_script)
    else:
        built = builder.yed_createpricetx(price, refill or '')
    unsigned = built['hex']
    partials = []
    for s in signers:
        if prevtxs is not None:
            r = s.signrawtransaction(unsigned, prevtxs)
        else:
            r = s.signrawtransaction(unsigned)
        partials.append(r['hex'])
    if len(partials) == 1:
        merged = signers[0].signrawtransaction(partials[0], prevtxs if prevtxs is not None else [])
    else:
        merged = signers[0].signrawtransaction(''.join(partials), prevtxs if prevtxs is not None else [])
    assert_equal(merged['complete'], True)
    return builder.sendrawtransaction(merged['hex'])


def sync_all_nodes(nodes):
    sync_blocks(nodes)
    sync_mempools(nodes)
