# Post-quantum validation cost and DoS measurement (quantum plan Q6, §4.9)

Measured 2026-10-10 on `upgrade/quantum-vaults` at `0131122ab` with
[`contrib/yellowback/pq-dos/pq_dos.py`](../contrib/yellowback/pq-dos/pq_dos.py); the raw output is
[`results-20261010.json`](../contrib/yellowback/pq-dos/results-20261010.json). It answers the plan's §4.9 DoS item:
validation time of near-full 2 MB blocks of SLH-DSA spends, of Falcon spends and of invalid PQ signatures, the
worst case left by spec ruling R-B1 (the staged-output class), against a full P2PKH block and the Sapling figure the
yb-calibration study lacked, plus the mempool under a flood of 8 KB PQ transactions at the relay floor.

## Method

- **Build.** `ycashd` from the stock depends tree (`config.site`: `CFLAGS`/`CXXFLAGS` `-pipe -O3`), plain
  `./configure` (debug disabled, no sanitizers), aarch64-apple-darwin25.
- **Machine.** Apple M5, 4 performance + 6 efficiency cores, 24 GB. The machine was shared with other builds
  (1-minute load 12 to 28 during the runs), so a thread could land on an efficiency core or wait. Each block was
  validated 7 times and the tables give the **minimum**, the best estimate of the uncontended cost. Parallel
  speedups are understated.
- **Blocks.** Regtest, `UPGRADE_VAULT` and `pqFalconHeight` at 105, `fRequireStandard = false`. Node 0 builds each
  block, from its mempool or, where the stock template refuses it, by hand: getblocktemplate's coinbase, the
  transactions, and an Equihash solution found in Python. Seven fresh nodes that never saw the transactions (empty
  mempool, empty signature cache) each receive the block by `submitblock`. Nodes 1 to 4 run `-par=1` (scripts
  checked inline, one thread). Nodes 5 to 7 run the default `-par`, which is one check thread per core (10 here).
- **Figures.** `-debug=bench`'s `Verify N txins` (the transaction loop plus the script-check queue's wait, which
  includes the coin lookups) and `Connect block` (all of ConnectTip, including the undo write and the coins flush),
  the process CPU time spent in the `submitblock` call, peak RSS sampled every 50 ms, and the growth of the undo
  (`rev*.dat`) files. Sapling proofs are checked in `ContextualCheckBlock`, before ConnectBlock and outside the
  bench lines, so Sapling uses the call's CPU time.
- **Signatures** come from the tree's own vendored C (`src/crypto/pq/`) through `pqdos_shim.c` (ctypes). The
  framework's independent Python verifier checks one signature per batch.

## 1. Full blocks on a fresh node

| Block (≈ 2 MB) | Bytes | PQ / ECDSA verifies | `-par=1` verify | µs per verify | default `-par` verify | Connect block `-par=1` / default | Peak RSS | Undo |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| P2PKH, 13,200 inputs (baseline) | 1,948,714 | 13,200 ECDSA | 0.262 s | 19.8 | 0.066 s | 0.281 / 0.092 s | 85 MB | – |
| SLH-DSA `TX_PQPKH`, 240 inputs | 1,917,273 | 240 | 0.059 s | 247 | 0.014 s | 0.060 / 0.022 s | 62 MB | – |
| Falcon `TX_PQPKH`, 1,200 inputs ¹ | 1,945,832 | 1,200 | 0.032 s | 26.5 | 0.007 s | 0.034 / 0.010 s | 78 MB | – |
| **Staged SLH-DSA** (R-B1 residual) ² | 1,969,880 | 48,000 | **18.2 s** | 379 | **4.07 s** | **22.1 / 8.0 s** | **1.22 GB** | **383 MB** |
| Staged Falcon ² | 1,969,880 | 48,000 | 2.17 s | 45 | 0.73 s | 3.1 / 1.5 s | 371 MB | 78 MB |
| Legacy staged ECDSA loop ³ | 1,969,880 | 3,216,000 ECDSA | 65.2 s | 20.3 | 20.7 s | 65.6 / 21.1 s | 143 MB | 15 MB |

The undo column is blank where the block's undo was still in the write buffer when the files were measured.

¹ Hand-built. The stock template counts 20 policy sigops per `OP_CHECKPQSIG` against `MAX_BLOCK_SIGOPS` = 20,000
(spec F-5 ruling; `miner.cpp:566`), so a node's own template holds at most 1,000 PQ spends. Consensus counts none,
so 1,200 Falcon spends fit by size. 240 SLH spends are under the cap.

