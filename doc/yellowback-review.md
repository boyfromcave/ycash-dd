# Ycash Yellowback (YED) v2 — review package

For the Ycash maintainers, and for anyone deciding whether this fork is safe to run or to review.

Everything below was measured on the branch `feature/yellowback-sf` at commit `5ea56c577`, on
2026-09-11, on macOS 26 / Apple Silicon, against the pristine baseline branch `ycash-legacy`
(= upstream Ycash `v4.5.0`, `624c12814`). Commands are given so that every number can be
reproduced. Where a claim in the development plan's §8.1 or §8.2 is **not** supported by what the
tree actually does, this document says so in §8 rather than restating the claim.

**Read the evidence markers literally.** A row that says `grep:` carries the real output of that
grep, run in this session. A row naming a test names a test that exists on some branch, and says
*which* branch. Phases 5, 6 and 8 are not finished: several of the tests the plan's §8.4 names
live on `feature/yellowback-sf-p5` / `-p6` and are **not merged into this branch yet**, and some
do not exist anywhere. Those are marked, not glossed. §7 lists everything this document must not
be read as claiming.

---

## 1. Diff budget (plan §4.1) — actuals

```
git diff --numstat ycash-legacy...HEAD -- <file>
```

| Existing file | Budget (§4.1) | Actual (+/−) | Changed lines | Verdict |
|---|---|---|---|---|
| `src/main.cpp` | ≤ 40 | +11 / −0 | **11** | within budget |
| `src/miner.cpp` | ≤ 35 | +10 / −2 | **12** | within budget |
| `src/rpc/mining.cpp` | ≤ 35 | +7 / −0 | **7** | within budget |
| `src/init.cpp` | no budget (≈ 110 est.) | +159 / −3 | 162 | over the estimate, no budget; node tier |
| `src/transaction_builder.cpp` | 35 (both files) | +17 / −0 | 17 | additive, wallet tier |
| `src/transaction_builder.h` | — | +17 / −1 | 18 | additive, wallet tier |
| `src/rpc/client.cpp` | build/test | +22 / −0 | 22 | argument conversions only |
| `src/experimental_features.{h,cpp}` | build/test | +7 / −0 | 7 | the `-yellowback` flag |
| `src/rpc/register.h` | build/test | +5 / −0 | 5 | two registration functions |
| `src/Makefile.am`, `src/Makefile.test.include` | build/test | +38 / −2 | 40 | new sources |
| `qa/pull-tester/rpc-tests.py`, `test_framework/{util,script}.py`, `multi_rpc.py` | build/test | +16 / −3 | 19 | runner + the ZIP-243 `<q` fix (mapping §13) |
| `README.md`, `.github/PULL_REQUEST_TEMPLATE.md` | — | +137 / −5 | 142 | documentation |

**The consensus zero set is at zero.** Run in this session:

```
$ git diff --stat ycash-legacy...HEAD -- src/consensus src/script src/primitives src/pow \
      src/chainparams.cpp src/wallet/wallet.h src/wallet/wallet.cpp src/txdb.cpp src/txdb.h configure.ac
$          (no output)
```

`src/wallet` as a whole is also at zero on this tip: the wallet-tier hardening H5/H7/H8 of Phase 8
(`src/wallet/rpcwallet.cpp`, `src/rpc/rawtransaction.cpp`) is **not written yet**.

Whole-fork totals: 265 changed files, **+33,485 / −17** lines. Seventeen deleted lines is the
entire subtraction the fork makes from Ycash v4.5.0: two in `miner.cpp` (one replaced statement,
one replaced `IncrementExtraNonce` call), three in `init.cpp`, and the remainder in build files,
`README.md`, `rpc-tests.py` and the three one-character test-framework fixes.

---

## 2. The hook table — every §4.3 site, with the four-part check

Every site is an **insertion**. Every behaviour-changing statement is inside `if (g_yellowback)`
(the pointer is null unless the node was started with `-yellowback`). Full diff:
`git diff ycash-legacy...HEAD -- src/main.cpp src/miner.cpp src/rpc/mining.cpp`.

### 2.1 `main.cpp` `ConnectBlock` — the block-validity check

```cpp
+    if (yellowback::g_yellowback) {
+        if (auto ybBad = yellowback::g_yellowback->CheckConnect(block, pindex, fJustCheck)) return state.DoS(0, error("ConnectBlock(): %s", ybBad->c_str()), REJECT_INVALID, "yellowback-vault-spend");
+    }
     if (fJustCheck)
         return true;
```
inserted at fork `src/main.cpp:3198`, i.e. the `ref/ycash/src/main.cpp:3191-3193` window between
`control.Wait()` and `if (fJustCheck) return true;`.

- **X / Y / M:** DigiByte rejects an invalid DigiDollar transaction inside `ConnectBlock`
  (`ref/digibyte/src/validation.cpp`, via `digidollar/validation.cpp:1145-1149`) with DoS 100,
  and gets its soft-fork safety from a BIP9 buried deployment plus BIP342 `OP_SUCCESSx`.
- **Z, which lacks M:** Ycash v4.5.0 has no versionbits, no Tapscript, and
  `InvalidBlockFound` calls `Misbehaving` whenever `nDoS > 0` (`ref/ycash/src/main.cpp:2304-2305`),
  so a DoS 100 rejection would ban the stock peers that relayed a chain most of the network
  considers valid.
- **W (the adaptation):** one call returning `std::optional<std::string>`; a verdict becomes
  `state.DoS(0, …, REJECT_INVALID, "yellowback-vault-spend")`. `DoS(0)` marks the block
  `BLOCK_FAILED_VALID` (`:2308-2312`) without ever scoring a peer. Activation is derived from the
  chain's own coinbase tags, not from versionbits.
- **Tier T:** soft fork (Tier 1+hook). A cheaper tier will not do: a template filter alone
  (Tier 0) cannot stop a *stock* miner from including an owner-path vault spend with no burn, and
  Ycash script cannot express "this spend must burn N YED elsewhere in the transaction" —
  there is no introspection opcode and no Tapscript.

**Why the site is safe.** It runs after every consensus check has already passed, so a Yellowback
verdict can only ever *subtract* from the set of blocks the node accepts — never add. It is on the
path of `ConnectTip`, `TestBlockValidity` and `VerifyDB` level 4 alike, and returns a reason only
when all of: `-yellowbackenforce`, index healthy, index tip == `pindex->pprev`, not
IBD/`-reindex`/`fImporting`, the network has not already built `VALVE_BLOCKS` of work on the block,
the valve has not tripped, and ACT-5 (including the sunset) holds at `H`. It never dereferences
`pindex->phashBlock` — under `TestBlockValidity` `pindex` is `indexDummy` with a null hash
(`ref/ycash/src/main.cpp:4697-4700`) — which `check_null_hash` pins.

### 2.2 `main.cpp` `ConnectBlock` — the commit

