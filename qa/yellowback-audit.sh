#!/usr/bin/env bash
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

# The CI `audit` job of .github/workflows/yellowback-tests.yml, as one script, so a laptop and CI run
# the same gates (the ycash6 line has the same idea in its own qa/yellowback-audit.sh). The same file
# serves both node-line branches; the mode picks the legs:
#
#   harden   harden/yellowback and the branch of record (v3 + hardening): every leg blocks, exactly
#            as the inline audit steps did before 2026-10-06.
#   upgrade  upgrade/vault (the vault primitive + YED module network upgrade, upgrade plan §7, §9 G-9):
#            the frozen-file and line-budget legs are REPORT-ONLY (printed, never failing; the consensus
#            review gate replaces them), the DoS leg is the upgrade's rule (mempool 0, block 100), and
#            three legs are added: the RPC contract through the workspace generator's upgrade mode, the
#            cross-line byte identity of the golden vectors, and the consensus-diff report.
#   auto     (default) upgrade when src/vault/ exists, harden otherwise.
#
# Usage: qa/yellowback-audit.sh [--mode harden|upgrade|auto] [--out DIR]
#   --out DIR   where the upgrade mode writes consensus-diff.patch and consensus-diff.txt
#               (default: a fresh directory under ${TMPDIR:-/tmp})
# Refs it needs (CI fetches them; a missing ref is an error, never a vacuous pass):
#   ycash-legacy (branch, = upstream v4.5.0) and yellowback-v3-baseline (tag).
# Upgrade-mode inputs (CI sets them; locally a leg whose input is missing says NOT CHECKED and,
# only outside CI, does not fail):
#   YB_WORKSPACE         a checkout of the workspace repository (scripts/extract_spec.py, docs/plans/);
#                        default: ../.. when it holds scripts/extract_spec.py (a wt/<name> worktree)
#   YB_OTHER_LINE_REF    a ref in THIS repository holding the other node line (CI fetches ycash6 into it)
#   YB_OTHER_LINE_DIR + YB_OTHER_LINE_REV   or another local repository and a revision in it
#                        (e.g. YB_OTHER_LINE_DIR=../../ycash6 YB_OTHER_LINE_REV=upgrade/vault)
# The functional-test lists are read from the workflow file itself (its top-level env), the one
# place they are declared.
# Path lists ($frozen, $pure, $(existing ...)) are word-split on purpose; no path here holds a space.
# shellcheck disable=SC2046,SC2086
set -uo pipefail
cd "$(dirname "$0")/.."

mode=auto
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) mode="${2:-}"; shift 2 ;;
    --mode=*) mode="${1#--mode=}"; shift ;;
    --out) out="${2:-}"; shift 2 ;;
    --out=*) out="${1#--out=}"; shift ;;
    -h|--help) sed -n '6,32p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [ "$mode" = auto ]; then
  if [ -d src/vault ]; then mode=upgrade; else mode=harden; fi
fi
case "$mode" in harden|upgrade) ;; *) echo "--mode must be harden, upgrade or auto" >&2; exit 2 ;; esac
# A mode that does not match the tree is a wiring error (wrong branch list in the workflow), not a pass.
if [ "$mode" = harden ] && [ -d src/vault ]; then echo "mode harden on a tree with src/vault/ (the upgrade line)" >&2; exit 2; fi
if [ "$mode" = upgrade ] && [ ! -d src/vault ]; then echo "mode upgrade on a tree without src/vault/" >&2; exit 2; fi
in_ci() { [ "${GITHUB_ACTIONS:-}" = true ]; }

LEGACY=ycash-legacy
BASELINE=yellowback-v3-baseline
for r in "$LEGACY" "$BASELINE"; do
  git rev-parse --verify -q "$r^{commit}" >/dev/null || {
    echo "missing ref $r: git fetch --no-tags origin +refs/heads/ycash-legacy:refs/heads/ycash-legacy +refs/tags/yellowback-v3-baseline:refs/tags/yellowback-v3-baseline" >&2
    exit 2
  }
done

WF=.github/workflows/yellowback-tests.yml
wfenv() { sed -n "s/^  $1: *//p" "$WF" | head -1 | sed 's/[[:space:]]*#.*$//'; }
YELLOWBACK_SCRIPTS=$(wfenv YELLOWBACK_SCRIPTS)
SANITIZER_SCRIPTS=$(wfenv SANITIZER_SCRIPTS)
NIGHTLY_SCRIPTS=$(wfenv NIGHTLY_SCRIPTS)
STOCK_BASELINE=$(wfenv STOCK_BASELINE)
[ -n "$YELLOWBACK_SCRIPTS" ] && [ -n "$STOCK_BASELINE" ] && [ -n "$NIGHTLY_SCRIPTS" ] || {
  echo "could not read YELLOWBACK_SCRIPTS / STOCK_BASELINE / NIGHTLY_SCRIPTS from $WF" >&2; exit 2; }

