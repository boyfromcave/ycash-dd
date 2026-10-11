#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Post-quantum DoS measurement (quantum plan §4.9; results in doc/yellowback-pq-dos.md).

Not a CI test: a measurement harness built on the functional-test framework. One miner (node 0)
builds near-full 2 MB regtest blocks; seven fresh measuring nodes that never saw the transactions
(no mempool, no signature cache) receive each block by ``submitblock``: nodes 1-4 run ``-par=1``
(serial script checks), nodes 5-7 the default ``-par`` (one thread per core). The machine is
shared, so the summary takes the minimum over the repetitions.
Each measuring node reports its ``-debug=bench`` lines (``Verify N txins``, ``Connect block``),
the ``submitblock`` wall time and the process CPU time spent in the call.

Scenarios (``--scenarios``, comma-separated; default all):
  p2pkh    a block of P2PKH ECDSA inputs (the baseline)
  slh      a block of SLH-DSA-SHA2-128s TX_PQPKH spends
  falcon   a block of FN-DSA-512 TX_PQPKH spends (-pqfalconheight)
  invalid  garbage PQ signatures under a correct key hash: RPC accept/reject cost and P2P ban score
  staged   the R-B1 residual: a bare non-standard output carrying an SLH-DSA key and a (necessarily
           wrong) signature, ``... OP_CHECKPQSIG OP_NOT``, spent by ~41-byte inputs; a full block
           of such spends (the outputs are mined first, ~194 blocks of 2 MB)
  stagedf  the same with Falcon
  legacy   the legacy reference: a bare output running 67 ECDSA checks (200 ops), spent the same way
  sapling  Sapling spends and outputs (µs per spend / per output; proofs are checked in
           ContextualCheckBlock, so only the submitblock wall and CPU figures include them)
  flood    the mempool under a flood of ~8 KB SLH-DSA transactions at the relay floor

Signing uses the tree's own vendored C (src/crypto/pq) through ``pqdos_shim.c``, compiled here
with ``$CC`` (default ``cc``) at -O3; the framework's Python verifier checks a sample.

    BITCOIND=<ycashd> .venv/bin/python -u contrib/yellowback/pq-dos/pq_dos.py \\
        --srcdir=<src> --tmpdir=<dir> --portseed=<n> [--scenarios=slh,p2pkh] [--out=<json>]
