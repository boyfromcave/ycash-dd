# Yellowback v1 — review package

For the Ycash maintainers. Everything Yellowback adds is an *overlay*: no consensus rule, no
policy rule, no opcode, no network upgrade, no new build dependency. This file is the self-review
against the plan's checklist (plan §8.4) with the actual numbers, plus the DoS review notes. The
plan itself, the normative protocol and the mechanism crosswalk live in the development
workspace (`docs/plans/yellowback-v1-development-plan.md`, `docs/spec/yellowback-adaptation-spec.md`,
`docs/mapping.md`).

## 1. Diff budget (plan §4.1) — actual

`git diff --stat ycash-legacy...feature/digidollar` on 2026-09-05:

| Existing file | Budget | Actual | What |
|---|---|---|---|
| `src/init.cpp` | ≈ 40 | 75 (+72/−3) | help text, `-prune` incompatibility, `-yellowbackfee` floor, `-yellowbackmintlag` bounds, regtest-only argument check, index open/sync/register before the notifier thread, wallet-layer creation, shutdown sequence, wallet-RPC registration |
| `src/experimental_features.{h,cpp}` | ≈ 8 | 7 | `fExperimentalYellowback` |
| `src/rpc/register.h` | ≈ 6 | 5 | two registration functions |
| `src/rpc/client.cpp` | ≈ 15 | 10 | numeric-argument conversions |
| `src/Makefile.am`, `src/Makefile.test.include` | ≈ 25 | 34 (+33/−1) | new sources and tests |
| `qa/pull-tester/rpc-tests.py` | ≈ 6 | 9 (+8/−1) | seven test scripts; `BITCOIND` = `src/ycashd` (the inherited runner named `src/zcashd`) |
| `qa/rpc-tests/test_framework/util.py`, `qa/rpc-tests/multi_rpc.py` | 2 | 2 | `zcash.conf` → `ycash.conf` (G1) |
| `src/main.cpp`, `src/consensus/*`, `src/script/*`, `src/primitives/*`, `src/pow/*`, `src/chainparams.cpp`, `src/wallet/*`, `src/txdb.*`, `configure.ac` | **0** | **0** | verified by `git diff --stat ycash-legacy...HEAD -- <those paths>` (empty), also a CI step |

Total (at `93805aca6`, the Phase 6 part 2 commit): 60 files, +11,263 / −7 lines, of which 138 added
and 7 removed in existing files. New code: `src/yellowback/` + `src/rpc/yellowback*.cpp` 5,623
lines; unit tests 1,852; functional tests 2,290 (incl. `test_framework/yellowback_util.py`);
coordinator and redemption client 721 (Python); fuzz targets 122; CI workflow 122; documentation
395. Regenerate with `git diff --numstat ycash-legacy...HEAD` before the review request.

The `init.cpp` overshoot (75 vs 40) is the argument validation the audits asked for (C2, C4, C16)
and the wallet-layer creation; every added line is inside `if (fExperimentalYellowback)` or the
shutdown sequence, and a node started without `-yellowback` executes none of it beyond three
`mapArgs.count` checks that refuse Yellowback options.