# Comment lines (// ..., * ..., /* ...) as grep -n prints them: path:line:  // text
nocomment() { grep -v ':[0-9]*:[[:space:]]*\(//\|\*\|/\*\)'; }
# The arguments that exist (an unmatched glob stays literal, and grep exits 2 on a missing file even
# when another file matched, which a negated check would read as a pass; none at all is /dev/null,
# never an empty list that would make grep read stdin).
existing() { local f n=0; for f in "$@"; do [ -e "$f" ] && { echo "$f"; n=1; }; done; [ $n = 1 ] || echo /dev/null; }
sha256() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi; }

failed=""
reported=""
leg() {   # leg <block|report> <name> <function>
  local kind=$1 name=$2 fn=$3 rc
  in_ci && echo "::group::$name"
  echo "=== $name"
  ( set -e; "$fn" ); rc=$?
  in_ci && echo "::endgroup::"
  if [ $rc -eq 0 ]; then
    echo "PASS  $name"
  elif [ "$kind" = report ]; then
    echo "REPORT $name (non-gating in mode $mode)"
    reported="$reported
  $name"
  else
    echo "FAIL  $name"
    in_ci && echo "::error::audit leg failed: $name"
    failed="$failed
  $name"
  fi
}

# ── frozen files and line budgets ─────────────────────────────────────────────────────────────

leg_frozen() {
  # v3 adds no line to the soft-fork hooks, the policy layer or the consensus set: the list is the one
  # file qa/yellowback-frozen-files.txt, one path per line, directories allowed. The diff is printed
  # first so the reviewer sees what moved. On upgrade/vault these files change by design (§9 G-9).
  test -s qa/yellowback-frozen-files.txt
  local frozen
  frozen=$(grep -v '^#' qa/yellowback-frozen-files.txt | grep -v '^$')
  echo "frozen set (vs $BASELINE):"; echo "$frozen" | sed 's/^/  /'
  git --no-pager diff --stat "$BASELINE...HEAD" -- $frozen
  git diff --quiet "$BASELINE...HEAD" -- $frozen
}

