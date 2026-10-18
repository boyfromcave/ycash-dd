#!/usr/bin/env bash
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .
#
# One weekly libFuzzer target, and the publisher that stitches the per-target artifacts back
# together. The weekly workflow runs one hosted-runner job per target (plan §6.0 item 6, §7
# "Fuzzing"): thirty minutes, then corpus minimisation. A runner that loses communication with
# GitHub (run 39111185611, mid-fuzz, logs never uploaded) takes one target with it. Every target
# that finished still uploads its corpus, and the publisher opens the corpus PR from those.
#
#   qa/yellowback-fuzz-target.sh --list
#       JSON array of Yellowback* / Vault* / PQ* directories that contain fuzz.cpp.
#   qa/yellowback-fuzz-target.sh --self-test
#   qa/yellowback-fuzz-target.sh <Target>
#       Relink ycashd against that target, fuzz, copy crash-* into crashes/, minimise into input/.
#       Requires a tree already configured with --enable-fuzz-main (the workflow does that).
#   qa/yellowback-fuzz-target.sh --assemble
#       Overlay $YB_ARTIFACT_DIR/yellowback-weekly-fuzz-<Target>/ onto this tree and git-add the
#       corpus. Writes $YB_FUZZ_STATUS (default: $YB_ARTIFACT_DIR/publish-status) as "ok" or "fail".
#
# YB_FUZZ_ROOT          tree to operate on (default: this repo).
# YB_FUZZ_SKIP_BUILD    do not relink (self-test).
# FUZZ_SECONDS          -max_total_time (default 1800).
# YB_FUZZ_BIN           ycashd binary (default: $YB_FUZZ_ROOT/src/ycashd).
# YB_ARTIFACT_DIR       where --assemble reads downloaded artifacts.
# YB_FUZZ_SKIP_GENERATORS  do not run gen_*_corpus.py --write.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
ROOT=${YB_FUZZ_ROOT:-$REPO_ROOT}

list_names() {
    local d
    # An unmatched glob (no Vault* on harden/yellowback) stays literal; the [ -f ] test fails
    # and the name is not printed. `|| true` keeps `set -e` from ending the list on that test.
    ( cd "$ROOT/src/fuzzing" && for d in Yellowback* Vault* PQ*; do
        if [ -f "$d/fuzz.cpp" ]; then printf '%s\n' "$d"; fi
      done )
}

list_json() {
    list_names | python3 -c 'import json,sys; print(json.dumps([l.rstrip("\n") for l in sys.stdin if l.strip()]))'
}

