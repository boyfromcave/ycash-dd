# Ycash Yellowback (YED) — node guide

Ycash Yellowback (YED) is a decentralized, over-collateralised US-dollar stablecoin **overlay** on
Ycash. **Yellowback v2 is a miner-enforced soft fork** (plan §1): Tier 1 mining policy plus one
block-validity hook in `main.cpp` that is inert until an activation derived from the chain itself,
that only ever fires on transactions spending a Yellowback vault, that never bans a peer, that fails
open only on a storage failure, and that an operator can switch off with one flag. Every block an
enforcing miner produces is valid to a stock node; a stock miner's block is rejected only if it
spends a vault in a way the rules forbid — a soft fork in the P2SH/CLTV sense. Everything else is an
overlay: ordinary Ycash v4 transactions, one `OP_RETURN` payload, a self-contained index under
`<datadir>/yellowback/`. A node that does not enable the feature runs v4.5.0's code paths.

The normative protocol is `doc/yellowback-spec.md` (§3 of the workspace plan, published verbatim
by `make spec`); the design record, the decisions and the phase plan are the workspace's
`docs/plans/yellowback-v2-development-plan.md`. This file is the user-facing node guide.

## Status

| Phase | State |
|---|---|
| 0 — branch `feature/yellowback-sf`, strip the federation, re-baseline | node side done (this tree); wallet side and workspace manifest in the same series |
| 1 — pure library (params, math, tag, payload, script) | done (this tree) |
| 2 — state machine and view (v2 rules, `EvaluateBlock`) | done (this tree; the 8 CPU-hour fuzz run is the Linux jobs') |
| 3 — index hooks, node RPCs, `rpcversion 2` | not started |
| 4 — mining policy (template filter, coinbase tag) | not started |
| 5 — enforcement (the `main.cpp` hook), devnet on the v2 topology | not started |
| 6 — wallet RPCs (`yed_mint`, `yed_redeem`, `yed_claim`, `yed_sweep`) | not started |
| 7 — quote agent (`contrib/yellowback/`) | in progress (built beside the prototype's coordinator) |
| 7b — YecWallet screens | not started |
| 8–10 — hardening, testnet, mainnet | not started |

## Federation prototype, being replaced by phase

Everything below this line describes the **federation prototype, being replaced by phase**: the
code on this branch is still the prototype's overlay (a k-of-n federation script in the vault,
anchor-chain PRICE transactions, DCA/ERR/volatility protections), minus the federation itself,
which Phase 0 removed from the node. Until Phase 6 lands the v2 wallet flows:

- minting, sending and the index work as before on regtest, with the price published by the test
  harness (`qa/rpc-tests/test_framework/yellowback_util.py: publish_price`) instead of the
  removed `yed_createpricetx`;
- **redemption is out of service**: `yed_redeem` still builds and owner-signs the v1 redemption,
  but the vault script needs the retired federation's signatures, which no node can add
  (`yed_cosignredeem`, `yed_submitredeem`, `yed_abortredeem` are gone); the functional tests add
  them from the roster nodes' keys in Python (`cosign_and_submit`);
- `yed_getroster` is gone (`yed_getinfo.rosterIndex` remains until Phase 3); the coordinator's
  price rounds, the `/cosign` endpoint, the `yellowback-redeem` client and the one-laptop devnet
  are out of service (see `contrib/yellowback/README.md`).

### Enabling

```
experimentalfeatures=1
yellowback=1
```

`-yellowback` refuses to start with `-prune` (the index rebuilds from blocks on disk). Other
options: `-reindex-yellowback` (wipe and rebuild the index), `-yellowbackfee=<zat>` (flat fee, minimum
1000), `-yellowbackmintlag=<blocks>` (default 2), `-debug=yellowback`. Regtest additionally takes
`-yellowbackstartheight`, `-yellowbackgenesisanchor`, `-yellowbackgenesisroster` (all three together,
until Phase 3 replaces them) and `-yellowbacksupplycap`.

### Using Yellowback from `ycash-cli`

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
ycash-cli yed_mint 10000 1 ys1...           # the same, funded from that Sapling address in one transaction
ycash-cli yed_mint 10000 1 s1...            # ... or from that transparent address only
ycash-cli yed_listpositions                 # your vaults: status, unlock height, required burn, canRedeem
ycash-cli yed_send ye… 2500                 # send $25.00
ycash-cli yed_listtransactions
```

A mint locks YEC in a vault until the tier's unlock height. The collateral requirement is fixed at
the moment you sign (it is evaluated at the index tip minus two blocks), so what `yed_mint` reports
is what the vault holds. Minting is refused when the system health is below 100 % (ERR), while
minting is frozen after a volatility breach, when no price is in effect, or when the supply cap
has no room; `yed_getprotectionstatus.mintingAllowed` says which.

Never spend a YED output with a plain YEC command: the YED it carries is burned. The wallet locks
every YED output it owns (`listlockunspent` shows them) so `sendtoaddress` and friends cannot pick
them by accident; `lockunspent true` on one of them removes that protection.

### Rebuilding the index

The index lives under `<datadir>/yellowback/` and is rebuilt from the blocks on disk when it is
missing, when the node was reindexed, or on `-reindex-yellowback`. `yed_getinfo.healthy: false`
names the reason and always means "restart with `-reindex-yellowback`". Every `yed_*` call except
`yed_getinfo` refuses while the index is unhealthy or behind the chain tip.

### Backups

Ycash transparent keys are a random keypool, not derived from a seed. The vault owner key of every
mint lives only in `wallet.dat`. **Back up `wallet.dat` after every mint.** Wallet encryption in
Ycash is experimental; protect the file with full-disk encryption, keep RPC on localhost and hold an
offline copy.

The v1 trust statement retired with the federation; what follows is the v2 statement, §8.1 of
`doc/yellowback-spec.md`, which the `audit` job checks byte for byte against that file.

## Trust statement

Yellowback v2 is a miner-enforced, over-collateralised stablecoin overlay on Ycash.

- Consensus-enforced (by every Ycash node, upgraded or not): collateral cannot leave a vault
  before its lock height; before the claim height only the minter's key can spend it.
- Enforced by every Yellowback-aware node deterministically: Yellowback accounting (conservation,
  supply, collateral totals, vault status, prices, activation, halts).
- Enforced by the mining pools that run the Yellowback module, and effective for the whole
  network once a supermajority of blocks signal: collateral is released only against the burn of
  the vault's debt; an underwater, abandoned vault can be claimed only by burning that debt; both
  pay a fee to a pool that published a price quote in the 100 blocks up to the transaction's
  reference height.
- Prices are the medians of the quotes pools publish in their own blocks; moving them needs a
  majority of *quote-tagged* blocks over a window, which is a majority of hashpower only when
  most blocks carry quotes — so the windows that decide claims are undefined until two-thirds of
  their blocks carry quotes, and the mint window until half do. A pool whose quotes stray from
  its peers' loses the fee income that wallets' default payee choice would otherwise send it.
- **Therefore:** a majority of hashpower that runs the module and follows it makes the rules
  hold; a majority that does not — or a minority that the trailing signal count mistakes for a
  majority, since the count is self-reported — could release collateral without burns or, with
  enough quote-tagged blocks, move the price; the same majority could already reorganise the
  chain. No operator, committee or key other than the minter's can move collateral before the
  claim height; after it, only a burn of the vault's debt can. No pool can move a user's YED or
  take collateral before the grace period; a pool can create YED only by first moving the mint
  price with a majority of quote-tagged blocks.
- An enforcing node that finds itself on the minority side of a split — a rejected chain that
  outruns its own by six blocks — stops enforcing for the session, rejoins the network's chain,
  raises an alert and waits for its operator; it is never stranded for more than six blocks, and
  it never bans the peers that relayed the other chain, neither for the rejected block nor for
  its descendants. A node that catches up after an outage never rejects a block the network has
  already built six blocks on; it accepts it, records that it did, and keeps enforcing.
  Enforcement means "majority in fact", not "majority by count".
- Every release enforces only until a sunset height about a year past its start; past it the
  node keeps publishing quotes and accounting but rejects nothing until upgraded, so two
  releases with different rules can never both be enforcing.
- As with any soft fork, every pool — participating or not — should run the module at least in
  filter-only mode, or its blocks can be orphaned by rule-breaking transactions it cannot see.
- Signalling is announced only once the pools running the module are diverse enough that no one
  of them decides alone (at least three independent pools, none above 40 % of quoting blocks,
  measured and published before the announcement).
- If pools stop participating: below 60 % of blocks signalling, minting pauses; below 50 %, block
  rejection pauses as well, and while it is paused neither the owner path nor the claim path is
  policed — collateral can leave a vault without its burn and YED so unbacked stays in
  circulation; vaults untouched during the pause are protected again when it ends. Minting resumes
  at 75 % and rejection at 60 %. Existing YED always remains redeemable by a minter who holds it.
- If the module is abandoned — rejection paused for two full windows, which is also where a
  sunset with no successor release ends up — every vault's claim path becomes spendable by
  anyone at its claim height: owners must sweep their collateral before that height
  (`yed_sweep`, which every node of every release offers under that one same condition, and
  whose transaction every node then relays and mines like any other) or lose it to whoever
  claims first; the claim path stays open to everyone, so the race is fair, but the YED minted
  against a swept or claimed vault is unbacked from then on. A failed mint's collateral (a VOID
  vault, which never carried a debt) is released by its owner with `yed_redeem` at its lock
  height at any time, abandonment or not.

## Build and test baseline

Everything below runs from `ycash-dd/` on `feature/yellowback-sf` (plan §6.0 item 0). Python is
always the workspace venv (`../.venv/bin/python`), never the system interpreter.

```
# host conditions (macOS, Apple Silicon; Linux CI needs none of the three exports)
export PATH="/opt/homebrew/opt/libtool/libexec/gnubin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/bin:$PATH"
export CARGO_TARGET_DIR="$PWD/target"          # the Makefile links target/<triple>/release/librustzcash.a relative to the repo
LIBTOOLIZE=glibtoolize BUILD_STAGE=depends ./zcutil/build.sh -j8    # once (~25 min): the depends tree
./zcutil/build.sh -j8                                              # ycashd, ycash-cli, ycash-tx, src/test/test_bitcoin
./zcutil/fetch-params.sh                                           # needs GNU sha256sum
# after a path change (workspace rename): configure bakes absolute depends paths, so reconfigure, then incremental
CONFIG_SITE="$PWD/depends/$(ls depends | grep -m1 darwin)/share/config.site" ./configure && make -C src -j8 test/test_bitcoin ycashd ycash-cli
make -C src -j8 ycash-cli                                          # after EVERY src/rpc/client.cpp change (conversions live in the CLI)

# unit tests
src/test/test_bitcoin --run_test='yellowback_*'
# one functional script (distinct --portseed per concurrent run; --nocleanup --noshutdown keeps the playground)
BITCOIND="$PWD/src/ycashd" ../.venv/bin/python -u qa/rpc-tests/yellowback_index.py --srcdir="$PWD/src" --tmpdir=/tmp/yb-index --portseed=11
# the suite by name (the runner does not glob; scripts are registered in BASE_SCRIPTS / EXTENDED_SCRIPTS)
qa/pull-tester/rpc-tests.py -j4 --nozmq yellowback_index yellowback_activation
# the Python owner-path signer (build_vault_spend_raw) loads libcrypto through ctypes; on macOS it aborts (exit 134) without:
export DYLD_LIBRARY_PATH="$(brew --prefix openssl@3)/lib"
# devnet (§5)
../.venv/bin/python contrib/yellowback/devnet/yellowback-devnet up && ../.venv/bin/python contrib/yellowback/devnet/yellowback-devnet check

# fuzz (Ycash's own harness: --enable-fuzz-main replaces main(); zcutil/clean.sh removes src/fuzz.cpp, confirming the layout)
CONFIG_SITE="$PWD/depends/<triple>/share/config.site" ./configure --enable-fuzz-main CC=clang CXX=clang++ CXXFLAGS='-fsanitize=fuzzer,address'
ln -sf fuzzing/YellowbackEvaluate/fuzz.cpp src/fuzz.cpp && make -C src -j8 ycashd
src/ycashd src/fuzzing/YellowbackEvaluate/output src/fuzzing/YellowbackEvaluate/input -max_len=4096   # libFuzzer; corpus in, findings out
../.venv/bin/python src/test/gen_yellowback_corpus.py --check                                          # embedded C++ corpus == input/ files
```

A failed `make` leaves the old `test_bitcoin` in place, so "tests pass" after a failed build means
nothing: check the make exit code. The `build_vault_spend_raw`, `yellowback_activation`, devnet
`check` and fuzz lines name Phase 1–5 artefacts; on the Phase 0 tree the Python signer that needs
`DYLD_LIBRARY_PATH` is `cosign_and_submit`, and the fuzz targets are the prototype's
`src/fuzzing/Yellowback{Payload,Script}/`.

### Host notes (macOS, Apple Silicon)

Host: macOS 26 (Darwin 25.0.0), Apple clang 17, GNU make 3.81; depends toolchain
`aarch64-apple-darwin` (clang 18.1.8, rust, boost, libevent, zeromq, libsodium, utfcpp,
googletest, bdb) builds from `depends/` unchanged. `zcutil/build.sh` needs three host-side
conditions that are not fork changes: Homebrew `automake` and GNU `libtool` on the PATH
(`LIBTOOLIZE=glibtoolize`; libevent's `autoreconf` needs them), GNU coreutils' `sha256sum` for
`zcutil/fetch-params.sh` (macOS ships a BSD `sha256sum` whose flags differ), and
`CARGO_TARGET_DIR` pointed at `<repo>/target` when the shell sets a global cargo target directory.
Python 3.12+: the inherited `test_framework/mininode.py` imports `asyncore` (removed in 3.12) and
`pyblake2` (unmaintained); the workspace venv carries `pyasyncore` and a one-line `pyblake2` shim
over `hashlib.blake2b`; no framework file is changed. `grep` on this host is ugrep.

### Recorded baseline (Phase 0, 2026-09-10, node side)

Recorded from this tree (commits `6f4380028`, `e394ce7a7`, `bbb132630` on `feature/yellowback-sf`),
incremental `make -C src -j8 test/test_bitcoin ycashd ycash-cli` on the host above.

- `src/test/test_bitcoin --run_test='yellowback_*'`: **31 cases, all green** (33 before Phase 0;
  `genesis_and_prices` and `rotation_and_custody` are behind `#if 0` in
  `yellowback_state_tests.cpp` until Phase 2).