leg_budget() {
  local rc=0 f p b n
  for f in src/main.cpp:40 src/miner.cpp:35 src/rpc/mining.cpp:35; do
    p=${f%%:*}; b=${f##*:}
    n=$(git diff --numstat "$LEGACY...HEAD" -- "$p" | awk '{s+=$1+$2} END {print s+0}')
    echo "$p: $n changed lines (budget $b)"
    [ "$n" -le "$b" ] || rc=1
  done
  # Zero lines in the consensus set of plan §4.1. Phase 8's wallet-tier hardening (H5/H7/H8)
  # landed 2026-09-11, so src/wallet/rpcwallet.cpp, src/wallet/rpcdump.cpp and
  # src/rpc/rawtransaction.cpp have left this set (M7) and the wallet core is named
  # explicitly: src/wallet/wallet.{h,cpp} stay at zero, the rest of src/wallet does not.
  local zero
  zero=$(git diff --stat "$LEGACY...HEAD" -- src/consensus src/script src/primitives src/pow src/chainparams.cpp src/wallet/wallet.h src/wallet/wallet.cpp src/txdb.cpp src/txdb.h configure.ac)
  if [ -n "$zero" ]; then echo "consensus zero set vs $LEGACY:"; echo "$zero"; rc=1; fi
  return $rc
}

# ── determinism ───────────────────────────────────────────────────────────────────────────────

CLOCK='GetTime\|GetAdjustedTime'
RANDOM_='GetRand\|insecure_rand\|random_device\|mt19937\|std::rand\|std::chrono'
leg_determinism() {
  # The state machine and the pure modules may not read the clock, a random source, the mempool,
  # the wallet or configuration, and never use floating point. Comment lines are skipped (they may
  # name what is forbidden). attest and bundle joined in v3 (plan §3.10); module and params are the
  # YED module of the upgrade (upgrade plan §5) and are skipped where they do not exist.
  local pure="" f x homes
  for f in state math tag payload script view attest bundle module params; do
    for x in src/yellowback/$f.cpp src/yellowback/$f.h; do [ -f "$x" ] && pure="$pure $x"; done
  done
  echo "pure modules:$pure"
  ! grep -n "$CLOCK\|$RANDOM_\|mempool\|pwalletMain\|GetArg\|double\|float" $pure | nocomment || return 1
  # libsecp256k1 is reached through one wrapper only, VerifyCompactSig in attest.cpp (v3 §3.10,
  # §8.4 item 2), so the fuzz harness can stub it and low-S is enforced in one place.
  ! grep -n "secp256k1_" $(existing src/yellowback/*.cpp src/yellowback/*.h src/rpc/yellowback*.cpp) | grep -v '^src/yellowback/attest\.cpp:' | nocomment || return 1
  # index.cpp may read the mempool (RemoveInvalidVaultSpends) but never the clock or floating point.
  ! grep -n "$CLOCK\|$RANDOM_\|double\|float" src/yellowback/index.cpp src/yellowback/index.h | nocomment || return 1
  # The clock has three homes: the quote RPC (stamps receivedAt), policy.cpp (BuildTagScript, M11),
  # and the wallet RPC's carrier wait (A3's two-step mint -- a user-facing deadline in wallet-tier
  # code, never a consensus input).
  homes=$(grep -l "$CLOCK" $(existing src/yellowback/*.cpp src/rpc/yellowback*.cpp) | grep -Ev 'yellowback/policy\.cpp$|rpc/yellowback\.cpp$|rpc/yellowbackwallet\.cpp$' || true)
  [ -z "$homes" ] || { echo "GetTime outside its three homes: $homes"; return 1; }
  # In that third home the only clock symbol is GetTimeMillis, and only for the wait timeout.
  ! grep -n "$CLOCK" src/rpc/yellowbackwallet.cpp | grep -v "GetTimeMillis" || return 1
  # The vault primitive (upgrade plan §15): consensus code with no clock, no random source and no
  # floating point anywhere under src/vault/; only the node glue (node.{h,cpp}) may name the mempool
  # or read configuration.
  if [ -d src/vault ]; then
    ! grep -n "$CLOCK\|$RANDOM_\|double\|float" src/vault/*.cpp src/vault/*.h | nocomment || return 1
    ! grep -n "mempool\|pwalletMain\|GetArg" src/vault/*.cpp src/vault/*.h | grep -Ev '^src/vault/node\.(cpp|h):' | nocomment || return 1
    ! grep -n "secp256k1_\|evhttp\|socket" src/vault/*.cpp src/vault/*.h | nocomment || return 1
  fi
}

# ── registration: executable, registered, run by a CI job ────────────────────────────────────

leg_registration() {
  # rpc-tests.py exec's a script directly, so it must be executable, and it must be run by some job:
  # registered-but-never-run is how the v2 enforcement suite and then the whole v3 attestation suite
  # sat silent in CI.
  local f n
  for f in qa/rpc-tests/yellowback_*.py qa/rpc-tests/vault_*.py; do
    [ -e "$f" ] || continue
    [ -x "$f" ] || { echo "$f is not executable; chmod +x it"; return 1; }
    n=$(basename "$f" .py)
    grep -q "'$n\.py'" qa/pull-tester/rpc-tests.py || { echo "$n is not registered in qa/pull-tester/rpc-tests.py"; return 1; }
    case " $YELLOWBACK_SCRIPTS $SANITIZER_SCRIPTS $NIGHTLY_SCRIPTS " in
      *" $n "*) ;;
      *) echo "$n is in qa/rpc-tests/ but no CI job runs it (add it to YELLOWBACK_SCRIPTS or NIGHTLY_SCRIPTS in $WF)"; return 1 ;;
    esac
  done
  # rpc-tests.py silently drops a named script it does not list (ALL_SCRIPTS filter), so every name a
  # job passes it must be registered there (ported from ycash6, audit I-1).
  for n in $YELLOWBACK_SCRIPTS $SANITIZER_SCRIPTS $STOCK_BASELINE $NIGHTLY_SCRIPTS; do
    [ -f "qa/rpc-tests/$n.py" ] || { echo "$n is in a CI list but qa/rpc-tests/$n.py does not exist"; return 1; }
    grep -qE "[\"']$n\.py[\"']" qa/pull-tester/rpc-tests.py || { echo "$n is in a CI list but not in qa/pull-tester/rpc-tests.py"; return 1; }
  done
  # The push tier runs YELLOWBACK_SCRIPTS + STOCK_BASELINE in shards: every name in exactly one shard.
  python3 qa/yellowback-ci-shard.py --check $YELLOWBACK_SCRIPTS $STOCK_BASELINE
  # EDGES is the topology declaration (it names the v3 attestor slots 6-7); a six-node v2 script that
  # iterates it raw indexes self.nodes out of range. Walk live_edges() instead.
  ! grep -rn "in self\.EDGES" qa/rpc-tests/ | grep -v 'yellowback_util\.py:.*a < n and b < n' || return 1
}

# ── bans and the DoS rule ─────────────────────────────────────────────────────────────────────

leg_structure() {
  # The overlay modules never ban (they return verdicts; validation maps them) and never call the
  # other ban entry point Misbehaving( (audit I-9).
  ! grep -n "DoS([1-9]\|Misbehaving(" $(existing src/yellowback/*.cpp src/rpc/yellowback*.cpp src/vault/*.cpp src/rpc/vault*.cpp) || return 1
  # EvaluateBlock is total (K1): no throw, no assert in the state machine or the arithmetic.
  ! grep -n "throw\|assert(" src/yellowback/state.cpp src/yellowback/math.h || return 1
  # The overlay adds no networking to the node (anchored: UndoDisconnect( must not match).
  ! grep -nE "evhttp|socket|\bconnect\(" src/yellowback/*.cpp src/yellowback/*.h src/rpc/yellowback*.cpp | nocomment || return 1
  # The transaction_builder extension is called only by the Yellowback builder and its tests (§8.4 item 22).
  ! git grep -n "AddTransparentInputUnsigned\|SetLockTime(" -- src ':!src/transaction_builder.h' ':!src/transaction_builder.cpp' ':!src/yellowback/' ':!src/test/yellowback_*' || return 1
}

leg_dos_harden() {
  # Never ban a peer for a Yellowback verdict (N1): every DoS( the fork inserts into main.cpp is DoS(0),
  # and it inserts no Misbehaving(. The whole of stock main.cpp is full of DoS(100); only the fork's
  # own lines (the diff against ycash-legacy) are checked.
  git diff "$LEGACY...HEAD" -- src/main.cpp | grep '^+' | grep 'DoS(\|Misbehaving(' | { ! grep -v 'DoS(0'; } || return 1
}

leg_dos_upgrade() {
  # The upgrade's rule (upgrade plan U-21), encoded on the enclosing function, not on line numbers:
  # a fork-inserted DoS( in a mempool path is DoS(0) (vault and YED state can change with the next
  # block; a peer relaying such a transaction is not misbehaving), in a block path DoS(100) (the
  # primitive and the module are consensus at UPGRADE_VAULT), and in a shared helper either 0, or
  # `fMempool ? 0 : 100`, or a 100 that follows the helper's `if (fMempool)` early return. No inserted
  # Misbehaving( anywhere. Every changed C++ file under src/ (tests and fuzz harnesses aside).
  local files
  files=$(git diff --name-only --diff-filter=AM "$LEGACY...HEAD" -- 'src/*.cpp' 'src/*.h' ':!src/test' ':!src/gtest' ':!src/fuzzing' ':!src/bench')
  python3 - "$LEGACY" $files <<'PY'
import re, subprocess, sys
base, files = sys.argv[1], sys.argv[2:]
MEMPOOL = {"AcceptToMemoryPool"}
BLOCK = {"ConnectBlock", "CheckBlock", "ContextualCheckBlock", "CheckBlockHeader", "ContextualCheckBlockHeader",
         "AcceptBlock", "AcceptBlockHeader", "ConnectTip", "DisconnectBlock"}
FUNC = re.compile(r'^(?![#/ \t{}])(?!(?:if|for|while|switch|return|else|do|case|namespace|class|struct|enum|typedef|using)\b)[A-Za-z_].*?\b([A-Za-z_][A-Za-z0-9_:~]*)\s*\(')
DOS = re.compile(r'\bDoS\(\s*([^,]+?)\s*,')
bad, seen = [], 0
for path in files:
    diff = subprocess.run(["git", "diff", "-U0", base + "...HEAD", "--", path], capture_output=True, text=True, check=True).stdout
    added, ln = set(), 0
    for line in diff.splitlines():
        m = re.match(r'^@@ -\S+ \+(\d+)(?:,(\d+))? @@', line)
        if m:
            ln = int(m.group(1)); continue
        if line.startswith('+') and not line.startswith('+++'):
            added.add(ln); ln += 1
    with open(path, encoding="utf-8", errors="replace") as f:
        src = f.read().split("\n")
    for n in sorted(added):
        text = src[n - 1]
        code = text.split("//")[0]
        if "Misbehaving(" in code:
            bad.append("%s:%d: inserted Misbehaving(: %s" % (path, n, text.strip())); continue
        m = DOS.search(code)
        if not m:
            continue
        seen += 1
        level = re.sub(r'\s+', ' ', m.group(1))
        fn, sig = None, ""
        for k in range(n - 1, -1, -1):
            fm = FUNC.match(src[k])
            if fm and not src[k].rstrip().endswith(';'):
                fn = fm.group(1).split("::")[-1]
                j, sig = k, ""
                while j < len(src) and "{" not in src[j]:
                    sig += src[j]; j += 1
                sig += src[j] if j < len(src) else ""
                break
        prev = next((src[k].strip() for k in range(n - 2, -1, -1) if src[k].strip()), "")
        where = "%s:%d (%s)" % (path, n, fn)
        if fn in MEMPOOL:
            ok, rule = level == "0", "mempool path: DoS(0)"
        elif fn in BLOCK:
            ok, rule = level == "100", "block path: DoS(100)"
        elif "fMempool" in sig:
            guarded = any(src[k].strip().startswith("if (fMempool)") for k in range(max(0, n - 4), n - 1))
            ok = level in ("0", "fMempool ? 0 : 100") or (level == "100" and guarded and not prev.startswith("if (fMempool)"))
            rule = "shared helper: 0, fMempool ? 0 : 100, or 100 after its if (fMempool) return"
        else:
            ok, rule = level == "0", "helper with no mempool/block distinction: DoS(0)"
        print("  %-60s DoS(%s)  %s" % (where, level, "ok" if ok else "VIOLATES " + rule))
        if not ok:
            bad.append("%s: DoS(%s) violates %s" % (where, level, rule))
print("%d inserted DoS( call(s) checked" % seen)
if bad:
    print("\n".join(bad)); sys.exit(1)
PY
}

# ── documents and the RPC contract ────────────────────────────────────────────────────────────

leg_documents() {
  # Staleness against the plan is a workspace check (make status -> spec-check), not a fork check.
  ! grep -q 'no consensus change' doc/yellowback.md || return 1
  grep -q 'federation prototype, being replaced' doc/yellowback.md
  grep -q 'Build and test baseline' doc/yellowback.md
  # The published spec copy was not hand-edited: its header hash equals the hash of its body.
  local want got
  want=$(sed -n 's/^Source: .*sha256: \([0-9a-f]*\).*/\1/p' doc/yellowback-spec.md)
  got=$(sed '1,/^---$/d' doc/yellowback-spec.md | sha256 | cut -c1-64)
  [ "$want" = "$got" ] || { echo "doc/yellowback-spec.md: header sha256 $want, body $got"; return 1; }
  # The trust statement in doc/yellowback.md is the spec's §8.1, byte for byte (leading and trailing
  # blank lines aside).
  section() { awk -v h="$1" '$0 ~ h {f=1; next} f && /^#/ {exit} f' "$2" | awk 'NF{p=1} p' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'; }
  diff <(section '^### 8\.1 ' doc/yellowback-spec.md) <(section '^## Trust statement' doc/yellowback.md)
  python3 -c 'import json; json.load(open("doc/yellowback-rpc-contract.json"))'
  # The vault RPC contract (upgrade line, finding (47)): generated from doc/vault-rpc.md and in step
  # with the RPC table and the client conversions. Checked once the generator is in the tree.
  if [ -f qa/vault-rpc-contract.py ]; then python3 qa/vault-rpc-contract.py --check; fi
}

leg_contract() {
  # Every yed_* the node registers is in the RPC contract (P7); the federation is gone from the module
  # (the whole-directory roster grep, N26); the contract's version and shape for this line.
  local registered missing want
  registered=$(grep -oh '"yed_[a-z]*"' src/rpc/yellowback*.cpp | tr -d '"' | sort -u)
  missing=$(for c in $registered; do python3 -c "import json,sys; d=json.load(open('doc/yellowback-rpc-contract.json')); sys.exit(0 if '$c' in d else 1)" || echo "$c"; done)
  [ -z "$missing" ] || { echo "registered but not in doc/yellowback-rpc-contract.json: $missing"; return 1; }
  ! git grep -in roster -- src/yellowback || return 1
  if [ "$mode" = upgrade ]; then want=5; else want=4; fi   # 3 in v3 (W14); 4 since hardening H-9.3; 5 the vault upgrade (§15.10)
  grep -q "rpcversion $want" doc/yellowback-rpc.md || { echo "doc/yellowback-rpc.md does not state rpcversion $want"; return 1; }
  if [ "$mode" = upgrade ]; then
    # yed_getinfo carries the upgrade object and none of the retired enforcement fields.
    python3 -c 'import json,sys; d=json.load(open("doc/yellowback-rpc-contract.json")); r=d["yed_getinfo"]["returns"]; sys.exit(0 if "upgrade" in r and not set(r) & {"suppressedBlocks","rejectedBlocks","enforcing","valveTripped","sunset","abandoned","templatePolicy"} and "yed_sweep" not in d else 1)'
  else
    python3 -c 'import json,sys; d=json.load(open("doc/yellowback-rpc-contract.json")); sys.exit(0 if "yed_getinfo" in d and "suppressedBlocks" in d["yed_getinfo"]["returns"] else 1)'
  fi
  # v3 contract (Phase A0): the attest object on yed_getinfo, every §4.5 command and error identifier
  # present in the generated JSON -- the C++ agents implement these names.
  YB_WANT_RPCVERSION=$want python3 - <<'PY'
import json, os, sys
d = json.load(open("doc/yellowback-rpc-contract.json"))
want = int(os.environ["YB_WANT_RPCVERSION"])
missing = []
if d.get("rpcversion") != want: missing.append("rpcversion=%d" % want)
if "attest" not in d["yed_getinfo"]["returns"]: missing.append("yed_getinfo.attest")
for c in ["yed_listattestors", "yed_getattestations", "yed_addattestation", "yed_buildbundle", "yed_getselection",
          "yed_getnotice", "yed_claimnotice", "yed_sweepcarriers", "yed_registerattestor", "yed_withdrawbond",
          "yed_revive", "yed_reportequivocation", "yed_signattestation"]:
    if c not in d: missing.append(c)
for e in ["attest-unknown-seq", "attest-not-eligible", "attest-stale", "attest-bad-sig", "attest-range",
          "bundle-insufficient", "mint10-diverged", "notice-standing", "notice-not-underwater", "bond-below-min",
          "lock-below-min", "bond-locked", "not-dormant", "not-equivocation", "attest-key-not-held",
          "equivocation-guard"]:
    if e not in d["errors"]: missing.append("errors." + e)
if missing: print("contract missing:", missing); sys.exit(1)
PY
  # The spec copy is this line's: on harden the v3 delta (make spec appends the v3 plan's section 3);
  # on upgrade the vault specification (make spec-upgrade: the upgrade plan's section 10 trust
  # statement as 8.1 and its section 15), which carries no v3 delta.
  if [ "$mode" = upgrade ]; then
    grep -q '^## Vault upgrade specification' doc/yellowback-spec.md
  else
    grep -q '^## v3 delta' doc/yellowback-spec.md
  fi
  [ -f doc/yellowback-attestor.md ] && [ -f qa/yellowback-frozen-files.txt ]
  # Every inserted main.cpp statement is inside `if (g_yellowback)` (§8.4 item 2): the residual is
  # printed for the reviewer, never gating.
  echo "main.cpp residual (lines not mentioning g_yellowback):"
  git diff "$LEGACY...HEAD" -- src/main.cpp | grep '^+' | grep -Ev 'g_yellowback|#include|^\+$|^\+\+\+' | sed 's/^/  /' || true
}

leg_contract_generator() {
  # The committed contract and spec copy are exactly what the workspace generator writes for the
  # upgrade line (scripts/extract_spec.py --check-upgrade, EXTRACT_SPEC_LINE from the doc's title).
  local ws="${YB_WORKSPACE:-}" outp
  if [ -z "$ws" ] && [ -f ../../scripts/extract_spec.py ]; then ws=../..; fi
  if [ -z "$ws" ] || [ ! -f "$ws/scripts/extract_spec.py" ]; then
    echo "NOT CHECKED: no workspace checkout (set YB_WORKSPACE)"; in_ci && return 1; return 0
  fi
  outp=$(EXTRACT_SPEC_NODE_DIR="$PWD" EXTRACT_SPEC_NODE6_DIR=/nonexistent EXTRACT_SPEC_WALLET_DIR=/nonexistent \
         EXTRACT_SPEC_LWD_DIR=/nonexistent EXTRACT_SPEC_LINE=upgrade python3 "$ws/scripts/extract_spec.py" --check-upgrade) || { echo "$outp"; return 1; }
  echo "$outp"
  # A skipped node tree is a vacuous pass: insist that both of this tree's copies were compared.
  echo "$outp" | grep -q 'copies match.*doc/yellowback-spec\.md.*doc/yellowback-rpc-contract\.json' \
    || { echo "the generator did not compare this tree's copies"; return 1; }
}

# ── rule -> test tags ─────────────────────────────────────────────────────────────────────────

require_tags() {   # every identifier must appear on a `// Rule:` / `# Rule:` line of a test
  local id rc=0
  for id in "$@"; do
    git grep -qP "^(//|#) Rule: .*\b$id\b" -- src/test/yellowback_*.cpp qa/rpc-tests/yellowback_*.py || { echo "no test tagged: $id"; rc=1; }
  done
  return $rc
}
leg_rule_tags() {
  local tagged act
  tagged=$(git grep -ho '^\(//\|#\) Rule: .*' -- src/test/yellowback_*.cpp qa/rpc-tests/yellowback_*.py | sed 's/.*Rule: //' | tr ' ' '\n' | sort -u)
  echo "rules with a tagged test:"; echo "$tagged" | tr '\n' ' '; echo
  [ -n "$tagged" ]
  # ACT-4 (participation), ACT-6 (the signal) and ACT-7 (the valve) left with the enforcement
  # machinery in the vault upgrade (upgrade plan §6): no test can carry them there.
  if [ "$mode" = upgrade ]; then act="ACT-1 ACT-2 ACT-3 ACT-5"; else act="ACT-1 ACT-2 ACT-3 ACT-4 ACT-5 ACT-6 ACT-7"; fi
  require_tags TAG-1 TAG-2 TAG-3 TAG-4 TAG-5 PRICE-1 PRICE-2 SIGMA-1 FEE-0 FEE-1 FEE-2 FEE-W REG-1 REG-2 REG-3 REG-4 IN-1 IN-2 IN-3 TX-0 \
    MINT-1 MINT-2 MINT-3 MINT-4 MINT-5 MINT-6 MINT-7 MINT-8 XFER-1 XFER-2 XFER-3 RED-1 RED-2 RED-3 RED-4 \
    $act HALT-1 HALT-2 HALT-3 HALT-4 BLK-1 BLK-2 BLK-3 TPL-1 TPL-2 TPL-3 \
    MP-1 MINER-1 MINER-2 MINER-3 MINTPOL-1 SNAP UNDO
}
leg_rule_tags_v3() {
  # The v3 rules of plan §3.8 (Phase A1 on).
  require_tags ARM-1 ARM-2 REG-A1 BUNDLE-1 MINT-9 MINT-10 NOT-1 EQV-1 REV-1 PIN-1 PIN-2 AFEE-0 AFEE-1 RED-5
}

# ── attribution and naming ────────────────────────────────────────────────────────────────────

leg_attribution() {
  # AGENTS.md / the briefing's rule 7: every file the fork adds carries "The Ycash developers"; every
  # upstream file it modifies that has copyright lines gains one; no Zcash or Bitcoin notice is ever
  # removed or altered. Files in the frozen set are exempt from the second rule (their bytes are gated;
  # an attribution line would itself be a frozen-set delta) and listed so the exemption stays visible.
  local frozen f bad="" exempt=""
  frozen=$(grep -v '^#' qa/yellowback-frozen-files.txt | grep -v '^$')
  for f in $(git diff --name-only --diff-filter=A "$LEGACY...HEAD" -- src qa contrib zcutil | grep -E '\.(cpp|h|hpp|c|py|sh|rs)$' | grep -Ev '^src/(test/data|fuzzing)/|/fixtures/'); do
    grep -q 'The Ycash developers' "$f" || bad="$bad
  added without a Ycash line: $f"
  done
  for f in $(git diff --name-only --diff-filter=M "$LEGACY...HEAD" -- src qa contrib zcutil | grep -E '\.(cpp|h|hpp|c|py|sh|rs)$'); do
    grep -q 'Copyright' "$f" || continue
    grep -q 'The Ycash developers' "$f" && continue
    if printf '%s\n' $frozen | grep -qx "$f"; then exempt="$exempt $f"; else bad="$bad
  modified upstream file without a Ycash line: $f"; fi
  done
  [ -z "$exempt" ] || echo "frozen-set files exempt from the modified-file rule:$exempt"
  local removed
  removed=$(git diff "$LEGACY...HEAD" -- src qa contrib zcutil doc | grep -E '^-[^-].*Copyright.*(Bitcoin|Zcash|Satoshi)' || true)
  [ -z "$removed" ] || bad="$bad
  removed or altered upstream notice: $removed"
  [ -z "$bad" ] || { echo "$bad"; return 1; }
}

leg_naming() {
  # AGENTS.md rule 6 and the briefing's rule 8: "ycash6" names the repository and its tooling, never
  # anything under src/; DigiByte's names (DigiDollar, DD) appear only in comments that cite the
  # upstream file; the retired working name (YDollar / ydollar / yd_) appears nowhere. Checked on the
  # lines the fork adds to src/ (upstream's own text is not ours to rename).
  local added
  added=$(git diff "$LEGACY...HEAD" -- src ':!src/test/data' | grep '^+' | grep -v '^+++' || true)
  ! printf '%s\n' "$added" | grep -nE '\bycash6\b|[Yy][Dd]ollar|\byd_' || return 1
  ! printf '%s\n' "$added" | grep -E '[Dd]igi[Dd]ollar|DIGIDOLLAR|\bDD\b' | grep -Ev '^\+[[:space:]]*(//|\*|/\*)' || return 1
}

# ── upgrade line only ─────────────────────────────────────────────────────────────────────────

leg_cross_line() {
  # Both node lines replay the same golden vectors: byte-identical files, or one line's consensus
  # implementation has moved without the other (AGENTS.md "two node lines, one overlay").
  local f mine other
  if [ -n "${YB_OTHER_LINE_REF:-}" ]; then
    show() { git show "$YB_OTHER_LINE_REF:$1"; }
    echo "other line: $YB_OTHER_LINE_REF ($(git rev-parse --short "$YB_OTHER_LINE_REF^{commit}"))"
  elif [ -n "${YB_OTHER_LINE_DIR:-}" ] && [ -n "${YB_OTHER_LINE_REV:-}" ]; then
    show() { git -C "$YB_OTHER_LINE_DIR" show "$YB_OTHER_LINE_REV:$1"; }
    echo "other line: $YB_OTHER_LINE_DIR at $YB_OTHER_LINE_REV ($(git -C "$YB_OTHER_LINE_DIR" rev-parse --short "$YB_OTHER_LINE_REV^{commit}"))"
  else
    echo "NOT CHECKED: no other node line (set YB_OTHER_LINE_REF, or YB_OTHER_LINE_DIR + YB_OTHER_LINE_REV)"
    in_ci && return 1; return 0
  fi
  local rc=0
  for f in src/test/data/vault_vectors.json src/test/data/yellowback_golden.json; do
    mine=$(sha256 < "$f" | cut -c1-64)
    other=$(show "$f" | sha256 | cut -c1-64)
    if [ "$mine" = "$other" ]; then echo "  $f  $mine  identical"; else echo "  $f  this $mine, other $other  DIFFERENT"; rc=1; fi
  done
  return $rc
}

leg_consensus_diff() {
  # The consensus review gate (upgrade plan §7, G-9): every changed line under the consensus paths, both
  # lines, reviewed by two people independent of the author. This writes the material for that review;
  # CI uploads it as the `consensus-diff` artifact. It never fails on content, only on a broken diff.
  local paths="src/consensus src/script src/main.cpp src/primitives src/vault"
  [ -n "$out" ] || out=$(mktemp -d "${TMPDIR:-/tmp}/yellowback-audit.XXXXXX")
  mkdir -p "$out"
  git diff "$LEGACY...HEAD" -- $paths > "$out/consensus-diff.patch"
  {
    echo "consensus diff of $(git rev-parse HEAD) vs $LEGACY ($(git rev-parse "$LEGACY")), merge base $(git merge-base "$LEGACY" HEAD)"
    echo "paths: $paths"
    echo
    git diff --numstat "$LEGACY...HEAD" -- $paths | awk '{printf "%6d +%-6d -%-6d %s\n", $1+$2, $1, $2, $3; a+=$1; d+=$2} END {printf "%6d +%-6d -%-6d total (%d added, %d removed)\n", a+d, a, d, a, d}'
  } > "$out/consensus-diff.txt"
  cat "$out/consensus-diff.txt"
  echo "written: $out/consensus-diff.patch ($(wc -l < "$out/consensus-diff.patch" | tr -d ' ') lines), $out/consensus-diff.txt"
}

# ── run ───────────────────────────────────────────────────────────────────────────────────────

echo "yellowback audit, mode $mode, HEAD $(git rev-parse --short HEAD)"
if [ "$mode" = upgrade ]; then
  leg report "frozen files vs $BASELINE (report-only: G-9 review gate)" leg_frozen
  leg report "line budgets and the consensus zero set vs $LEGACY (report-only: G-9 review gate)" leg_budget
else
  leg block "frozen files vs $BASELINE (v3 plan §4.1, §8.4 item 1)" leg_frozen
  leg block "line budgets and the consensus zero set vs $LEGACY (plan §4.1)" leg_budget
fi
leg block "determinism: no clock, no random source, no floating point (plan §3.10, §8.4 items 2, 8)" leg_determinism
leg block "functional scripts executable, registered and run by a CI job" leg_registration
leg block "module structure: no bans, total state machine, no networking" leg_structure
if [ "$mode" = upgrade ]; then
  leg block "DoS rule: mempool DoS(0), block DoS(100) (upgrade plan U-21)" leg_dos_upgrade
else
  leg block "DoS rule: every inserted DoS is DoS(0) (N1)" leg_dos_harden
fi
leg block "documents (fork-local, P4)" leg_documents
leg block "RPC contract (P7; rpcversion of this line)" leg_contract
leg block "attribution: The Ycash developers on every fork file, no upstream notice touched" leg_attribution
leg block "naming: no ycash6 / DigiDollar / ydollar in src/ (AGENTS.md rule 6)" leg_naming
leg block "rule -> test tags (v2 plan §7)" leg_rule_tags
leg block "rule -> test tags, v3 identifiers (v3 plan §7)" leg_rule_tags_v3
if [ "$mode" = upgrade ]; then
  leg block "RPC contract = the workspace generator's upgrade-line output (rpcversion 5)" leg_contract_generator
  leg block "golden vectors byte-identical on both node lines" leg_cross_line
  leg block "consensus-diff report for the two-reviewer gate (G-9)" leg_consensus_diff
fi

[ -z "$reported" ] || echo "
report-only legs with a delta (mode $mode; not gating):$reported"
if [ -n "$failed" ]; then
  echo "
FAILED legs:$failed"
  exit 1
fi
echo "
audit: every gating leg passed (mode $mode)"