² A bare output `<sig chunks> <s> <pk chunks> <p> <keyHash> <scheme> OP_CHECKPQSIG OP_NOT` (7,974 B for SLH-DSA,
1,613 B for Falcon) carrying a real signature over another message. The spending transaction does not exist when
the output is made, so no signature in it can verify. Every spend therefore runs the full verification (one per
input, R-B1) and `OP_NOT` makes it valid with an empty scriptSig. Each spend is a 41-byte input: 24 transactions of
2,000 inputs fill the block. Making the outputs took 194 full blocks for SLH-DSA (≈ 380 MB, about 4 hours of
blocks at the 75 s spacing) and 40 for Falcon. Both the mempool and the template refuse these spends: the 20-sigop
policy count of the spent output exceeds `MAX_STANDARD_TX_SIGOPS` and the template's 20,000. Only a miner running
its own software can build such a block, so it was hand-built.

³ The reference R-A1/R-B1 compare against, which consensus already admits: a bare output `<sig> <pk>
[OP_2DUP OP_CHECKSIG OP_DROP]×66 OP_CHECKSIG OP_NOT` (306 B, 200 ops, a DER signature over another message). That is
67 ECDSA verifications per 41-byte spend. Legacy sigops are counted when the output is created (67 per output,
20,000 per block, so 163 small creation blocks), never when it is spent.

The per-verify figures include each input's script, sighash and coin overhead. For the staged SLH-DSA block that
overhead is ≈ 130 µs on top of the bare verify: the 8 KB scriptCode is hashed by ZIP-243, and the per-transaction
`CCoins` model loads each creating transaction's ≈ 1 MB of outputs to spend one. The bare verify, timed in the
shim on the same machine, is SLH-DSA 252 µs and Falcon 21 µs (minimum of 3 × 200 / 4,000 calls).

## 2. Sapling (the missing calibration figure)

Proof verification in v4.5.0 is serial in `ContextualCheckBlock` (`main.cpp:1248-1290`): `-par` has no effect, and
wall time equals CPU time.

| Block | Sapling | CPU in `submitblock` (min / median) | Per description |
|---|---:|---:|---:|
| 4 z→t transactions | 200 spends | 0.79 / 0.81 s | **≈ 4.0 ms per spend** (proof + spend-auth signature) |
| 4 t→z transactions | 200 outputs | 0.90 / 1.06 s | **≈ 4.5–5.3 ms per output** |
| 200 t→z transactions | 200 outputs | 0.97 / 1.00 s | ≈ 4.9 ms per output (with 200 ECDSA inputs) |

Extrapolated to a full 2 MB block, still single-threaded:

- A full block of z→z transactions (1 spend + 2 outputs, 2,373 B, 842 per block) takes ≈ 842 × (4.0 + 2 × 4.5) ms
  = **≈ 11 s**, or ≈ 13 s at the medians.
- A block of spends only (384 B each, ≈ 5,000 per block) takes **≈ 20 s**.
- A block of outputs only (948 B each, ≈ 2,100 per block) takes ≈ 9.5 s.

For yb-calibration's `sapling_block_verify_s`: **≈ 11 s** for its `sapling_z2z` block (842 transactions).

## 3. Invalid PQ signatures

Each case used the correct key hash, exact sizes and a garbage signature.

| | SLH-DSA | Falcon | P2PKH |
|---|---:|---:|---:|
| Bare verify (shim): valid / wrong message / random bytes | 252 / 317 / 268 µs | 21.3 / 22.0 / < 0.1 µs | – |
| `sendrawtransaction`, 1 input: valid accepted (median of 30) | 1.54 ms | 0.44 ms | 0.34 ms |
| `sendrawtransaction`, 1 input: garbage rejected | 1.93 ms | 0.48 ms (random) / 0.57 ms (well-formed) | – |
| Reject reason | `mandatory-script-verify-flag-failed` | same | – |
| P2P: one such transaction from a peer | Misbehaving 0 → 100, disconnected (a non-local peer is banned) | same | – |

- **No early exit for SLH-DSA.** A garbage SLH-DSA signature costs a full verification: verification rebuilds
  the root and compares it at the end.
- **Falcon fails fast on random bytes.** Random bytes fail Falcon's decoder at once. A well-formed wrong
  signature, such as a valid one for another message, costs a full verification.