```cpp
     view.SetBestBlock(pindex->GetBlockHash());
+    if (yellowback::g_yellowback) yellowback::g_yellowback->CommitConnect(block, pindex);
```
fork `src/main.cpp:3276`.

- **X / Y / M:** DigiByte writes DigiDollar state as part of the chainstate flush.
- **Z:** Ycash's chainstate flushes lazily — `FLUSH_STATE_IF_NEEDED` only when the coin cache is
  full (`ref/ycash/src/main.cpp:3324-3334`), a full flush every 24 h (`main.h:108`).
- **W:** a separate per-block LevelDB commit placed *after* `view.SetBestBlock`, i.e. after every
  write path that can `AbortNode` (`:3204-3263`), with `SyncToChain` undo-walking at startup while
  the stored tip is not in `chainActive`. On a storage failure the index is marked unhealthy and
  the block still connects.
- **Tier:** same hook; no cheaper placement exists, because a block-validity check needs the state
  at the parent under `cs_main`, not a state that trails by a second (V2).

**Why it is safe.** Never reached under `fJustCheck` (the check above returns first). Writes one
batch. A failure never fails the block.

### 2.3 `main.cpp` `DisconnectBlock` — the undo

```cpp
+    if (updateIndices && yellowback::g_yellowback) yellowback::g_yellowback->UndoDisconnect(pindex);
     return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
```
fork `src/main.cpp:2799` (baseline `ref/ycash/src/main.cpp:2796`).

- **X / Y / M:** DigiByte's index updates are symmetric in `ConnectBlock`/`DisconnectBlock`.
- **Z:** Ycash's own atomic-swap/insight hooks are split between `ConnectBlock` and `DisconnectTip`,
  and `DisconnectBlock` is `static` with an `updateIndices` parameter — `true` from `DisconnectTip`
  (`:3541`), `false` from `VerifyDB` level 3 (`:5133`).
- **W:** place the undo inside `if (updateIndices)`, at the very end, so it runs only after the
  coins view, the nullifiers and both anchors have already been rolled back
  (`view.SetNullifiers(tx, false)` `:2707`, `view.PopAnchor(…, SPROUT)` `:2754`,
  `PopAnchor(…, SAPLING)` `:2762-2764`, `view.SetBestBlock(pindex->pprev->GetBlockHash())` `:2776`).
- **Tier:** hook; no consensus effect at all — this call cannot change a return value.

### 2.4 `main.cpp` `AcceptToMemoryPool` — MP-1

```cpp
         view.SetBackend(dummy);
+        if (yellowback::g_yellowback && !yellowback::g_yellowback->MempoolCheck(tx))
+            return state.DoS(0, false, REJECT_NONSTANDARD, "yellowback-vault-spend");
```
fork `src/main.cpp:1645` (baseline `:1643`).

- **X / Y / M:** DigiByte rejects invalid DD transactions from the mempool as part of consensus.
- **Z:** Ycash's `AcceptToMemoryPool` holds `pool.cs` for its whole body (`:1520`), and policy
  refusals here are `REJECT_NONSTANDARD` (style of `:1567`).
- **W:** a policy-only predicate that returns `true` unless some input spends an ACTIVE vault —
  `O(inputs)` lookups, no snapshot — and `true` for every vault spend while abandonment holds
  (L13, so an owner's sweep is relayable).
- **Tier:** Tier 0 (relay policy). This is the cheapest tier and it is used here.

**Measured cost (N6, §8.2 row "`MempoolCheck` CPU per ordinary transaction"):**

```
$ ./src/test/test_bitcoin --run_test='yellowback_index_tests/mempoolcheck_bench' --log_level=message
MempoolCheck over 10000 plain transactions: 5948 us
```
0.59 µs per non-Yellowback transaction on this host. The test's own bound is 1,000,000 µs.

### 2.5 `main.cpp` `AcceptBlockHeader` — the BLK-2 descendant clause and the valve odometer

```cpp
     if (hash != chainparams.GetConsensus().hashGenesisBlock) {
+        if (yellowback::g_yellowback && yellowback::g_yellowback->NoteHeaderOnRejectedChain(block))
+            return state.DoS(0, error("%s: descends from a Yellowback-rejected block", __func__), REJECT_INVALID, "bad-prevblk-yellowback");
         BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
```
fork `src/main.cpp:4562` — before both stock `bad-prevblk` returns (DoS 10 at `:4555`, DoS 100 at
`:4557-4558` in the baseline).

- **X / Y / M:** DigiByte tracks any invalid chain it knows headers for through `pindexBestInvalid`.
- **Z:** in Ycash, `pindexBestInvalid` grows only from *indexed* blocks
  (`ref/ycash/src/main.cpp:2280-2283`, `:3716-3717`), and `AcceptBlockHeader` refuses a header whose
  parent is marked failed (`:4557-4558`) — so descendants of a live Yellowback rejection are never
  indexed, and the node would both ban the relaying peers and be blind to how much work the other
  chain has.
- **W:** replace the DoS 100 answer with a DoS 0 one for exactly this case, and use the note map as
  the valve's odometer: record `{hash → rootHash, work}` only when the header's target is within
  the consensus per-block loosening of its parent's and the root holds fewer than
  `VALVE_NOTE_CAP` notes.
- **Tier:** hook, DoS 0. A cheaper tier cannot work: without this clause a stock peer relaying the
  network's own chain would be banned by an enforcing node.

**Why it is safe.** `DoS(0)` reaches `Misbehaving` in neither message handler
(`ref/ycash/src/main.cpp:6604-6610`, `:6651-6658`). The note cap bounds memory; the work sum is
real proof of work either way. Under `-yellowbackenforce=0`, IBD, or past the sunset, `Rejected` is
empty and the clause is inert.

### 2.6 `main.cpp` `ConnectTip` — the N5 mempool sweep

```cpp
     auto ids = mempool.removeExpired(pindexNew->nHeight);
+    if (yellowback::g_yellowback) yellowback::g_yellowback->RemoveInvalidVaultSpends(mempool);
```
fork `src/main.cpp:3645`.

- **X / Y / M:** in DigiByte, a transaction made invalid by a tip change is dropped by consensus
  re-evaluation.
- **Z:** Ycash's mempool re-checks nothing Yellowback-shaped on a tip change.
- **W:** re-run `MempoolCheck` at the new tip over the mempool's vault spends and remove failures,
  so an enforcing node stops relaying a spend into stock miners' mempools (which is what feeds the
  N3 griefing row).
- **Tier:** Tier 0, relay policy only.

### 2.7 `miner.cpp` `CreateNewBlock` — the tag and the template view

```cpp
         LOCK2(cs_main, mempool.cs);
+        COINBASE_FLAGS = yellowback::g_yellowback ? yellowback::policy::TagScript(*yellowback::g_yellowback) : CScript();
+        std::optional<yellowback::TemplateView> ybview;
+        if (yellowback::g_yellowback) ybview.emplace(yellowback::g_yellowback->TemplateView());
```
fork `src/miner.cpp:370-372`.

