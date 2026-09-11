# `contrib/yellowback/` — Yellowback tooling (federation prototype, being replaced by phase)

Python 3 tooling that runs **beside** `ycashd`, never inside it. The node has no outbound
networking for Yellowback; everything here drives the node over its local JSON-RPC.

**Phase 0 (v2):** the federation is gone from the node (`yed_createpricetx`, `yed_cosignredeem`,
`yed_submitredeem`, `yed_abortredeem`, `yed_getroster` no longer exist), so the coordinator's
price rounds, its `/cosign` endpoint and the devnet's "ready to mint" state are **out of service**
until Phase 7 replaces the coordinator with the quote agent (`yed_setquote`) and Phase 5 redoes
the devnet on the v2 topology. `yellowback_fed.py` is kept for its price-source layer, which the
quote agent reuses; `test_yellowback_fed.py` (no node, no network) still runs in CI.

| File | Role | Status |
|---|---|---|
| `yellowback_fed.py` | Prototype coordinator. Its price-source layer (config-driven sources with presets for CoinGecko, Nonkyc and SafeTrade or a generic URL + JSON path, YEC/BTC pairs converted with a median of configured BTC/USD references, per-source freshness and spread guards, TWAP, outlier filter, median over `min_sources`/`min_venues`, ±10 % move clamp) is what Phase 7's quote agent keeps; its price rounds (`yed_createpricetx`), `/cosign` endpoint (`yed_cosignredeem`) and `rotate` sub-command call RPCs that no longer exist | feed layer kept; the rest out of service |
| `yellowback-redeem` | User-side co-signature collector | removed in Phase 0 |
| `yellowback-fed.toml` | Coordinator configuration | out of service (the operators' guide was removed with the federation) |
| `devnet/yellowback-devnet` | One-laptop Yellowback network (five regtest nodes, funded user wallet, 2-of-3 federation, mock price); `up` / `status` / `mine` / `price` / `wallet` / `cli` / `down`. Run with the workspace venv's Python | out of service until Phase 5 (needs `yed_getroster` and the coordinators) |
| `test_yellowback_fed.py` | Unit tests for the source layer against captured exchange replies (no node, no network): `python3 -m unittest contrib/yellowback/test_yellowback_fed.py` | runs in CI (`python` job) |

`yellowback_fed.py sources --config <toml>` fetches every configured source once and prints what resolved (no node needed).

Test-only flags: `--mock-price <path>` (price read from a file the test rewrites) and
`--insecure-localhost` (plain HTTP on 127.0.0.1).

Design: the v2 development plan in the workspace (`docs/plans/yellowback-v2-development-plan.md`),
§4.5 (`yed_setquote`) and Phase 7 (the quote agent).
