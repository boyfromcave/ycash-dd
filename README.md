# Ycash v4.5.0 with the vault upgrade and Yellowback (YED)

This is the Ycash node, **plus a network upgrade** that adds a generic vault primitive to Ycash
consensus, and two applications built on it: a bridge template for wrapped YEC (wYEC) and
**Ycash Yellowback (YED)**, a decentralized, over-collateralised US-dollar stablecoin
(`1 YED = $1`). This branch (`upgrade/vault`) is a fork of upstream Ycash `v4.5.0`; the pristine
baseline is the `ycash-legacy` branch, and `git diff ycash-legacy...upgrade/vault` is the entire
delta. The ycashd 6.20.0 line (`boyfromcave/ycash6`, branch `upgrade/vault`) carries the same upgrade
with the same vault and YED rules, checked by golden test vectors shared by both lines. The branch `harden/yellowback` is the
no-upgrade fallback line, where Yellowback is a miner-enforced soft fork instead; everything below
describes `upgrade/vault` only.

**This is a hard fork.** The upgrade is `Consensus::UPGRADE_VAULT` with the new consensus branch ID
`0x6d5b7a31` ("Vault"). From its activation height, a node that has not upgraded (a stock
v4.5.0 node, say) can no longer follow the chain. **No public network has an activation height:**
mainnet and testnet leave `UPGRADE_VAULT` unset and name no YED attestor set, so today the upgrade
runs only on regtest and the one-laptop devnet.

## What the upgrade adds

**The vault primitive** (application-agnostic; `src/vault/`, workspace
`docs/plans/yellowback-upgrade-plan.md` §3 and §15):

- BIP68 relative lock-times (height-based only) and `OP_CHECKSEQUENCEVERIFY` (`0xb2`), plus two
  new opcodes: `OP_CHECKSETSIG` (`0xc0`) and `OP_CHECKSETDORMANT` (`0xc1`). Before the activation
  height they behave exactly as in v4.5.0.
- **Signer sets** as a consensus object: bonded members, unlock / cancel / slash thresholds, a
  per-epoch rate limit on unlocks, slashing on equivocation, and member and set liveness
  (dormancy). Set acts (`SET_CREATE`, `SET_JOIN`, `SET_HEARTBEAT`, …) travel in a `YV`
  `OP_RETURN`; the set state lives in its own database (`<datadir>/vaults/`) with per-block undo.
- **Templates:** the vault output **V** and the intent output **I**, both bare scripts. A set
  unlocks a vault into an intent; the intent is released to its recipient after a delay and can
  be cancelled during it; the owner has an owner branch, and recovers vault and intent alike once
  the set is dormant or wound down. A registered module may add rules on top (an `APP` branch).
- 21 RPCs (`set_*`, `vault_*`), present on every node with no flag:
  [doc/vault-rpc.md](doc/vault-rpc.md).

**The wYEC bridge template** (the Ycash side only): lock into a `WYEC`-tagged vault, an intent
posted by the bridge's signer set, release after the delay, cancel during it, owner recovery if the
set goes silent. Both signer shapes (one relayer with an open challenger set, or a 9-seat guardian
set) are configurations of the same primitive. The bridge needs no rule module: nothing about it
is in consensus beyond the primitive itself. The Ethereum side is not in this repository.

**Yellowback as the one registered rule module** (tag `YED\0`): from the activation height, YED's
block verdict is a **consensus** rule on every upgraded node, not a policy that miners choose to
enforce.

- A YED balance is an ordinary transparent output whose dollar value is declared in the
  transaction's `OP_RETURN` payload; every node keeps the same ledger in a rebuildable index under
  `<datadir>/yellowback/` and serves it through the `yed_*` RPCs (`rpcversion` 5).