- The whole `src/test/test_bitcoin`: **453 cases, 2 failures, both pre-existing at the pin** in
  files the fork does not touch — `main_tests/subsidy_limit_test` (`nSum` off by one halving:
  `2099999981520000 != 2099999990760000`) and `rpc_wallet_tests/rpc_z_sendmany_internals`
  (two change outputs hash to the same address). 121 s. The CI `main` job runs the whole suite;
  these two must be looked at (or excluded by name) before `main` can be green there.
- Yellowback functional scripts (run one process each, `--portseed` 11–15, `BITCOIND` set,
  `DYLD_LIBRARY_PATH` for the Python co-signer): `yellowback_index`, `yellowback_lifecycle`,
  `yellowback_void_mint`, `yellowback_wallet_restore`, `yellowback_sapling` — **all five green**.
  `yellowback_reorg_stress` (nightly) was not run in this session.
- Inherited stock baseline, `qa/pull-tester/rpc-tests.py -j4 --nozmq` over the eleven scripts of
  plan §6.0 item 6, against the fork binary without `-yellowback`: **7 pass** — `mempool_reorg`,
  `mempool_tx_expiry`, `reorg_limit`, `reindex`, `wallet`, `rawtransactions`, `txn_doublespend`
  (these are the CI `STOCK_BASELINE`); **4 fail at the pin**: `getblocktemplate_proposals`,
  `getblocktemplate_longpoll` and `invalidateblock` crash `ycashd` with `SIGABRT` in
  `CChainParams::GetFoundersRewardAddressAtHeight` under `getblocktemplate`/`generate` (a stock
  regtest node activates no Ycash upgrade; every Yellowback script passes the six `-nuparams` at
  height 1 and never hits it), and `p2p-acceptblock` fails "Unrequested block from whitelisted
  peer not accepted" (Bitcoin behaviour Zcash/Ycash does not have). The four scripts, the test
  framework and `main.cpp`/`net.cpp`/`miner.cpp`/`rpc/mining.cpp`/`chainparams.cpp` are
  byte-identical to `ycash-legacy`, and the failure reproduces with the one framework fix of this
  phase reverted, so they are the pin's, not the fork's.