- **X / Y / M:** DigiByte appends the signed oracle price bundle as an extra zero-value coinbase
  output `OP_RETURN OP_ORACLE <v3 data>` after the witness commitment
  (`ref/digibyte/src/oracle/bundle_manager.cpp:815-828`, `node/miner.cpp:535-538`).
- **Z:** Ycash's coinbase *output* layout is fixed by founders'/YDF/funding-stream rules and GBT
  reads `vout[1]` as `foundersreward` (`ref/ycash/src/rpc/mining.cpp:733`). No output may be added.
- **W:** a 36-byte tag pushed into the coinbase **scriptSig** after the BIP34 height, carried by
  `COINBASE_FLAGS` — declared at `ref/ycash/src/main.cpp:134` and **never assigned** anywhere in
  v4.5.0 — so the internal miner, regtest `generate` and `getblocktemplate` all emit it with no new
  plumbing. The scriptSig bound is 2..100 bytes (`main.cpp:1456-1458`) and only its height prefix
  is checked (`:4477-4481`); the tag leaves ≥ 50 bytes of headroom.
- **Tier:** Tier 0, mining policy. A consensus-carried price field would be a network upgrade.

**Why it is safe.** `COINBASE_FLAGS` has no other writer, and it is empty when `g_yellowback` is
null — so a node without the flag produces the identical coinbase scriptSig v4.5.0 produces.
`TemplateView` is an RAII holder of `cs_yellowback` taken *after* `mempool.cs`, matching the
recorded lock order.

### 2.8 `miner.cpp` `CreateCoinbaseTransaction` and the `-gen` loop

```cpp
-        mtx.vin[0].scriptSig = CScript() << nHeight << OP_0;
+        mtx.vin[0].scriptSig = (CScript() << nHeight << OP_0) + COINBASE_FLAGS;
```
and
```cpp
-            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
+            {
+                LOCK(cs_main);
+                IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
+            }
```

These are the fork's **only two replaced lines in mining or validation code**, and the only two
insertions that are not inside `if (g_yellowback)`:

- the append is a no-op when `COINBASE_FLAGS` is empty, which it always is without `-yellowback`
  (nothing else in the tree assigns it);
- the `LOCK(cs_main)` is K17: `IncrementExtraNonce` (`ref/ycash/src/miner.cpp:720`) now reads
  `COINBASE_FLAGS`, whose writer holds `cs_main`, so every reader must too. It is taken in the
  `-gen` internal miner thread only, where `cs_main` is not already held.

### 2.9 `miner.cpp` `CreateNewBlock` — the template filter

```cpp
+            if (ybview && !yellowback::policy::FilterTemplate(*ybview, tx, nHeight)) continue;
             UpdateCoins(tx, view, nHeight);
```
fork `src/miner.cpp:586`. It must precede `UpdateCoins` or descendants would see phantom coins.
Tier 0: this is the entire pre-activation enforcement mechanism, and it cannot fork anything.

### 2.10 `rpc/mining.cpp` — `getblocktemplate`

Four insertions, all inside `if (g_yellowback)`: the `-yellowbackrequirehealthy` refusal,
`"coinbase/append"` in `mutable`, `coinbaseaux` alongside `coinbasetxn`, and the `yellowback`
object. Informational; without the flag the response is v4.5.0's, key for key.

### 2.11 The unguarded residue

The §8.4 item 2 grep, run in this session:

```
$ git diff ycash-legacy...HEAD -- src/main.cpp src/miner.cpp src/rpc/mining.cpp \
    | grep '^+' | grep -v 'g_yellowback\|#include\|LOCK(cs_main)\|COINBASE_FLAGS\|^+$\|^+++'
+            return state.DoS(0, false, REJECT_NONSTANDARD, "yellowback-vault-spend");
+    }
+            return state.DoS(0, error("%s: descends from a Yellowback-rejected block", __func__), REJECT_INVALID, "bad-prevblk-yellowback");
+        std::optional<yellowback::TemplateView> ybview;
+            if (ybview && !yellowback::policy::FilterTemplate(*ybview, tx, nHeight)) continue;
+            {
+                IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
+            }
```

Seven lines, each justified:

| Line | Why it is not an unguarded behaviour change |
|---|---|
| `return state.DoS(0, false, …"yellowback-vault-spend")` | the **second line** of the two-line MP-1 statement whose first line is `if (yellowback::g_yellowback && …)` |
| `}` | the closing brace of the `if (yellowback::g_yellowback) { … }` block in `ConnectBlock` |
| `return state.DoS(0, error(…), …"bad-prevblk-yellowback")` | the second line of the two-line `NoteHeaderOnRejectedChain` statement |
| `std::optional<yellowback::TemplateView> ybview;` | declares an **empty** optional; no observable effect until the next line emplaces it under the guard |
| `if (ybview && !…FilterTemplate(…)) continue;` | guarded by `ybview`, which is engaged only when `g_yellowback` is non-null |
| `{` / `IncrementExtraNonce(…)` / `}` | the K17 `LOCK(cs_main)` scope; the call itself is v4.5.0's, unchanged |

**This contradicts the plan's own wording** — §8.4 item 2 and §4.1 both say the grep's residue is
"exactly the two `#include` lines, the K17 `LOCK(cs_main)` and the `+ COINBASE_FLAGS` append",
which describes a grep with an empty result. The grep is line-based and the fork's guarded
statements span more than one line, so seven residual lines are structural, not a defect. See §8,
finding **R1**, and `docs/mapping.md` §13.9.

Second wording mismatch: there are **three** added `#include` lines, not two —
`src/main.cpp` and `src/rpc/mining.cpp` include `yellowback/index.h`, `src/miner.cpp` includes
`yellowback/policy.h`. See §8, finding **R2**.

---

## 3. Safety-property evidence

