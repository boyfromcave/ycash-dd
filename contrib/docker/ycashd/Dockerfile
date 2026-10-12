# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .
#
# The official ycashd image, built from this tree. Build context = the repository root:
#   docker build -f contrib/docker/ycashd/Dockerfile -t ycash/ycashd:local .
# Stage 1 (ubuntu 22.04, the release matrix's tier-1 native host) runs zcutil/build.sh with the
# depends system; depends/built, depends/sources, ccache and the cargo registry live in BuildKit
# cache mounts, so the first build is 1-2 h on a laptop and a rebuild after a source change is
# minutes. Stage 2 carries only the three binaries. ycashd 4.5 needs the three zk parameter files at start
# (~760 MB); the entrypoint runs zcutil/fetch-params.sh into the params volume on first start.
# Runs as uid 2001 with the datadir at /var/lib/ycash; contrib/docker/ycashd/entrypoint.sh
# turns YCASHD_* environment variables into ycash.conf (README.md there).
FROM ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config libc6-dev m4 autoconf libtool ncurses-dev unzip git \
      python3 zlib1g-dev curl bsdmainutils automake libtinfo5 libxml2 ccache ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ARG JOBS=4
ENV CCACHE_DIR=/root/.cache/ccache
RUN --mount=type=cache,id=ycash-dd-depends-built,target=/src/depends/built \
    --mount=type=cache,id=ycash-dd-depends-sources,target=/src/depends/sources \
    --mount=type=cache,id=ycash-dd-ccache,target=/root/.cache/ccache \
    --mount=type=cache,id=ycash-dd-cargo,target=/root/.cargo/registry \
    { [ "$(uname -m)" != aarch64 ] || BUILD_STAGE=depends ./zcutil/build.sh -j${JOBS} || true; } \
 && sh contrib/docker/ycashd/aarch64-clang-fixup.sh \
 && { ./zcutil/build.sh -j${JOBS} || { echo "=== depends config.log tails"; for f in depends/work/build/*/*/*/config.log; do echo "--- $f"; grep -n -B2 -A12 "cannot create executables\|error:" "$f" | tail -60; done; exit 1; }; } \
 && mkdir -p /out && cp src/ycashd src/ycash-cli src/ycash-tx /out/ && strip /out/*

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libtinfo5 curl wget jq netcat-openbsd \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --home-dir /srv/ycash --create-home --uid 2001 --shell /bin/sh ycash \
 && mkdir -p /var/lib/ycash /srv/ycash/.zcash-params && chown -R ycash:ycash /srv/ycash /var/lib/ycash
COPY --from=builder /out/ycashd /out/ycash-cli /out/ycash-tx /usr/local/bin/
COPY contrib/docker/ycashd/entrypoint.sh /usr/local/bin/ycashd-entrypoint
COPY zcutil/fetch-params.sh /usr/local/bin/fetch-params.sh
RUN chmod +x /usr/local/bin/ycashd-entrypoint
USER ycash
ENV HOME=/srv/ycash
VOLUME ["/var/lib/ycash", "/srv/ycash/.zcash-params"]
EXPOSE 8832 8833 18832 18833
ENTRYPOINT ["ycashd-entrypoint"]
