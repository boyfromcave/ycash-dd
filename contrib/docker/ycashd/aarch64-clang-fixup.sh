#!/bin/sh
# Copyright (c) 2026 The Ycash developers
# On aarch64 Linux the depends clang 18 tarball keeps libc++'s per-target files under
# include/aarch64-unknown-linux-gnu/c++/v1 (and lib/aarch64-unknown-linux-gnu), but boost's b2
# clang toolset compiles with --target=arm64-pc-linux, so clang looks under arm64-pc-linux and
# fails with "'__config_site' file not found". The depends recipes are untouched: the Dockerfile
# runs the depends stage once (it stages native_clang, then fails at boost), this adds the two
# aliases, and the second run resumes. A no-op on every other architecture.
set -eu
[ "$(uname -m)" = "aarch64" ] || exit 0
HOST=aarch64-unknown-linux-gnu
N=depends/$HOST/native
echo "native include layout:"; ls "$N/include" 2>/dev/null | head; ls "$N/include/$HOST/c++/v1" 2>/dev/null | head -3
for d in include lib; do
  [ -d "$N/$d/$HOST" ] && [ ! -e "$N/$d/arm64-pc-linux" ] && ln -s "$HOST" "$N/$d/arm64-pc-linux" && echo "aliased $N/$d/arm64-pc-linux -> $HOST"
done
exit 0