- **How many verifications a peer can force.** The receiver runs the failing input three times before it scores
  the peer:
  1. the standard-flags check;
  2. the retry under the previous branch ID (`main.cpp:2646`);
  3. the mandatory-flags retry (`main.cpp:2662`).

  A valid PQ input runs twice in the mempool (the standard pass and the mandatory re-check, `main.cpp:1872/1888`).
  PQ verifications are never cached: `CheckPQSig` bypasses `CachingTransactionSignatureChecker`
  (`script/sigcache.cpp:92` caches ECDSA only). So a relaying node verifies a PQ signature again when it connects
  the block, and a miner verifies it once more in `TestBlockValidity`.
- **An invalid signature in a block** is bounded by the valid-block case: the block is rejected at the failing
  input (with `-par=1`) or the check queue stops (with threads), so it never costs more than the valid full block
  in §1.

## 4. Mempool under a flood of 8 KB PQ transactions

The flood was 400 SLH-DSA transactions of 8,044 B each (1 input, 1 output) paying exactly the relay floor (809 zat,
1× `-pqfeerate`), sent into a node started with `-mempooltxcostlimit=2000000`. Then 50 SLH-DSA transactions paying
1,053 zat (≥ `DEFAULT_FEE`) and 100 P2PKH transactions paying 1,000 zat were sent into the full pool.

| | Result |
|---|---|
| Acceptance | 400 / 400 accepted, 621 tx/s (median 1.06 ms each) from one RPC client |
| Pool at the cap | ≈ 246 transactions, 1.98 MB; ZIP-401 cost ≈ 8.8 KB per transaction; evicted transactions are barred for 60 min |
| After the mixed batch | 154 / 400 floor-fee PQ, 43 / 50 PQ paying ≥ 1,000 zat, 98 / 100 P2PKH remain |
| Memory | RSS 45.8 → 56.7 MB |

- **Floor-fee PQ transactions are evicted first.** An 8 KB SLH-DSA transaction at the 1× floor pays less than
  `DEFAULT_FEE`, so ZIP-401 adds `LOW_FEE_PENALTY` (16,000) to its eviction weight (`mempool_limit.cpp:156`).
- **Wallet-built transactions avoid the penalty.** They pay `max(DEFAULT_FEE, rate × size)` ≥ 1,000 zat. At
  yb-calibration's m = 50 the fee would be ≈ 40,000 zat.
- **The CPU the node spends accepting is not the bottleneck.** About 1 ms per 8 KB, two verifications, is
  thousands of times what relay bandwidth can deliver.

## 5. Against the plan, the spec rulings and yb-calibration

| Claim | Source | Measured |
|---|---|---|
| ECDSA 18.9, SLH-DSA 235.0, Falcon 18.9 µs per op | yb-calibration `855faaa` (Q1 table) | 19.8, 247, 26.5 µs per input in a block; bare 252 / 21 µs |
| `ecdsa_block_verify_s` 0.255; `slh_block_verify_s` 0.058 (248); `falcon_block_verify_s` 0.0225 (1,188) | yb-calibration | 0.262; 0.059 (240); 0.032 (1,200) |
| `sapling_block_verify_s` MISSING | yb-calibration | ≈ 11 s (842 z→z), single-threaded; 4.0 ms per spend, 4.5 ms per output |
| "≈ 20 µs/op ECDSA vs ≈ 22 µs SLH, ≈ 7 µs Falcon" | spec R-A1 | the SLH-DSA figure is ≈ 11× too low (247–379 µs); Falcon is 3–4× too low (21–27 µs) |
| "staged outputs ≈ 48k verifies per 2 MB block" | spec R-B1 (re-review A) | exactly 48,000 in 1.97 MB |
| "the legacy CHECKSIG-at-spend order" | spec R-B1 | true by count. By time: SLH-DSA 18.2 s (one thread) / 8.0 s (10 threads); see below |
| ≈ 252 SLH spends per block | plan §6 (withdrawn by R-A1) | 240 standard spends (size-bound) |
| a 1,000-check template cap | yb-calibration `policy_pq_checks_per_block` | confirmed (`miner.cpp:566`) |

How the staged SLH-DSA block (18.2 s on one thread, 8.0 s on 10) compares:

- ≈ 70× a full P2PKH block.
- ≈ 1.7× a full z→z Sapling block.
- About the same as a spend-only Sapling block.
- 3.6× below the legacy 67-CHECKSIG staged block (65 s / 21 s), which consensus already admits.

## Does any result change a ruling?

**No ruling has to change. Two corrections and one new observation are recorded for the coordinator.**

