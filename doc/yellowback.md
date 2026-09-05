# Yellowback (experimental)

Yellowback is a federated, over-collateralised US-dollar stablecoin **overlay** on Ycash. It is a
Tier-0 feature: no consensus change, no policy change, no new opcodes and no network upgrade. A
Yellowback balance is an ordinary transparent output whose dollar value is declared in the
transaction's single `OP_RETURN` payload; collateral sits in a P2SH vault of
`CLTV + owner key + k-of-n federation keys`; the YEC/USD price is published by the federation
spending a well-known anchor UTXO. Every node that runs with `-yellowback` computes the same Yellowback
state from the same chain in a self-contained, rebuildable index under `<datadir>/yellowback/`.

Nodes that do not enable the feature are unaffected in every way.

The normative protocol, the design record and the trust statement live in the workspace that
develops this fork (`docs/plans/yellowback-v1-development-plan.md`); this file is the user-facing
guide and will grow with each phase.

## Status

| Phase | State |
|---|---|
| 0 — groundwork (test framework fix, CI, baseline) | done except the inherited functional-test baseline run |
| 1 — pure protocol library (`src/yellowback/{params,math,payload,script,address}`) | done |
| 2 — state machine, index, node RPCs | done |
| 3 — wallet RPCs (mint, send, redeem, co-sign) | done |
| 4 — federation coordinator (`contrib/yellowback/`) | done |
| 5 — protections (DCA, ERR, volatility) | done |
| 6 — hardening and review | in progress (fuzz targets, stress test, review package written; external review pending) |

## Enabling

```
experimentalfeatures=1
yellowback=1
```

`-yellowback` refuses to start with `-prune` (the index rebuilds from blocks on disk). Other
options: `-reindex-yellowback` (wipe and rebuild the index), `-yellowbackfee=<zat>` (flat fee, minimum
1000), `-yellowbackmintlag=<blocks>` (default 2), `-debug=yellowback`. Regtest additionally takes
`-yellowbackstartheight`, `-yellowbackgenesisanchor`, `-yellowbackgenesisroster` (all three together) and
`-yellowbacksupplycap`.

## Using Yellowback from `ycash-cli`

Every amount is in **cents** (`10000` = $100.00). The node must have caught its index up
(`yed_getinfo` → `synced: true`, `healthy: true`) before any of these work.

```
ycash-cli yed_getinfo                       # index height, synced, healthy, anchor, roster index
ycash-cli yed_getstats                      # supply, collateral, price, health, DCA, ERR, freeze
ycash-cli yed_getprotectionstatus           # the three protection systems in one view
ycash-cli yed_getnewaddress                 # a YED address (ye… on mainnet)
ycash-cli yed_getbalance
ycash-cli yed_estimatecollateral 10000 1    # YEC needed now to mint $100 at tier 1 (30 days)
ycash-cli yed_mint 10000 1                  # mint; back up wallet.dat afterwards
ycash-cli yed_listpositions                 # your vaults: status, unlock height, required burn, canRedeem
ycash-cli yed_send ye… 2500                 # send $25.00
ycash-cli yed_listtransactions
```

A mint locks YEC in a vault until the tier's unlock height. The collateral requirement is fixed at
the moment you sign (it is evaluated at the index tip minus two blocks), so what `yed_mint` reports
is what the vault holds. Minting is refused when the system health is below 100 % (ERR), while
minting is frozen after a volatility breach, when no price is in effect, or when the supply cap
has no room; `yed_getprotectionstatus.mintingAllowed` says which.

Redeeming (getting the collateral back) burns YED equal to the mint (more during ERR) and needs
the federation's co-signature. From the unlock height on:

```
contrib/yellowback/yellowback-redeem --rpc-url http://user:pass@127.0.0.1:8232 \
    --vault <mint txid> --endpoints-file operators.txt
```

which runs `yed_redeem` on your node, collects `k` co-signatures from the operators' `/cosign`
endpoints and submits through `yed_submitredeem`. Your node re-verifies the returned transaction
before broadcasting it; the operators cannot change where the collateral goes. If the deadline (36
blocks after `yed_redeem`) passes, the client aborts with `yed_abortredeem` and you start over.
Without the client: `yed_redeem <txid>` gives you the hex, each operator's `yed_cosignredeem`
adds a signature, and `yed_submitredeem <hex>` broadcasts.

