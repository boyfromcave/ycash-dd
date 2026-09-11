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
- publish_price(): builds the PRICE transaction here (build_price_tx; the
  node's yed_createpricetx went with the federation in Phase 0), signs on k
  nodes with signrawtransaction (partials merged in one call, F2), broadcasts.
- cosign_and_submit(): completes an owner-signed yed_redeem with the roster
  signatures made here from the signer nodes' keys (yed_cosignredeem and
  yed_submitredeem went with the federation too), then broadcasts.
These v1 helpers are test-only and retire with the v1 flows in Phase 2.
- assert_yed_synced(): sync_blocks already waits for the notifier cycle in
  which the index ran (C13), so this is a sync plus an assertion.
"""

import hashlib
from decimal import Decimal

from .util import (
    assert_equal,
    connect_nodes_bi,
    nuparams,
    start_node,
    stop_node,
    sync_blocks,
    sync_mempools,
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


_PUBKEY_ADDR = {}   # pubkey hex -> the address it was drawn from, so cosign_and_submit can dumpprivkey it


def node_pubkey(node):
    """A fresh compressed public key from the node's wallet."""
    addr = node.getnewaddress()
    pubkey = node.validateaddress(addr)['pubkey']
    assert_equal(len(pubkey), 66)
    _PUBKEY_ADDR[pubkey] = addr
    return pubkey


_B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
_SECP256K1_N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141


def wif_to_secret(wif):
    """Decode a WIF private key to its 32 secret bytes (the framework has no base58 decoder)."""
    n = 0
    for c in wif:
        n = n * 58 + _B58.index(c)
    raw = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    raw = b'\x00' * (len(wif) - len(wif.lstrip('1'))) + raw
    body, check = raw[:-4], raw[-4:]
    assert_equal(hashlib.sha256(hashlib.sha256(body).digest()).digest()[:4], check)
    secret = body[1:]                                   # the version byte
    if len(secret) == 33 and secret[-1] == 1:
        secret = secret[:-1]                            # the compressed-key marker
    assert_equal(len(secret), 32)
    return secret