1. **R-A1's per-op figures.** SLH-DSA verification costs ≈ 250 µs, not ≈ 22 µs, so "the same order as the legacy
   OP_CHECKSIG loop" holds only by count of verifications. R-B1's cap (one PQ check per EvalScript) is what
   keeps the class bounded. With it, the worst a 2 MB block can do is ≈ 18 s of verification on one thread, or
   ≈ 8 s of connect time on 10 threads. That stays inside the envelope Ycash already accepts:
   - a legacy multi-CHECKSIG staged block: 65 s / 21 s;
   - a spend-only Sapling block: ≈ 20 s, which cannot be parallelised in v4.5.0.

   The class is still non-standard and miner-only, and setting it up costs ≈ 380 MB of earlier block space.
2. **R-B1's "legacy CHECKSIG-at-spend order".** In time, the staged SLH-DSA class is ≈ 18× a legacy single-CHECKSIG
   staged block (48,000 × 20 µs ≈ 1 s), not the same order. The text should say "below the legacy multi-CHECKSIG
   class and on par with a Sapling spend block".
3. **New: memory and undo amplification.** The staged SLH-DSA block writes **383 MB of undo data** and peaks at
   **≈ 1.2 GB RSS** for a 2 MB block. Each 41-byte input spends a 7,985-byte output, and the per-transaction
   `CCoins` model loads the creating transaction's ≈ 1 MB of outputs per first touch. For comparison, the legacy
   staged block wrote 15 MB of undo at 143 MB peak, and the staged Falcon block 78 MB at 371 MB.
   - **Not new to PQ:** any bare output up to the 10,000-byte executed-script limit, spent by a tiny input, gives the
     same ratio (≈ 240× its spend size) today.
   - **But R-B1 did not price it.** It is worth one line in plan §6, and in the release notes' resource guidance
     (≥ 2 GB free RAM per node for adversarial blocks).

Non-ruling follow-ups, both policy or performance work:

- **(a) Cache PQ verifications.** A PQ sigcache entry in `src/script/sigcache.cpp`, keyed on (scheme, key,
  signature, sighash), would stop the re-verification at block connect. That file is under `src/script/`, so it
  goes through the review gate.
- **(b) The template's 1,000-check cap.** It makes a node's own Falcon blocks size-bound at 1,000 spends rather than
  1,200. That is harmless, and the yb-calibration study already models it.

### Transactions that spend a V or I (audit 07/F-1 and its review, measured 2026-10-13)

Until 2026-10-12 `AcceptToMemoryPool` scored **every** script failure of a transaction with a V or I input DoS 0
(upgrade plan finding 25), so a peer could add any template outpoint and relay invalid PQ or ECDSA signatures
without being scored. A first narrowing matched the error in the reject reason; its review showed that
`ContextualCheckInputs` reports only the first failing input, so a garbage signature on a standard `TX_PQCHANNEL`
input (`<client slot> OP_CHECKPQSIG OP_VERIFY`) beside an untouched V spent with `OP_3` still scored 0.

The rule now (`main.cpp`, both node lines): a transaction that reaches the script checks has at most one template
input (`bad-txns-vault-multi` is refused before). `FindVaultTemplateInput` finds it, and its script runs **alone
first**, with the same flags, set snapshot and spend height. If it fails with an error of its own that depends on
the set state at this node's tip (`IsVaultSetStateScriptError`), the transaction is refused with DoS 0 and nothing
else is verified:

- `SCRIPT_ERR_SETSIG` (`OP_CHECKSETSIG`: membership, threshold and dormancy move with the tip);
- `SCRIPT_ERR_VERIFY` when its selector is 3: the templates' one `OP_VERIFY` follows `OP_CHECKSETDORMANT`, which
  pushes false while the set is not released.

Otherwise the stock `ContextualCheckInputs` calls run and score every failure as stock: 100 for a mandatory-flag
failure, 10 for `old-consensus-branch-id`, 0 for a non-mandatory flag. `SCRIPT_ERR_SETSIG_COUNT` is not exempt: a
template runs exactly one `OP_CHECKSETSIG`. Measured with `pq_dos.py --scenarios=invalid` against the ycash-dd
binary at `e4528eeb0` and the ycash6 binary at the same revision of `fix/mempool` (raw output
[`results-20261012-vault-dd.json`](../contrib/yellowback/pq-dos/results-20261012-vault-dd.json),
[`results-20261012-vault-ycash6.json`](../contrib/yellowback/pq-dos/results-20261012-vault-ycash6.json)); the
machine's 1-minute load was 10 to 25, so the times are upper bounds:

