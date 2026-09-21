# Scenario 3 — "I am a mining pool" (the `pool` seat, node 4)

## Before you start

`yellowback-devnet` is a script in `contrib/yellowback/devnet/`, not a command on your PATH, and
its `#!/usr/bin/env python3` must find the **workspace venv's** Python: it imports the inherited
test framework from `qa/rpc-tests/`, which needs `simplejson` — present in the venv, absent from
the system Python 3.9 that macOS ships. Three lines, once per terminal, make every command below
work verbatim from any directory:

```bash
cd <workspace>/ycash-dd
source ../.venv/bin/activate
export PATH="$PWD/contrib/yellowback/devnet:$PATH"
```

Or skip them and spell each command out from the `ycash-dd` directory: `../.venv/bin/python
contrib/yellowback/devnet/yellowback-devnet <command>`.

You also need `src/ycashd` built and the attestor agent built (`cd contrib/yellowback/attest &&
cargo build --release`; `YELLOWBACK_ATTEST_BIN` points at it if the search does not find it). If a
devnet already exists in `~/yb-devnet`, `up` refuses and tells you to pass `--force`, which
rebuilds over it.

```bash
yellowback-devnet up --role pool        # ~3 min; node 4 is a plain miner: no payout, no signal
yellowback-devnet cli --node 4 -- yed_getinfo
```

No GUI: pools are headless, which is the point (D-2). This is how a pool operator meets
Yellowback — one more process, one more config file, one more thing that can page them at 3 a.m.

**Automated around you:** 2 other pools (nodes 2 and 3) quoting and signalling, 4 attestors,
the heartbeat on *those two pools only* (your blocks are always yours to mine), the price walk,
the simulated population and the liquidator.

## Walk-through

1. **A plain miner.** Mine a few blocks (`yellowback-devnet mine 3 4`). Observe that your blocks
   carry no quote (`cli -- yed_gettag <height>` → `found: false`; MINER-2) and you earn no
   Yellowback fees (`yed_listminers` does not know you).
   - notes:

2. **Become a pool.** `yellowback-devnet pool 4 configure` restarts node 4 with its payout
   address and `-yellowbacksignal=1` (what an operator does by editing `ycash.conf` — read
   `doc/yellowback-mining.md` §2 and check the two lines match). Mine again. Watch
   `yed_listminers` move you to registered (`N_REG` = 24 tagged blocks) and then eligible, and
   watch fees start arriving (`getbalance` on node 4; a mint's `payee`).
   - notes:

3. **The real quote agent.** `yellowback-devnet pool 4 quote start` runs `yellowback-quote
   --mock-price` beside your node — the path an actual pool runs — instead of `yed_setquote`
   by hand. `cli --node 4 -- yed_getinfo` → `miner.quoteKind` "quote", `quoteAgeSeconds`.
   Then `pool 4 quote stop` and watch the quote go stale past `-yellowbackquotemaxage`
   (120 s here): `quoteKind` falls back to "signal".
   - notes:

4. **Stop signalling.** `yellowback-devnet pool 4 signal off` (you keep mining and quoting,
   your tags carry no signal bit). With 2 of 3 still signalling you stay above the 60 %
   threshold. Now `pool 3 signal off` and watch: minting pauses below 60 %
   (`yed_getstats.mintingAllowed`, `haltMask` PARTICIPATION), rejection pauses below 50 %
   (`yed_getactivation.enforcementSuspended`), then recovery at 75 % / 60 % once you `signal
   on` again. Mine your share while you watch (`mine 5 4`); the window is 64 blocks.
   - notes:

5. **Get pinned.** Stop your agent and quote a constant: `cli --node 4 -- yed_setquote
   50000000 1` while the walk moves the attestors; after `PIN_WINDOW` (16) blocks with the
   cross-section 5 % away, PIN-1 marks you pinned (`yed_getprice.pinnedKeys`, `yed_listminers`)
   and drops you from the medians. `pool 4 quote start` to recover.
   - notes:

6. **Read `doc/yellowback-mining.md`** as a pool operator would. Does it answer the questions
   this exercise raised? Which section did you need that was not there?
   - notes:

## What we want to know

- Is the operational burden on a pool acceptable — one more agent, one more config, one more
  thing to monitor?
- Are the failure modes discoverable from `yed_getinfo` alone, without a dashboard, since that
  is what a daemon operator has? If **no**, the fix is more likely better fields and a clearer
  mining runbook than a GUI — the finding R8 is waiting on before anything is built.
- A pool that finds this annoying simply will not run it, and the whole design rests on pools
  running it.

```bash
yellowback-devnet report
yellowback-devnet down --wipe
```