| Property | Rule text | Code | Test | State |
|---|---|---|---|---|
| Fail open on **storage only** | V13/BLK-3: only a LevelDB failure may make an enforcing node accept a block it would otherwise reject, and then the index is unhealthy and enforcement is off | `CheckConnect`/`CommitConnect` catch storage failures and return `nullopt` | `yellowback_index_tests/check_storage_fault_accepts_and_sets_unhealthy`, `commit_storage_fault_sets_unhealthy` | green on this branch (this session) |
| Totality of `EvaluateBlock` (K1) | §3.8 "undefined ⇒ the rule is false"; no `throw`, no `assert` on input, no unchecked division | `src/yellowback/state.cpp` | `totality_every_lookup_misses`, `exception_boundary`, plus the five corpus replays `tag_/payload_/script_/evaluate_/payee_corpus_replay` | green (this session). `git grep -nE '\bthrow\b\|\bassert\(' -- src/yellowback/state.cpp` → **no output** |
| Kill switch | V1/V13: `-yellowbackenforce=0` stops rejection **and** un-rejects stored blocks at the next start | `init.cpp` `ReconsiderBlock` loop, the `rpc/blockchain.cpp:1497-1509` pattern | `yellowback_index.py` steps `killswitch_fresh_datadir`, `killswitch_rejected_hash_not_in_mapblockindex`, and "the kill switch on cleanly stopped nodes" | ran to "Tests successful" in this session |
| Work valve (ACT-7) | §8.1: an enforcing node on the minority side of a split stops enforcing for the session once the rejected chain is six blocks of work ahead, rejoins, alerts, and never bans | `NoteHeaderOnRejectedChain` note map in `AcceptBlockHeader` | `valve_trips_at_six_blocks`, `check_tripped_valve_never_rejects`, `valve_trips_through_failed_child`, `valve_note_map_bounded`, `valve_ignores_lowdiff_headers` | green (this session). The multi-node scenario `valve_catchup_offline_node` is in `yellowback_enforcement.py` on **`feature/yellowback-sf-p5`, pending merge** |
| Catch-up suppression (L11) | BLK-2 clause 3: never reject a block the network has already built `VALVE_BLOCKS` on; accept, count in `suppressedBlocks`, keep enforcing | `CheckConnect`, against `pindexBestHeader` | `catchup_suppresses_reject` | green (this session) |
| Sunset (L8) | every release enforces only until `ENFORCE_UNTIL_HEIGHT`; past it the node publishes quotes and accounts but rejects nothing | ACT-5 in `state.cpp`; `-yellowbackenforceuntil` on regtest | `act5_sunset_stops_rejection`, `sunset_flag_stops_rejection_and_signal`, `act5_params_tables` | green (this session). `sunset_alone_is_not_abandonment` and `sweep_after_sunset_without_successor` are in `yellowback_claim.py` on **`-p6`, pending merge** |
| Never ban a peer (N1, BLK-2 descendant clause) | DoS 0 only, ever — for the rejected block and for its descendants | `state.DoS(0, …)` at both `main.cpp` sites | `blk2_dos_level_zero`; `git grep -n 'DoS([1-9]' -- src/yellowback src/rpc/yellowback*` → **no output**; every `DoS(` in the `main.cpp` diff is `DoS(0` (3 of 3) | green (this session). The `banscore == 0` multi-node proof is `yellowback_enforcement.py` case 1 on **`-p5`, pending merge** |
| No rejection during IBD / `-reindex` / import (N2) | BLK-2 initial-sync clause | `CheckConnect` reads `IsInitialBlockDownload()`, `fReindex`, `fImporting` | `check_ibd_suppresses_reject`; `yellowback_index.py` step `ibd_across_accepted_invalid` | both green (this session) |
| Enforcement preconditions (§8.4 item 3) | a reason only if enforce ∧ healthy ∧ tip == pprev ∧ ¬IBD ∧ ¬caught-up-suppressed ∧ ¬valve ∧ ACT-5 | `CheckConnect` | `check_no_enforce`, `check_unhealthy`, `check_tip_mismatch`, `check_suspended`, `check_null_hash`, `check_reverify` | all green (this session) |

All 115 `yellowback_*` unit cases pass:

```
$ ./src/test/test_bitcoin --run_test='yellowback_*'
*** No errors detected
```

---

## 4. The §8.4 review checklist, all 25 items

Evidence gathered 2026-09-11 at `5ea56c577`. "pending merge" means the named test exists on a
Phase 5 / Phase 6 branch and is not on this branch yet; **no such row is counted as passing.**

