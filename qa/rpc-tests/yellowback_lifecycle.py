#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""Live v2 transparent wallet lifecycle: mint, send, and owner redemption."""

from test_framework.util import assert_equal, assert_greater_than
from test_framework.yellowback_util import (
    POOLS,
    REF_LAG,
    TOKEN_VALUE,
    YellowbackTestFramework,
    assert_same_statehash,
    set_quote,
)


class YellowbackLifecycleTest(YellowbackTestFramework):
    """# Rule: MINTPOL-1 MINT-1 MINT-2 MINT-3 MINT-4 MINT-5 MINT-8 XFER-1 XFER-2 XFER-3 RED-1 RED-2 RED-3 RED-4 MP-1"""

    def run_test(self):
        nodes = self.nodes
        user = nodes[0]
        recipient = nodes[5]

        print('activate and fill price windows')
        self.activate(POOLS, quote_usd='2.00')
        self.mine_round_robin(POOLS, REF_LAG + 1)
        for node in self.enforcing_nodes():
            assert_equal(node.yed_getstats()['mintingAllowed'], True)

        print('fund the recipient wallet for its return-transfer network fee')
        recipient_yec = recipient.getnewaddress()
        user.sendtoaddress(recipient_yec, 1)
        self.sync_all()
        self.mine(POOLS[0])

        print('mint 100 YED from the user wallet')
        before_yec = user.getbalance()
        mint = user.yed_mint(10_000, 48)
        assert_equal(mint['termClass'], 'A')
        assert_greater_than(mint['collateralZat'], 0)
        assert_greater_than(mint['feeZat'], 0)
        self.sync_all()
        self.mine(POOLS[0])
        vault = user.yed_getvault(mint['txid'])
        assert_equal(vault['status'], 'ACTIVE')
        assert_equal(user.yed_getbalance()['confirmedCents'], 10_000)
        assert_equal(nodes[2].yed_getstats()['supplyCents'], 10_000)
        assert_same_statehash(self.enforcing_nodes())

        print('send 40 YED to the observer wallet')
        address = recipient.yed_getnewaddress()
        sent = user.yed_send(address, 4_000)
        assert_equal(sent['changeCents'], 6_000)
        self.sync_all()
        self.mine(POOLS[1])
        assert_equal(user.yed_getbalance()['confirmedCents'], 6_000)
        assert_equal(recipient.yed_getbalance()['confirmedCents'], 4_000)
        assert_same_statehash(self.enforcing_nodes())

        print('return the 40 YED so the owner can redeem the vault debt')
        owner_address = user.yed_getnewaddress()
        returned = recipient.yed_send(owner_address, 4_000)
        assert_equal(returned['changeCents'], 0)
        self.sync_all()
        self.mine(POOLS[2])
        assert_equal(user.yed_getbalance()['confirmedCents'], 10_000)

        print('wait for the vault lock and redeem its debt')
        current = user.getblockcount()
        remaining = mint['lockHeight'] - current
        assert remaining > 0
        self.mine_round_robin(POOLS, remaining)
        positions = user.yed_listpositions()
        assert_equal(len(positions), 1)
        assert_equal(positions[0]['canRedeem'], True)
        redeemed = user.yed_redeem(mint['txid'])
        assert_equal(redeemed['burnedCents'], 10_000)
        assert_greater_than(redeemed['collateralOut'], 0)
        self.sync_all()
        self.mine(POOLS[0])
        closed = user.yed_getvault(mint['txid'])
        assert_equal(closed['status'], 'CLOSED')
        assert_equal(closed['burnedCents'], 10_000)
        assert_equal(user.yed_getbalance()['confirmedCents'], 0)
        assert_equal(nodes[2].yed_getstats()['supplyCents'], 0)
        assert_greater_than(user.getbalance(), before_yec - 2)
        self.checkpoint('transparent mint-send-redeem')


if __name__ == '__main__':
    YellowbackLifecycleTest().main()
