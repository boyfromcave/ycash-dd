# `contrib/ydollar/` — YDollar federation tooling

Python 3 tooling that runs **beside** `ycashd`, never inside it. The node has no outbound
networking for YDollar; everything here drives the node over its local JSON-RPC and talks to other
operators over HTTPS.

| File | Role | Status |
|---|---|---|
| `ydollar_fed.py` | Federation coordinator: price rounds (fetch, TWAP, outlier filter, ±10 % move clamp, median), builds the PRICE transaction with `yd_createpricetx`, collects partial signatures from peers with `signrawtransaction`, broadcasts; serves `POST /cosign` for redemptions by calling `yd_cosignredeem`; `rotate` sub-command | Phase 4 |
| `ydollar-redeem` | User-side co-signature collector: `yd_redeem` → POST to each operator's `/cosign` until k signatures → `yd_submitredeem` | Phase 4 |
| `ydollar-fed.toml.example` | Coordinator configuration (node RPC URL, operator id, peer URLs, price sources, thresholds) | Phase 4 |

Test-only flags: `--mock-price <path>` (price read from a file the test rewrites) and
`--insecure-localhost` (plain HTTP on 127.0.0.1). The regtest functional test
`qa/rpc-tests/ydollar_federation.py` launches one coordinator per operator node with both.

Design: development plan §5 (federation coordinator) and §4.6 (operator node).
