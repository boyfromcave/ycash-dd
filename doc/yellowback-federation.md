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
[[sources]]
name = "exchange-a"
url = "https://api.exchange-a/ticker/YECUSD"
path = "last"                              # dotted path into the JSON reply
[[sources]]
name = "exchange-b"
url = "https://api.exchange-b/ticker/YEC-BTC"
path = "price"
quote = "BTC"
btc_usd = "https://api.exchange-b/ticker/BTC-USD"
btc_usd_path = "price"
```

At least three independent sources. The coordinator keeps a five-minute average per source,
drops a source silent for two minutes, drops sources more than 10 % from the median, and clamps
each round to ±10 % of the last on-chain price. Peers accept a proposal only within 2 % of their
own figure.

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
| Exchange outage / bad feed | The source is dropped; below three live sources the coordinator publishes nothing | Fix or replace the source; prices resume |
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