Never spend a YED output with a plain YEC command: the YED it carries is burned. The wallet locks
every YED output it owns (`listlockunspent` shows them) so `sendtoaddress` and friends cannot pick
them by accident; `lockunspent true` on one of them removes that protection.

## Rebuilding the index

The index lives under `<datadir>/yellowback/` and is rebuilt from the blocks on disk when it is
missing, when the node was reindexed, or on `-reindex-yellowback`. `yed_getinfo.healthy: false`
names the reason and always means "restart with `-reindex-yellowback`". Every `yed_*` call except
`yed_getinfo` refuses while the index is unhealthy or behind the chain tip.

## Trust statement

Yellowback v1 is a federated, over-collateralised stablecoin overlay on Ycash.

- Consensus-enforced (by every Ycash node, upgraded or not): collateral cannot leave a vault before
  its lock height; only the owner *and* k of n federation keys can spend it; price updates carry k of
  n federation signatures.
- Enforced by every Yellowback-aware node deterministically: Yellowback accounting (conservation, supply,
  collateral totals, vault status, DCA/ERR/volatility state).
- Enforced by the federation's mechanical policy: collateral is released only against the required
  burn; published prices reflect market medians.
- **Therefore:** a colluding quorum of k operators can release collateral without a burn or publish
  a false price. A federation with fewer than k live keys halts redemptions and mints until it
  recovers. Nothing the federation does can create Yellowback out of nothing, move a user's Yellowback, or
  take collateral without the owner's signature.

## Backups

Ycash transparent keys are a random keypool, not derived from a seed. The vault owner key of every
mint lives only in `wallet.dat`. **Back up `wallet.dat` after every mint.** Wallet encryption in
Ycash is experimental; protect the file with full-disk encryption, keep RPC on localhost and hold an
offline copy.

## Baseline test run (Phase 0)

Recorded here once the Phase 0 build completes; see the section "Build and test baseline" below.

### Build and test baseline

Host: macOS 26 (Darwin 25.0.0), Apple Silicon, Apple clang 17, GNU make 3.81. Recorded 2026-09-05
at `ycash-legacy` = v4.5.0 plus the Yellowback commits.

- `zcutil/build.sh` (without `YCASH_WR=1`) builds `ycashd`, `ycash-cli`, `ycash-tx` and
  `src/test/test_bitcoin` on this host with three host-side conditions that are not fork changes:
  Homebrew `automake` and GNU `libtool` on the PATH (`LIBTOOLIZE=glibtoolize`; libevent's
  `autoreconf` needs them), GNU coreutils' `sha256sum` on the PATH for `zcutil/fetch-params.sh`
  (macOS ships a BSD `sha256sum` whose flags differ), and `CARGO_TARGET_DIR` pointed at
  `<repo>/target` when the user's shell sets a global cargo target directory (the Makefile links
  `target/<triple>/release/librustzcash.a` relative to the repo). The `YCASH_WR=1` build has not
  been run yet.
- Depends: the native `aarch64-apple-darwin` toolchain (clang 18.1.8, rust, boost, libevent,
  zeromq, libsodium, utfcpp, googletest, bdb) builds from `depends/` unchanged.
- `src/test/test_bitcoin --run_test='yellowback_*'`: 28 cases green.
- `qa/rpc-tests/yellowback_index.py`: green (about five minutes, four nodes).
- Full `src/test/test_bitcoin` (450 cases): 2 failures, both pre-existing at the pin and in files
  the fork does not touch — `main_tests/subsidy_limit_test` (subsidy sum) and
  `rpc_wallet_tests/rpc_z_sendmany_internals`. Everything else passes, including all 28
  `yellowback_*` cases.
- `qa/rpc-tests/yellowback_lifecycle.py`, `yellowback_void_mint.py`, `yellowback_wallet_restore.py`,
  `yellowback_federation.py`, `yellowback_protection.py`: green (five nodes each, 5-12 minutes).
- The inherited `qa/pull-tester/rpc-tests.py` baseline (which tests at the pin pass on Ycash after
  the `ycash.conf` and `src/ycashd` framework fixes) and the `YCASH_WR=1` build are still to be run.
- Python 3.12+ note: the inherited `test_framework/mininode.py` imports `asyncore` (removed in
  3.12) and `pyblake2` (unmaintained). The workspace venv carries `pyasyncore` and a one-line
  `pyblake2` shim over `hashlib.blake2b`; no framework file is changed.
