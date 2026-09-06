# Ycash v4.5.0 with Yellowback (YED)

This is the Ycash node, **plus Ycash Yellowback (YED)**: a decentralized digital dollar on Ycash,
built the way DigiByte's DigiDollar is built but adapted to what Ycash actually has. This branch
(`feature/digidollar`) is a fork of upstream Ycash `v4.5.0` (the pristine baseline is the
`ycash-legacy` branch; `git diff ycash-legacy...feature/digidollar` is the entire delta).

**Yellowback is experimental and off by default.** A node that does not enable it behaves exactly
like upstream Ycash v4.5.0: same consensus, same policy, same P2P, same RPC surface.

## What Yellowback is

Yellowback is a federated, over-collateralised US-dollar stablecoin **overlay**. YED is the unit
(`1 YED = $1`). Nothing in consensus changed: no new opcodes, no soft fork, no network upgrade.

- A YED balance is an ordinary transparent output whose dollar value is declared in the
  transaction's single `OP_RETURN` payload.
- Collateral (YEC) sits in a P2SH vault of `CLTV + owner key + k-of-n federation keys`, so it can
  leave only after the lock height, and only with the owner *and* a federation quorum signing.
- The YEC/USD price is published by the federation spending a well-known anchor UTXO; the
  federation's coordinator reads it from the exchanges that list YEC and from BTC/USD references.
- Every node started with `-yellowback` computes the same Yellowback state from the same chain
  in a self-contained, rebuildable index under `<datadir>/yellowback/`, and exposes it through
  the `yed_*` RPCs.

The trust statement (what a colluding federation quorum can and cannot do) is in
[doc/yellowback.md](doc/yellowback.md).

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
| [doc/yellowback-federation.md](doc/yellowback-federation.md) | Federation operators: host and key ceremony, coordinator configuration, price sources, redemption co-signing, incidents |
| [doc/yellowback-review.md](doc/yellowback-review.md) | Reviewers: the review package for the fork delta |
| [contrib/yellowback/](contrib/yellowback/README.md) | The federation coordinator and the user-side redemption client (Python, run beside `ycashd`) |

The normative protocol, the decision record and the file-by-file crosswalk against DigiByte's
DigiDollar live in the workspace that develops this fork (`yellowback-workspace`:
`docs/plans/yellowback-v1-development-plan.md`, `docs/mapping.md`), not in this repository.

## Where the Yellowback code is

| Path | Contents |
|---|---|
| `src/yellowback/` | Protocol library and state machine: params, payload, scripts, address, index, state, policy checks, transaction builder |
| `src/rpc/yellowback.cpp`, `src/rpc/yellowbackwallet.cpp` | Node and wallet `yed_*` RPCs |
| `src/test/yellowback_*_tests.cpp` | Unit tests (`src/test/test_bitcoin --run_test='yellowback_*'`) |
| `qa/rpc-tests/yellowback_*.py` | Functional tests on regtest, including a three-operator federation |
| `contrib/yellowback/` | Coordinator, redemption client, source-layer unit tests |
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
qa/pull-tester/rpc-tests.py yellowback_index yellowback_lifecycle yellowback_void_mint \
    yellowback_wallet_restore yellowback_federation yellowback_protection yellowback_sapling
python3 -m unittest contrib/yellowback/test_yellowback_fed.py
```

## Deprecation Policy

This release is considered deprecated at a certain block height,
at which point the node will halt and you will need to upgrade.
See the release notes for the deprecation height for this release.

## License

For license information see the file [COPYING](COPYING).
