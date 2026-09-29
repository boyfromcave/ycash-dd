<!-- Ycash Yellowback (YED) pull request.
     Fill in §1 and §2 for every PR. §3–§5 apply only to PRs that change code under src/; for
     anything else (docs, CI, tests, tooling, corpus) write "n/a — no src/ change" under §3 and
     delete §4–§5. The rules referred to are in AGENTS.md at the workspace root; the review rule
     is v3 plan §1 item 2 / v2 plan §6. -->

## 1. What and why

Two or three sentences: what changes, why now, and the plan item it advances (or "maintenance").
If a CI run or issue motivated it, link it.

## 2. How it was verified

What was run, where, and the result. Name the suites (`src/test/test_bitcoin`, the
`qa/rpc-tests/yellowback_*.py` scripts, `cargo test`, a devnet walk) and paste the summary
line, not the log. If a check could not be run here (CI-only, needs a network), say so.

## 3. What this PR touches in `src/` (AGENTS.md rules 4 and 7)

Tick one and follow its instructions; the `audit` job checks the frozen and consensus sets
regardless of what is ticked.

- [ ] **Nothing under `src/`** (docs, CI, tests, contrib, corpus) → write "n/a" here, delete §4–§5.
- [ ] **Overlay only** (`src/yellowback/`, `src/rpc/yellowback*.cpp`, wallet glue) → §4 required;
      §5 only if a symbol was ported from DigiByte.
- [ ] **Overlay rule shared by enforcing miners** (`state.cpp`, `bundle.cpp`, `attest.cpp`) →
      §4 required; two approving reviewers, one who did not write or pair on it.
- [ ] **Frozen soft-fork hook files** (`qa/yellowback-frozen-files.txt`: `main.cpp`, `miner.cpp`,
      `rpc/mining.cpp`) → **refused** since v3 (plan §4.1). Say what you needed and stop.
- [ ] **Consensus set** (`src/consensus`, `src/script`, `src/primitives`, `src/pow`,
      `chainparams.cpp`, `wallet/wallet.*`, `txdb.*`, `configure.ac`) → **refused** (rule 7).

## 4. Rules and tests (only when `src/` changes)

- Rule identifiers this PR adds, changes or covers (`// Rule: XXX-n` on the line before each
  test case; the `audit` job reads them):
- Tier statement: every rule this PR adds or changes is evaluated inside the overlay's
  `ProcessTx` / `ComputeSnapshot`, reached from the v2 hooks unchanged, and block validity stays
  "ACTIVE-vault spends obey RED-1..5". If that is not true, say so here and stop.
- Tests disabled, skipped or removed from CI, and the phase that restores them (or "none"):

## 5. Ported symbols (only when code was ported from DigiByte; AGENTS.md rule 4)

One block per symbol. If **M** is Taproot, SegWit, BIP9, `nVersion` bit-packing, the `Coin`
model or MuSig2, cite the `docs/mapping.md` row instead of re-deriving it; every new mismatch
gets a new row (rule 5).

> DigiByte does **X** in file **Y** using mechanism **M**.
> The Ycash equivalent is **Z**, which lacks **M**.
> So the adaptation is **W**.

## Checklist

- [ ] Verification in §2 actually ran on this branch (a failed `make` leaves the old test binary in place)
- [ ] `docs/mapping.md` has a row for every new mismatch; `doc/yellowback.md` updated if user-visible
- [ ] No `DigiDollar` / `digidollar` / `DD` / `ydollar` / `yd_` names introduced (rule 6)
- [ ] No tidying, renaming or reformatting of existing Ycash code (rule 7)
- [ ] Commits carry no attribution trailers