run_target() {
    local t=$1
    local secs=${FUZZ_SECONDS:-1800}
    local bin=${YB_FUZZ_BIN:-$ROOT/src/ycashd}
    cd "$ROOT"
    [ -f "src/fuzzing/$t/fuzz.cpp" ] || { echo "no fuzz target src/fuzzing/$t/fuzz.cpp" >&2; exit 1; }
    echo "fuzz-target: $t start $(date -u +%Y-%m-%dT%H:%M:%SZ) seconds=$secs"
    if [ "${YB_FUZZ_SKIP_BUILD:-}" != 1 ]; then
        # Retargeting the symlink does not rebuild; remove every product of bitcoind.cpp (see nightly).
        ln -sfn "fuzzing/$t/fuzz.cpp" src/fuzz.cpp
        rm -f src/ycashd-bitcoind.o src/bitcoind.o src/bitcoind.lo src/.libs/bitcoind.o src/ycashd src/.libs/ycashd
        # shellcheck disable=SC2016 # make expands $(RELDFLAGS) and the other make variables; the shell must not
        make -C src -j"$(nproc)" ycashd \
            ycashd_LDFLAGS='$(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS) -fsanitize=fuzzer,address'
    fi
    mkdir -p "src/fuzzing/$t/output" "src/fuzzing/$t/merged" "src/fuzzing/$t/input" \
        "src/fuzzing/$t/crashes" "src/fuzzing/$t/weekly-result"
    # detect_leaks=0: ycashd leaks on the way out and LeakSanitizer would report a false crash.
    # hard_rss_limit_mb is the backstop for the hosted runner (7 GB). libFuzzer's own
    # -rss_limit_mb stops a single input at 2 GB; if that accounting misses ASan's shadow and
    # the process climbs toward the cgroup limit, the kernel OOM killer takes the runner
    # agent and GitHub reports "lost communication" with no log. ASan aborts this process
    # first instead. The abort is a non-zero exit without a crash-* file; the publisher
    # fails the run on that so it is not a green week.
    local asan='detect_leaks=0:halt_on_error=1:hard_rss_limit_mb=6144'
    local runner=()
    if command -v stdbuf >/dev/null; then runner=(stdbuf -oL -eL); fi
    set +e
    ASAN_OPTIONS=$asan "${runner[@]}" "$bin" "src/fuzzing/$t/output" "src/fuzzing/$t/input" \
        -max_len=4096 -max_total_time="$secs" -rss_limit_mb=2048
    local rc=$?
    set -e
    echo "fuzz-target: $t fuzzer exit $rc"
    local c
    for c in "src/fuzzing/$t"/output/crash-*; do
        [ -f "$c" ] || continue
        cp "$c" "src/fuzzing/$t/crashes/$(date +%Y%m%d)-$(basename "$c")"
    done
    set +e
    ASAN_OPTIONS=$asan "${runner[@]}" "$bin" -merge=1 \
        "src/fuzzing/$t/merged" "src/fuzzing/$t/input" "src/fuzzing/$t/output"
    local mrc=$?
    set -e
    if [ "$rc" -eq 0 ] && [ "$mrc" -ne 0 ]; then rc=$mrc; fi
    printf '%s\n' "$rc" > "src/fuzzing/$t/weekly-result/fuzz-exit-code"
    local i=0 f base
    for f in "src/fuzzing/$t"/merged/*; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        case "$base" in
            *.bin) ;;
            *) mv "$f" "src/fuzzing/$t/input/fuzz-$(date +%Y%m%d)-$i.bin"; i=$((i + 1));;
        esac
    done
    echo "fuzz-target: $t done $(date -u +%Y-%m-%dT%H:%M:%SZ) new_inputs=$i"
}

assemble() {
    local art=${YB_ARTIFACT_DIR:?YB_ARTIFACT_DIR is required}
    local status=${YB_FUZZ_STATUS:-$art/publish-status}
    local fail=0 t names art_t rc found base
    cd "$ROOT"
    names=$(list_names)
    [ -n "$names" ] || { echo "no fuzz targets" >&2; exit 1; }
    for t in $names; do
        art_t="$art/yellowback-weekly-fuzz-$t"
        # upload-artifact keeps workspace-relative paths when several paths share a root, and
        # keeps a single staged directory's contents as the artifact root. Accept both.
        if [ -d "$art_t/src/fuzzing/$t/input" ]; then
            base="$art_t/src/fuzzing/$t"
        elif [ -d "$art_t/input" ]; then
            base="$art_t"
        else
            echo "::error::weekly-fuzz: $t produced no corpus artifact (that runner did not finish)"
            fail=1
            continue
        fi
        mkdir -p "src/fuzzing/$t/input" "src/fuzzing/$t/crashes"
        cp -a "$base/input/." "src/fuzzing/$t/input/"
        if [ -n "$(find "$base/crashes" -type f -print 2>/dev/null)" ]; then
            cp -a "$base/crashes/." "src/fuzzing/$t/crashes/"
        fi
        if [ -f "$base/weekly-result/fuzz-exit-code" ]; then
            rc=$(tr -d '[:space:]' < "$base/weekly-result/fuzz-exit-code")
            echo "weekly-fuzz: $t fuzzer exit $rc"
            [ "$rc" = 0 ] || { echo "::error::weekly-fuzz: $t fuzzer exited $rc"; fail=1; }
        else
            echo "::error::weekly-fuzz: $t artifact has no fuzz-exit-code (the run did not finish)"
            fail=1
        fi
        found=$(find "src/fuzzing/$t/crashes" -type f -name '*crash-*' 2>/dev/null || true)
        if [ -n "$found" ]; then
            echo "::error::weekly-fuzz: $t crashed"
            printf '%s\n' "$found"
            fail=1
            git add -f "src/fuzzing/$t/crashes"
        fi
        git add -f "src/fuzzing/$t/input"
    done
    if [ "${YB_FUZZ_SKIP_GENERATORS:-}" != 1 ]; then
        python3 src/test/gen_yellowback_corpus.py --write || true
        git add -f src/test/yellowback_fuzz_tests.cpp
        if [ -f src/test/gen_vault_corpus.py ]; then
            python3 src/test/gen_vault_corpus.py --write || true
            git add -f src/test/vault_fuzz_tests.cpp
        fi
        if [ -f src/test/gen_pq_corpus.py ]; then
            python3 src/test/gen_pq_corpus.py --write || true
            git add -f src/test/vault_fuzz_tests.cpp
        fi
        if [ -f src/test/gen_vault_state_corpus.py ]; then
            python3 src/test/gen_vault_state_corpus.py --write || true
            git add -f src/test/vault_fuzz_tests.cpp
        fi
    fi
    if [ "$fail" = 0 ]; then printf ok > "$status"; else printf fail > "$status"; fi
    echo "weekly-fuzz assemble: $(cat "$status")"
}

self_test() {
    local bin art repo listed
    # Not local: the EXIT trap runs after this function returns, and `set -u` would then
    # see a local as unset.
    YB_SELF_TEST_TMP=$(mktemp -d)
    trap 'rm -rf "$YB_SELF_TEST_TMP"' EXIT
    tmp=$YB_SELF_TEST_TMP
    bin="$tmp/ycashd"
    cat > "$bin" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$YB_FUZZ_LOG"
case "${ASAN_OPTIONS-}" in
    *detect_leaks=0*) ;;
    *) echo "ASAN_OPTIONS missing detect_leaks=0: ${ASAN_OPTIONS-}" >&2; exit 9;;
esac
case "${ASAN_OPTIONS-}" in
    *hard_rss_limit_mb=6144*) ;;
    *) echo "ASAN_OPTIONS missing hard_rss_limit_mb: ${ASAN_OPTIONS-}" >&2; exit 9;;
esac
if [ "${1-}" = "-merge=1" ]; then
    mkdir -p "$2"
    printf found > "$2/cov1"
    exit 0
fi
case " $* " in
    *" -rss_limit_mb=2048 "*) ;;
    *) echo "missing -rss_limit_mb=2048 in: $*" >&2; exit 9;;
esac
mkdir -p "$1"
printf boom > "$1/crash-deadbeef"
exit 0
EOF
    chmod +x "$bin"
    repo="$tmp/tree"
    mkdir -p "$repo/src/fuzzing/YellowbackBundle/input" "$repo/src/fuzzing/Vault/input" \
        "$repo/src/fuzzing/CheckBlock"
    printf seed > "$repo/src/fuzzing/YellowbackBundle/input/seed.bin"
    printf seed > "$repo/src/fuzzing/Vault/input/seed.bin"
    printf 'int fuzz(){return 0;}\n' > "$repo/src/fuzzing/YellowbackBundle/fuzz.cpp"
    printf 'int fuzz(){return 0;}\n' > "$repo/src/fuzzing/Vault/fuzz.cpp"
    printf 'int fuzz(){return 0;}\n' > "$repo/src/fuzzing/CheckBlock/fuzz.cpp"
    git -C "$repo" init -q
    git -C "$repo" config user.email "fuzz-self-test@example.com"
    git -C "$repo" config user.name "fuzz-self-test"
    listed=$(YB_FUZZ_ROOT=$repo "$0" --list)
    [ "$listed" = '["YellowbackBundle", "Vault"]' ] || {
        echo "list mismatch: $listed" >&2; exit 1; }
    YB_FUZZ_LOG=$tmp/log YB_FUZZ_ROOT=$repo YB_FUZZ_SKIP_BUILD=1 YB_FUZZ_BIN=$bin \
        FUZZ_SECONDS=1 "$0" YellowbackBundle
    find "$repo/src/fuzzing/YellowbackBundle/crashes" -type f -name '*crash-deadbeef' | grep -q .
    [ -f "$repo/src/fuzzing/YellowbackBundle/input/seed.bin" ]
    find "$repo/src/fuzzing/YellowbackBundle/input" -name 'fuzz-*.bin' | grep -q .
    [ "$(cat "$repo/src/fuzzing/YellowbackBundle/weekly-result/fuzz-exit-code")" = 0 ]
    # A non-zero exit with no crash file is recorded, not swallowed.
    cat > "$bin" <<'EOF'
#!/bin/sh
if [ "${1-}" = "-merge=1" ]; then mkdir -p "$2"; exit 0; fi
mkdir -p "$1"
exit 71
EOF
    chmod +x "$bin"
    YB_FUZZ_ROOT=$repo YB_FUZZ_SKIP_BUILD=1 YB_FUZZ_BIN=$bin FUZZ_SECONDS=1 "$0" Vault
    [ "$(cat "$repo/src/fuzzing/Vault/weekly-result/fuzz-exit-code")" = 71 ]
    [ ! -e "$repo/src/fuzzing/Vault/crashes" ] || [ -z "$(find "$repo/src/fuzzing/Vault/crashes" -type f)" ]
    # Assemble: YellowbackBundle finished with a crash; Vault's exit is non-zero; CheckBlock is not a target.
    art="$tmp/art"
    for t in YellowbackBundle Vault; do
        mkdir -p "$art/yellowback-weekly-fuzz-$t/src/fuzzing"
        cp -a "$repo/src/fuzzing/$t" "$art/yellowback-weekly-fuzz-$t/src/fuzzing/"
    done
    # Reset the tree's corpus so assemble is what brings the crash and the new input back.
    rm -rf "$repo/src/fuzzing/YellowbackBundle/crashes" "$repo/src/fuzzing/YellowbackBundle/input"
    mkdir -p "$repo/src/fuzzing/YellowbackBundle/input"
    printf seed > "$repo/src/fuzzing/YellowbackBundle/input/seed.bin"
    git -C "$repo" add -A
    git -C "$repo" commit -q -m seed
    YB_FUZZ_ROOT=$repo YB_ARTIFACT_DIR=$art YB_FUZZ_STATUS=$tmp/status YB_FUZZ_SKIP_GENERATORS=1 \
        "$0" --assemble
    [ "$(cat "$tmp/status")" = fail ]
    git -C "$repo" diff --cached --name-only | grep -q 'YellowbackBundle/crashes/'
    git -C "$repo" diff --cached --name-only | grep -q 'YellowbackBundle/input/fuzz-'
    # Missing artifact fails the status and leaves the other target's seed in place.
    rm -rf "$art/yellowback-weekly-fuzz-Vault"
    git -C "$repo" reset -q
    YB_FUZZ_ROOT=$repo YB_ARTIFACT_DIR=$art YB_FUZZ_STATUS=$tmp/status YB_FUZZ_SKIP_GENERATORS=1 \
        "$0" --assemble
    [ "$(cat "$tmp/status")" = fail ]
    [ -f "$repo/src/fuzzing/Vault/input/seed.bin" ]
    # Flat artifact root: one staged directory, which is how the workflow uploads. A clean pair
    # of targets (exit 0, a new input, no crash) assembles as ok.
    git -C "$repo" reset -q --hard
    git -C "$repo" clean -qfd
    flat="$tmp/flat"
    for t in YellowbackBundle Vault; do
        mkdir -p "$flat/yellowback-weekly-fuzz-$t/input" "$flat/yellowback-weekly-fuzz-$t/weekly-result"
        printf seed > "$flat/yellowback-weekly-fuzz-$t/input/seed.bin"
        printf new > "$flat/yellowback-weekly-fuzz-$t/input/fuzz-flat.bin"
        printf '0\n' > "$flat/yellowback-weekly-fuzz-$t/weekly-result/fuzz-exit-code"
    done
    YB_FUZZ_ROOT=$repo YB_ARTIFACT_DIR=$flat YB_FUZZ_STATUS=$tmp/status YB_FUZZ_SKIP_GENERATORS=1 \
        "$0" --assemble
    [ "$(cat "$tmp/status")" = ok ]
    git -C "$repo" diff --cached --name-only | grep -q 'fuzz-flat.bin'
    git -C "$repo" diff --cached --name-only | grep -q 'crashes/' && { echo "clean run staged a crash" >&2; exit 1; }
    # The real tree's list is exactly the Yellowback* / Vault* / PQ* targets, stock ones excluded.
    local real have d base
    real=$(YB_FUZZ_ROOT=$REPO_ROOT "$0" --list)
    have=$(cd "$REPO_ROOT/src/fuzzing" && for d in Yellowback* Vault* PQ*; do
        if [ -f "$d/fuzz.cpp" ]; then printf '%s\n' "$d"; fi
    done | python3 -c 'import json,sys; print(json.dumps([l.rstrip("\n") for l in sys.stdin if l.strip()]))')
    [ "$real" = "$have" ]
    python3 -c 'import json,sys; names=json.loads(sys.argv[1]);
assert "CheckBlock" not in names and "DecodeHexTx" not in names
assert "YellowbackBundle" in names and "Vault" in names and "VaultState" in names and "PQScript" in names' \
        "$real"
    for d in "$REPO_ROOT"/src/fuzzing/*/; do
        base=$(basename "$d")
        case "$base" in
            Yellowback*|Vault*|PQ*) [ -f "$d/fuzz.cpp" ];;
        esac
    done
    if YB_FUZZ_ROOT=$repo "$0" NotATarget >/dev/null 2>&1; then
        echo "missing target should fail" >&2; exit 1
    fi
    echo "yellowback-fuzz-target: self-test ok ($real)"
}

case "${1:-}" in
    --list) list_json ;;
    --self-test) self_test ;;
    --assemble) assemble ;;
    "") echo "usage: qa/yellowback-fuzz-target.sh --list | --self-test | --assemble | <Target>" >&2; exit 2 ;;
    *) run_target "$1" ;;
esac
