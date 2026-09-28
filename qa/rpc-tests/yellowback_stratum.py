#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Real pool software against the node: yolo (Rust) and a stratum miner, tag on every path
(docs/plans/role-pool-regtest-plan.md section 3.5, Y5).

Every other script mines with ``generate``, the one block-building path no Ycash pool uses.  This
one puts the block through node -> ``getblocktemplate`` -> yolo -> ``mining.notify`` ->
``contrib/yellowback/devnet/stratum-miner`` (the framework's 48,5 solver) -> ``mining.submit`` ->
``submitblock``, once per coinbase policy yolo has:

  solo     ``coinbasetxn.data`` as is
  pool     the payout output rewritten to the miner's address (the stratum username)
  cenote   the scriptSig rebuilt as height push + ``coinbaseaux.flags`` + text, output rewritten

and asserts, per mode: two blocks accepted, both nodes at the new height, ``yed_gettag`` found
with the quote's ``priceMicroUsd`` and the pool node's payout address, the coinbase paying the
miner's address in pool/cenote (the node's own wallet in solo), and ``check-coinbase`` agreeing.
Then the negative: ``cenote --no-flags`` (the Perl cenote's behaviour, Y-F1) yields an accepted
block whose tag is ``found: false``.

Needs a yolo binary: ``YOLO_BIN`` names it (the fork's CI builds boyfromcave/yolo and sets it);
without one the script SKIPs (exit 0 with a message) so the inherited matrix does not break.  No
``setmocktime``: yolo stamps ``max(template.curtime, now)`` and the node's ``curtime`` is already
``max(MTP + 1, now)``, so a burst-generated chain is not ``time-too-old`` (Y-F5 is the Perl's
wall-clock stamp, not the node's).
"""

import json
import os
import subprocess
import sys
import time
import urllib.request

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    PORT_MIN,
    PORT_RANGE,
    assert_equal,
    connect_nodes_bi,
    rpc_auth_pair,
    rpc_port,
    start_nodes,
    sync_blocks,
)
from test_framework.yellowback_util import (
    POOL_WIFS,
    address_of,
    pool_args,
    set_quote,
    usd_to_micro,
    yellowback_node_args,
)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
WORKSPACE = os.path.dirname(REPO)
if os.path.basename(WORKSPACE) == 'wt':                      # a worktree: wt/<name>/ -> the workspace
    WORKSPACE = os.path.dirname(WORKSPACE)
STRATUM_MINER = os.path.join(REPO, 'contrib', 'yellowback', 'devnet', 'stratum-miner')
CHECK_COINBASE = os.path.join(REPO, 'contrib', 'yellowback', 'pool', 'check-coinbase')
QUOTE_USD = '0.05'
BLOCKS_PER_MODE = 2
MODES = ('solo', 'pool', 'cenote')
STRATUM_PORT_BASE = 30000       # + (rpc_port(0) - PORT_MIN - PORT_RANGE): follows --portseed, clear of the framework's 11000-21000


def find_yolo():
    """YOLO_BIN, else the workspace build beside the fork; None means SKIP."""
    override = os.environ.get('YOLO_BIN')
    if override:
        path = os.path.expanduser(override)
        return path if os.access(path, os.X_OK) else None
    candidate = os.path.join(WORKSPACE, 'yolo', 'target', 'release', 'yolo')
    return candidate if os.access(candidate, os.X_OK) else None


# Rule: MINER-2 TAG-1
class YellowbackStratumTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.yolo = None
        self.procs = []
        self.pool_address = address_of(POOL_WIFS[0])

    def setup_network(self, split=False):
        args = [pool_args(self.pool_address), yellowback_node_args()]
        self.nodes = start_nodes(2, self.options.tmpdir, extra_args=args)
        connect_nodes_bi(self.nodes, 0, 1)
        self.is_network_split = False
        self.nodes[0].importprivkey(POOL_WIFS[0], 'yellowback-payout', False)
        self.sync_all()

    # ------------------------------------------------------------------ processes

    def ports(self, index):
        base = STRATUM_PORT_BASE + (rpc_port(0) - PORT_MIN - PORT_RANGE) + index
        return base, base + 5000

    def start_yolo(self, mode, index, extra=()):
        user, password = rpc_auth_pair(0)
        port, status_port = self.ports(index)
        argv = [self.yolo, '--mode', mode, '--bind', '127.0.0.1', '--port', str(port), '--status-port', str(status_port),
                '--rpc', 'http://127.0.0.1:%d' % rpc_port(0), '--rpc-user', user, '--rpc-password', password,
                '--equihash', 'auto', '--log', 'debug'] + list(extra)
        log = open(os.path.join(self.options.tmpdir, 'yolo-%s-%d.log' % (mode, index)), 'ab')
        print('starting %s' % ' '.join(argv))
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        self.procs.append(proc)
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc.poll() is not None:
                raise AssertionError('yolo --mode %s exited %s at start; see %s' % (mode, proc.returncode, log.name))
            doc = self.status(status_port)
            if doc and doc.get('height') is not None:
                return proc, port, status_port
            time.sleep(0.5)
        raise AssertionError('yolo --mode %s served no template within 30 s; see %s' % (mode, log.name))

    @staticmethod
    def status(status_port):
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/status' % status_port, timeout=3) as reply:
                return json.load(reply)
        except Exception:
            return None

    def stop_proc(self, proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()

    def mine(self, port, user, blocks):
        argv = [sys.executable, STRATUM_MINER, '--pool', '127.0.0.1:%d' % port, '--user', user, '--blocks', str(blocks)]
        print('running %s' % ' '.join(argv))
        out = subprocess.run(argv, capture_output=True, text=True, timeout=600)
        print(out.stdout.strip())
        if out.returncode != 0:
            raise AssertionError('stratum-miner exited %d: %s' % (out.returncode, out.stderr.strip()))
        return out.stdout

    def check_coinbase(self, height):
        """check-coinbase's exit code: 0 a tag was found, 1 none."""
        cli = os.path.join(os.path.dirname(os.environ.get('BITCOIND', os.path.join(REPO, 'src', 'ycashd'))), 'ycash-cli')
        argv = [sys.executable, CHECK_COINBASE, '--cli', cli, str(height), '--', '-regtest',
                '-datadir=%s' % os.path.join(self.options.tmpdir, 'node0')]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        print('check-coinbase %d -> exit %d: %s' % (height, out.returncode, ' | '.join(out.stdout.split('\n')[:3])))
        assert out.returncode in (0, 1), out.stderr
        return out.returncode

    # ------------------------------------------------------------------ the checks

    def coinbase_payee(self, height):
        block = self.nodes[0].getblock(str(height), 2)
        vouts = [o for o in block['tx'][0]['vout'] if o['value'] > 0]
        return [a for o in vouts for a in o['scriptPubKey'].get('addresses', [])]

    def run_mode(self, mode, index, miner_user, extra=(), expect_tag=True):
        proc, port, status_port = self.start_yolo(mode, index, extra)
        before = self.nodes[0].getblockcount()
        try:
            self.mine(port, miner_user, BLOCKS_PER_MODE)
            sync_blocks(self.nodes)
            height = before + BLOCKS_PER_MODE
            assert_equal([n.getblockcount() for n in self.nodes], [height, height])
            doc = self.status(status_port)
            print('/status: %s' % json.dumps(doc, sort_keys=True))
            assert_equal(doc['mode'], mode)
            assert_equal(doc['accepted'], BLOCKS_PER_MODE)
            assert_equal(doc['rejected'], 0)
            assert_equal(doc['lastSubmitVerdict'], 'accepted')
            for h in range(before + 1, height + 1):
                for n in self.nodes:
                    tag = n.yed_gettag(str(h))
                    print('%s: node %d yed_gettag %d -> %s' % (mode, self.nodes.index(n), h, json.dumps(tag, sort_keys=True)))
                    assert_equal(tag['found'], expect_tag)
                    if expect_tag:
                        assert_equal(tag['kind'], 'quote')
                        assert_equal(tag['priceMicroUsd'], usd_to_micro(QUOTE_USD))
                        assert_equal(tag['payoutAddress'], self.pool_address)
                sig = self.nodes[0].getblock(str(h), 2)['tx'][0]['vin'][0]['coinbase']
                print('%s: coinbase scriptSig at %d: %s' % (mode, h, sig))
                assert_equal(self.check_coinbase(h), 0 if expect_tag else 1)
                # vout 0 is the block reward; vout 1 the founders' reward (regtest pays one too)
                payees = self.coinbase_payee(h)
                print('%s: coinbase pays %s' % (mode, payees))
                if mode == 'solo':
                    assert self.nodes[0].validateaddress(payees[0])['ismine'], payees
                else:
                    assert_equal(payees[0], miner_user)
            assert_equal(doc['tag'], 'quote' if expect_tag else 'none')
        finally:
            self.stop_proc(proc)

    def run_test(self):
        self.yolo = find_yolo()
        if not self.yolo:
            print('SKIP: no yolo binary (set YOLO_BIN, or build <workspace>/yolo with cargo build --release)')
            return
        print('yolo: %s' % self.yolo)
        self.nodes[0].generate(101)
        self.sync_all()
        set_quote(self.nodes[0], QUOTE_USD)
        tpl = self.nodes[0].getblocktemplate()
        print('template flags (coinbaseaux): %s' % tpl.get('coinbaseaux', {}).get('flags'))
        assert tpl.get('coinbaseaux', {}).get('flags'), 'the pool node builds no tag'
        miner_user = self.nodes[1].getnewaddress()          # a foreign wallet: the rewrite is visible on chain
        try:
            self.run_mode('solo', 0, miner_user)
            self.run_mode('pool', 1, miner_user)
            self.run_mode('cenote', 2, miner_user, extra=('--text', 'yellowback_stratum.py'))
            # Y-F1 pinned: a cenote that does not append coinbaseaux.flags loses the tag on a block
            # the node accepts. --no-flags is yolo's hidden test-only switch for exactly this.
            self.run_mode('cenote', 3, miner_user, extra=('--text', 'no-flags', '--no-flags'), expect_tag=False)
        finally:
            for proc in self.procs:
                self.stop_proc(proc)
        print('yellowback_stratum: every mode carried the tag; cenote --no-flags dropped it')


if __name__ == '__main__':
    YellowbackStratumTest().main()
