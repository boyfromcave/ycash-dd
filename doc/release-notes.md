(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes at release time)

Notable changes
===============

Post-quantum addresses show their scheme (D-9, `rpcversion` 9)
-------------------------------------------------------------

- **New prefixes.** A post-quantum address now names its signature scheme in its third character:
  `sqf…` (FN-DSA-512, Falcon) and `sqs…` (SLH-DSA-SHA2-128s) on mainnet, `tqf…`/`tqs…` on testnet,
  `rqf…`/`rqs…` on regtest; the YED form is `ybf…`/`ybs…` (mainnet), `ytf…`/`yts…`, `yrf…`/`yrs…`. Still 53
  characters. The key, the script it pays and every balance are unchanged: only the text differs.
- **The earlier text form is no longer accepted.** An address in the form used before this release
  (`sq…`/`tq…`/`rq…` or `yb…`/`yt…`/`yr…` at 53 characters, whose third character did not name the scheme) is
  refused by `sendtoaddress`, `validateaddress`, `yed_send` and every other RPC. Get the new string for a key
  you already hold with `getaddressesbyaccount ""` or `listreceivedbyaddress 0 true` (the wallet lists it in the new form); for a key in a
  wallet dump, `z_importpqkey` with the dump's whole `pqseed=` line returns it (`address`). The one exception:
  `z_importpqkey` still accepts a dump line whose `addr=`/`yecaddr=` are in the earlier form, so old dumps import.
- **Wallets upgrade their address book once.** A wallet's post-quantum address-book entries written by an
  earlier build are rewritten under the new form at the first start (labels kept; logged once).
- **`rpcversion` 9.** `yed_getinfo.params.pqAddressVersion` and `.params.pq.plainAddressVersion` were hex
  strings and are now objects keyed by scheme number (`{"1": "<hex>", "2": "<hex>"}`), one 3-byte version
  each. Clients that build or check post-quantum addresses must read them this way or use
  `src/test/data/pq_address_vectors.json`.

Plain-YEC post-quantum addresses (`sq…`)
----------------------------------------

- **Inputs spending PQ coins carry no address** in `getblockdeltas`, `getspentinfo` and
  `getrawtransaction` (verbose `vin[].address`): the insight index keys a spent output by its
  Hash160, and a `TX_PQPKH` key hash is 32 bytes, so the index cannot name it from the input side.
  `getaddressdeltas` (and `getaddressutxos`/`getaddressbalance`/`getaddresstxids`) name them as
  requested, by `sq…`/`tq…`/`rq…` or the PQ `yb…`/`yt…`/`yr…` form.
- **`-reindex` once** on a node with `-insightexplorer` or `-lightwalletd` that synced before this
  build: the address index of earlier blocks holds no `TX_PQPKH` outputs until it is rebuilt.
- **`createrawtransaction` takes an `sq…` output** without the wallet's payability checks (Falcon
  activation, the PQ dust threshold); relay policy still refuses a non-standard or dust output. Prefer `sendtoaddress`/`sendmany`, which check.
- **`yed_getnewaddress "pq"` now files holder keys in the address book** (under the `sq…` form, so
  the YEC their token outputs carry is not taken for change); holder keys issued by an earlier build
  are not added retroactively (`getnewaddress "" "pq"` files plain-YEC PQ keys; `"pqowner"` keys stay
  out).
- `z_sendmany` pays an `sq…` recipient (as a transparent output); no `z_*` RPC spends from one.
- **Seed-only restore (`-hdseedfile`)** restores the seed's first Sapling address and its
  post-quantum keys up to a gap of `PQ_KEY_LOOKAHEAD` = 20 unused keys per scheme: a key beyond 20
  unused ones in a row is not re-found. Recover such keys by issuing PQ addresses past the gap
  (`getnewaddress "" "pq"`) and running `rescanblockchain`; further Sapling addresses likewise with
  `z_getnewaddress` before `rescanblockchain`. On an existing wallet the option is ignored when the
  seed matches (checked by fingerprint, so a locked wallet starts too) and refused when it does not.
- **`-sendchangeback=1`** returns a PQ input's change to its `sq…` address; the change's dust check
  uses the PQ spend size (`GetPQDustThreshold`), as relay policy does.
- **`z_importpqkey`** takes a dump's whole `pqseed=` line and refuses it when its `owner=`/`addr=`/
  `yecaddr=` do not name the key the seed makes under the line's scheme (the scheme decides the key;
  both schemes take a 48-byte seed). **`vault_release`** refuses a 66-hex pqkeyid as the recipient
  (pass the address or the script).

