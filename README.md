# Ycash v4.5.0 with Yellowback (YED)

This is the Ycash node, **plus Ycash Yellowback (YED)**: a decentralized digital dollar on Ycash,
built the way DigiByte's DigiDollar is built but adapted to what Ycash actually has. This branch
(`feature/yellowback-sf`) is a fork of upstream Ycash `v4.5.0` (the pristine baseline is the
`ycash-legacy` branch; `git diff ycash-legacy...feature/yellowback-sf` is the entire delta). The
earlier federation prototype is kept on `feature/digidollar` as a record.

**Yellowback is experimental and off by default.** A node that does not enable it runs upstream
Ycash v4.5.0's code paths: same consensus, same policy, same P2P, same RPC surface.

## What Yellowback is

Yellowback v2 is a miner-enforced, over-collateralised US-dollar stablecoin **overlay** — a soft
fork in the P2SH/CLTV sense, enforced by the mining pools that run the module. YED is the unit
(`1 YED = $1`). The design, the protocol and what it costs are in the workspace plan; the node
guide is [doc/yellowback.md](doc/yellowback.md).

- A YED balance is an ordinary transparent output whose dollar value is declared in the
  transaction's single `OP_RETURN` payload.
- Collateral (YEC) sits in a P2SH vault with an owner path (the minter's key, after the lock
  height) and an anyone-can-claim path (after the grace period); enforcing miners reject a block
  that releases collateral without the matching YED burn.
- The YEC/USD price is the median of quotes that pools publish as a tag in their own coinbase.
- Every node started with `-yellowback` computes the same Yellowback state from the same chain
  in a self-contained, rebuildable index under `<datadir>/yellowback/`, and exposes it through
  the `yed_*` RPCs.

**This tree is in transition:** the code on this branch is still the federation prototype, being
replaced phase by phase (see the status table in [doc/yellowback.md](doc/yellowback.md)). The
trust statement there is the v2 one.

## Enabling Yellowback

```
experimentalfeatures=1
yellowback=1
```

`-yellowback` refuses to start with `-prune`. See [doc/yellowback.md](doc/yellowback.md) for the
other options, the `ycash-cli` walkthrough (mint, send, redeem), index rebuilds and backups.

## Yellowback documentation

| Document | For whom |
|---|---|
| [doc/yellowback.md](doc/yellowback.md) | Users and node operators: enabling, `ycash-cli` usage, trust statement, backups, build and test baseline |
| [doc/yellowback-rpc.md](doc/yellowback-rpc.md) | Wallet and tool developers: the `yed_*` RPC contract |
| `doc/yellowback-spec.md` | The v2 protocol (§3 of the plan) and the trust statement (§8.1), published verbatim by the workspace's `make spec` |
| [doc/yellowback-review.md](doc/yellowback-review.md) | Reviewers: the review package for the fork delta |
| [contrib/yellowback/](contrib/yellowback/README.md) | The price-feed layer of the prototype's coordinator (Phase 7 turns it into the quote agent) and the one-laptop devnet |

The normative protocol, the decision record and the file-by-file crosswalk against DigiByte's
DigiDollar live in the workspace that develops this fork (`yellowback-workspace`:
`docs/plans/yellowback-v1-development-plan.md`, `docs/mapping.md`), not in this repository.

## Where the Yellowback code is

| Path | Contents |
|---|---|
| `src/yellowback/` | Protocol library and state machine: params, payload, scripts, address, index, state, policy checks, transaction builder |
| `src/rpc/yellowback.cpp`, `src/rpc/yellowbackwallet.cpp` | Node and wallet `yed_*` RPCs |
| `src/test/yellowback_*_tests.cpp` | Unit tests (`src/test/test_bitcoin --run_test='yellowback_*'`) |
| `qa/rpc-tests/yellowback_*.py` | Functional tests on regtest |
| `contrib/yellowback/` | Coordinator, redemption client, source-layer unit tests, and `devnet/yellowback-devnet` (a private Yellowback network on one machine for trying the wallet) |
| `.github/workflows/yellowback-tests.yml` | CI for all of the above |

Nothing under `src/consensus/`, `src/script/`, `src/main.cpp`, `src/pow/` or
`src/primitives/` is changed by this fork.

## Getting Started

Ycash is a digital currency. For more information, see https://y.cash.

For a comparison of Ycash to Bitcoin, see https://y.cash/fact-sheet.

This software is the Ycash node. It downloads and stores the entire history
of Ycash transactions; depending on the speed of your computer and network
connection, the synchronization process could take a day or more once the
blockchain has reached a significant size.

**Ycash is experimental and a work-in-progress.** Use at your own risk.

Ycash is a chain fork of Zcash. For most topics, you can rely on Zcash-related documentation, including the [user guide for zcashd](https://zcash.readthedocs.io/en/latest/rtd_pages/zcashd.html).

## Need Help?

* Visit https://y.cash
* Ask for help in one of the [Ycash forums](https://y.cash/forums).

## Code of Conduct

Participation in the Ycash project is subject to a
[Code of Conduct](code_of_conduct.md).

## Building

Follow the instructions for building Zcash:

https://zcash.readthedocs.io/en/latest/rtd_pages/zcashd.html#install

For any Ycash-specific build instructions, see the release notes. Host-specific notes for
building this fork on macOS (Apple Silicon) are recorded in
[doc/yellowback.md](doc/yellowback.md) under "Build and test baseline".

Tests for the Yellowback code:

```
src/test/test_bitcoin --run_test='yellowback_*'
qa/pull-tester/rpc-tests.py -j4 --nozmq yellowback_index yellowback_lifecycle yellowback_void_mint \
    yellowback_wallet_restore yellowback_sapling
python3 -m unittest contrib/yellowback/test_yellowback_fed.py
```

## Deprecation Policy

This release is considered deprecated at a certain block height,
at which point the node will halt and you will need to upgrade.
See the release notes for the deprecation height for this release.

## License

For license information see the file [COPYING](COPYING).
