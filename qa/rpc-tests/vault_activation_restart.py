#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
A restart while the node holds an invalidated branch that crosses a network upgrade's activation
block: a known v4.5.0 (stock Zcash 4.x) defect, pinned here so it is visible and so the day it is
fixed this script fails and is turned round (ycash6 carries the passing form of the same script).

Stock RewindBlockIndex (src/main.cpp, RewindBlockIndex) runs at every start and erases from the
block index every entry off the active chain that is not "sufficiently validated", which needs
nCachedBranchId. LoadBlockIndexDB only derives nCachedBranchId from the parent for an entry that
IsValid(BLOCK_VALID_CONSENSUS), and IsValid is false for any BLOCK_FAILED_* entry; only an
activation block (BLOCK_ACTIVATES_UPGRADE) has its branch id on disk. So after `invalidateblock`
of a block below the activation height whose branch contains the activation block, the next start
erases the invalidated block and every descendant except the activation block, which is kept with
a pprev that points at a deleted entry. CheckBlockIndex then fails
"nNodes == forward.size()" (the activation block is not reachable from genesis). Without
-checkblockindex (the default on mainnet and testnet) the node starts with that dangling entry.

Cases, each on its own node:
  - Sapling (a stock upgrade, no Yellowback or vault code involved) and UPGRADE_VAULT;
  - invalidating the activation block itself is the control: its parent is valid and kept, the
    restart is clean;
  - invalidating the block below the activation: the restart aborts under -checkblockindex
    (SIGABRT, the assertion on stderr); without it the node starts at the invalidated tip; a
    -reindex rebuilds a consistent index (and, as on v4.5.0 generally, forgets the invalidation).
"""

import os
import signal
import tempfile
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    VAULT_BRANCH_ID,
    assert_equal,
    bitcoind_processes,
    nuparams,
    start_node,
    stop_node,
)
from test_framework.yellowback_util import YCASH_UPGRADE_ARGS

ACT = 10                     # the activation height under test
TOP = ACT + 3                # the tip before the invalidation

# start_node passes Overwinter and Sapling at height 1; a later -nuparams for the same branch wins
# (as in mempool_nu_activation.py), so this moves Sapling to ACT with nothing else scheduled.
SAPLING_ARGS = ['-nuparams=5ba81b19:1', '-nuparams=76b809bb:%d' % ACT]
VAULT_ARGS = list(YCASH_UPGRADE_ARGS) + [nuparams(VAULT_BRANCH_ID, ACT)]

# node: (upgrade arguments, the height invalidated)
CASES = {
    0: ('Sapling', SAPLING_ARGS, ACT - 1),
    1: ('Sapling', SAPLING_ARGS, ACT),
    2: ('Vault', VAULT_ARGS, ACT - 1),
    3: ('Vault', VAULT_ARGS, ACT),
}


class VaultActivationRestartTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = len(CASES)
        self.setup_clean_chain = True

    def setup_network(self, split=False):
        self.nodes = [start_node(i, self.options.tmpdir, CASES[i][1] + ['-checkblockindex=1'])
                      for i in sorted(CASES)]
        self.is_network_split = True          # each node is its own chain

    def start(self, i, extra):
        self.nodes[i] = start_node(i, self.options.tmpdir, CASES[i][1] + extra)
        return self.nodes[i]

    def start_expect_abort(self, i, extra):
        """Start node i and expect it to abort during init; return (exit status, stderr)."""
        path = os.path.join(self.options.tmpdir, 'node%d' % i, 'abort-stderr.txt')
        with open(path, 'w+b') as err:
            try:
                start_node(i, self.options.tmpdir, CASES[i][1] + extra, stderr=err)
            except Exception as e:
                assert 'exited with status' in str(e), e
            else:
                raise AssertionError('node %d started; an abort in CheckBlockIndex was expected' % i)
            status = bitcoind_processes[i].returncode
            del bitcoind_processes[i]
            err.seek(0)
            return status, err.read().decode('utf-8', 'replace')

    def wait_height(self, node, height, timeout=120):
        deadline = time.time() + timeout
        while node.getblockcount() < height and time.time() < deadline:
            time.sleep(0.5)
        assert_equal(node.getblockcount(), height)

    def run_test(self):
        for i in sorted(CASES):
            name, _, bad = CASES[i]
            n = self.nodes[i]
            n.generate(TOP)
            assert_equal(n.getblockcount(), TOP)
            n.invalidateblock(n.getblockhash(bad))
            assert_equal(n.getblockcount(), bad - 1)
            stop_node(n, i)

        for i in sorted(CASES):
            name, _, bad = CASES[i]
            if bad == ACT:
                print('%s: invalidated at the activation block: the restart is clean' % name)
                n = self.start(i, ['-checkblockindex=1'])
                assert_equal(n.getblockcount(), ACT - 1)
                n.generate(2)                                  # a new branch across the activation
                assert_equal(n.getblockcount(), ACT + 1)
                continue

            print('%s: invalidated below the activation: -checkblockindex aborts in CheckBlockIndex' % name)
            status, err = self.start_expect_abort(i, ['-checkblockindex=1'])
            assert_equal(status, -signal.SIGABRT)
            assert 'nNodes == forward.size()' in err, err

            # Without -checkblockindex the same start succeeds and the tip is the invalidated one.
            # The activation block's entry is left with a dangling parent: nothing here may walk it
            # (getchaintips, getblock/getblockheader of that hash, reconsiderblock would).
            print('%s: the same start without -checkblockindex runs at the invalidated tip' % name)
            n = self.start(i, ['-checkblockindex=0'])
            assert_equal(n.getblockcount(), bad - 1)
            stop_node(n, i)

            print('%s: -reindex rebuilds a consistent index (the invalidation is not kept)' % name)
            n = self.start(i, ['-checkblockindex=1', '-reindex'])
            self.wait_height(n, TOP)
            stop_node(n, i)
            n = self.start(i, ['-checkblockindex=1'])
            assert_equal(n.getblockcount(), TOP)


if __name__ == '__main__':
    VaultActivationRestartTest().main()