| Transaction (one per message, from a mininode peer) | Bytes | RPC reject, median of 30 (dd / ycash6) | Per KB (dd / ycash6) | P2P result (both lines) |
|---|---:|---:|---:|---|
| V owner spend (selector 2), garbage SLH-DSA signature | 8,045 | 3.14 / 2.58 ms | 0.40 / 0.33 ms | Misbehaving 0 → 100, disconnected |
| V owner spend (selector 2), well-formed wrong Falcon signature | 1,684 | 0.81 / 1.04 ms | 0.49 / 0.63 ms | Misbehaving 0 → 100, disconnected |
| Wrong ECDSA signature on a P2PKH input beside a V input | 252 | 0.90 / 0.64 ms | 3.6 / 2.6 ms | Misbehaving 0 → 100, disconnected |
| UNLOCK (selector 1) with a non-member's 65-byte set signature | 523 | 0.36 / 0.90 ms | 0.70 / 1.8 ms | banscore 0, still connected |

Before 07/F-1 the first three rows were banscore 0. `qa/rpc-tests/vault_dos.py` (merge gate, both lines) also
covers: an OWNER-RELEASED spend while the set is not released (0); an UNLOCK signed by a member whose JOIN block the
receiver does not have (0); the PQ-channel case above (0 while the set is not released, with the verdict logged for
the template input so the channel signature is never verified; 100 once it is released); and an honest
OWNER-RELEASED spend relayed from a tip one block past the release (0, then accepted).

**What a peer can still force at DoS 0, accepted.** Only the template input's own evaluation up to its
state-dependent failure: for selector 3 the dormancy check, which comes before any signature; for `OP_CHECKSETSIG`
one recovery of a garbage 65-byte set signature (`CheckSetSigs` returns at the first bad signature), plus one more
in the retry under the previous epoch's branch ID, so **two** in all. That cannot be told apart from honest relay:
a membership change on the peer's tip produces the same `SCRIPT_ERR_SETSIG`. The retry keeps the stock diagnosis:
set signatures that pass under the previous branch ID take the stock route, which answers `old-consensus-branch-id`
(DoS 10) as for any other input. The isolated check runs before `Consensus::CheckTxInputs`, so a transaction that
is also value-invalid but fails `OP_CHECKSETSIG` scores 0, at the same cost of at most two recoveries. The price of exact
attribution on valid spends: an owner spend's second SLH-DSA or Falcon verification hits the signature cache
(the isolated check stores), so it costs no extra verification; a set-signature spend (UNLOCK, CANCEL) pays `k`
extra recoveries, which are not cached.

**Regtest-only boundary.** A Falcon owner spend relayed one block ahead of `-pqfalconheight` now scores 100, as a
Falcon `TX_PQPKH` spend does; on mainnet and testnet Falcon activates with `UPGRADE_VAULT`.

### Orphans (audit 07/F-6)

A transaction whose parent the node has not seen is parked in the orphan map. v4.5.0 parked orphans up to 5,000
bytes, so an SLH-DSA spend (≈ 8 KB) that arrived before its parent was dropped and never re-announced. On
ycash-dd the bound is now `MAX_ORPHAN_TX_SIZE_VAULT` = 99,999 bytes once `UPGRADE_VAULT` is active at the next
block (5,000 before), the bound ycash6 (6.20.0) already has; a two-input SLH-DSA consolidation (≈ 16 KB) is parked
too. The count bound `-maxorphantx` (default 100) is unchanged, so the orphan map holds at most 100 × 100 KB = 10 MB
on both lines (0.5 MB on ycash-dd before activation); eviction stays random (`LimitOrphanTxSize`).
`qa/rpc-tests/vault_pq_relay.py` covers both sides of the activation.

## Reproduce

```
W=/Users/boy/projects/yellowback-workspace   # or any checkout
BITCOIND=$PWD/src/ycashd $W/.venv/bin/python -u contrib/yellowback/pq-dos/pq_dos.py \
    --srcdir=$PWD/src --tmpdir=/tmp/pqdos --portseed=2100 --nocleanup \
    --scenarios=p2pkh,slh,falcon,invalid,sapling,flood,stagedf,staged,legacy --out=/tmp/pqdos.json
```

- **Space and time.** The staged and legacy scenarios mine ≈ 200 extra blocks and need ≈ 12 GB of disk for the
  8 nodes. A full run takes ≈ 35 minutes on the machine above.
- **Partial runs.** `--staged-spends=N` sets the size of the staged blocks, and `--scenarios` selects scenarios.