- Collateral (YEC) sits in a V vault. The owner redeems with the owner branch by burning the debt.
  An underwater vault is claimed by moving its collateral into a claimant intent, which the YED
  attestor set may cancel during `CLAIM_DELAY` and anyone may release after it. If the attestor
  set goes dormant, the owner recovers the collateral without a burn.
- Prices come from two populations: pool quotes in coinbase tags, and bonded attestors who sign
  YEC/USD off-chain (v3 price attestation). Attestors register by joining the YED attestor set
  (`SET_JOIN`) and stay live with `SET_HEARTBEAT`s.
- Launch parameters on mainnet and testnet: class A (30–90 day) locks only, minting requires an
  armed attestation layer (`MINT_REQUIRES_ARMED`), $2,500 maximum mint, a 15 bps pool fee plus
  50 % again for the attestor, a 300 % global-ratio halt and a 600 % recapitalisation floor.
  Regtest keeps the v3 values.

**Retired on this line:** pool signalling and lock-in, the work valve, the kill switch, the sunset,
abandonment, `yed_sweep`, and the `-experimentalfeatures -yellowback` gate.
`-yellowbackenforce`, `-yellowbacksignal`, `-yellowbacktemplatepolicy` and
`-yellowbackrequirehealthy` are logged and ignored; `-yellowbackstartheight` and
`-yellowbackenforceuntil` are init errors. Because the YED rules are consensus, a node whose
Yellowback index is unhealthy stops rather than validate without it (restart with
`-reindex-yellowback`).

## Running it (regtest and devnet)

YED is live wherever `UPGRADE_VAULT` has a height and the YED attestor set is known. On regtest
that is two options, on top of the `-nuparams` the regtest harness already passes for Ycash's own
upgrades (`qa/rpc-tests/test_framework/yellowback_util.py`, `yellowback_node_args`):

```
-nuparams=6d5b7a31:<height>        # the vault upgrade
-yellowbackattestorset=<setid>     # the YED attestor set: the txid of its SET_CREATE
```

The set can only be created once the upgrade is active, so a node starts with the first option,
creates the set with `set_create` after the activation height, and restarts with both. A datadir
that holds a Yellowback index refuses to start with a wallet when YED is not live (pass
`-yellowback=0` to acknowledge that its YED outputs are spendable as plain YEC). YED refuses
`-prune`.

The quickest way to see all of it is the devnet, which does those steps for you:

```
contrib/yellowback/devnet/yellowback-devnet up     # 8 regtest nodes: vault upgrade active, attestor set created, YED armed
contrib/yellowback/devnet/upgrade-walk             # on an `up --role attestor --no-heartbeat --no-walk --no-sim` devnet: the whole ecosystem, end to end
contrib/yellowback/devnet/yellowback-devnet bridge up --shape guardians|relayer   # the wYEC bridge persona
```

See [contrib/yellowback/devnet/README.md](contrib/yellowback/devnet/README.md) (sections 0 and 6)
for the venv the scripts need and what each step does.

## Status