def _low_s(der):
    """Re-encode a DER signature with a low S (STANDARD_SCRIPT_VERIFY_FLAGS include LOW_S)."""
    assert der[0] == 0x30 and der[2] == 0x02
    rlen = der[3]
    r = der[4:4 + rlen]
    assert der[4 + rlen] == 0x02
    slen = der[5 + rlen]
    s = int.from_bytes(der[6 + rlen:6 + rlen + slen], 'big')
    if s > _SECP256K1_N // 2:
        s = _SECP256K1_N - s
    sb = s.to_bytes((s.bit_length() + 7) // 8, 'big')
    if sb[0] & 0x80:
        sb = b'\x00' + sb
    body = b'\x02' + bytes([len(r)]) + r + b'\x02' + bytes([len(sb)]) + sb
    return b'\x30' + bytes([len(body)]) + body


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


def build_price_tx(node, price, refill=None, rotate_script=None):
    """
    Hand-build the unsigned PRICE transaction (or, with rotate_script, the ROTATION) that spends the
    current anchor into a new anchor of the same script (or the new roster's P2SH) plus the OP_RETURN
    price. The anchor is the index's, followed through the node's mempool when an earlier price is
    still unconfirmed (C11); an optional confirmed refill 'txid:n' is absorbed whole (C6). Returns
    {hex, anchor{txid, vout, valueZat, unconfirmed}, refillValueZat, newAnchorValueZat, feeZat}.
    """
    from .mininode import COutPoint, CTransaction, CTxIn, CTxOut
    from .script import CScript, OP_RETURN
    from .util import bytes_to_hex_str, hex_str_to_bytes
    a = node.yed_getinfo()['anchor']
    assert a['valid'], 'anchor custody is broken'
    txid, vout, value = a['txid'], a['vout'], a['valueZat']
    spk = hex_str_to_bytes(node.validateaddress(a['address'])['scriptPubKey'])
    unconfirmed = False
    for _ in range(100):
        spender = None
        for mtxid in node.getrawmempool():
            raw = node.getrawtransaction(mtxid, 1)
            if any(v.get('txid') == txid and v.get('vout') == vout for v in raw['vin']):
                spender = raw
                break
        if spender is None:
            break
        txid, vout, value = spender['txid'], 0, int(spender['vout'][0]['value'] * 100000000)
        spk = hex_str_to_bytes(spender['vout'][0]['scriptPubKey']['hex'])
        unconfirmed = True
    tx = CTransaction()
    tx.vin.append(CTxIn(COutPoint(int(txid, 16), vout)))
    refill_value = 0
    if refill:
        rtxid, rn = refill.split(':')
        out = node.gettxout(rtxid, int(rn))
        assert out is not None, 'refill outpoint is not a confirmed unspent output'
        refill_value = int(out['value'] * 100000000)
        tx.vin.append(CTxIn(COutPoint(int(rtxid, 16), int(rn))))
    new_value = value + refill_value - YELLOWBACK_FEE
    assert new_value > 0, 'anchor value does not cover the fee'
    if rotate_script is not None:
        p2sh = node.decodescript(rotate_script)['p2sh']
        spk = hex_str_to_bytes(node.validateaddress(p2sh)['scriptPubKey'])
    tx.vout.append(CTxOut(new_value, CScript(spk)))
    if rotate_script is None:
        payload = bytes([0x59, 0x42, 0x01, 0x10]) + int(price).to_bytes(8, 'little')
        tx.vout.append(CTxOut(0, CScript([OP_RETURN, payload])))
    tx.nExpiryHeight = node.getblockcount() + 1 + MINT_WINDOW
    return {'hex': bytes_to_hex_str(tx.serialize()),
            'anchor': {'txid': txid, 'vout': vout, 'valueZat': value, 'unconfirmed': unconfirmed},
            'refillValueZat': refill_value, 'newAnchorValueZat': new_value, 'feeZat': YELLOWBACK_FEE}


def publish_price(builder, signers, price, refill=None, rotate_script=None, prevtxs=None):
    """
    Build the PRICE (or ROTATION) transaction with build_price_tx, collect
    partial signatures from `signers` in parallel style (each signs the
    unsigned hex), merge them with one signrawtransaction call on the first
    signer, and broadcast from `builder`. Returns the txid. Does not mine or sync.
    """
    built = build_price_tx(builder, price, refill=refill, rotate_script=rotate_script)
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


def build_mint_tx(node, cents, tier, lock_height, eval_height, collateral_zat, owner_pubkey=None, roster_script_hex=None):
    """
    Hand-build a MINT transaction (the wallet only builds valid ones): vault
    P2SH at vout 0 with `collateral_zat`, token P2PKH to the owner at vout 1,
    the MINT payload at vout 2, YEC change at vout 3. Returns the signed hex
    and the owner pubkey. Uses the node for every script derivation (no
    Python crypto, G2).
    """
    from .mininode import COutPoint, CTransaction, CTxIn, CTxOut
    from .script import CScript, OP_NOP2, OP_CHECKSIGVERIFY, OP_DROP, OP_RETURN
    OP_CHECKLOCKTIMEVERIFY = OP_NOP2
    from .util import bytes_to_hex_str, hex_str_to_bytes
    if owner_pubkey is None:
        owner_pubkey = node.validateaddress(node.getnewaddress())['pubkey']
    if roster_script_hex is None:
        roster_script_hex = node.yed_getroster()['scriptHex']
    owner = hex_str_to_bytes(owner_pubkey)
    # Concatenate raw bytes: CScript + CScript would push the roster as data (PUSHDATA1), not append it.
    vault = CScript(bytes(CScript([lock_height, OP_CHECKLOCKTIMEVERIFY, OP_DROP, owner, OP_CHECKSIGVERIFY])) + hex_str_to_bytes(roster_script_hex))
    p2sh = node.decodescript(bytes_to_hex_str(bytes(vault)))['p2sh']
    vault_spk = hex_str_to_bytes(node.validateaddress(p2sh)['scriptPubKey'])
    token_spk = hex_str_to_bytes(node.validateaddress(node.getnewaddress())['scriptPubKey'])  # any script; the owner key is what matters
    payload = (bytes([0x59, 0x42, 0x01, 0x01, tier]) + int(cents).to_bytes(4, 'little') +
               int(lock_height).to_bytes(4, 'little') + int(eval_height).to_bytes(4, 'little') + owner)
    needed = collateral_zat + TOKEN_VALUE + YELLOWBACK_FEE
    utxos = sorted([u for u in node.listunspent(1)], key=lambda u: u['amount'])
    tx = CTransaction()
    total = 0
    for u in utxos:
        tx.vin.append(CTxIn(COutPoint(int(u['txid'], 16), u['vout'])))
        total += int(u['amount'] * 100000000)
        if total >= needed:
            break
    assert total >= needed, 'insufficient YEC to hand-build the mint'
    tx.vout.append(CTxOut(collateral_zat, CScript(vault_spk)))
    tx.vout.append(CTxOut(TOKEN_VALUE, CScript(token_spk)))
    tx.vout.append(CTxOut(0, CScript([OP_RETURN, payload])))
    if total - needed > 0:
        change_spk = hex_str_to_bytes(node.validateaddress(node.getnewaddress())['scriptPubKey'])
        tx.vout.append(CTxOut(total - needed, CScript(change_spk)))
    tx.nExpiryHeight = node.getblockcount() + 40
    signed = node.signrawtransaction(bytes_to_hex_str(tx.serialize()))
    assert_equal(signed['complete'], True)
    return signed['hex'], owner_pubkey


def cosign_and_submit(owner_node, signer_nodes, redeem_hex):
    """
    Complete the owner-signed redemption from yed_redeem with the roster
    signatures of `signer_nodes` (each node's roster key is read with
    dumpprivkey and its signature made here, placed in roster order), then
    broadcast it from the owner node. Returns the txid.
    """
    from io import BytesIO
    from .key import CECKey
    from .mininode import CTransaction
    from .script import CScript, SIGHASH_ALL, SignatureHash
    from .util import bytes_to_hex_str, hex_str_to_bytes
    tx = CTransaction()
    tx.deserialize(BytesIO(hex_str_to_bytes(redeem_hex)))
    pushes = list(CScript(tx.vin[0].scriptSig))        # OP_0 <ownerSig> <vaultScript>; OP_0 iterates as b''
    assert_equal(len(pushes), 3)
    owner_sig, vault_script = pushes[1], pushes[2]
    keys = [p for p in CScript(vault_script) if isinstance(p, bytes) and len(p) == 33]
    roster_keys = keys[1:]                              # keys[0] is the owner
    vault = owner_node.yed_getvault('%064x' % tx.vin[0].prevout.hash)
    sighash = SignatureHash(CScript(vault_script), tx, 0, SIGHASH_ALL, vault['collateralZat'], YCASH_CANOPY_BRANCH_ID)[0]
    sigs = {}
    for n in signer_nodes:
        for i, pub in enumerate(roster_keys):
            addr = _PUBKEY_ADDR.get(bytes_to_hex_str(pub))
            if addr is None or i in sigs or not n.validateaddress(addr)['ismine']:
                continue
            k = CECKey()
            k.set_secretbytes(wif_to_secret(n.dumpprivkey(addr)))
            k.set_compressed(True)
            assert_equal(bytes_to_hex_str(k.get_pubkey()), bytes_to_hex_str(pub))
            sigs[i] = _low_s(k.sign(sighash)) + bytes([SIGHASH_ALL])
            break
    assert sigs, 'no signer node holds a roster key of this vault'
    tx.vin[0].scriptSig = CScript([b''] + [sigs[i] for i in sorted(sigs)] + [owner_sig, vault_script])
    return owner_node.sendrawtransaction(bytes_to_hex_str(tx.serialize()))
