#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Recognise and refuse Yugen (ruling 18). This line does not implement Yugen (ycashd 6.23.x does):
it knows the Yugen branch ID 0x59474e30 and version group 0x59474e47 only to refuse them by name.

- a transaction in the Yugen version group does not decode (it is never parsed as v4);
- signrawtransaction with the Yugen branch id fails with an error that names Yugen;
- -nuparams=59474e30:<h> (any case) and -eqparams with that id are init errors that name Yugen.
"""

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_start_raises_init_error, start_node

YUGEN_BRANCH_ID = 0x59474e30
YUGEN_VERSION_GROUP_ID = 0x59474e47


class YugenRefuseTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.num_nodes = 2             # node 1 is only the -nuparams init-error start
        self.setup_clean_chain = True   # v4.5.0 harness
        self.cache_behavior = 'clean'   # 6.20.0 harness

    def setup_network(self, split=False):
        self.nodes = [start_node(0, self.options.tmpdir)]
        self.is_network_split = False

    def run_test(self):
        node = self.nodes[0]
        raw = node.createrawtransaction([{'txid': '11' * 32, 'vout': 0}], {})
        decoded = node.decoderawtransaction(raw)
        assert_equal(decoded['versiongroupid'], '892f2085')

        # The same bytes with the Yugen version group: refused, not misparsed as v4.
        yugen = raw[:8] + YUGEN_VERSION_GROUP_ID.to_bytes(4, 'little').hex() + raw[16:]
        assert raw[8:16] == '85202f89', raw[:16]
        for call in (node.decoderawtransaction, node.sendrawtransaction):
            try:
                call(yugen)
                raise AssertionError('a Yugen-format transaction was accepted')
            except JSONRPCException as e:
                assert 'decode failed' in e.error['message'], e.error

        try:
            node.signrawtransaction(raw, [], [], 'ALL', '%08x' % YUGEN_BRANCH_ID)
            raise AssertionError('the Yugen branch id was accepted')
        except JSONRPCException as e:
            assert 'Yugen' in e.error['message'], e.error

        assert_start_raises_init_error(
            1, self.options.tmpdir, ['-nuparams=%x:10' % YUGEN_BRANCH_ID],
            'is Yugen, which this node does not implement')
        assert_start_raises_init_error(
            1, self.options.tmpdir, ['-nuparams=%X:10' % YUGEN_BRANCH_ID],
            'is Yugen, which this node does not implement')
        assert_start_raises_init_error(
            1, self.options.tmpdir, ['-eqparams=%X:200:9' % YUGEN_BRANCH_ID],
            'is Yugen, which this node does not implement')
        print('yugen_refuse OK')


if __name__ == '__main__':
    YugenRefuseTest().main()
