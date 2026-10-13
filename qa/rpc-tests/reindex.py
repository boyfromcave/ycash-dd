#!/usr/bin/env python3
# Copyright (c) 2014-2016 The Bitcoin Core developers
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

#
# Test -reindex with CheckBlockIndex
#

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, \
    start_node, stop_node, wait_bitcoinds
import time


class ReindexTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.setup_clean_chain = True
        self.num_nodes = 1

    def setup_network(self):
        self.nodes = []
        self.is_network_split = False
        self.nodes.append(start_node(0, self.options.tmpdir))

    def run_test(self):
        self.nodes[0].generate(3)
        stop_node(self.nodes[0], 0)
        wait_bitcoinds()
        self.nodes[0]=start_node(0, self.options.tmpdir, ["-debug", "-reindex", "-checkblockindex=1"])
        # The RPC server answers once init finishes, while ThreadImport may still be
        # replaying the block files (init.cpp: ThreadImport is started before
        # SetRPCWarmupFinished). Wait for the reindex to catch up, as upstream does
        # (Zcash's reindex.py, ref/ycash6/qa/rpc-tests/reindex.py), with a deadline.
        deadline = time.time() + 120
        while self.nodes[0].getblockcount() < 3 and time.time() < deadline:
            time.sleep(0.1)
        assert_equal(self.nodes[0].getblockcount(), 3)
        print("Success")

if __name__ == '__main__':
    ReindexTest().main()