"""

import ctypes
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(TREE, 'qa', 'rpc-tests'))

from io import BytesIO  # noqa: E402
from test_framework.authproxy import JSONRPCException  # noqa: E402
from test_framework.mininode import CBlock, CTransaction  # noqa: E402
from test_framework.test_framework import BitcoinTestFramework  # noqa: E402
from test_framework.util import (  # noqa: E402
    VAULT_BRANCH_ID,
    assert_equal,
    bitcoind_processes,
    initialize_chain_clean,
    nuparams,
    p2p_port,
    signing_branch_id,
    start_node,
    stop_node,
)
from test_framework import pq  # noqa: E402
from test_framework import vault as v  # noqa: E402
from test_framework.yellowback_util import YCASH_UPGRADE_ARGS, wif_to_secret  # noqa: E402

ACTIVATION = 105           # UPGRADE_VAULT and pqFalconHeight
COIN = 100000000
FEE_RATE = 1000            # zat/KB for block-filling transactions (10x the relay floor)
RELAY_FLOOR = 100          # DEFAULT_MIN_RELAY_TX_FEE, zat/KB
BLOCK_TARGET = 1990000     # bytes of transactions per measured block (MAX_BLOCK_SIZE 2,000,000)
MEASURE = [1, 5, 2, 6, 3, 7, 4]   # submission order, interleaving -par=1 and the default
PAR = {1: 1, 2: 1, 3: 1, 4: 1, 5: 0, 6: 0, 7: 0}
FALCON_SOURCES = ['codec.c', 'common.c', 'fft.c', 'fips202.c', 'fpr.c', 'keygen.c', 'rng.c',
                  'sign.c', 'vrfy.c', 'ycash_falcon_sign.c', 'ycash_falcon_verify.c']
OP_NOT = 0x91
OP_CHECKPQSIG = 0xc2


def build_shim(tmpdir):
    pqdir = os.path.join(TREE, 'src', 'crypto', 'pq')
    out = os.path.join(tmpdir, 'libpqdos' + ('.dylib' if sys.platform == 'darwin' else '.so'))
    srcs = [os.path.join(HERE, 'pqdos_shim.c')]
    srcs += [os.path.join(pqdir, 'slhdsa', f) for f in sorted(os.listdir(os.path.join(pqdir, 'slhdsa')))
             if f.endswith('.c')]
    srcs += [os.path.join(pqdir, 'falcon', f) for f in FALCON_SOURCES]
    cc = os.environ.get('CC', 'cc').split()
    subprocess.check_call(cc + ['-O3', '-shared', '-fPIC', '-I' + pqdir, '-o', out] + srcs)
    lib = ctypes.CDLL(out)
    lib.pqdos_slh_sign.restype = ctypes.c_size_t
    lib.pqdos_bench_verify.restype = ctypes.c_double
    return lib


class Signer:
    """One PQ key (scheme 1 or 2) over the shim; ``sign`` releases the GIL (ctypes)."""

    def __init__(self, lib, scheme, seed48):
        self.lib, self.scheme = lib, scheme
        if scheme == pq.SCHEME_SLH_DSA_SHA2_128S:
            pk, sk = ctypes.create_string_buffer(32), ctypes.create_string_buffer(64)
            assert_equal(lib.pqdos_slh_keygen(pk, sk, seed48), 0)
        else:
            pk, sk = ctypes.create_string_buffer(897), ctypes.create_string_buffer(1281)
            assert_equal(lib.ycash_falcon512_keygen_from_seed(pk, sk, seed48, ctypes.c_size_t(48)), 0)
        self.pk, self.sk = pk.raw, sk.raw
        self.spk = pq.pqpkh_script(scheme, pq.key_hash(scheme, self.pk))

    def sign(self, msg32):
        if self.scheme == pq.SCHEME_SLH_DSA_SHA2_128S:
            sig = ctypes.create_string_buffer(7856)
            assert_equal(self.lib.pqdos_slh_sign(sig, msg32, self.sk), 7856)
        else:
            sig = ctypes.create_string_buffer(666)
            assert_equal(self.lib.ycash_falcon512_sign(sig, msg32, ctypes.c_size_t(32), self.sk,
                                                       os.urandom(40), os.urandom(48)), 0)
        return sig.raw

    def verify(self, sig, msg32):
        return self.lib.pqdos_verify(self.scheme, self.pk, sig, msg32) == 1


def cpu_seconds(pid):
    """Cumulative user+system CPU of ``pid`` (all threads), from ps."""
    out = subprocess.check_output(['ps', '-o', 'time=', '-p', str(pid)]).decode().strip()
    parts = out.replace('-', ':').split(':')
    secs = 0.0
    for p in parts:
        secs = secs * 60 + float(p)
    return secs


def rss_kb(pid):
    return int(subprocess.check_output(['ps', '-o', 'rss=', '-p', str(pid)]).decode().strip())


class RssSampler:
    """Peak RSS of ``pid`` sampled every 50 ms in a thread (``ps``), until ``stop``."""

    def __init__(self, pid):
        import threading
        self.pid, self.peak, self.done = pid, rss_kb(pid), False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self.done:
            try:
                self.peak = max(self.peak, rss_kb(self.pid))
            except Exception:
                pass
            time.sleep(0.05)

    def stop(self):
        self.done = True
        self.t.join()
        return max(self.peak, rss_kb(self.pid))


def staged_script(pk, sig_with_hashtype, scheme):
    """``<sig chunks> <s> <pk chunks> <p> <keyHash> <scheme> OP_CHECKPQSIG OP_NOT``: one
    executed OP_CHECKPQSIG (R-B1) whose signature cannot verify (it is fixed before the spending
    transaction exists), so it always runs the full verification and pushes false; OP_NOT makes
    the spend valid with an empty scriptSig."""
    body = pq.pq_scriptsig(pk, sig_with_hashtype)
    return body + bytes([32]) + pq.key_hash(scheme, pk) + bytes([0x50 + scheme, OP_CHECKPQSIG, OP_NOT])


class PQDoS(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 8
        self.setup_clean_chain = True

    def add_options(self, parser):
        parser.add_option('--scenarios', dest='scenarios',
                          default='p2pkh,slh,falcon,invalid,sapling,flood,stagedf,staged')
        parser.add_option('--out', dest='out', default='')
        parser.add_option('--staged-spends', dest='staged_spends', type='int', default=0,
                          help='staged spends in the measured block (0 = fill 2 MB)')
        parser.add_option('--flood', dest='flood', type='int', default=400)
        parser.add_option('--threads', dest='threads', type='int', default=4)

    def setup_chain(self):
        initialize_chain_clean(self.options.tmpdir, self.num_nodes)

    def base_args(self):
        return list(YCASH_UPGRADE_ARGS) + [nuparams(VAULT_BRANCH_ID, ACTIVATION),
                                           '-pqfalconheight=%d' % ACTIVATION, '-debug=bench',
                                           '-mempooltxcostlimit=1000000000', '-rpcservertimeout=600']

    def node_args(self, i, extra=()):
        a = self.base_args()
        if i in PAR:
            a.append('-par=%d' % PAR[i])
        return a + list(extra)

    def setup_network(self, split=False):
        self.nodes = [start_node(i, self.options.tmpdir, self.node_args(i), timewait=900)
                      for i in range(self.num_nodes)]
        self.is_network_split = True     # never connected: blocks move by submitblock only

    def restart(self, i, extra=()):
        stop_node(self.nodes[i], i)
        self.nodes[i] = start_node(i, self.options.tmpdir, self.node_args(i, extra), timewait=900)

    def rev_bytes(self, i):
        d = os.path.join(self.options.tmpdir, 'node%d' % i, 'regtest', 'blocks')
        return sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d) if f.startswith('rev'))

    def log_path(self, i):
        return os.path.join(self.options.tmpdir, 'node%d' % i, 'regtest', 'debug.log')

    # ------------------------------------------------------------------ chain helpers

    def coinbase(self):
        """A mature node-0 coinbase output: (txid, value_zat, secret, spk)."""
        n = self.n0
        while True:
            h = self.next_cb
            self.next_cb += 1
            assert h <= n.getblockcount() - 100, 'no mature coinbase left'
            cb = n.getblock(n.getblockhash(h), 2)['tx'][0]
            out = cb['vout'][0]
            if out['scriptPubKey'].get('type') != 'pubkeyhash':
                continue
            secret = wif_to_secret(n.dumpprivkey(out['scriptPubKey']['addresses'][0]))
            return cb['txid'], int(out['valueZat']), secret, bytes.fromhex(out['scriptPubKey']['hex'])

    def fanout(self, spk, count, value, per_tx=1000):
        """``count`` outputs of ``value`` zat paying ``spk``, from coinbases, mined. Returns the
        list of (txid, n)."""
        n = self.n0
        outs = []
        left = count
        while left:
            k = min(left, per_tx)
            txid, amt, secret, cbspk = self.coinbase()
            size = 200 + k * (9 + len(spk) + 3)
            fee = size * FEE_RATE // 1000 + 1000
            change = amt - k * value - fee
            assert change > 0, 'coinbase too small for the fanout'
            tx = v.make_tx([(txid, 0, 0xffffffff)], [(value, spk)] * k + [(change, cbspk)])
            v.sign_p2pkh_input(tx, 0, secret, amt, signing_branch_id(n))
            t = n.sendrawtransaction(v.tx_hex(tx))
            outs += [(t, j) for j in range(k)]
            left -= k
        self.mine()
        return outs

    def mine(self):
        h = self.n0.generate(1)[0]
        assert_equal(self.n0.getrawmempool(), [])
        return h

    def sync_measurers(self, upto):
        """Submit every node-0 block up to height ``upto`` to the measuring nodes."""
        for i in sorted(PAR):
            node = self.nodes[i]
            for hgt in range(node.getblockcount() + 1, upto + 1):
                r = node.submitblock(self.n0.getblock(self.n0.getblockhash(hgt), 0))
                assert r is None, r
            assert_equal(node.getbestblockhash(), self.n0.getblockhash(upto))

    def custom_block(self, txs):
        """A block on node 0's tip carrying exactly ``txs`` (after getblocktemplate's coinbase),
        solved in Python: for blocks the stock template refuses (the 20-sigop PQ policy count
        caps a template at 1,000 PQ checks; consensus counts none). Returns the hex."""
        n = self.n0
        gbt = n.getblocktemplate()
        cb = CTransaction()
        cb.deserialize(BytesIO(bytes.fromhex(gbt['coinbasetxn']['data'])))
        cb.vout[0].nValue -= sum(int(t.get('fee', 0)) for t in gbt.get('transactions', []))
        cb.calc_sha256()
        block = CBlock()
        block.nVersion = gbt['version']
        block.hashPrevBlock = int(gbt['previousblockhash'], 16)
        block.hashFinalSaplingRoot = int(gbt.get('lightclientroothash', gbt['finalsaplingroothash']), 16)
        block.nTime = gbt['curtime']
        block.nBits = int(gbt['bits'], 16)
        for tx in txs:
            tx.calc_sha256()
        block.vtx = [cb] + list(txs)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        return block.serialize().hex()

    def mine_measured(self, label, meta, txs=None):
        """Mine node 0's mempool (or, with ``txs``, a hand-built block of exactly those) into one
        block and measure it on every measuring node."""
        n = self.n0
        while label in self.results:      # a scenario listed twice
            label += "'"
        mp = n.getmempoolinfo()
        custom = self.custom_block(txs) if txs is not None else None
        off0 = os.path.getsize(self.log_path(0))
        cpu0 = cpu_seconds(bitcoind_processes[0].pid)
        t0 = time.time()
        if custom is None:
            bh = n.generate(1)[0]
        else:
            r = n.submitblock(custom)
            assert r is None, r
            bh = n.getbestblockhash()
        gen_wall = time.time() - t0
        gen_cpu = cpu_seconds(bitcoind_processes[0].pid) - cpu0
        assert_equal(n.getrawmempool(), [])
        blk = n.getblock(bh)
        miner_bench = self.parse_bench(0, off0)
        hexblk = n.getblock(bh, 0)
        self.sync_measurers(blk['height'] - 1)
        runs = []
        for i in MEASURE:
            node = self.nodes[i]
            off = os.path.getsize(self.log_path(i))
            pid = bitcoind_processes[i].pid
            rev0 = self.rev_bytes(i)
            rss0 = rss_kb(pid)
            sampler = RssSampler(pid)
            c0 = cpu_seconds(pid)
            t0 = time.time()
            r = node.submitblock(hexblk)
            wall = time.time() - t0
            c1 = cpu_seconds(pid)
            peak = sampler.stop()
            assert r is None, r
            assert_equal(node.getbestblockhash(), bh)
            b = self.parse_bench(i, off)
            b.update({'node': i, 'par': PAR[i], 'submit_wall_s': round(wall, 4), 'submit_cpu_s': round(c1 - c0, 3),
                      'rss_kb_before': rss0, 'rss_kb_peak': peak, 'undo_bytes': self.rev_bytes(i) - rev0})
            runs.append(b)
            print('  %-8s node%d par=%d  verify %8.1f ms  connect %8.1f ms  submit wall %7.3f s cpu %7.2f s'
                  '  rss %d->%d MB  undo %.1f MB' % (
                      label, i, PAR[i], b.get('verify_ms', -1), b.get('connect_block_ms', -1), wall, c1 - c0,
                      rss0 // 1024, peak // 1024, b['undo_bytes'] / 1e6))
        res = dict(meta)
        res.update({'label': label, 'height': blk['height'], 'block_bytes': blk['size'], 'ntx': len(blk['tx']),
                    'mempool_before': mp, 'hand_built': custom is not None, 'miner': {'generate_wall_s': round(gen_wall, 3),
                                                    'generate_cpu_s': round(gen_cpu, 3), 'bench': miner_bench},
                    'runs': runs})
        self.results[label] = res
        self.save()
        return bh

    BENCH = [
        ('verify', re.compile(r'- Verify (\d+) txins: ([\d.]+)ms')),
        ('connect_txs', re.compile(r'- Connect (\d+) transactions: ([\d.]+)ms')),
        ('connect_total', re.compile(r'- Connect total: ([\d.]+)ms')),
        ('index', re.compile(r'- Index writing: ([\d.]+)ms')),
        ('connect_block', re.compile(r'^.*?- Connect block: ([\d.]+)ms')),
    ]

    def parse_bench(self, i, off):
        """The last bench figures written after ``off`` (all ConnectBlock calls are listed)."""
        with open(self.log_path(i), 'rb') as f:
            f.seek(off)
            text = f.read().decode('utf-8', 'replace')
        out = {'verify_all': []}
        for line in text.splitlines():
            for key, rx in self.BENCH:
                m = rx.search(line)
                if not m:
                    continue
                if key == 'verify':
                    out['verify_txins'] = int(m.group(1))
                    out['verify_ms'] = float(m.group(2))
                    out['verify_all'].append(float(m.group(2)))
                elif key == 'connect_txs':
                    out['connect_txs_ms'] = float(m.group(2))
                else:
                    out[key + '_ms'] = float(m.group(1))
        return out

    def save(self):
        if self.options.out:
            with open(self.options.out, 'w') as f:
                json.dump(self.results, f, indent=1, sort_keys=True, default=str)

    # ------------------------------------------------------------------ PQ spend builders

    def pq_spend_txs(self, signer, outs, value, per_tx, fee_rate=FEE_RATE, dest=None, bad=False):
        """Transactions spending ``outs`` (TX_PQPKH of ``signer``) ``per_tx`` at a time to ``dest``;
        ``bad`` replaces each signature with random bytes (correct key hash and sizes)."""
        n = self.n0
        dest = dest or self.dest_spk
        branch = signing_branch_id(n)
        txs, jobs = [], []
        for k in range(0, len(outs), per_tx):
            group = outs[k:k + per_tx]
            in_size = len(group) * (41 + len(pq.pq_scriptsig(signer.pk, bytes(pq.sig_size(signer.scheme) + 1))) + 3)
            fee = (in_size + 100) * fee_rate // 1000 + 1
            tx = v.make_tx([(t, j, 0xffffffff) for t, j in group], [(len(group) * value - fee, dest)])
            txs.append(tx)
            for i in range(len(group)):
                jobs.append((tx, i))

        def work(job):
            tx, i = job
            msg = pq.pq_sighash(tx, i, signer.spk, value, branch)
            sig = os.urandom(pq.sig_size(signer.scheme)) if bad else signer.sign(msg)
            return job, msg, sig

        sample = None
        with ThreadPoolExecutor(self.options.threads) as ex:
            for (tx, i), msg, sig in ex.map(work, jobs):
                tx.vin[i].scriptSig = pq.pq_scriptsig(signer.pk, sig + bytes([pq.SIGHASH_ALL]))
                if sample is None:
                    sample = (msg, sig)
        # the framework's independent Python verifier on one signature per batch (SLH in Python is slow)
        if not bad:
            assert pq.verify(signer.scheme, signer.pk, sample[1], sample[0])
        return txs

    def send_all(self, txs):
        times = []
        for tx in txs:
            h = v.tx_hex(tx)
            t0 = time.time()
            self.n0.sendrawtransaction(h)
            times.append(time.time() - t0)
        return times

    # ------------------------------------------------------------------ scenarios

    def sc_p2pkh(self):
        print('P2PKH: a block of ECDSA inputs')
        n = self.n0
        addr = n.getnewaddress()
        spk = v.node_spk(n, addr)
        per_tx = 600
        n_in = (BLOCK_TARGET // (per_tx * 148 + 60)) * per_tx
        value = 20000
        outs = self.fanout(spk, n_in, value)
        txs = []
        for k in range(0, n_in, per_tx):
            group = outs[k:k + per_tx]
            fee = (per_tx * 148 + 60) * FEE_RATE // 1000
            tx = v.make_tx([(t, j, 0xffffffff) for t, j in group], [(per_tx * value - fee, self.dest_spk)])
            txs.append(v.tx_from_hex(n.signrawtransaction(v.tx_hex(tx))['hex']))
        atmp = self.send_all(txs)
        self.mine_measured('p2pkh', {'inputs': n_in, 'verifies': n_in, 'scheme': 'ECDSA',
                                     'atmp_s_per_input': sum(atmp) / n_in})

    def sc_pq(self, label, scheme, per_tx, seed, hand_built=False):
        signer = Signer(self.lib, scheme, seed)
        in_bytes = 41 + 3 + len(pq.pq_scriptsig(signer.pk, bytes(pq.sig_size(scheme) + 1)))
        n_in = (BLOCK_TARGET // (per_tx * in_bytes + 60)) * per_tx
        print('%s: a block of %d TX_PQPKH spends (%d B per input)' % (label, n_in, in_bytes))
        value = 200000
        outs = self.fanout(signer.spk, n_in, value)
        t0 = time.time()
        txs = self.pq_spend_txs(signer, outs, value, per_tx)
        sign_s = time.time() - t0
        atmp = self.send_all(txs)
        self.mine_measured(label, {'inputs': n_in, 'verifies': n_in, 'input_bytes': in_bytes,
                                   'scheme': label, 'sign_wall_s': round(sign_s, 2),
                                   'atmp_s_per_input': sum(atmp) / n_in},
                           txs=txs if hand_built else None)
        return signer

    def sc_invalid(self):
        print('INVALID: garbage PQ signatures (correct key hash, exact sizes)')
        n = self.n0
        res = {}
        for label, scheme, seed in [('slh', 1, bytes(range(1, 49))), ('falcon', 2, bytes(range(2, 50)))]:
            signer = Signer(self.lib, scheme, seed)
            k = 30
            value = 200000
            outs = self.fanout(signer.spk, 2 * k, value)
            good = self.pq_spend_txs(signer, outs[:k], value, 1, fee_rate=FEE_RATE)
            bad = self.pq_spend_txs(signer, outs[k:], value, 1, fee_rate=FEE_RATE, bad=True)
            if scheme == 2:
                # a well-formed but wrong Falcon signature (another message) runs the full check;
                # random bytes fail the decoder in microseconds
                wf = []
                for tx in self.pq_spend_txs(signer, outs[k:], value, 1, fee_rate=FEE_RATE):
                    other = signer.sign(os.urandom(32))
                    tx.vin[0].scriptSig = pq.pq_scriptsig(signer.pk, other + bytes([pq.SIGHASH_ALL]))
                    wf.append(tx)
            tg, tb, twf = [], [], []
            for tx in bad:
                t0 = time.time()
                try:
                    n.sendrawtransaction(v.tx_hex(tx))
                    raise AssertionError('a garbage PQ signature was accepted')
                except JSONRPCException as e:
                    msg = e.error['message']
                tb.append(time.time() - t0)
            if scheme == 2:
                for tx in wf:
                    t0 = time.time()
                    try:
                        n.sendrawtransaction(v.tx_hex(tx))
                        raise AssertionError('a wrong Falcon signature was accepted')
                    except JSONRPCException:
                        pass
                    twf.append(time.time() - t0)
            for tx in good:
                t0 = time.time()
                n.sendrawtransaction(v.tx_hex(tx))
                tg.append(time.time() - t0)
            self.mine()
            # the shim's per-verify figures for the same key: valid, wrong message, random bytes
            msg32 = os.urandom(32)
            sig = signer.sign(msg32)
            nv = ctypes.c_int()
            it = 200 if scheme == 1 else 4000
            micro = {}
            for name, s, m in [('valid', sig, msg32), ('wrong_message', sig, os.urandom(32)),
                               ('random_bytes', os.urandom(len(sig)), msg32)]:
                best = min(self.lib.pqdos_bench_verify(scheme, signer.pk, s, m, it, ctypes.byref(nv))
                           for _ in range(3))
                micro[name + '_us'] = round(best, 2)
            res[label] = {'reject_reason': msg, 'atmp_valid_ms_median': self.median(tg) * 1e3,
                          'atmp_garbage_ms_median': self.median(tb) * 1e3,
                          'atmp_wellformed_wrong_ms_median': self.median(twf) * 1e3 if twf else None,
                          'micro': micro}
            print('  %s: accept valid %.2f ms, reject garbage %.2f ms%s; micro %s; reason %r' % (
                label, res[label]['atmp_valid_ms_median'], res[label]['atmp_garbage_ms_median'],
                (', reject well-formed wrong %.2f ms' % res[label]['atmp_wellformed_wrong_ms_median']) if twf else '',
                micro, msg))
        # single-input P2PKH baseline for the RPC accept time
        addr = n.getnewaddress()
        p_outs = self.fanout(v.node_spk(n, addr), 30, 200000)
        tp = []
        for t, j in p_outs:
            h = n.signrawtransaction(v.tx_hex(v.make_tx([(t, j, 0xffffffff)], [(199000, self.dest_spk)])))['hex']
            t0 = time.time()
            n.sendrawtransaction(h)
            tp.append(time.time() - t0)
        self.mine()
        res['p2pkh_atmp_valid_ms_median'] = self.median(tp) * 1e3
        print('  p2pkh: accept valid %.2f ms' % res['p2pkh_atmp_valid_ms_median'])
        res['p2p'] = self.p2p_ban()
        self.results['invalid'] = res
        self.save()

    def p2p_ban(self):
        """A mininode relays one transaction with a garbage SLH-DSA signature: ban score."""
        from test_framework.mininode import NodeConn, NetworkThread, msg_tx
        sys.path.insert(0, os.path.join(TREE, 'qa', 'rpc-tests'))
        from tx_expiry_helper import TestNode
        n = self.n0
        signer = Signer(self.lib, 1, bytes(range(3, 51)))
        outs = self.fanout(signer.spk, 1, 200000)
        bad = self.pq_spend_txs(signer, outs, 200000, 1, bad=True)[0]
        off = os.path.getsize(self.log_path(0))
        node = TestNode()
        conn = NodeConn('127.0.0.1', p2p_port(0), n, node, 'regtest', 270013)
        node.add_connection(conn)
        NetworkThread().start()
        node.wait_for_verack()
        before = [p for p in n.getpeerinfo()]
        assert_equal(len(before), 1)
        node.send_message(msg_tx(bad))
        time.sleep(3)
        after = n.getpeerinfo()
        banned = n.listbanned()
        with open(self.log_path(0), 'rb') as f:
            f.seek(off)
            lines = [ln for ln in f.read().decode('utf-8', 'replace').splitlines()
                     if 'isbehaving' in ln or 'banning' in ln or 'mandatory-script' in ln]
        out = {'log': lines[-4:], 'banscore_before': before[0].get('banscore'),
               'connected_after': len(after), 'banscore_after': after[0].get('banscore') if after else None,
               'listbanned': banned}
        print('  p2p: %r' % out)
        try:
            n.clearbanned()
        except Exception:
            pass
        try:
            conn.disconnect_node()
        except Exception:
            pass
        return out

    def sc_staged(self, label, scheme, seed):
        signer = Signer(self.lib, scheme, seed)
        wrong = signer.sign(os.urandom(32)) + bytes([pq.SIGHASH_ALL])
        self.staged(label, staged_script(signer.pk, wrong, scheme), 1, 0)

    def sc_legacy(self):
        """The legacy reference for R-A1/R-B1: a bare output ``<sig> <pk> [OP_2DUP OP_CHECKSIG
        OP_DROP]x66 OP_CHECKSIG OP_NOT`` (200 ops) with a DER signature over another message: 67
        ECDSA verifications per ~41-byte spend. Legacy sigops are counted at creation (67 per
        output, 20,000 per block), never at the spend."""
        secret = v.fixed_secret('pq-dos legacy staged')
        pk = v.pubkey_of(secret)
        sig = v.ecdsa_sign_der(secret, os.urandom(32)) + bytes([1])
        spk = v.push(sig) + v.push(pk) + bytes([0x6e, 0xac, 0x75]) * 66 + bytes([0xac, OP_NOT])
        self.staged('legacy', spk, 67, 67)

    def staged(self, label, spk, verifies_per_input, legacy_sigops):
        out_bytes = 8 + 3 + len(spk)
        spend_in = 41
        per_tx = 2000
        n_sp = self.options.staged_spends or (BLOCK_TARGET // (per_tx * spend_in + 60)) * per_tx
        per_block = (BLOCK_TARGET - 2000) // out_bytes
        per_ctx = per_block // 2
        if legacy_sigops:     # MAX_STANDARD_TX_SIGOPS 4,000 per creation tx, MAX_BLOCK_SIGOPS 20,000
            per_ctx = min(per_ctx, 3990 // legacy_sigops)
            per_block = min(per_block, (19990 // legacy_sigops) // per_ctx * per_ctx)
        else:
            per_block = per_block // per_ctx * per_ctx
        print('%s: %d-byte staged script; %d outputs (%d per block) then one block of %d spends' % (
            label, len(spk), n_sp, per_block, n_sp))
        n = self.n0
        value = 1000
        # funding: one output per creation transaction, to a Python-held key (the wallet never
        # stores the 1 MB creation transactions)
        fund_secret = v.fixed_secret('pq-dos staged funding')
        fund_spk = v.p2pkh_script_of_pubkey(v.pubkey_of(fund_secret))
        n_ctx = -(-n_sp // per_ctx)
        cfee = (per_ctx * out_bytes + 300) * RELAY_FLOOR // 1000 + 1000
        fund_value = per_ctx * value + cfee
        funds = self.fanout(fund_spk, n_ctx, fund_value, per_tx=1000)
        staged = []
        left = n_sp
        in_block = 0
        t0 = time.time()
        for fi, (ft, fj) in enumerate(funds):
            k = min(left, per_ctx)
            if in_block + k > per_block:
                self.mine()
                in_block = 0
            tx = v.make_tx([(ft, fj, 0xffffffff)], [(value, spk)] * k)    # the rest is the fee
            v.sign_p2pkh_input(tx, 0, fund_secret, fund_value, signing_branch_id(n))
            txid = n.sendrawtransaction(v.tx_hex(tx))
            staged += [(txid, j, value) for j in range(k)]
            in_block += k
            left -= k
            if left == 0:
                break
        self.mine()
        build_s = time.time() - t0
        print('  staged outputs mined: %d in %.0f s, chain height %d' % (len(staged), build_s, n.getblockcount()))
        txs = []
        for k in range(0, len(staged), per_tx):
            group = staged[k:k + per_tx]
            total = sum(x[2] for x in group)
            fee = (len(group) * spend_in + 60) * FEE_RATE // 1000
            txs.append(v.make_tx([(t, j, 0xffffffff) for t, j, _ in group], [(total - fee, fund_spk)]))
        # the policy count (20 per OP_CHECKPQSIG in the spent output) refuses these in the mempool
        # (> MAX_STANDARD_TX_SIGOPS) and the template (> 1,000 per block); consensus counts none
        self.mine_measured(label, {'inputs': n_sp, 'verifies': n_sp * verifies_per_input, 'staged_script_bytes': len(spk),
                                   'staged_output_bytes': out_bytes, 'creation_blocks_s': round(build_s, 1),
                                   'scheme': label}, txs=txs)

    def sc_sapling(self):
        print('SAPLING: per-spend and per-output validation cost')
        n = self.n0
        za = n.z_getnewaddress('sapling')
        zs = [n.z_getnewaddress('sapling') for _ in range(50)]      # z_sendmany refuses duplicates
        taddr = n.getnewaddress()
        tspk = v.node_spk(n, taddr)
        # transparent funds for the t->z sends: 320 coins of 0.02
        self.fanout(tspk, 160, 2000000)
        self.fanout(tspk, 160, 2000000)
        self.wait_wallet(lambda: len(n.listunspent(1, 9999999, [taddr])) >= 320)
        # block 1 (measured): four t->z transactions of 50 Sapling outputs each (200 outputs)
        for _ in range(4):
            amounts = [{'address': z, 'amount': Decimal('0.009')} for z in zs]
            self.wait_op(n.z_sendmany(taddr, amounts, 1, Decimal('0.001')))
        self.sapling_block('sapling_outputs', {})
        # block 2 (measured): 200 single-output t->z sends to one address, the notes of block 3
        for _ in range(200):
            self.wait_op(n.z_sendmany(taddr, [{'address': za, 'amount': Decimal('0.009')}], 1, Decimal('0.0001')))
        self.sapling_block('sapling_single_outputs', {})
        # block 3 (measured): z->t spending those 200 notes in four transactions of 50 spends each
        self.wait_wallet(lambda: len(n.z_listunspent(1, 9999999, False, [za])) >= 200)
        for _ in range(4):
            self.wait_op(n.z_sendmany(za, [{'address': taddr, 'amount': Decimal('0.4485')}], 1, Decimal('0.0015')))
        self.sapling_block('sapling_spends', {})

    def sapling_block(self, label, meta):
        n = self.n0
        mp = n.getrawmempool()
        spends = outputs = 0
        for t in mp:
            d = n.getrawtransaction(t, 1)
            spends += len(d.get('vShieldedSpend', []))
            outputs += len(d.get('vShieldedOutput', []))
        meta = dict(meta)
        meta.update({'sapling_spends': spends, 'sapling_outputs': outputs,
                     'transparent_inputs': sum(len(n.getrawtransaction(t, 1)['vin']) for t in mp)})
        print('  %s: %d txs, %d spends, %d outputs' % (label, len(mp), spends, outputs))
        self.mine_measured(label, meta)

    @staticmethod
    def wait_wallet(cond, timeout=120):
        """The wallet hears of blocks asynchronously (ThreadNotifyWallets): poll."""
        t0 = time.time()
        while not cond():
            assert time.time() - t0 < timeout, 'wallet did not catch up'
            time.sleep(0.2)

    def wait_op(self, opid, timeout=1800):
        n = self.n0
        t0 = time.time()
        while time.time() - t0 < timeout:
            r = n.z_getoperationresult([opid])
            if r:
                assert r[0]['status'] == 'success', r[0]
                return r[0]['result']['txid']
            time.sleep(0.5)
        raise AssertionError('operation timed out')

    def sc_flood(self):
        print('FLOOD: 8 KB SLH-DSA transactions at the relay floor into a small mempool')
        n = self.n0
        signer = Signer(self.lib, 1, bytes(range(4, 52)))
        count = self.options.flood
        value = 200000
        outs = self.fanout(signer.spk, count + 100, value)
        floor_txs = self.pq_spend_txs(signer, outs[:count], value, 1, fee_rate=RELAY_FLOOR)
        hi_txs = self.pq_spend_txs(signer, outs[count:count + 50], value, 1, fee_rate=130)  # fee >= DEFAULT_FEE
        # P2PKH at DEFAULT_FEE, signed by the wallet
        addr = n.getnewaddress()
        p_outs = self.fanout(v.node_spk(n, addr), 100, value)
        p2pkh = [v.tx_from_hex(n.signrawtransaction(v.tx_hex(v.make_tx([(t, j, 0xffffffff)],
                 [(value - 1000, self.dest_spk)])))['hex']) for t, j in p_outs]
        sizes = [len(v.tx_hex(t)) // 2 for t in floor_txs]
        fees = [value - t.vout[0].nValue for t in floor_txs]
        limit = 2000000
        self.restart(0, ['-mempooltxcostlimit=%d' % limit])
        self.n0 = n = self.nodes[0]
        pid = bitcoind_processes[0].pid
        rss0 = rss_kb(pid)
        timeline, accepted, rejected = [], 0, {}
        t_start = time.time()
        times = []
        sent = []
        for idx, tx in enumerate(floor_txs):
            t0 = time.time()
            try:
                txid = n.sendrawtransaction(v.tx_hex(tx))
                accepted += 1
                sent.append(txid)
            except JSONRPCException as e:
                rejected[e.error['message']] = rejected.get(e.error['message'], 0) + 1
            times.append(time.time() - t0)
            if (idx + 1) % 25 == 0:
                mi = n.getmempoolinfo()
                timeline.append({'sent': idx + 1, 'size': mi['size'], 'bytes': mi['bytes'], 'usage': mi['usage'],
                                 'rss_kb': rss_kb(pid)})
        flood_s = time.time() - t_start
        in_pool = set(n.getrawmempool())
        survivors_floor = len([t for t in sent if t in in_pool])
        # higher-fee PQ and DEFAULT_FEE P2PKH into the full pool
        hi_ids, p_ids = [], []
        for tx in hi_txs:
            try:
                hi_ids.append(n.sendrawtransaction(v.tx_hex(tx)))
            except JSONRPCException as e:
                rejected['hi:' + e.error['message']] = rejected.get('hi:' + e.error['message'], 0) + 1
        for tx in p2pkh:
            try:
                p_ids.append(n.sendrawtransaction(v.tx_hex(tx)))
            except JSONRPCException as e:
                rejected['p2pkh:' + e.error['message']] = rejected.get('p2pkh:' + e.error['message'], 0) + 1
        pool = set(n.getrawmempool())
        mi = n.getmempoolinfo()
        res = {'limit_cost': limit, 'sent': count, 'tx_bytes_median': self.median(sizes), 'fee_zat_median': self.median(fees),
               'accepted': accepted, 'rejected': rejected, 'flood_wall_s': round(flood_s, 2),
               'accept_rate_tx_per_s': round(count / flood_s, 1), 'atmp_ms_median': self.median(times) * 1e3,
               'survivors_floor_after_flood': survivors_floor,
               'after_mixed': {'floor_pq_left': len([t for t in sent if t in pool]),
                               'pq_fee_ge_default_sent': len(hi_ids),
                               'pq_fee_ge_default_left': len([t for t in hi_ids if t in pool]),
                               'p2pkh_sent': len(p_ids), 'p2pkh_left': len([t for t in p_ids if t in pool]),
                               'mempool': mi},
               'rss_kb_start': rss0, 'rss_kb_end': rss_kb(pid), 'timeline': timeline}
        print('  flood: %r' % {k: res[k] for k in res if k != 'timeline'})
        self.results['flood'] = res
        self.save()
        # back to the measuring configuration: empty the pool into a block
        self.mine()
        self.restart(0)
        self.n0 = self.nodes[0]

    @staticmethod
    def median(xs):
        xs = sorted(xs)
        return xs[len(xs) // 2] if xs else None

    # ------------------------------------------------------------------ main

    def run_test(self):
        self.n0 = n = self.nodes[0]
        self.results = {'config': {'par': PAR, 'activation': ACTIVATION, 'block_target': BLOCK_TARGET,
                                   'cores': os.cpu_count()}}
        self.lib = build_shim(self.options.tmpdir)
        self.next_cb = 1
        scen = self.options.scenarios.split(',')
        n.generate(ACTIVATION + 100)
        self.dest_spk = v.node_spk(n)
        self.sync_measurers(n.getblockcount())
        for s in scen:
            t0 = time.time()
            if s == 'p2pkh':
                self.sc_p2pkh()
            elif s == 'slh':
                self.sc_pq('slh', pq.SCHEME_SLH_DSA_SHA2_128S, 10, bytes(range(48)))
            elif s == 'falcon':
                # 1,200 Falcon spends: over the template's 1,000-check policy cap, so hand-built
                self.sc_pq('falcon', pq.SCHEME_FN_DSA_512, 50, bytes(range(48)), hand_built=True)
            elif s == 'invalid':
                self.sc_invalid()
            elif s == 'staged':
                self.sc_staged('staged', pq.SCHEME_SLH_DSA_SHA2_128S, bytes(range(5, 53)))
            elif s == 'stagedf':
                self.sc_staged('stagedf', pq.SCHEME_FN_DSA_512, bytes(range(6, 54)))
            elif s == 'legacy':
                self.sc_legacy()
            elif s == 'sapling':
                self.sc_sapling()
            elif s == 'flood':
                self.sc_flood()
            else:
                raise AssertionError('unknown scenario ' + s)
            print('  (%s took %.0f s)' % (s, time.time() - t0))
        self.save()


if __name__ == '__main__':
    PQDoS().main()
