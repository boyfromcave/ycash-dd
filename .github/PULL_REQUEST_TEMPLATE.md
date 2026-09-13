<!-- Ycash Yellowback (YED) pull request. Every section is read by the reviewers; write "n/a"
     where a section does not apply, never delete it. AGENTS.md rules 4-7; plan §6 review rule. -->

## Summary

What this PR does, in two sentences, and the plan phase / checkbox it advances.

## Four-part check (AGENTS.md rule 4) — one block per ported symbol

The four-part check, written out for every symbol this PR ports (leave the block empty if it ports none):

> DigiByte does **X** in file **Y** using mechanism **M**.
> The Ycash equivalent is **Z**, which lacks **M**.
> So the adaptation is **W**.

If **M** is Taproot, SegWit, BIP9, `nVersion` bit-packing, the `Coin` model or MuSig2, cite the
`docs/mapping.md` row instead of re-deriving it. Every new impedance mismatch gets a new row (rule 5).

## Tier, and why a cheaper tier will not do

- [ ] node / wallet / RPC (no rule change)
- [ ] mining policy (Tier 1)
- [ ] overlay rule shared by enforcing miners (`src/yellowback/state.cpp`, `bundle.cpp`, `attest.cpp`;
      v3 plan §1 item 2 — the v2 soft-fork rule set, no new hook): two approving reviewers, one of
      whom did not write or pair on it
- [ ] soft-fork hook (`src/main.cpp`, `src/miner.cpp`, `src/rpc/mining.cpp`): **frozen since v3**
      (v3 plan §4.1: no new line; the `audit` job enforces zero delta against `feature/yellowback-sf`)
- [ ] consensus set (`src/consensus`, `src/script`, `src/primitives`, `src/pow`, `chainparams.cpp`,
      `wallet/wallet.{h,cpp}`, `txdb.*`, `configure.ac`) — **refused**; the `audit` job enforces zero lines

Tier statement (v3 plan §1 item 2, §4 rule 4 of AGENTS.md): every rule this PR adds or changes is
evaluated inside the overlay's `ProcessTx`/`ComputeSnapshot`, reached from the v2 hooks unchanged;
block validity stays "ACTIVE-vault spends obey RED-1..5". If that sentence is not true of this PR,
say so here and stop.

Why the cheaper tier cannot do it:

## Rules covered by tests

Every new or changed test case carries `// Rule: XXX-n` (C++) or `# Rule: XXX-n` (Python) on the
line before it; the `audit` job finds rule coverage through those tags. Rule identifiers this PR covers:

## Line budget (`git diff --numstat ycash-legacy...HEAD`)

| File | Changed lines | Budget |
|---|---|---|
| `src/main.cpp` | | 40 |
| `src/miner.cpp` | | 35 |
| `src/rpc/mining.cpp` | | 35 |
| consensus set | | 0 |

Every behaviour-changing statement in those files sits inside `if (g_yellowback)`; `DoS(0)` only.

- [ ] **No delta in the frozen files** (v3 plan §4.1, §8.4 item 1):
      `git diff --quiet feature/yellowback-sf...HEAD -- $(cat qa/yellowback-frozen-files.txt)` exits 0.
      The list is the one file `qa/yellowback-frozen-files.txt`, read by the `audit` job; a PR that needs
      a line in any of them is refused, not budgeted — say so in the tier section instead.

## Disabled or removed tests

Enumerate every `#if 0`, skipped case or script dropped from CI, with the phase that restores it:

## Checklist

- [ ] `src/test/test_bitcoin` (the whole suite) green locally; the functional scripts of this phase green
- [ ] `docs/mapping.md` updated for every new mismatch; `doc/yellowback.md` updated if user-visible
- [ ] No `DigiDollar` / `digidollar` / `DD` / `ydollar` / `yd_` names introduced (rule 6)
- [ ] No tidying, renaming or reformatting of existing Ycash code (rule 7)
- [ ] No attribution trailers in commits
