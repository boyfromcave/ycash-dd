#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Split the push tier's functional scripts across the CI matrix of .github/workflows/yellowback-tests.yml.

    qa/yellowback-ci-shard.py --shard K --of N NAME...   print the names of shard K (1-based) of N
    qa/yellowback-ci-shard.py --plan N NAME...           print every shard of N with its estimated time
    qa/yellowback-ci-shard.py --check NAME...            fail unless, for every N in 1..8, the shards
                                                         partition NAME... (each name in exactly one
                                                         shard, none empty while names remain); warns
                                                         about names without a measured weight

Longest-processing-time first: names sorted by weight (descending, then by name), each given to the
least-loaded shard (lowest index on a tie). Deterministic, so every matrix job computes the same split
from the same list without talking to the others. A name the table does not know weighs DEFAULT; it
still runs, in some shard, and --check says so (re-measure and add it).

Standard library only (CI runs it with the runner's python3).
"""

import argparse
import sys

# Seconds per script. "m" rows were measured one script at a time on 2026-10-06 (integration tree
# a903fae2f, three scripts in parallel on a laptop at load 20-36, so absolute values run high); the
# rest are estimates from the scripts' size and the old single job's 36 min at -j4. Only the ratios
# matter: rpc-tests.py runs each shard's scripts -j4. Re-measure from a CI run's per-script durations
# (rpc-tests.py prints them) and replace the estimates.
WEIGHTS = {
    "yellowback_index": 440,            # m
    "yellowback_claim": 325,            # m
    "yellowback_interm": 160,           # m (2026-10-08, in-term line: 208 s beside yellowback_index at 587 s, scaled to its 440)
    "yellowback_mining": 290,           # m
    "yellowback_lifecycle": 265,        # m
    "yellowback_void_mint": 260,        # m
    "yellowback_wallet_restore": 250,   # m
    "yellowback_mint_armed": 120,       # m
    "yellowback_upgrade": 75,           # m
    "yellowback_hardening": 400,
    "yellowback_enforcement": 400,      # harden line only (retired by the upgrade)
    "yellowback_attest_enforcement": 300,  # harden line only
    "yellowback_activation": 250,       # harden line only
    "vault_primitive": 400,
    "yellowback_attest": 350,
    "yellowback_attest_wallet": 300,
    "yellowback_sapling": 300,
    "yellowback_wallet_lifecycle": 300,
    "vault_rpc": 250,
    "vault_rpc_contract": 120,
    "yellowback_stock_node": 200,
    "yellowback_stratum": 200,
    "yellowback_chainviz": 200,
    "vault_slashing": 200,
    "vault_bridge": 200,
    "yellowback_pricefeed": 150,
    "yellowback_quote": 150,
    "wallet": 150,
    "yellowback_rpc_contract": 120,
    "vault_upgrade": 90,
    "yellowback_framework_smoke": 60,
    "rawtransactions": 60,
    "mempool_reorg": 40,
    "mempool_tx_expiry": 40,
    "reorg_limit": 40,
    "reindex": 40,
    "txn_doublespend": 30,
}
DEFAULT = 240


def split(names, n):
    """Return n lists (some possibly empty when there are fewer names than shards)."""
    order = sorted(dict.fromkeys(names), key=lambda s: (-WEIGHTS.get(s, DEFAULT), s))
    shards = [[] for _ in range(n)]
    load = [0] * n
    for name in order:
        k = min(range(n), key=lambda i: (load[i], i))
        shards[k].append(name)
        load[k] += WEIGHTS.get(name, DEFAULT)
    return shards, load


def check(names):
    rc = 0
    uniq = list(dict.fromkeys(names))
    if len(uniq) != len(names):
        dup = sorted({s for s in names if names.count(s) > 1})
        print("duplicate names in the CI lists: %s" % " ".join(dup))
        rc = 1
    for n in range(1, 9):
        shards, _ = split(names, n)
        flat = [s for sh in shards for s in sh]
        if sorted(flat) != sorted(uniq):
            print("N=%d: the shards do not partition the list" % n)
            rc = 1
        if len(uniq) >= n and any(not sh for sh in shards):
            print("N=%d: an empty shard although %d names remain" % (n, len(uniq)))
            rc = 1
    unknown = [s for s in uniq if s not in WEIGHTS]
    if unknown:
        print("warning: no measured weight (counted as %d s): %s" % (DEFAULT, " ".join(unknown)))
    if rc == 0:
        print("shards: %d names partition cleanly for N = 1..8" % len(uniq))
    return rc


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--shard", type=int)
    p.add_argument("--of", type=int)
    p.add_argument("--plan", type=int)
    p.add_argument("--check", action="store_true")
    p.add_argument("names", nargs="*")
    a = p.parse_args(argv)
    # A quoted list ("$YELLOWBACK_SCRIPTS") and separate words mean the same.
    a.names = [w for arg in a.names for w in arg.split()]
    if not a.names:
        p.error("no names")
    if a.check:
        return check(a.names)
    if a.plan:
        shards, load = split(a.names, a.plan)
        for k, (sh, t) in enumerate(zip(shards, load), 1):
            print("shard %d/%d  ~%4d s serial  %s" % (k, a.plan, t, " ".join(sh)))
        return 0
    if a.shard is None or a.of is None or not 1 <= a.shard <= a.of:
        p.error("--shard K --of N with 1 <= K <= N")
    shards, _ = split(a.names, a.of)
    print(" ".join(shards[a.shard - 1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
