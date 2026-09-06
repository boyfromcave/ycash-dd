# Yellowback federation — operator runbook (draft)

You are one of *n* operators (v1: nine, threshold five). Your node holds one roster key. Together
the federation publishes the YEC/USD price and co-signs collateral releases. What you can and
cannot do is stated in `yellowback.md` ("Trust statement"); read it first.

## 1. Host and node

- A dedicated machine with full-disk encryption. Ycash's wallet encryption is experimental and
  is not relied on: the roster key sits in `wallet.dat` in the clear.
- `ycash.conf`: `server=1`, `rpcbind=127.0.0.1`, `rpcallowip=127.0.0.1`, `experimentalfeatures=1`,
  `yellowback=1`, a strong `rpcpassword`. Leave `datacarrier` at its default (or `OP_RETURN`
  transactions are not relayed). Do not use `prune`.
- The wallet is **dedicated**: it holds the roster key and the refill coins you fund yourself,
  nothing else. `signrawtransaction` signs every input it can solve, so a wallet that holds
  other coins could be tricked into signing one away (the coordinator refuses such proposals,
  but the wallet must not hold anything to lose).

## 2. Key ceremony

1. On your node: `getnewaddress` → `validateaddress <addr>` → copy `pubkey` (66 hex chars, a
   33-byte compressed key; anything else is rejected). The private key never leaves the node.
2. Send the pubkey to the ceremony chair over an authenticated channel; receive every operator's
   pubkey the same way.
3. Sort the pubkeys ascending by their bytes and run, on **your** node:
   `addmultisigaddress <k> '["<pk1>", ..., "<pkn>"]'`. Every operator must obtain the **same**
   address. Record it. This stores the roster script in your wallet, which is what lets
   `signrawtransaction` solve the anchor.
4. Genesis anchor (once per network): the chair pays 1 YEC to that address, waits for one
   confirmation, and records `<txid>:<n>`, the block height and the roster script hex. Those three
   values go into `src/yellowback/params.cpp` for the network (`startHeight`, `genesisAnchor`,
   `genesisRosterScript`) and ship in the release. On regtest they are the three
   `-yellowback*` start arguments instead.
5. After the release: `yed_getroster` must show the ceremony address and pubkeys.

Backup: `wallet.dat` to offline media after step 1; `importprivkey <key> "" false` restores it
without a rescan.

## 3. Coordinator

`contrib/yellowback/yellowback_fed.py serve --config yellowback-fed.toml`, run as a service beside
`ycashd`. Configuration:

```toml
rpc_url = "http://user:pass@127.0.0.1:8232"
id = 3                                     # your position in the sorted roster, 0-based
listen = "0.0.0.0:8443"                    # behind TLS termination with client-certificate auth
peers = ["https://op0.example", "https://op1.example", ...]   # every other operator
round_blocks = 8

# Price feed. Every value is optional; the defaults are shown.
poll_seconds = 30          # fetch cadence (CoinGecko's public API allows ~30 calls/min and caches 30 s)
min_sources = 3            # live sources needed, after the outlier filter, before a price is published
min_venues = 2             # distinct venues among them (CoinGecko re-reports the same venues)
twap_seconds = 300         # per-source time average
silence_seconds = 120      # a source with no sample this long is dropped
outlier_bps = 1000         # sources more than 10 % from the median are dropped
min_btc_sources = 2        # live BTC/USD references needed before a YEC/BTC pair is converted

[[sources]]                # CoinGecko's aggregate across every venue it tracks
name = "coingecko"
kind = "coingecko_simple"
max_age = 900              # drop when CoinGecko's own last update is older than 15 min
# api_key = "CG-..."       # optional demo key: higher rate limit

[[sources]]                # SafeTrade's last trade as CoinGecko reports it (with trade time and spread)
name = "safetrade-via-coingecko"
kind = "coingecko_ticker"
market = "safe_trade"
target = "USDT"
max_age = 3600             # last trade within the hour
max_spread_bps = 2000      # SafeTrade's book is thin: a 15 % spread was seen on 2026-09-05

[[sources]]                # Nonkyc's last trade as CoinGecko reports it
name = "nonkyc-via-coingecko"
kind = "coingecko_ticker"
market = "nonkyc_io"
target = "USDT"
max_age = 3600
max_spread_bps = 500

[[sources]]                # Nonkyc directly
name = "nonkyc"
kind = "nonkyc_market"
symbol = "YEC_USDT"
max_age = 3600
max_spread_bps = 500       # from bestBid/bestAsk

[[sources]]                # SafeTrade directly: a flat ticker with no trade time and no bid/ask
name = "safetrade"
kind = "peatio_ticker"
market = "yecusdt"

# YEC/BTC pairs. quote = "BTC" is set by the preset; the price is multiplied by the BTC/USD
# reference below in the same poll. Same venue names as the USDT pairs, so they count once.
[[sources]]
name = "nonkyc-btc"
kind = "nonkyc_market"
symbol = "YEC_BTC"
max_age = 3600
max_spread_bps = 300

[[sources]]
name = "nonkyc-btc-via-coingecko"
kind = "coingecko_ticker"
market = "nonkyc_io"
target = "BTC"             # reads the venue's own BTC last, not CoinGecko's conversion
max_age = 3600
max_spread_bps = 300

[[sources]]
name = "safetrade-btc"
kind = "peatio_ticker"
market = "yecbtc"

# BTC/USD references: the median of the live ones converts every BTC-quoted pair. Same table
# format and guards as [[sources]]; they must be USD-quoted. Fetched only when a BTC pair exists.
[[btc_usd_sources]]
name = "coingecko-btc"
kind = "coingecko_simple"
coin = "bitcoin"
max_age = 900

[[btc_usd_sources]]
name = "kraken"
kind = "kraken_ticker"     # XBT/USD, real dollars
max_spread_bps = 50

[[btc_usd_sources]]
name = "coinbase"
kind = "coinbase_ticker"   # BTC-USD, real dollars
max_age = 600
max_spread_bps = 50

[[btc_usd_sources]]
name = "nonkyc-btcusdt"
kind = "nonkyc_market"
symbol = "BTC_USDT"
max_age = 3600
max_spread_bps = 200
```

