#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
UPGRADE_VAULT activation on regtest (docs/plans/yellowback-upgrade-plan.md §15.1, §15.2):

- getblockchaininfo lists Vault (6d5b7a31) pending, then active; the consensus branch id moves
  from Sapling to Vault at the activation height. The harness activates Overwinter and Sapling
  only (util.py start_node), so this also shows Vault activating without Ycash, Blossom,
  Heartwood, Canopy or NU5.
- A wallet send signed for the activation block confirms in it (the wallet signs with the new
  branch id); a Python-signed spend bound to the Sapling branch id is accepted before activation
  and rejected after ("old-consensus-branch-id"), the same spend bound to Vault's is accepted.
- A P2SH OP_CHECKSEQUENCEVERIFY script is OP_NOP3 before activation (a block spending it with a
  failing CSV is valid) and enforced after.
- A BIP68 relative-locked input is ignored before activation; after it the spend is refused
  (mempool "non-BIP68-final", block "bad-txns-nonfinal") until the coin's age is reached, a
  time-based lock is "bad-txns-vault-timelock", and a reorg evicts a spend that became early.
- Scripts executing OP_CHECKSETSIG (0xc0) or OP_CHECKSETDORMANT (0xc1) are invalid before
  activation.

    BITCOIND=<ycashd> ../.venv/bin/python -u qa/rpc-tests/vault_upgrade.py --srcdir=<src> --tmpdir=<dir> --portseed=<n>
