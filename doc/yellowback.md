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
| 0 — groundwork (test framework fix, CI, baseline) | in progress |
| 1 — pure protocol library (`src/yellowback/{params,amount,payload,script,address}`) | not started |
| 2 — state machine, index, node RPCs | not started |
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

_To be filled in: host, `zcutil/build.sh` result with and without `YCASH_WR=1`, `make check`
result, and the list of `qa/pull-tester/rpc-tests.py` scripts that pass at the pin after the
`ycash.conf` framework fix._