**How a source is read.** Each `[[sources]]` table names a `kind`: a preset that fills in the URL and
the JSON paths for a known API (`coingecko_simple`, `coingecko_ticker`, `nonkyc_market`,
`peatio_ticker`), or `generic`, which takes `url` and `path` verbatim. Every preset field can be
overridden in the table, so when a venue changes the shape of its reply the fix is one line of
config, not a release: set `path` (and `timestamp_path`, `spread_path`, ...) to the new location.
Paths are dotted; a segment `[field=value,field2=value2]` selects an element of a list by its
fields, so a list whose order is not stable (CoinGecko's tickers) is still addressable, and a bare
integer indexes. Optional guards per source: `max_age` (seconds since the venue's own trade time,
needs `timestamp_path` and `timestamp_unit` = `s`, `ms` or `iso`), `max_spread_bps` (from
`spread_path` + `spread_unit` = `percent`, `bps` or `ratio`, or from `bid_path` + `ask_path`),
`reject_paths` (boolean flags that veto a sample, e.g. CoinGecko's `is_stale`/`is_anomaly`),
`headers` and `scale`. `venue` groups sources for `min_venues`; the presets use CoinGecko's market
identifiers (`safe_trade`, `nonkyc_io`) so a venue read directly and through CoinGecko counts once.

**BTC-quoted pairs.** YEC/BTC is where most of the book usually is, and BTC/USD is the one price
every exchange agrees on to a few basis points. A source with `quote = "BTC"` (set by the presets
for `symbol = "YEC_BTC"`, `market = "yecbtc"` and `target = "BTC"`) is multiplied, in the same
poll, by the median of the live `[[btc_usd_sources]]`. Those use the same table format, presets
and guards (`kraken_ticker` and `coinbase_ticker` exist for this purpose; Kraken and Coinbase quote
real dollars, the small venues quote USDT). Below `min_btc_sources` live references no BTC pair is
converted: each shows `btcref` and is dropped, the USD-quoted sources carry on. The references are
not fetched at all when no BTC pair is configured. Note the USDT pairs are treated as dollars
throughout; the BTC route is also the one that does not lean on the stablecoin peg.

**Verified endpoints (2026-09-05).** CoinGecko lists exactly two venues for YEC: SafeTrade
(YEC/USDT, YEC/BTC, YEC/ETH on its own API) and Nonkyc (YEC/USDT, USDC, BTC). The presets above
were checked against live replies on that date, as were the four BTC/USD references. SafeTrade sits behind Cloudflare and answered `curl` and a browser user agent with
403 but accepts the coordinator's own request headers; run the check below from your operator
host before relying on it. Because there are two venues, `min_sources = 3` is met by reading a
venue twice (directly and through CoinGecko), which guards against one API breaking, not against
one market being wrong; `min_venues = 2` is what fails closed when a whole venue is gone.

**Check your sources** after editing the file, after any venue announces an API change, and
whenever `/status` shows a source not `ok`:

```
$ contrib/yellowback/yellowback_fed.py sources --config yellowback-fed.toml
BTC/USD references (for BTC-quoted pairs):
  source          venue      kind              state  quote    micro_usd  age_s   spread  detail
  coingecko-btc   coingecko  coingecko_simple  ok     USD    79986000000    145
  kraken          kraken     kraken_ticker     ok     USD    79967300000           0 bps
  coinbase        coinbase   coinbase_ticker   ok     USD    79963130000      0    0 bps
  nonkyc-btcusdt  nonkyc_io  nonkyc_market     ok     USD    79962750000     17   93 bps
  reference: $79965.21 (4 live, need min_btc_sources=2)
YEC/USD sources:
  source                    venue       kind              state  quote  micro_usd  age_s    spread  detail
  coingecko                 coingecko   coingecko_simple  ok     USD       437417     75
  safetrade-via-coingecko   safe_trade  coingecko_ticker  ok     USD       430000    735  1489 bps
  nonkyc                    nonkyc_io   nonkyc_market     ok     USD       452300    294    26 bps
  nonkyc-btc                nonkyc_io   nonkyc_market     ok     BTC       445406    270   102 bps
  nonkyc-btc-via-coingecko  nonkyc_io   coingecko_ticker  ok     BTC       445406    224   125 bps
  safetrade                 safe_trade  peatio_ticker     ok     USD       430000
  safetrade-btc             safe_trade  peatio_ticker     ok     BTC       394229
live: 7 source(s) from 3 venue(s); need min_sources=3, min_venues=2; outlier_bps=1000
median: 437417 micro-USD ($0.437417)
```

That is the real output of 2026-09-05 (the CoinGecko aggregate counts as its own venue; note
SafeTrade's BTC book 10 % under the rest, which is what the outlier filter and the spread guard
are for). It needs no node. A state of `shape` means the reply parsed as JSON but the configured path did not
resolve (the API changed, or the venue returned an error page): fix `path` or switch the source
to `generic` with the new URL. `fetch` is a network or HTTP error; `stale`, `spread` and
`flagged` are the guards above, and `btcref` means the BTC/USD reference was unavailable. The same table is served at `GET /status` under `feed`, with
per-source success and failure counts, and the coordinator logs every state change of a source at
WARNING. While the live count is below `min_sources` or `min_venues` this coordinator proposes
nothing and refuses peers' proposals with "no own price to compare against"; when every operator
is in that state the chain has no fresh price after 48 blocks and mints pause (fail closed),
while transfers and redemptions continue.

Transport: production is HTTPS with mutual authentication (operator client certificates) in front
of the coordinator; `--insecure-localhost` exists only for the regtest tests. The public
`POST /cosign` endpoint is rate-limited per source IP; publish its URL with your roster pubkey so
wallets can pair the two.

Monitoring: `yellowback_fed.py status`, `GET /status` (rounds, co-signs, refusals, last price),
`yed_getinfo` (`synced`, `healthy`), `yed_getprice` (`age` must stay under 48 blocks), and the
anchor value in `yed_getinfo` (the coordinator refills it from your wallet below 0.01 YEC).

## 4. Incidents

| Event | What happens | What to do |
|---|---|---|
| Your node or coordinator is down | Rounds continue while ≥ k operators are up; your slot is skipped after two blocks | Restore; `SyncToChain` catches the index up at start |
| Fewer than k operators up | No new prices (mints pause after 48 blocks); no redemptions | Restore quorum; nothing else changes |
| Exchange outage / bad feed | The source is dropped; below `min_sources` live sources or `min_venues` venues the coordinator publishes nothing | Fix or replace the source; prices resume |
| A venue changes its API (URL or reply shape) | The source shows `shape` or `fetch` in `/status` and the log; it is dropped from the median | `yellowback_fed.py sources` to see what resolves; edit that source's `path`/`url` (or switch it to `kind = "generic"`); no release needed. Tell the other operators: everyone's config carries the same preset |
| Key compromise or operator departure | The compromised key can only act with k−1 accomplices | **Rotate**: new ceremony, every operator runs `addmultisigaddress` for the new roster, then one operator runs `yellowback_fed.py rotate --new-roster <hex>`; the departing operator keeps the old key until `yed_listvaults` shows zero open vaults on the old roster index |
| Anchor custody broken (spent to a non-P2SH) | No further prices are recorded | A release with a new genesis anchor |
| `yed_getinfo.healthy` false | The index found an inconsistency | Restart with `-reindex-yellowback` |

## 5. Redemption co-signing

Your coordinator answers `POST /cosign` by calling `yed_cosignredeem`, which applies RED-0..8:
the transaction spends one open vault, burns at least the required YED at the current health, is
standard, pays the flat fee, expires within 42 blocks, carries a valid owner signature over the
vault script the *index* reconstructs, and is co-signed at most once per vault per height. You
never see where the collateral goes and do not need to: the owner's signature already commits to
it. Refusals are logged with the rule id.