- `python3 -m unittest contrib/yellowback/test_yellowback_price.py contrib/yellowback/test_yellowback_quote.py`: 52 tests OK (Phase 7 replaced `test_yellowback_fed.py`). `pyflakes` over
  `qa/rpc-tests/yellowback_*.py` and `yellowback_util.py`: clean.
- Consensus set (`src/consensus`, `src/script`, `src/primitives`, `src/pow`, `chainparams.cpp`,
  `wallet/wallet.{h,cpp}`, `txdb.*`, `configure.ac`): zero lines changed vs `ycash-legacy`;
  `main.cpp`, `miner.cpp`, `rpc/mining.cpp`: 0 of 40/35/35.
- The runner (`rpc-tests.py`) execs each script through `#!/usr/bin/env python3`: put the
  workspace venv's `bin` first on `PATH` or the scripts start under the system interpreter and
  fail on `import simplejson`.
- The same exec through `/usr/bin/env` (a SIP-protected binary) **drops `DYLD_LIBRARY_PATH`**, so
  under the runner the Python signer (`build_vault_spend_raw`'s `CECKey`) loads the system
  libcrypto and macOS aborts the script ("is loading libcrypto in an unsafe way"). On this host run
  a script that signs vault spends directly (`../.venv/bin/python -u qa/rpc-tests/<script>.py
  --srcdir=... --tmpdir=... --portseed=...`, as the Phase 4 record below does); the Linux CI runner
  is unaffected. Verified: `/usr/bin/env python3 -c 'import os; print(os.environ.get("DYLD_LIBRARY_PATH"))'`
  prints `None` with the variable exported.

### Recorded baseline (Phase 2, 2026-09-10, node side)

Recorded from this tree on `feature/yellowback-sf` after the Phase 2 commits (state machine v2,
view schema 2, fuzz targets), same host and build recipe as above.

- **The four wallet-flow scripts left the CI `main` job's list at Phase 2's first commit**
  (`yellowback_lifecycle`, `yellowback_void_mint`, `yellowback_wallet_restore`,
  `yellowback_sapling`; plan §6 preamble, N26): the payload is now version 2 and the state
  machine follows the v2 rules while `yed_mint`/`yed_redeem` still build v1 transactions
  (`BuildMint`/`BuildRedeem` throw "lands in Phase 6" until then), so they cannot pass. They
  return at Phase 6. `YELLOWBACK_SCRIPTS` is `yellowback_index` alone until Phases 3–5 add
  `yellowback_activation`, `yellowback_mining` and `yellowback_enforcement`.
- `src/test/test_bitcoin --run_test='yellowback_*'`: all green — `yellowback_state_tests` was
  rewritten (53 cases, every rule of plan §3.7–3.9 tagged `// Rule:`), including
  `statehash_golden_vector`, which replays the Python model's 224-block vector
  (`src/test/data/yellowback_golden.json`, a copy of `qa/rpc-tests/test_framework/
  yellowback_golden.json` that `gen_yellowback_corpus.py --check` keeps equal) and reproduces the
  pinned hash `6eb05394…8682`; `yellowback_fuzz_tests` gained `evaluate_corpus_replay` and
  `payee_corpus_replay` over the two new targets' corpora.
- Fuzz targets: `src/fuzzing/YellowbackEvaluate` (the prefix grammar of
  `src/test/yellowback_fuzz_harness.h`, four properties: apply/undo identity, overlay
  equivalence, `blockInvalid ⇒ enforcementOn` computed at `H`, `supplyCents == Σ Tokens`) and
  `src/fuzzing/YellowbackPayee` (the FEE-W pick is in `E(R)`, nullopt iff `E(R)` is empty).
  Corpora 30 + 24 seeds; the CI `nightly` job runs every target 10 minutes under libFuzzer and
  `weekly-fuzz` two hours with corpus minimisation. **The 8 CPU-hour `YellowbackEvaluate` run of
  Phase 2's exit was not run on this host** (Apple clang ships no libFuzzer runtime,
  `docs/mapping.md` §13.1); it runs on the Linux jobs.