"""

from io import BytesIO

from test_framework.authproxy import JSONRPCException
from test_framework.mininode import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import (
    CScript,
    OP_1,
    OP_CHECKSEQUENCEVERIFY,
    OP_CHECKSETDORMANT,
    OP_CHECKSETSIG,
    OP_DROP,
    OP_TRUE,
    SIGHASH_ALL,
    SignatureHash,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    SAPLING_BRANCH_ID,
    VAULT_BRANCH_ID,
    assert_equal,
    bytes_to_hex_str,
    connect_nodes_bi,
    hex_str_to_bytes,
    nuparams,
    signing_branch_id,
    start_nodes,
    sync_blocks,
    sync_mempools,
)
from test_framework import yellowback_model as ym
from test_framework import yellowback_util as yu
from test_framework.yellowback_attest import der_encode, ecdsa_sign
from test_framework.yellowback_util import mine_block_raw

# Below regtest's first halving (150): past it, getblocktemplate on a node without the Ycash
# upgrade aborts in GetFoundersRewardAddressAtHeight (a defect at the pin, recorded in the
# workflow's STOCK_BASELINE note), and mine_block_raw uses getblocktemplate.
ACTIVATION = 115
FEE = 10000
SEQUENCE_FINAL = 0xffffffff
TYPE_FLAG = 1 << 22
CSV_DELAY = 3


def tx_from_hex(hex_):
    tx = CTransaction()
    tx.deserialize(BytesIO(hex_str_to_bytes(hex_)))
    return tx


def assert_raises_rpc(substr, fn, *args):
    try:
        fn(*args)
    except JSONRPCException as e:
        assert substr in e.error['message'], 'expected %r in %r' % (substr, e.error['message'])
        return e.error['message']
    raise AssertionError('expected an RPC error containing %r' % substr)


class VaultUpgradeTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 2
        self.setup_clean_chain = True

    def setup_network(self, split=False):
        args = [nuparams(VAULT_BRANCH_ID, ACTIVATION), '-debug=mempool']
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir, extra_args=[args] * self.num_nodes)
        connect_nodes_bi(self.nodes, 0, 1)
        self.is_network_split = False
        self.sync_all()

    # ------------------------------------------------------------------ helpers

    def mine(self, n=1):
        hashes = self.nodes[0].generate(n)
        sync_blocks(self.nodes)
        return hashes

    def mine_to(self, height):
        tip = self.nodes[0].getblockcount()
        assert tip <= height, (tip, height)
        if height > tip:
            self.mine(height - tip)

    def fund(self, address_or_script, amount=1):
        """Pay ``amount`` YEC to an address (or the P2SH of a redeem script) from node 0's wallet;
        returns ``(outpoint, value_zat, scriptPubKey)``. Unconfirmed."""
        node = self.nodes[0]
        if isinstance(address_or_script, CScript):
            address = node.decodescript(bytes_to_hex_str(address_or_script))['p2sh']
        else:
            address = address_or_script
        txid = node.sendtoaddress(address, amount)
        raw = node.getrawtransaction(txid, 1)
        for out in raw['vout']:
            if address in out['scriptPubKey'].get('addresses', []):
                return (COutPoint(int(txid, 16), out['n']), int(out['valueZat']),
                        hex_str_to_bytes(out['scriptPubKey']['hex']))
        raise AssertionError('no output to %s in %s' % (address, txid))

    def fund_bare(self, spk, amount_zat):
        """Create an output with a bare ``spk`` (nonstandard; regtest relays it), spending a P2PKH
        coin of the test key. Unconfirmed."""
        coin = self.fund(self.addr)
        tx = CTransaction()
        tx.vin = [CTxIn(coin[0], b'', SEQUENCE_FINAL)]
        tx.vout = [CTxOut(amount_zat, bytes(spk)), CTxOut(coin[1] - amount_zat - FEE, coin[2])]
        self.sign_p2pkh(tx, 0, coin, signing_branch_id(self.nodes[0]))
        txid = self.nodes[0].sendrawtransaction(bytes_to_hex_str(tx.serialize()))
        return (COutPoint(int(txid, 16), 0), amount_zat, bytes(spk))

    def spend(self, coin, script_sig=b'', sequence=SEQUENCE_FINAL):
        """A transaction spending ``coin`` back to the test key, minus the fee."""
        tx = CTransaction()
        tx.vin = [CTxIn(coin[0], bytes(script_sig), sequence)]
        tx.vout = [CTxOut(coin[1] - FEE, ym.p2pkh_script(ym.hash160(self.pubkey)))]
        return tx

    def sign_p2pkh(self, tx, i, coin, branch_id):
        sighash = SignatureHash(CScript(coin[2]), tx, i, SIGHASH_ALL, coin[1], branch_id)[0]
        r, s = ecdsa_sign(self.secret, sighash)
        tx.vin[i].scriptSig = bytes(CScript([der_encode(r, s) + bytes([SIGHASH_ALL]), self.pubkey]))
        return tx

    def p2pkh_spend(self, coin, branch_id, sequence=SEQUENCE_FINAL):
        tx = self.spend(coin, sequence=sequence)
        return bytes_to_hex_str(self.sign_p2pkh(tx, 0, coin, branch_id).serialize())

    def upgrade(self):
        return self.nodes[0].getblockchaininfo()['upgrades']['6d5b7a31']

    def assert_in_block(self, txid, blockhash):
        assert txid in self.nodes[0].getblock(blockhash)['tx'], '%s not in %s' % (txid, blockhash)

    def raw_block(self, hexes):
        """Mine ``hexes`` in a Python-built block on node 0; returns submitblock's result."""
        result, blockhash = mine_block_raw(self.nodes[0], hexes)
        if result in (None, 'duplicate'):
            sync_blocks(self.nodes)
        return result, blockhash

    def assert_block_rejected(self, hexes, label):
        tip = self.nodes[0].getbestblockhash()
        result, _ = mine_block_raw(self.nodes[0], hexes)
        assert result not in (None, 'duplicate'), '%s: block accepted' % label
        assert_equal(self.nodes[0].getbestblockhash(), tip)
        print('    %s: block rejected (%s)' % (label, result))
        return result

    # ------------------------------------------------------------------ test

    def run_test(self):
        node = self.nodes[0]
        self.mine(105)
        # A key outside the wallet, so the wallet never spends the coins the test funds to it.
        self.secret = bytes(range(1, 33))
        self.pubkey = yu.secret_to_pubkey(self.secret)
        self.addr = yu.pubkey_to_address(self.pubkey)

        print('pending: Vault listed with its height, the next block still Sapling')
        up = self.upgrade()
        assert_equal(up['name'], 'Vault')
        assert_equal(up['activationheight'], ACTIVATION)
        assert_equal(up['status'], 'pending')
        bci = node.getblockchaininfo()
        assert_equal(bci['consensus']['nextblock'], '%08x' % SAPLING_BRANCH_ID)
        assert_equal(signing_branch_id(node), SAPLING_BRANCH_ID)
        # Ycash, Blossom, Heartwood, Canopy and NU5 are not active (not even listed).
        for bid in ('374d694f', '8e471bd6', '66314da3', '19bd2d2f', 'f919a198'):
            assert bid not in bci['upgrades'], bid

        print('fund the coins both halves of the test spend')
        csv_redeem = CScript([CSV_DELAY, OP_CHECKSEQUENCEVERIFY, OP_DROP, OP_TRUE])
        dormant_redeem = CScript([b'\x01' * 32, OP_CHECKSETDORMANT, OP_DROP, OP_TRUE])
        setsig_spk = CScript([b'\x02' * 32, OP_1, OP_CHECKSETSIG])
        coins = {
            'sapling-pre': self.fund(self.addr),
            'sapling-post': self.fund(self.addr),
            'bip68-pre': self.fund(self.addr),
            'csv-pre': self.fund(csv_redeem),
            'csv-post': self.fund(csv_redeem),
            'dormant-pre': self.fund(dormant_redeem),
            'dormant-post': self.fund(dormant_redeem),
        }
        self.mine(1)
        coins['setsig-pre'] = self.fund_bare(setsig_spk, 50000)
        self.mine(1)

        print('before activation')
        print('  a spend signed with the Sapling branch id is accepted and mined')
        txid = node.sendrawtransaction(self.p2pkh_spend(coins['sapling-pre'], SAPLING_BRANCH_ID))
        self.assert_in_block(txid, self.mine(1)[0])

        print('  a relative lock (nSequence = 50) is ignored: accepted at once')
        txid = node.sendrawtransaction(self.p2pkh_spend(coins['bip68-pre'], SAPLING_BRANCH_ID, sequence=50))
        self.assert_in_block(txid, self.mine(1)[0])

        print('  OP_CHECKSEQUENCEVERIFY is OP_NOP3: a failing CSV (input sequence final) is valid in a block')
        csv_nop = self.spend(coins['csv-pre'], CScript([csv_redeem]), SEQUENCE_FINAL)
        csv_nop_hex = bytes_to_hex_str(csv_nop.serialize())
        assert_raises_rpc('NOPx reserved', node.sendrawtransaction, csv_nop_hex)   # policy, as today
        result, blockhash = self.raw_block([csv_nop_hex])
        assert result in (None, 'duplicate'), result
        csv_nop.rehash()
        self.assert_in_block(csv_nop.hash, blockhash)

        print('  OP_CHECKSETSIG (0xc0, bare) and OP_CHECKSETDORMANT (0xc1, P2SH) are invalid')
        setsig_hex = bytes_to_hex_str(self.spend(coins['setsig-pre']).serialize())
        assert_raises_rpc('Opcode missing or not understood', node.sendrawtransaction, setsig_hex)
        self.assert_block_rejected([setsig_hex], '0xc0 before activation')
        dormant_hex = bytes_to_hex_str(self.spend(coins['dormant-pre'], CScript([dormant_redeem])).serialize())
        assert_raises_rpc('Opcode missing or not understood', node.sendrawtransaction, dormant_hex)
        self.assert_block_rejected([dormant_hex], '0xc1 before activation')

        print('activation: the wallet signs for the activation block and the send confirms in it')
        self.mine_to(ACTIVATION - 1)
        assert_equal(self.upgrade()['status'], 'pending')
        bci = node.getblockchaininfo()
        assert_equal(bci['consensus']['chaintip'], '%08x' % SAPLING_BRANCH_ID)
        assert_equal(bci['consensus']['nextblock'], '%08x' % VAULT_BRANCH_ID)
        # The wallet signs with CurrentEpochBranchId(tip + 1), Vault's, valid in the next block.
        send = node.sendtoaddress(self.nodes[1].getnewaddress(), 2)
        blockhash = self.mine(1)[0]
        assert_equal(node.getblock(blockhash)['height'], ACTIVATION)
        self.assert_in_block(send, blockhash)
        assert_equal(self.nodes[1].gettransaction(send)['confirmations'], 1)
        assert_equal(self.upgrade()['status'], 'active')
        bci = node.getblockchaininfo()
        assert_equal(bci['consensus']['chaintip'], '%08x' % VAULT_BRANCH_ID)
        assert_equal(signing_branch_id(node), VAULT_BRANCH_ID)
        # And the wallet keeps working after it.
        send2 = self.nodes[1].sendtoaddress(node.getnewaddress(), 1)
        sync_mempools(self.nodes)
        self.assert_in_block(send2, self.mine(1)[0])

        print('after activation')
        print('  the Sapling branch id is refused; the Vault branch id is accepted')
        old = self.p2pkh_spend(coins['sapling-post'], SAPLING_BRANCH_ID)
        msg = assert_raises_rpc('old-consensus-branch-id', node.sendrawtransaction, old)
        assert '6d5b7a31' in msg and '76b809bb' in msg, msg
        self.assert_block_rejected([old], 'Sapling-bound signature after activation')
        txid = node.sendrawtransaction(self.p2pkh_spend(coins['sapling-post'], VAULT_BRANCH_ID))
        self.assert_in_block(txid, self.mine(1)[0])

        print('  OP_CHECKSEQUENCEVERIFY is enforced')
        csv_fail = bytes_to_hex_str(self.spend(coins['csv-post'], CScript([csv_redeem]), SEQUENCE_FINAL).serialize())
        assert_raises_rpc('Locktime requirement not satisfied', node.sendrawtransaction, csv_fail)
        self.assert_block_rejected([csv_fail], 'failing CSV after activation')
        short = bytes_to_hex_str(self.spend(coins['csv-post'], CScript([csv_redeem]), CSV_DELAY - 1).serialize())
        assert_raises_rpc('Locktime requirement not satisfied', node.sendrawtransaction, short)
        timed = bytes_to_hex_str(self.spend(coins['csv-post'], CScript([csv_redeem]), TYPE_FLAG | CSV_DELAY).serialize())
        assert_raises_rpc('bad-txns-vault-timelock', node.sendrawtransaction, timed)
        ok = bytes_to_hex_str(self.spend(coins['csv-post'], CScript([csv_redeem]), CSV_DELAY).serialize())
        txid = node.sendrawtransaction(ok)
        self.assert_in_block(txid, self.mine(1)[0])

        print('  OP_CHECKSETDORMANT executes (the base checker: not released) and the script succeeds')
        dormant_hex = bytes_to_hex_str(self.spend(coins['dormant-post'], CScript([dormant_redeem])).serialize())
        txid = node.sendrawtransaction(dormant_hex)
        self.assert_in_block(txid, self.mine(1)[0])

        print('  BIP68: a relative lock of 5 waits for the coin\'s age')
        coin = self.fund(self.addr)
        h = node.getblockcount() + 1
        self.assert_in_block('%064x' % coin[0].hash, self.mine(1)[0])
        early = self.p2pkh_spend(coin, VAULT_BRANCH_ID, sequence=5)
        early_txid = tx_from_hex(early)
        early_txid.rehash()
        for tip in (h, h + 1, h + 2, h + 3):
            self.mine_to(tip)
            assert_raises_rpc('non-BIP68-final', node.sendrawtransaction, early)
        self.assert_block_rejected([early], 'block at h+4 with a 5-block lock')
        timed = self.p2pkh_spend(coin, VAULT_BRANCH_ID, sequence=TYPE_FLAG | 1)
        assert_raises_rpc('bad-txns-vault-timelock', node.sendrawtransaction, timed)
        self.assert_block_rejected([timed], 'time-based relative lock')
        self.mine_to(h + 4)
        txid = node.sendrawtransaction(early)
        assert txid in node.getrawmempool()

        print('  reorg: a spend that becomes early again is evicted from the mempool')
        tip = node.getbestblockhash()
        node.invalidateblock(tip)
        assert_equal(node.getblockcount(), h + 3)
        assert txid not in node.getrawmempool(), 'BIP68-early spend survived the reorg'
        node.reconsiderblock(tip)
        assert_equal(node.getbestblockhash(), tip)
        txid = node.sendrawtransaction(early)
        self.assert_in_block(txid, self.mine(1)[0])

        print('  OP_CHECKSETSIG with no set checker still fails')
        assert_raises_rpc('OP_CHECKSETSIG', node.sendrawtransaction,
                          bytes_to_hex_str(self.spend(coins['setsig-pre']).serialize()))

        self.sync_all()
        assert_equal(self.nodes[1].getbestblockhash(), node.getbestblockhash())
        assert node.getblockcount() < 150, node.getblockcount()
        print('vault_upgrade OK')


if __name__ == '__main__':
    VaultUpgradeTest().main()