Implemented and tested on regtest and the devnet on both node lines. **Not adopted by the Ycash
Foundation, not audited, and not activated on any public network.** The launch gates (G-1 to G-10
of the workspace's hardening plan) and the parameter calibration are open; the release that passes
them is the one that sets the mainnet activation height and attestor set. A parameter change after
that is another network upgrade.

## Documentation

| Document | For whom |
|---|---|
| [doc/yellowback.md](doc/yellowback.md) | Users and node operators: `ycash-cli` usage, *Activation and enforcement since the vault upgrade*, backups, build and test baseline |
| [doc/vault-rpc.md](doc/vault-rpc.md) | Wallet, bridge and tool developers: the `set_*` / `vault_*` RPC contract (machine-readable: `doc/vault-rpc-contract.json`) |
| [doc/yellowback-rpc.md](doc/yellowback-rpc.md) | Wallet and tool developers: the `yed_*` RPC contract, `rpcversion` 5 |
| [doc/yellowback-attestor.md](doc/yellowback-attestor.md), [doc/yellowback-mining.md](doc/yellowback-mining.md) | Attestors and mining pools |
| [doc/yellowback-devnet.md](doc/yellowback-devnet.md) | A local devnet or a regtest node in a few minutes |
| `doc/yellowback-spec.md` | The YED protocol and trust statement, published from the workspace's plans by `make spec` |
| [doc/yellowback-review.md](doc/yellowback-review.md) | Reviewers: the review package for the fork delta |

Several of these still carry v2/v3 text about signalling, enforcement and the sweep; where they do,
`doc/yellowback.md`'s note at the top applies: that text is the history of the design, not this
node's behaviour. The design record (`docs/plans/yellowback-upgrade-plan.md`) and the
file-by-file crosswalk against DigiByte's DigiDollar (`docs/mapping.md` §22 for the upgrade) live in
the workspace that develops this fork (`yellowback-workspace`), not in this repository.

## Where the code is

| Path | Contents |
|---|---|
| `src/vault/` | The primitive: act codec (`act`), set-signature checker (`checker`), set-state database with undo (`db`), set state (`state`), V / I / bond templates (`template`), the module table (`module`), block and mempool glue (`node`) |
| `src/consensus/params.h`, `src/consensus/upgrades.cpp`, `src/chainparams.cpp` | `UPGRADE_VAULT`, branch ID `0x6d5b7a31`; no height on mainnet and testnet, `-nuparams` on regtest |
| `src/script/` (`interpreter`, `script`, `standard`, `sign`, `ismine`, `script_error`) | `OP_CHECKSEQUENCEVERIFY`, `OP_CHECKSETSIG`, `OP_CHECKSETDORMANT`; the V and I standard templates |
| `src/primitives/transaction.h`, `src/main.cpp`, `src/txmempool.cpp` | BIP68 sequence locks; block connect / disconnect of acts, template rules and the YED verdict; mempool acceptance |
| `src/miner.cpp`, `src/rpc/mining.cpp`, `src/policy/` | Block templates that apply acts and the YED module against a running copy of the set state; the `YV` act size limit |
| `src/yellowback/` | YED: params, payload, scripts, address, attestation, index, state, the rule module (`module`), policy checks, transaction builder, wallet glue |
| `src/rpc/vault.cpp`, `src/rpc/yellowback.cpp`, `src/rpc/yellowbackwallet.cpp` | `set_*` / `vault_*` RPCs; node and wallet `yed_*` RPCs |
| `src/test/vault_*_tests.cpp`, `src/test/yellowback_*_tests.cpp` | Unit, vector and fuzz tests |
| `qa/rpc-tests/vault_*.py`, `qa/rpc-tests/yellowback_*.py` | Functional tests on regtest (`qa/rpc-tests/test_framework/vault.py` is an independent Python implementation of the primitive) |
| `contrib/yellowback/` | The pool quote agent and kit, the attestor agent (`attest/`, Rust), and `devnet/`: `yellowback-devnet`, `upgrade-walk`, `bridge-sim`, `yellowback-sim` |
| `.github/workflows/yellowback-tests.yml` | CI for all of the above |

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

Tests for the vault and Yellowback code:

```
src/test/test_bitcoin --run_test='vault_*'
src/test/test_bitcoin --run_test='yellowback_*'
qa/pull-tester/rpc-tests.py -j4 --nozmq vault_upgrade vault_primitive vault_bridge yellowback_upgrade \
    yellowback_lifecycle yellowback_claim yellowback_void_mint yellowback_wallet_restore yellowback_sapling
python3 -m unittest contrib/yellowback/test_yellowback_price.py contrib/yellowback/test_yellowback_quote.py
```

## Deprecation Policy

This release is considered deprecated at a certain block height,
at which point the node will halt and you will need to upgrade.
See the release notes for the deprecation height for this release.

## License

For license information see the file [COPYING](COPYING).
