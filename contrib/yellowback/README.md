# `contrib/yellowback/` — Yellowback federation tooling

Python 3 tooling that runs **beside** `ycashd`, never inside it. The node has no outbound
networking for Yellowback; everything here drives the node over its local JSON-RPC and talks to other
operators over HTTPS.

| File | Role | Status |
|---|---|---|
| `yellowback_fed.py` | Federation coordinator: price rounds (config-driven sources with presets for CoinGecko, Nonkyc and SafeTrade or a generic URL + JSON path, YEC/BTC pairs converted with a median of configured BTC/USD references, per-source freshness and spread guards, TWAP, outlier filter, median over `min_sources`/`min_venues`, ±10 % move clamp), builds the PRICE transaction with `yed_createpricetx`, collects partial signatures from peers with `signrawtransaction`, broadcasts; serves `POST /cosign` for redemptions by calling `yed_cosignredeem`; `rotate` sub-command | done |
| `yellowback-redeem` | User-side co-signature collector: `yed_redeem` → POST to each operator's `/cosign` until k signatures → `yed_submitredeem` | done |
| `yellowback-fed.toml` (see `doc/yellowback-federation.md`) | Coordinator configuration (node RPC URL, operator id, peer URLs, price sources and feed thresholds) | done |
| `devnet/yellowback-devnet` | One-laptop Yellowback network: five regtest nodes, funded user wallet, 2-of-3 federation with coordinators on a mock price, stopped at "ready to mint"; `up` / `status` / `mine` / `price` / `wallet` / `cli` / `down` (plan §6.0 item 4). Run with the workspace venv's Python | done |
| `test_yellowback_fed.py` | Unit tests for the source layer against captured exchange replies (no node, no network): `python3 -m unittest contrib/yellowback/test_yellowback_fed.py` | done |

`yellowback_fed.py sources --config <toml>` fetches every configured source once and prints what resolved (no node needed).

Test-only flags: `--mock-price <path>` (price read from a file the test rewrites) and
`--insecure-localhost` (plain HTTP on 127.0.0.1). The regtest functional test
`qa/rpc-tests/yellowback_federation.py` launches one coordinator per operator node with both.

Design: development plan §5 (federation coordinator) and §4.6 (operator node).
