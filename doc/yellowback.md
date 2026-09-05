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
| 3 — wallet RPCs (mint, send, redeem, co-sign) | not started |
| 4 — federation coordinator (`contrib/yellowback/`) | not started |
| 5 — protections (DCA, ERR, volatility) | not started |
| 6 — hardening and review | not started |

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
- The full `test_bitcoin` run and the inherited `qa/pull-tester/rpc-tests.py` baseline (which
  tests at the pin pass on Ycash after the `ycash.conf` and `src/ycashd` framework fixes) are
  to be recorded here once run.
