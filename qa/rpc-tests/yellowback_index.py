#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
The synchronous v2 index end to end (plan Phase 3, §7): tags from `generate` land in Tags,
medians appear as the windows fill, activation, reorgs across a tag change and across the fast
window's fill bound, the REG-4 judgement undone by a reorg, restart, -reindex-yellowback,
-reindex, -prune refused, crash recovery with an unflushed chainstate (and the wipe-and-rebuild
fallback beyond UNDO_KEEP), the first Python-built owner-path redemption confirmed through
sendrawtransaction, a rejection that survives kill -9 (the kill switch's two branches), a fresh
node syncing across an accepted rule-breaking block (BLK-2 clause 3), the start height above the
tip, the contiguous history, a loud parameter mismatch, an unhealthy node across a reorg,
verifychain at runtime, and yed_getblockverdict's precondition.

Raw builders only (N26): the vaults come from build_mint_tx and the spends from
build_vault_spend_raw.
"""

import os
import shutil
import time

from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_start_raises_init_error,
    connect_nodes_bi,
    initialize_datadir,
    start_node,
    stop_node,
    sync_blocks,
)
from test_framework.yellowback_util import (
    ACTIVATION_BLOCKS,
    ENFORCING,
    MIN_FILL,
    OBSERVER,
    PEER_LAG,
    POOLS,
    P_FAST_WINDOW,
    P_MID_WINDOW,
    P_SLOW_WINDOW,
    REF_LAG,
    START_HEIGHT,
    STOCK,
    UNDO_KEEP,
    USER,
    VALVE_BLOCKS,
    YellowbackTestFramework,
    assert_banscore_zero,
    assert_best_hash,
    assert_same_statehash,
    debug_log_contains,
    mine_rejected_block,
    mint_vault_raw,
    redeem_vault_raw,
    wait_for_rejection,
    wait_yed_healthy,
    yellowback_node_args,
)


def rpc_error_message(fn, *args):
    try:
        fn(*args)
    except Exception as e:
        err = getattr(e, 'error', None)
        if isinstance(err, dict):
            return str(err.get('message', ''))
        return str(e)
    raise AssertionError('expected an RPC error from %r%r' % (fn, args))


# Rule: TAG-1 TAG-2 TAG-3 PRICE-1 PRICE-2 ACT-1 ACT-2 ACT-3 SNAP UNDO REG-4 BLK-2 BLK-3 RED-1 RED-2 RED-3 MP-1
class YellowbackIndexTest(YellowbackTestFramework):

    def statehash(self, i):
        return self.nodes[i].yed_getstatehash()['statehash']

    def run_test(self):
        nodes = self.nodes
        user = nodes[USER]
        tmpdir = self.options.tmpdir

        print('initial synchronous index state')
        for node in self.enforcing_nodes():
            info = wait_yed_healthy(node)
            assert_equal(info['rpcversion'], 2)
            assert_equal(info['network'], 'regtest')
            assert_equal(info['height'], node.getblockcount())
            assert_equal(info['startHeight'], START_HEIGHT)
        assert_best_hash(nodes)
        assert_same_statehash(self.enforcing_nodes())

        print('quote tags from the three pool nodes land in Tags; medians appear once the windows fill')
        for pool in POOLS:
            result = self.quote(pool, '2.00')
            assert_equal(result['priceMicroUsd'], 2_000_000)
            assert_equal(result['nextTag']['kind'], 'quote')
            assert_equal(result['nextTag']['signal'], True)
        self.mine_round_robin(POOLS, MIN_FILL[0] - 1)
        assert_equal(nodes[0].yed_getprice()['pFast'], None)
        self.mine_round_robin(POOLS, 1)
        assert_equal(nodes[0].yed_getprice()['pFast'], 2_000_000)     # PRICE-1: defined at the fill bound
        assert_equal(nodes[0].yed_getprice()['pMid'], None)
        self.mine_round_robin(POOLS, P_SLOW_WINDOW - MIN_FILL[0])
        tip = nodes[0].getblockcount()
        tag = nodes[2].yed_gettag(str(tip))
        assert_equal(tag['found'], True)
        assert_equal(tag['kind'], 'quote')
        assert_equal(tag['priceMicroUsd'], 2_000_000)
        price = nodes[0].yed_getprice()
        assert_equal(price['fill']['fast']['quoteTags'], P_FAST_WINDOW)
        assert_equal(price['fill']['mid']['quoteTags'], P_MID_WINDOW)
        assert_equal(price['fill']['slow']['quoteTags'], P_SLOW_WINDOW)
        assert_equal((price['pFast'], price['pMid'], price['pSlow']), (2_000_000, 2_000_000, 2_000_000))
        assert_same_statehash(self.enforcing_nodes())

        # ------------------------------------------------------------------ crash recovery
        print('crash_unflushed_chainstate: node 3 dies with 30 unflushed blocks; the undo walk runs')
        # A node flushes its chainstate at shutdown and then only hourly (FLUSH_STATE_PERIODIC), so a
        # clean restart puts the flushed tip here and the 30 blocks below stay unflushed until the kill.
        self.restart(3)
        wait_yed_healthy(nodes[3])
        nodes[2].generate(30)
        sync_blocks([nodes[2], nodes[3]])
        assert_equal(nodes[3].yed_getinfo()['height'], nodes[3].getblockcount())
        self.kill9(3)
        self.restart(3)
        wait_yed_healthy(nodes[3], timeout=120)
        sync_blocks([nodes[2], nodes[3]], timeout=120)
        assert_equal(nodes[3].yed_getinfo()['height'], nodes[3].getblockcount())
        assert_equal(self.statehash(3), self.statehash(2))
        assert debug_log_contains(tmpdir, 3, 'SyncToChain: undoing'), 'the undo walk did not run'
        self.sync_all(blocks_only=True)

        # ------------------------------------------------------------------ activation
        print('activation after the full signalling window and delay')
        self.mine_round_robin(POOLS, ACTIVATION_BLOCKS)
        for node in self.enforcing_nodes():
            activation = node.yed_getactivation()
            assert_equal(activation['status'], 'active')
            assert_equal(activation['signalCount'], 64)
            assert_equal(node.yed_getstats()['mintingAllowed'], True)
        self.checkpoint('after activation')

        # ------------------------------------------------------------------ reorgs
        print('reorg via split/join across a tag change with equal state hashes')
        self.split_network()
        self.quote(2, '2.10')
        nodes[2].generate(1)
        self.sync_all(blocks_only=True)
        assert_equal(nodes[2].yed_gettag(str(nodes[2].getblockcount()))['priceMicroUsd'], 2_100_000)
        nodes[STOCK].generate(2)
        self.join_network()
        assert_best_hash(nodes)
        assert_equal(nodes[2].yed_gettag(str(nodes[2].getblockcount()))['found'], False)
        assert_same_statehash(self.enforcing_nodes() + [nodes[OBSERVER]])
        self.quote(2, '2.00')

        print('price1_reorg_across_fill_boundary')
        self.mine_round_robin(POOLS, P_FAST_WINDOW)
        nodes[STOCK].generate(P_FAST_WINDOW - MIN_FILL[0])          # exactly the fill bound of the fast window
        self.sync_all(blocks_only=True)
        p = nodes[2].yed_getprice()
        assert_equal(p['fill']['fast']['quoteTags'], MIN_FILL[0])
        assert_equal(p['pFast'], 2_000_000)
        self.split_network()
        nodes[2].generate(1)                                         # branch A: one more quote tag, still defined
        self.sync_all(blocks_only=True)
        assert_equal(nodes[2].yed_getprice()['pFast'], 2_000_000)
        nodes[STOCK].generate(2)                                     # branch B: stock blocks, below the bound
        self.sync_all(blocks_only=True)
        assert_equal(nodes[OBSERVER].yed_getprice()['pFast'], None)
        self.join_network()                                          # B wins: pFast undefined on every node
        assert_best_hash(nodes)
        for node in self.enforcing_nodes() + [nodes[OBSERVER]]:
            assert_equal(node.yed_getprice()['pFast'], None)
            assert_equal(node.yed_getprice()['fill']['fast']['quoteTags'], MIN_FILL[0] - 2)
        assert_same_statehash(self.enforcing_nodes() + [nodes[OBSERVER]])
        self.mine_round_robin(POOLS, P_SLOW_WINDOW)                  # refill every window

        print('reg4_judgement_undone_on_reorg')
        self.split_network()
        self.quote(2, '3.00')                                  # 50 % off the peer median: penalised
        nodes[2].generate(1)
        self.sync_all(blocks_only=True)
        t = nodes[2].getblockcount()
        self.quote(2, '2.00')
        self.mine_round_robin([3, 4], PEER_LAG)                      # judged at t + PEER_LAG (REG-4)
        pool2 = self.pool_addresses[0]
        rows = {r['payoutAddress']: r for r in nodes[3].yed_listminers()}
        assert_greater_than(rows[pool2]['quoted'], rows[pool2]['inBand'])          # the judgement is on record
        self.mine_round_robin([3, 4], 1)                             # REG-2: the penalty is in force from the next block
        rows = {r['payoutAddress']: r for r in nodes[3].yed_listminers()}
        assert_greater_than(rows[pool2]['penalizedUntil'], t)
        judged_hash = self.statehash(3)
        nodes[STOCK].generate(PEER_LAG + 3)                          # deeper than PEER_LAG + 1: the judgement is undone
        self.join_network()
        assert_best_hash(nodes)
        rows = {r['payoutAddress']: r for r in nodes[3].yed_listminers()}
        assert_equal(rows[pool2]['penalizedUntil'], 0)
        assert_equal(rows[pool2]['quoted'], rows[pool2]['inBand'])
        assert self.statehash(3) != judged_hash
        assert_same_statehash(self.enforcing_nodes() + [nodes[OBSERVER]])
        self.restart(3, ['-reindex-yellowback'])
        wait_yed_healthy(nodes[3], timeout=120)
        assert_equal(self.statehash(3), self.statehash(2))
        self.mine_round_robin(POOLS, P_SLOW_WINDOW)

        # ------------------------------------------------------------------ restarts
        print('restart preserves the synchronous index')
        before = self.statehash(2)
        self.restart(2)
        self.sync_all(blocks_only=True)
        assert_equal(wait_yed_healthy(nodes[2])['height'], nodes[2].getblockcount())
        assert_equal(self.statehash(2), before)
        self.checkpoint('after restart')

        print('-reindex-yellowback and -reindex rebuild to the same hash; -prune is refused')
        self.restart(3, ['-reindex-yellowback'])
        wait_yed_healthy(nodes[3], timeout=120)
        assert_equal(self.statehash(3), before)
        self.restart(4, ['-reindex'])
        wait_yed_healthy(nodes[4], timeout=180)
        sync_blocks([nodes[2], nodes[4]], timeout=120)
        assert_equal(self.statehash(4), before)
        stop_node(nodes[OBSERVER], OBSERVER)
        nodes[OBSERVER] = None
        assert_start_raises_init_error(OBSERVER, tmpdir, self.node_args(OBSERVER, ['-prune=550']),
                                       '-yellowback is incompatible with -prune')
        self.restart(OBSERVER)
        self.sync_all(blocks_only=True)

        print('index_start_height_above_tip and params_mismatch_fails_loudly')
        for i in ENFORCING + [OBSERVER]:
            self.restart(i, ['-yellowbackstartheight=50'])
        for i in ENFORCING:
            wait_yed_healthy(nodes[i], timeout=120)
        rows = nodes[2].yed_gethistory(1, 60)
        assert_equal([r['height'] for r in rows], list(range(50, 61)))
        p49 = nodes[2].yed_getprice(49)
        assert_equal((p49['pFast'], p49['pMid'], p49['pSlow'], p49['pMint'], p49['pClaim']), (None,) * 5)
        assert_equal(nodes[2].yed_getinfo()['startHeight'], 50)
        assert_same_statehash(self.enforcing_nodes())
        for i in ENFORCING + [OBSERVER]:
            self.restart(i)
        for i in ENFORCING:
            wait_yed_healthy(nodes[i], timeout=120)
        assert_equal(self.statehash(2), before)
        self.restart(4, ['-yellowbacksigmaref=1'])
        wait_yed_healthy(nodes[4], timeout=120)
        assert_equal(nodes[4].yed_getinfo()['params']['sigmaRefBps'], 1)
        assert self.statehash(4) != self.statehash(2), 'a parameter mismatch must change the state hash'
        self.restart(4)
        wait_yed_healthy(nodes[4], timeout=120)
        assert_equal(self.statehash(4), self.statehash(2))
        self.sync_all(blocks_only=True)

        print('snap_history_contiguous, verifychain 4 20 (K6), verdict_parent_not_tip (N12)')
        tip = nodes[2].getblockcount()
        start = max(START_HEIGHT, tip - 2015)
        rows = nodes[2].yed_gethistory(start, tip)
        assert_equal(len(rows), tip - start + 1)
        for row in rows:
            assert_equal(row['blockHash'], nodes[2].getblockhash(row['height']))
        h0 = self.statehash(2)
        assert_equal(nodes[2].verifychain(4, 20), True)
        assert_equal(self.statehash(2), h0)
        assert_equal(nodes[2].yed_getinfo()['healthy'], True)
        assert_equal(nodes[2].yed_getinfo()['height'], tip)
        msg = rpc_error_message(nodes[2].yed_getblockverdict, nodes[2].getblockhash(tip - 2))
        assert msg.startswith('verdict-parent-not-tip'), msg
        msg = rpc_error_message(nodes[2].yed_getblockverdict, nodes[2].getblockhash(tip))
        assert msg.startswith('verdict-parent-not-tip'), msg

        # ------------------------------------------------------------------ vaults and a rejection
        print('two vaults from the raw builders; the first Python-built owner-path redemption through sendrawtransaction')
        self.mine_round_robin(POOLS, REF_LAG + 1)
        vault_a_txid, vault_a = mint_vault_raw(self, user, POOLS[0])
        vault_b_txid, vault_b = mint_vault_raw(self, user, POOLS[1])
        assert_equal(nodes[2].yed_getstats()['supplyCents'], 20_000)
        while user.getblockcount() < int(vault_b['lockHeight']):
            self.mine_round_robin(POOLS, 1)
        redeem_txid = redeem_vault_raw(self, user, POOLS[2], vault_a, [(vault_a_txid, 1)])
        closed = user.yed_getvault(vault_a_txid)
        assert_equal(closed['status'], 'CLOSED')
        assert_equal(closed['burnedCents'], 10_000)
        assert_equal(closed['closingTxid'], redeem_txid)
        assert_equal(nodes[2].yed_gettxinfo(redeem_txid)['verdict'], 'ok')
        assert_equal(nodes[2].yed_getstats()['supplyCents'], 10_000)
        self.checkpoint('after the redemption')

        print('rejected_survives_kill9: node 1 mines a malformed spend; kill -9 right after the rejection')
        rejected, bad_txid = mine_rejected_block(self, user, vault_b)
        wait_for_rejection(self.enforcing_nodes(), rejected)
        assert_banscore_zero(nodes)
        verdict = nodes[2].yed_getblockverdict(rejected)
        assert_equal(verdict['blockInvalid'], True)
        assert verdict['reason'].startswith('vault-spend-malformed:' + bad_txid), verdict
        self.kill9(2)
        self.restart(2, ['-yellowbackenforce=0'])
        wait_yed_healthy(nodes[2], timeout=120)
        sync_blocks([nodes[STOCK], nodes[2]], timeout=120)
        assert_equal(nodes[2].getbestblockhash(), nodes[STOCK].getbestblockhash())    # the reorg happened (N8)
        assert_equal(nodes[2].getblock(rejected)['confirmations'], 1)
        assert_equal(nodes[2].yed_getinfo()['rejectedBlocks'], 0)
        assert_equal(nodes[2].yed_getinfo()['enforcing'], False)
        # killswitch_rejected_hash_not_in_mapblockindex: the block index was never flushed after the
        # rejection, so the recorded hash is not in mapBlockIndex at start: skipped and cleared.
        assert debug_log_contains(tmpdir, 2, 'is not in the block index; skipped'), 'expected the skipped branch of the kill switch'
        assert_equal(user.yed_getvault(vault_b_txid)['status'], 'ACTIVE')                    # the enforcing nodes still refuse it
        assert_equal(nodes[2].yed_getvault(vault_b_txid)['unbacked'], True)                  # node 2 recorded the sweep
        print('the kill switch on cleanly stopped nodes: the rejected block is re-downloaded and accepted')
        # Ycash's RewindBlockIndex erases every index entry without a cached branch id (set only by
        # a successful ConnectBlock, main.cpp:3225) at start, so a rejected block is never in
        # mapBlockIndex when the loop runs — the "skipped" branch — and the node rejoins by
        # re-downloading it once the mark is gone (docs/mapping.md section 13.4).
        for i in (0, 3, 4):
            self.restart(i, ['-yellowbackenforce=0'])
            wait_yed_healthy(nodes[i], timeout=120)
        self.sync_all(blocks_only=True)
        assert_best_hash(nodes)
        assert debug_log_contains(tmpdir, 3, 'is not in the block index; skipped')
        for i in ENFORCING:
            assert_equal(nodes[i].yed_getinfo()['rejectedBlocks'], 0)
        assert_same_statehash(self.enforcing_nodes() + [nodes[OBSERVER]])

        print('ibd_across_accepted_invalid: a fresh enforcing node syncs across the accepted block (BLK-2 clause 3)')
        nodes[STOCK].generate(VALVE_BLOCKS + 1)
        self.sync_all(blocks_only=True)
        initialize_datadir(tmpdir, 6)
        nodes.append(start_node(6, tmpdir, yellowback_node_args()))
        connect_nodes_bi(nodes, 6, STOCK)
        sync_blocks([nodes[STOCK], nodes[6]], timeout=180)
        info = wait_yed_healthy(nodes[6], timeout=120)
        assert_equal(info['rejectedBlocks'], 0)
        assert_equal(info['suppressedBlocks'], 1)
        assert_equal(info['valveTripped'], False)
        assert_equal(info['enforcing'], True)
        assert debug_log_contains(tmpdir, 6, 'catch-up: accepted rule-breaking block')
        assert_equal(self.statehash(6), self.statehash(2))

        # ------------------------------------------------------------------ unhealthy across a reorg
        print('blk3_unhealthy_across_reorg: an unhealthy node follows a reorg without applying')
        self.restart(4, ['-yellowbacktestfault=storage:commit'])
        self.mine(POOLS[0])
        info = nodes[4].yed_getinfo()
        assert_equal(info['healthy'], False)
        assert_equal(info['enforcing'], False)
        stale = nodes[4].yed_getstatehash()['statehash'] if False else info['height']
        self.split_network()
        nodes[2].generate(1)
        self.sync_all(blocks_only=True)
        nodes[STOCK].generate(2)
        self.join_network()
        assert_best_hash(nodes)
        assert_equal(nodes[4].yed_getinfo()['healthy'], False)
        assert_equal(nodes[4].yed_getinfo()['height'], stale)
        self.restart(4, ['-reindex-yellowback'])
        wait_yed_healthy(nodes[4], timeout=120)
        assert_equal(self.statehash(4), self.statehash(2))

        print('killswitch_fresh_datadir: node 2 wiped and started with -yellowbackenforce=0 (K21)')
        stop_node(nodes[2], 2)
        nodes[2] = None
        shutil.rmtree(os.path.join(tmpdir, 'node2', 'regtest'))
        self.restart(2, ['-yellowbackenforce=0'])
        sync_blocks([nodes[STOCK], nodes[2]], timeout=300)
        wait_yed_healthy(nodes[2], timeout=120)
        assert_equal(self.statehash(2), self.statehash(3))

        # ------------------------------------------------------------------ the wipe-and-rebuild fallback
        print('crash_unflushed_chainstate beyond UNDO_KEEP: the wipe-and-rebuild fallback (N35)')
        self.restart(3)
        wait_yed_healthy(nodes[3], timeout=120)
        t0 = time.time()
        nodes[3].generate(UNDO_KEEP + 10)
        print('  mined %d blocks in %.0fs' % (UNDO_KEEP + 10, time.time() - t0))
        sync_blocks([nodes[2], nodes[3]], timeout=600)
        assert_equal(nodes[3].yed_getinfo()['height'], nodes[3].getblockcount())
        self.kill9(3)
        self.restart(3, timewait=300)
        wait_yed_healthy(nodes[3], timeout=300)
        sync_blocks([nodes[2], nodes[3]], timeout=600)
        assert_equal(nodes[3].yed_getinfo()['height'], nodes[3].getblockcount())
        assert_equal(self.statehash(3), self.statehash(2))
        assert debug_log_contains(tmpdir, 3, 'wiping index (undo record missing'), 'expected the wipe-and-rebuild fallback'
        self.sync_all(blocks_only=True)
        self.checkpoint('end')


if __name__ == '__main__':
    YellowbackIndexTest().main()