## 2. Checklist (plan §8.4)

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | diff matches §4.1; zero lines in the consensus set | done | table above; CI step "Determinism audit" |
| 2 | forbidden symbols absent from the pure modules | done | `grep` over `state.{h,cpp}`, `math.h`, `payload.{h,cpp}`, `script.{h,cpp}`, `view.{h,cpp}` is empty; CI step |
| 3 | every §3.7 rule has a unit test, every §3.8 rule a functional test | done | `yellowback_state_tests.cpp` (IN-1..3, TX-0, MINT-1..7, XFER-1..3, PRICE-1..3, SNAP, UNDO); `yellowback_lifecycle.py` (RED-1/3/4/7/8, SUB-1, C22), `yellowback_void_mint.py` (RED-6 exemption for VOID, B19), `yellowback_federation.py` (RED-6, C5), `yellowback_wallet_restore.py`; MINTPOL-1 in lifecycle/void/protection |
| 4 | apply/undo identity and cold rebuild under reorg stress | done | `yellowback_state_tests/apply_undo_identity`; `yellowback_reorg_stress.py` (random 1–6 block reorgs, state-hash equality after every reorg, restart and `-reindex-yellowback`) |
| 5 | every `yed_*` refuses when the flag is off or the index is unhealthy | done | `EnsureIndex`/`EnsureYW` throw `-32601`; `EnsureHealthy` in every RPC except `yed_getinfo`; unhealthy path exercised by `yellowback_index_tests.cpp` |
| 6 | coin locking covers every mine-owned token after restart | done | `Reconcile()` at start (init.cpp) and after every applied block; `yellowback_wallet_restore.py` (`yed_lockcoins` after import), lifecycle (`listlockunspent` after `yed_mint` and across a disconnect) |
| 7 | `yed_cosignredeem` never signs when §3.8 fails | done | lifecycle: stripped owner signature, far expiry, substituted script, short burn, wrong vault input; federation: RED-6, C5 |
| 8 | no new build dependencies; no networking in the node | done | `configure.ac` untouched; `grep "evhttp\|socket\|connect("` over `src/yellowback`, `src/rpc/yellowback*.cpp` is empty; CI step |
| 9 | `ChainTip` exception-safe; fault injection proves it | done | `yellowback_index_tests.cpp` (`testBeforeApply` throws; index unhealthy, no propagation; later deliveries ignored) |
| 10 | `-yellowback -prune` fails at init; `-reindex` rebuilds to the same hash | done | `yellowback_index.py` |
| 11 | `IsStandardTx`/`AreInputsStandard` pass for every template | done | `yellowback_script_tests/templates_are_standard` (MINT, TRANSFER with 15 assignments, REDEEM with k = n = 13, PRICE with refill) |
| 12 | `sendtoaddress` right after `yed_mint`/`yed_send` cannot select a YED output | done | lifecycle (B4) |
| 13 | MINT-4/5 read only `Snapshots[evalHeight]` | done | `state.cpp` `CheckMint` reads `E = GetSnapshot(evalHeight)` and nothing else; lifecycle B3/C4 |
| 14 | `yed_createpricetx` round-trips through `signrawtransaction` only where `addmultisigaddress` ran | done | `yellowback_index.py` (node 3 without the script: `complete: false`) |
| 15 | co-signer sighash from the index's vault record only; substituted script refused | done | `policy.cpp` RED-7; lifecycle C5 |
| 16 | disconnect never removes a lock; VOID token unlocked only once applied | done | `Reconcile()` releases only outpoints whose transaction is in `TxLog`; lifecycle C3 |
| 17 | default-lag mint survives a 2-block reorg with a different price | done | lifecycle C4 |
| 18 | fee floor and regtest-only arguments rejected at init | done | `yellowback_index.py` (four `assert_start_raises_init_error` cases) |
| 19 | phantom input refused before `AreInputsStandard`/fee; no assert | done | `policy.cpp` checks `AccessCoins`/`IsAvailable` first (D1); lifecycle (`yed_cosignredeem` with an unknown input: refusal, node stays healthy) |
| 20 | index unregistered/stopped under its lock in `Shutdown()`, never deleted; fault-injection for a blocked handler | partly | `init.cpp` shutdown sequence and `Stop()` semantics are unit-tested (`yellowback_index_tests.cpp`); an in-process test that blocks a handler while `UnregisterValidationInterface` runs is not written — the sequence relies on `cs_yellowback` being taken by `Stop()`/`Flush()`, which waits out any in-flight handler by construction |

## 3. DoS review (plan §6 Phase 6)

- **Payload parsing.** `DecodePayload` is a bounds-checked reader over a byte vector of at most
  80 bytes; it never throws and never allocates beyond 15 assignments. Fuzzed: `src/fuzzing/YellowbackPayload/`
  with a seed corpus of 22 inputs replayed by `yellowback_fuzz_tests.cpp` (every prefix of every
  input as well). Script parsers likewise (`src/fuzzing/YellowbackScript/`, 20 seeds).
- **`yed_validaterawtransaction` / `yed_cosignredeem` cost.** One transaction decode, one
  `ProcessTx` dry run over an overlay of the index (a handful of LevelDB point reads), and for the
  co-signer one `IsStandardTx`, one `AreInputsStandard` and one `SignatureHash`. No unbounded loop:
  inputs are bounded by the transaction size the RPC accepted. Every input must exist in the coins
  view before `AreInputsStandard` or `GetValueIn` are called (D1: both assert on a missing coin).
- **`/cosign`.** Per-IP token bucket (30 requests per minute) in the coordinator; the node refuses
  to co-sign the same vault twice at one height; every refusal is logged with the rule id. Operators
  put TLS termination with client-certificate authentication in front of the coordinator's
  `/pricesign` and `/rotatesign` peer endpoints.
- **HTTP client timeouts.** 60 s on every coordinator and client request; 15 s on price sources.
- **Index growth.** One `Snapshot` per block ≈ 90 bytes serialised (≈ 38 MB/yr at 75-second
  blocks), one `TxLog` per Yellowback-relevant transaction, one `Undo` per block pruned below
  tip − 1,000. A VOID vault costs its creator a normal transaction fee and the index ≈ 150 bytes;
  at Ycash's block size the worst case is a few hundred records per block, bounded exactly like any
  UTXO growth (D9). `yed_listvaults` is paged.
- **Notifier thread.** Every handler is wrapped; an exception turns the index unhealthy and every
  later `yed_*` call except `yed_getinfo` fails fast with the reason.

## 4. Things a reviewer should know

- The one behavioural surprise: volatility is judged against the prices 48 and 96 blocks back, so a
  move made before those windows hold a price breaches only when its echo reaches them, and a step
  change re-breaches until `price(H − 48)` reads the new level (`docs/mapping.md` §11).
- `yed_validaterawtransaction` runs the state rules (§3.7) as a dry run; the co-signer policy rules
  (§3.8) run in `yed_cosignredeem` only, because they need the wallet-side coins view and the
  federation key.
- Two inherited unit tests fail at the v4.5.0 pin on this host (`main_tests/subsidy_limit_test`,
  `rpc_wallet_tests/rpc_z_sendmany_internals`); neither file is touched by the fork.
- The wallet application (`yecwallet-dd`) compiles against a system Qt 6 and its QTest target
  passes offscreen, but it has not yet been run against a live node; every `yed_*` reply shape it
  consumes was reconciled against `doc/yellowback-rpc.md` and this fork's RPC sources by reading
  (plan Phase 5b).