- Removed with the prototype (§4.2, N20, N21): the v1 payload codec (version 1 is non-Yellowback,
  V23), `Payload::Price`, the tier tables, `Health`/`DcaBps`/`ErrBps`/`RequiredBurn`/
  `VolatilityBreach`, the genesis anchor and roster parameters and their regtest flags
  (`-yellowbackgenesisanchor`, `-yellowbackgenesisroster`, `-yellowbacksupplycap`; the four v2
  flags are `-yellowbackstartheight`, `-yellowbacksigmaref`, `-yellowbacksupplycapbps`,
  `-yellowbackenforceuntil`), `yed_getprice` and `yed_getprotectionstatus`. The node RPCs render
  the v2 records in a transitional shape; Phase 3 rewrites them per plan §4.5.

### Open items carried over from the prototype's Phase 0

- The `YCASH_WR=1` build (the Ycash-specific build variant) has still not been run on this host.
- The inherited `qa/pull-tester/rpc-tests.py` baseline: the eleven scripts of plan §6.0 item 6
  are recorded above (7 pass, 4 fail at the pin); the rest of `BASE_SCRIPTS` is still unrun. The
  four that fail need the six `-nuparams` on every node (the Phase 3 `qa/yellowback-wrapped-ycashd.sh`
  wrapper, or a `regtest`-wide default) before they can join the CI list.
- The two pre-existing `test_bitcoin` failures (`subsidy_limit_test`, `rpc_z_sendmany_internals`)
  make the whole-suite step of the CI `main` job red until they are fixed or excluded by name.