| # | Item (§8.4) | Evidence |
|---|---|---|
| 1 | Diff matches §4.1; consensus set zero; `main.cpp` ≤ 40, `miner.cpp` ≤ 35, `rpc/mining.cpp` ≤ 35 | grep: `git diff --numstat ycash-legacy...HEAD` → `main.cpp 11+0`, `miner.cpp 10+2`, `rpc/mining.cpp 7+0`; the consensus-set `--stat` prints nothing. §1 above. **PASS** |
| 2 | Every behaviour-changing inserted statement guarded; the residue justified | grep: the §2.11 grep printed 7 lines, each justified in the §2.11 table; the plan's wording is wrong about the residue and about "two includes" (findings R1, R2). No unguarded behaviour change. **PASS with the wording corrected**; the named unit test `coinbase_flags_empty_without_flag` **does not exist in any tree** (finding R3) |
| 3 | The `ConnectBlock` check's seven preconditions, `check_null_hash`, `check_reverify` | yellowback_index_tests: `check_no_enforce`, `check_unhealthy`, `check_tip_mismatch`, `check_suspended`, `check_ibd_suppresses_reject`, `catchup_suppresses_reject`, `check_tripped_valve_never_rejects`, `act5_sunset_stops_rejection`, `check_null_hash`, `check_reverify` — all ran green this session. **PASS** |
| 4 | `DoS(0)` only, ever | grep: `git grep -n 'DoS([1-9]' -- src/yellowback src/rpc/yellowback*` → no output; `git diff … -- src/main.cpp \| grep '^+' \| grep -o 'DoS([0-9]*'` → `3  DoS(0`. The `banscore == 0` scenario is `yellowback_enforcement.py` case 1 on `-p5`. **PARTIAL** — greps pass, the multi-node ban proof is pending merge |
| 5 | Fail open on storage only; `EvaluateBlock` total | yellowback_index_tests: `check_storage_fault_accepts_and_sets_unhealthy`, `commit_storage_fault_sets_unhealthy`, `totality_every_lookup_misses`, `exception_boundary`, five `*_corpus_replay` cases — green. `git grep -nE '\bthrow\b\|\bassert\(' -- src/yellowback/state.cpp` → no output. **PASS** (fuzz *runs*, as opposed to replays, are §7) |
| 6 | `DisconnectBlock` undo inside `if (updateIndices)`; `VerifyDB -checklevel=4` leaves the hash unchanged | grep: fork `src/main.cpp:2799` reads `if (updateIndices && yellowback::g_yellowback) …`; `yellowback_index.py` asserts `nodes[2].verifychain(4, 20) == True` with the state hash unchanged (ran green this session). **PASS** |
| 7 | `CommitConnect` never reached under `fJustCheck` | yellowback_index_tests: the `fJustCheck` cases in `yellowback_index_tests.cpp` (`check_*` family; the check returns before `if (fJustCheck) return true;`). Structural: the commit is 78 lines *below* the `fJustCheck` early return. **PASS** |
| 8 | Kill switch and valve; `Rejected` survives `kill -9`; a consensus-invalid block is absent from `Rejected` | yellowback_index.py: steps `rejected_survives_kill9`, `killswitch_fresh_datadir`, `killswitch_rejected_hash_not_in_mapblockindex` — ran to "Tests successful" this session; plus `valve_trips_at_six_blocks` (unit). `yellowback_enforcement.py` cases 5, 7, 9 are on `-p5`. **PARTIAL** |
| 9 | Every rule identifier has a `// Rule:` / `# Rule:` tag found by the §7 loop | grep: the CI loop (run here with `-P`, mapping §13.3) reports **4 untagged: TPL-1 TPL-2 TPL-3 MINTPOL-1**. TPL-1/2 and MINTPOL-1 *are* tagged, but indented or inside a docstring, which the column-0 anchor misses; TPL-3 has no test tag anywhere. **FAIL on this tip** (finding R4) |
| 10 | Apply/undo identity and cold-rebuild equality under reorg stress with enforcement events | yellowback_state_tests: `apply_undo_identity_over_sequences`, `overlay_view_equivalence` green; `yellowback_index.py`'s reorg and `-reindex`/`-reindex-yellowback` sections green. `yellowback_reorg_stress.py --blocks 600` (3 seeds) **not run this session** — Phase 8. **PARTIAL** |
| 11 | `IsStandardTx` / `AreInputsStandard` pass for every template incl. the claim path | yellowback_script_tests: `mint3_templates_are_standard_under_sapling`, `mint3_templates_are_standard_under_canopy`, `red1_owner_path_verifies`, `red4_claim_path_verifies`, `mint3_vault_script_sizes_sigops_and_parse` — green. **PASS** |
| 12 | Determinism grep empty; `GetTime` only in `rpc/yellowback.cpp` and `policy.cpp` | grep: over `state.cpp math.h tag.cpp payload.cpp script.cpp view.cpp` for `GetTime\|GetArg\|rand\|double\|float\|mempool\|chainActive` → **no output**. `GetTime` outside `policy.cpp`: only `rpc/yellowback.cpp:358` and `:734`. §8.4 names `index.cpp` in the pure set, which is wrong — it legitimately reads `GetArg` (`index.cpp:890-896`, regtest params) and the mempool; the CI job already carves it out. **PASS with the item's file list corrected** (finding R5) |
| 13 | Coin locking: every owned YED output locked before `CommitTransaction`, across reorgs and restarts; `lockunspent` cannot unlock | yellowback_wallet_restore.py and `yellowback_wallet_lifecycle.py` exist on this branch and cover locking across restart; **H5's `yed_unlockcoin` does not exist yet** (`git grep yed_unlockcoin` → no output) — Phase 8 H1–H12 is not started. **NOT YET — owned by Phase 8** |
| 14 | The wallet refuses a mint before activation and under each halt with the matching `mintpol-*`; refuses a redeem/claim `MempoolCheck` rejects | yellowback_void_mint.py on this branch covers the VOID paths; the `mintpol-*` and claim refusals are `yellowback_claim.py` / `yellowback_pricefeed.py` on `-p6`, **pending merge**. **PARTIAL** |
| 15 | `getblocktemplate` carries the tag in all three carriers; a pool-rebuilt coinbase yields a readable tag | yellowback_mining.py — on this branch, with `gbt_shape_without_flag` and the three-carrier assertions; plus `yellowback_tag_tests/tag1_byte_budget_at_every_height_push_width`, `tag1_encoding_is_36_bytes_one_direct_push` (green this session). Not re-run end to end this session. **PARTIAL** |
| 16 | The index survives `kill -9` with the chainstate unflushed; refuses `-prune` | yellowback_index.py: `crash_unflushed_chainstate` (30 unflushed blocks), the beyond-`UNDO_KEEP` wipe-and-rebuild fallback, and the `-prune` refusal — all in the run that printed "Tests successful" this session. **PASS** |
| 17 | For every `Rejected` hash, `yed_getblockverdict` gives `blockInvalid == true` with a reason; refuses when the parent is not the tip | yellowback_index.py: step `verdict_parent_not_tip` (N12) plus the rejection section — green this session. `yellowback_enforcement.py` case 7 is on `-p5`. **PARTIAL** |
| 18 | Docs: no "no consensus change"; trust statement byte-identical to §8.1; runbook covers the seven topics | grep: `grep -n 'no consensus change' doc/yellowback.md` → no output; the audit job's `section()` diff of `doc/yellowback-spec.md` §8.1 against `doc/yellowback.md` "Trust statement" → **identical**; the spec's self-hash matches; `grep -qi` found all seven of `yellowbackenforce=0`, `reindex`, `valve`, `sunset`, `filter-only`, `catch-up`, `incident` in `doc/yellowback-mining.md`. **PASS** |
| 19 | No new build dependency; `configure.ac` untouched; no sockets in node Yellowback code | grep: `git diff --stat ycash-legacy...HEAD -- configure.ac` → no output; `git grep -nE '\bconnect\(\|socket\(\|curl' -- src/yellowback src/rpc/yellowback*` → no output. **PASS** |
| 20 | The shutdown sequence holds for the synchronous hooks (hooks run under `cs_main`, which `Shutdown()`'s flush takes) | reviewer: a reviewer-signed claim, **awaiting a human**. Mechanical companions `crash_unflushed_chainstate` and `rejected_survives_kill9` both green this session. |
| 21 | Stock parity (N10): fork binary without `-yellowback` indistinguishable from `ref/ycash` v4.5.0 | yellowback_stockparity.py **does not exist in any tree**; neither does `yellowback_stock_node.py`. **NOT YET — owned by Phase 5/8** (finding R6) |
| 22 | Shielded pool (N13): builder tests unchanged and green; the three new methods called only from `src/yellowback/` | grep: `git grep -n 'AddRawScriptOutput\|AddUnsignedTransparentInput\|SetLockTime' -- src ':!src/transaction_builder.*' ':!src/yellowback' ':!src/test'` → **no output**; `git diff --stat ycash-legacy...HEAD -- src/gtest` → no output; `./src/ycash-gtest --gtest_filter='TransactionBuilder*'` → **11 tests, 11 PASSED**. §5 below. **PASS** |
| 23 | Concurrency (P6): `lockorder` and `sanitizers` jobs green on the last seven nightly runs | job: `lockorder` and `sanitizers` are defined in `.github/workflows/yellowback-tests.yml` but **have never run** — `feature/yellowback-sf` has no remote branch (`git rev-parse origin/feature/yellowback-sf` → unknown revision). A static lock-order audit done here found a **real inversion** (finding R7). **FAIL / NOT YET** |
| 24 | Coverage (P6): `qa/yellowback-coverage-floor.sh` exits 0 on the last run's `lcov.info` | job: the script exists with per-file floors, but no `lcov.info` exists on this host and the `coverage` job has never run. **NOT YET — owned by Phase 8** |
| 25 | RPC contract (P7): `yellowback_rpc_contract.py` green on every PR; the wallet job's field check green | yellowback_rpc_contract.py exists and is registered in `qa/pull-tester/rpc-tests.py`; the plan's execution status records it green on 2026-09-11 with all 19 node commands matching `doc/yellowback-rpc-contract.json`. **Not re-run this session**; the audit job's registration cross-check is a static grep that passes. **PARTIAL** |

Summary: **11 PASS, 7 PARTIAL, 4 NOT YET, 2 FAIL, 1 awaiting a reviewer.**

---

## 5. Shielded-pool evidence (N13)

Four claims, each verified by reading `ref/ycash/src/main.cpp` at `v4.5.0` and the fork.

**(a) The `ConnectBlock` failure path discards `view` before any flush.** In `ConnectTip`:

```cpp
    {
        CCoinsViewCache view(pcoinsTip);
        bool rv = ConnectBlock(*pblock, state, pindexNew, view, chainparams);
        GetMainSignals().BlockChecked(*pblock, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state, chainparams);
            return error("ConnectTip(): ConnectBlock %s failed", …);
        }
        …
        assert(view.Flush());
    }
```
`ref/ycash/src/main.cpp:3610-3622`. `view` is a stack `CCoinsViewCache` over `pcoinsTip`; the
`!rv` path returns at `:3618` — **four lines before** the `assert(view.Flush())` at `:3622` — so the
cache is destroyed without writing. A Yellowback verdict returns through exactly this path
(`state.DoS(0, …)` is `state.IsInvalid()`), so nothing it touches can reach `pcoinsTip`: the Sprout
and Sapling anchors, the nullifier sets and the value-pool counters are all left as they were.
This is also the reason the check is placed *after* every consensus check rather than before: it can
only remove blocks from the accepted set, never add one.

**(b) TX-0 ignores shielded components.** `git grep` over `src/yellowback/` for
`vjoinsplit\|vShieldedSpend\|vShieldedOutput\|valueBalance\|JSDescription\|nullifier` returns
exactly two hits, both the *signature* of the `CValidationInterface::ChainTip` override
(`index.cpp:842`, `index.h:299`), which passes the two merkle trees through and never reads them.
The state machine reads only `tx.vout`, `tx.vin`, the `OP_RETURN` payload and the coinbase
scriptSig. A transaction may have any shielded content; Yellowback neither sees nor constrains it.

**(c) `DisconnectBlock`'s undo runs after the coins rollback.** The fork's call sits at
`src/main.cpp:2799`, immediately before `return fClean ? …`. Everything shielded is already undone
by then, in the baseline order: `view.SetNullifiers(tx, false)` (`:2707`),
`view.PopAnchor(blockUndo.old_sprout_tree_root, SPROUT)` (`:2754`),
`view.PopAnchor(…, SAPLING)` (`:2762-2764`), `view.SetBestBlock(pindex->pprev->GetBlockHash())`
(`:2776`). The call is additionally inside `if (updateIndices)`, so `VerifyDB` level 3
(`updateIndices = false`, `:5133`) never reaches it.

**(d) The `transaction_builder` extension is additive, and the existing tests are unchanged and
green.**

```
$ git grep -n 'AddRawScriptOutput\|AddUnsignedTransparentInput\|SetLockTime' -- src \
      ':!src/transaction_builder.*' ':!src/yellowback' ':!src/test'
$          (no output)

$ git diff --stat ycash-legacy...HEAD -- src/gtest
$          (no output)

$ ./src/ycash-gtest --gtest_filter='TransactionBuilder*'
[----------] 11 tests from TransactionBuilder (32933 ms total)
[  PASSED  ] 11 tests.
```

The upstream builder tests are gtests in `src/gtest/test_transaction_builder.cpp`, not a
`transaction_builder_tests` Boost suite; §8.4 item 22's phrasing ("the existing
`transaction_builder` tests") should be read as naming that file. The 11 cases include
`SaplingToSapling`, `SaplingToSprout` and `SproutToSproutAndSapling` — the shielded paths — and
all pass on the fork. The three added methods are `+17` lines in `transaction_builder.cpp` and
`+17/−1` in the header, and are reachable only from `src/yellowback/txbuilder.cpp` and the
Yellowback tests.

---

## 6. The statement requested from the Ycash maintainers (plan §8.5)

### What is being asked

A review of the hook lines — **11 changed lines in `src/main.cpp`, 12 in `src/miner.cpp`, 7 in
`src/rpc/mining.cpp`**, listed in full in §2 above — and of the evidence in this document for the
three bounded claims below.

### What is **not** being asked

Not a merge into Ycash. Not an endorsement of Yellowback. Not a statement about its economics, its
price feed, its fee, its medians, its σ multiplier, its activation floors, its launch bar, whether
pools will run it, or whether YED holds its peg. Those belong to the pool operators and to the
product owner, and the trust statement (§8.1 of the plan, reproduced verbatim in
`doc/yellowback.md`) says so.

### The three bounded claims

1. **No consensus file changes.**
   `git diff --stat ycash-legacy...feature/yellowback-sf -- src/consensus src/script src/primitives
   src/pow src/chainparams.cpp src/wallet/wallet.h src/wallet/wallet.cpp src/txdb.cpp src/txdb.h
   configure.ac` is empty. Verified in this session (§1). On this tip `src/wallet` as a whole is
   also empty.

2. **Stock nodes are unaffected.** A node started without `-yellowback` has `g_yellowback == nullptr`;
   every behaviour-changing insertion is inside `if (g_yellowback)`. The unguarded differences are
   three `#include` lines, one `LOCK(cs_main)` scope around an unchanged call, one empty-optional
   declaration, and a `+ COINBASE_FLAGS` append that concatenates an empty script.
   **Caveat the reviewer should know:** the mechanical proof of this claim —
   `yellowback_stockparity.py`, which diffs the fork binary against a `ref/ycash` v4.5.0 binary over
   a scripted scenario — **has not been written yet** (§8.4 item 21). What supports the claim today
   is the diff itself, which is 30 lines and is printed in §2 in full.

3. **The hook cannot corrupt the chainstate or the shielded pool.** The check runs in
   `ConnectBlock`'s window *after* every consensus check and returns through the same
   `state.DoS(0, …)` path any other failure uses, so a rejection discards `view` before any flush
   (`ConnectTip`, `ref/ycash/src/main.cpp:3610-3622`) and leaves the Sprout/Sapling anchors and the
   value-pool counters untouched; the state machine never reads a shielded component; the commit
   runs after every write that can `AbortNode`; the undo runs after the coins rollback and only
   under `updateIndices`; the `transaction_builder` extension is additive and called only from
   `src/yellowback/`. Evidence in §5.

### The honest facts, including the uncomfortable ones

- **The Ycash team holds no kill switch. Pool operators do** (`-yellowbackenforce=0`). If a split
  happens it is resolved by pools and bounded mechanically by the work valve (six blocks, ACT-7),
  not by the maintainers.
- **A stock pool's blocks can be orphaned.** This is N3 and it is reusable: an owner-path vault
  spend with no burn costs an attacker one minimum collateral, is relayed by every stock node, and
  is re-included after every orphaning. A pool that runs nothing at all can therefore have honest
  blocks orphaned by a transaction it cannot see. **This is accepted, not mitigated.** The remedy
  asked of every pool — participating or not — is to run the module in filter-only mode
  (`-yellowbackenforce=0`), which costs nothing and never rejects a block.
- **The signal count is self-reported and forgeable** (N4). Anyone can set the signal bit in a
  coinbase tag. A minority of genuine enforcers plus forged bits can reach the floors. The count
  under-counts only among honest pools; the mechanical backstop is the work valve, not the count.
  "Enforcement" means majority in fact, never majority by count.
- **A quote-tag majority is cheaper than a hashpower majority** (L9). At 50 % window fill, a pool
  with 26 % of blocks holds more than half the quote tags and sets the medians when the windows
  roll. The defences are the two-thirds fill floors on the mid and slow windows, the 3× σ cap and
  the min/max selectors — not an impossibility argument.
- **While enforcement is suspended, neither vault path is policed.** Below 50 % signalling,
  collateral can leave a vault without its burn, and the YED so unbacked stays in circulation.
- **Enforcing nodes never ban stock peers** — not for a rejected block, not for its descendants
  (N1) — never reject during initial sync or `-reindex` (N2), and never reject a block the network
  has already built six blocks on (L11).
- **Every release enforces only until a sunset height** about a year past its start (L8), so two
  releases with different rules can never both be enforcing.
- **Release cadence (§11):** a `ycash-dd` release within **14 days** of every Ycash release, with
  the sunset never set beyond the next known Ycash network-upgrade activation height. A Ycash
  network upgrade is therefore never blocked or lagged by this work; if the fork ever did lag, the
  pools on it drop off the network and the enforcing set collapses — which is a cost borne by
  Yellowback, not by Ycash.

### Where this statement lives

It is here, in `doc/yellowback-review.md`, and not in `doc/yellowback.md`. That file's
"Trust statement" section is byte-locked to §8.1 of `doc/yellowback-spec.md` by the CI `audit`
job's `diff <(section …) <(section …)` step (verified identical in this session); adding the §8.5
statement there would break that check and is not what §8.1 is for.

---

## 7. What this document does **not** claim

- **Phase 5 is in progress.** `yellowback_enforcement.py` — the scripted proof of `banscore == 0`
  after a rejection and two relayed descendants, of the valve trip at six blocks, of the kill
  switch's reconsider loop, and of `valve_catchup_offline_node` — lives on
  `feature/yellowback-sf-p5` and **is not on this branch**. No row above counts it as passing.
- **Phase 6 is 7 of 9.** `yellowback_claim.py` (the sweep, the sunset-vs-abandonment separation,
  `sweep_relayed_by_own_node`) and `yellowback_pricefeed.py` (`price1_quote_majority_needs_fill`)
  live on `feature/yellowback-sf-p6` and are not on this branch.
- **Phase 8 has not started.** Wallet hardening H1–H12 does not exist (`yed_unlockcoin` is absent
  from the tree); `yellowback_runbook.py`, `yellowback_rc1.py`, `yellowback_stockparity.py` and
  `yellowback_stock_node.py` do not exist in any tree; the rc1 run-through has never been performed.
- **The nightly CI jobs have never run.** `lockorder`, `sanitizers`, `coverage`, `nightly` and
  `weekly-fuzz` are defined in `.github/workflows/yellowback-tests.yml`, but the branch has never
  been pushed (`git rev-parse origin/feature/yellowback-sf` → *unknown revision*), so there are no
  runs to link and no `lcov.info` to check floors against. Every "seven green nightly runs"
  requirement (§8.4 items 23, 24) is unmet, not merely unverified.
- **The 8 CPU-hour fuzz run has not been performed** and cannot be performed on this host: Apple
  clang (Xcode 17) ships no libFuzzer runtime, so `--enable-fuzz-main` needs a Homebrew LLVM
  (`docs/mapping.md` §13.1). What *is* green here is the deterministic **corpus replay** of all
  five targets inside `test_bitcoin` — a regression check over saved inputs, not a fuzzing run.
- **Phases 9 and 10 need real pools** and cannot be executed on one machine. Nothing here says
  anything about mainnet behaviour, hashpower distribution or the launch bar (L2: ≥ N independent
  pools, none above X %), which is measured and published before any announcement.
- **Two `test_bitcoin` cases fail, and they failed before this fork existed.** The full suite:

  ```
  $ ./src/test/test_bitcoin
  Running 537 test cases...
  test/main_tests.cpp:107: error: in "main_tests/subsidy_limit_test": check nSum == 2099999990760000LL has failed [2099999981520000 != 2099999990760000]
  wallet/test/rpc_wallet_tests.cpp:1274: error: in "rpc_wallet_tests/rpc_z_sendmany_internals": check out1.scriptPubKey.AddressHash().GetHex() != out2.scriptPubKey.AddressHash().GetHex() has failed
  *** 2 failures are detected in the test module "Bitcoin Test Suite"
  ```

  Both files are byte-identical to the pin (`git diff --stat ycash-legacy...HEAD --
  src/test/main_tests.cpp src/wallet/test/rpc_wallet_tests.cpp` → empty), and neither touches
  Yellowback code. They are pre-existing v4.5.0 failures on this host, recorded here so that no
  reviewer reads a two-failure suite as a fork regression.
- **`yellowback_index.py` was run once, in this session, to completion** ("Tests successful", 4,106
  blocks, portseed 91). No other functional script was run in this session. Anything this document
  says about `yellowback_mining.py`, `yellowback_lifecycle.py`, `yellowback_rpc_contract.py`,
  `yellowback_activation.py` or `yellowback_quote.py` rests on the plan's recorded execution
  status, not on a run performed here.

---

## 8. Where the evidence contradicts the plan

Each of these is also a row in `docs/mapping.md` §13.9.

**R1 — §8.4 item 2's grep cannot be empty, by construction.** The item describes the residue as
"exactly the two `#include` lines, the K17 `LOCK(cs_main)` and the `+ COINBASE_FLAGS` append". The
grep is line-based; the fork's guarded statements span two or three lines each, so seven lines
survive it. Every one is a continuation, a brace, or an empty-optional declaration (§2.11). The
item's *intent* holds; its wording does not. The reviewer should read the §2.11 table, not expect
silence from the grep.

**R2 — three `#include` lines, not two.** `src/main.cpp` and `src/rpc/mining.cpp` include
`yellowback/index.h`; `src/miner.cpp` includes `yellowback/policy.h` (the plan's §4.1 predicted
`index.h` there). Immaterial to behaviour; the count in §4.1 and §8.4 item 2 is stale.

**R3 — the unit test `coinbase_flags_empty_without_flag` does not exist.** §8.4 item 2 and §4.1
both cite it as the proof that `COINBASE_FLAGS` is empty without `-yellowback`.
`git grep -l coinbase_flags_empty_without_flag` returns nothing in `ycash-dd`, `wt/p5` or `wt/p6`.
The property is currently supported only by inspection (nothing else in the tree assigns
`COINBASE_FLAGS`) and by `gbt_shape_without_flag` in `yellowback_mining.py`, which checks the RPC
shape rather than the scriptSig. **Someone owns writing it** — it is one `BOOST_AUTO_TEST_CASE`.

**R4 — §8.4 item 9 fails on this tip: four rule identifiers have no tag the CI loop can see.**
Running the loop from the `audit` job (with `-P`, per mapping §13.3, since BSD `regcomp` has no
`\b`):

```
untagged: TPL-1 TPL-2 TPL-3 MINTPOL-1
```

TPL-1 and TPL-2 are tagged in `qa/rpc-tests/yellowback_mining.py` at lines 410, 469, 509, 537 —
but indented inside the method body, and the loop anchors `^(//|#)` at column 0. MINTPOL-1 is
tagged only inside a docstring (`yellowback_lifecycle.py:20`, `"""# Rule: MINTPOL-1 …`), which the
same anchor misses. **TPL-3 has no test tag anywhere** — the only occurrences are source comments
in `index.h` and `policy.h`. Mapping §13.5 records the docstring trap and §13.4 records the
column-0 rule; the Phase 4 tags in `yellowback_mining.py` did not follow it. This is a
documentation-coverage failure, not a correctness failure, but §8.4 item 9 is blocking from
Phase 6 and it is red.

**R5 — §8.4 item 12 names `index.cpp` in the determinism set; it cannot be.** `index.cpp` reads
`GetArg` at `:890-896` (the four regtest parameters) and includes `txmempool.h` for
`RemoveInvalidVaultSpends`. Both are correct and intended. The plan's §3.10 lists the pure set as
`{state, math, tag, payload, script, view}`, which *is* clean (grep returns nothing), and the CI
`audit` job already applies the narrower rule to `index.cpp` (no clock, no floating point). §8.4
item 12's file list should be corrected to match §3.10 and the CI job.

**R6 — the mechanical basis of §8.5 claim 2 does not exist yet.** `yellowback_stockparity.py` and
`yellowback_stock_node.py` are named in §4.1, §8.2 and §8.4 item 21, and in the trust statement's
supporting material. Neither file exists in `ycash-dd`, `wt/p5` or `wt/p6`. Until one does, "stock
nodes are unaffected" rests on reading the 30-line diff in §2, which is a reasonable thing to ask a
reviewer to do but is not what the plan promised.

**R7 — a real lock-order inversion in `yed_getbalance`.** The recorded order (§4.3, N25) is
`cs_main → cs_wallet → mempool.cs → cs_yellowback`, and the plan says explicitly: *"no RPC may take
`mempool.cs` after `cs_yellowback`"*. In `src/rpc/yellowbackwallet.cpp`:

```
137:    LOCK2(cs_main, pwalletMain->cs_wallet);
138:    LOCK(index.cs_yellowback);
...
143:        LOCK(mempool.cs);
```

`yed_getbalance` takes `cs_yellowback` at line 138 and then `mempool.cs` at line 143 — the
inversion the rule forbids. The counterpart order (`mempool.cs` then `cs_yellowback`) is taken by
`CreateNewBlock` via `TemplateView()` inside its `LOCK2(cs_main, mempool.cs)`, and by
`RemoveInvalidVaultSpends` on the `ConnectTip` path. Both sides also hold `cs_main`, which is what
has kept this from deadlocking in practice — but `cs_main` is not a guarantee the design relies on,
and `DEBUG_LOCKORDER` would flag it. The file's own header comment
(`yellowbackwallet.cpp:8`) states the order without `mempool.cs` at all, which is how it was missed.
This is the exact defect §8.4 item 23 and the Phase 8 lock-order grep exist to catch, and it is why
the `lockorder` job needs to run before rc1. **Not fixed here** — this document is evidence-gathering
and the file is owned by another phase; it is reported so that the owner fixes it.

---

## 9. Reproducing everything in this document

```bash
export PATH="/opt/homebrew/opt/libtool/libexec/gnubin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/bin:$PATH"
export CARGO_TARGET_DIR="$PWD/target"
make -C src -j8 test/test_bitcoin ycashd ycash-cli ycash-gtest

# §1  diff budget
git diff --numstat ycash-legacy...HEAD -- src/main.cpp src/miner.cpp src/rpc/mining.cpp
git diff --stat   ycash-legacy...HEAD -- src/consensus src/script src/primitives src/pow \
    src/chainparams.cpp src/wallet src/txdb.cpp src/txdb.h configure.ac

# §2  the hooks and the residue
git diff ycash-legacy...HEAD -- src/main.cpp src/miner.cpp src/rpc/mining.cpp
git diff ycash-legacy...HEAD -- src/main.cpp src/miner.cpp src/rpc/mining.cpp \
    | grep '^+' | grep -v 'g_yellowback\|#include\|LOCK(cs_main)\|COINBASE_FLAGS\|^+$\|^+++'

# §3, §4  unit evidence
./src/test/test_bitcoin --run_test='yellowback_*'
./src/test/test_bitcoin --run_test='yellowback_index_tests/mempoolcheck_bench' --log_level=message

# §4  the greps
git grep -n 'DoS([1-9]' -- src/yellowback src/rpc/yellowback*
git grep -nE '\bthrow\b|\bassert\(' -- src/yellowback/state.cpp
git grep -nE 'GetTime|GetArg|\brand\b|\bdouble\b|\bfloat\b|mempool|chainActive' \
    -- src/yellowback/state.cpp src/yellowback/math.h src/yellowback/tag.cpp \
       src/yellowback/payload.cpp src/yellowback/script.cpp src/yellowback/view.cpp
git grep -nE 'evhttp|socket|\bconnect\(' -- src/yellowback src/rpc/yellowback*

# §5  shielded pool
git grep -n 'AddRawScriptOutput\|AddUnsignedTransparentInput\|SetLockTime' -- src \
    ':!src/transaction_builder.*' ':!src/yellowback' ':!src/test'
git diff --stat ycash-legacy...HEAD -- src/gtest
./src/ycash-gtest --gtest_filter='TransactionBuilder*'

# §3, §4  functional (portseed and tmpdir must be unique per concurrent run)
export DYLD_LIBRARY_PATH="$(brew --prefix openssl@3)/lib"
BITCOIND="$PWD/src/ycashd" ../.venv/bin/python -u qa/rpc-tests/yellowback_index.py \
    --srcdir="$PWD/src" --tmpdir=<scratch> --portseed=91

# §8 R4  the rule-to-test loop (use -P on macOS; BSD regcomp has no \b)
for id in TPL-1 TPL-2 TPL-3 MINTPOL-1; do
  git grep -qP "^(//|#) Rule: .*\b$id\b" -- 'src/test/yellowback_*.cpp' 'qa/rpc-tests/yellowback_*.py' \
    || echo "no test tagged: $id"
done
```

The workspace-level crosswalk, the normative protocol and the development plan live outside this
fork, in `docs/mapping.md`, `docs/spec/` and `docs/plans/yellowback-v2-development-plan.md`.
