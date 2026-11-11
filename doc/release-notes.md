(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes at release time)

Notable changes
===============

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
- **`z_getbalance` refuses a transparent address the wallet does not hold** (ECC or post-quantum: no key and no
  watch-only script) with `From address does not belong to this node, transparent or post-quantum key or watch-only
  script not found.` (`RPC_INVALID_ADDRESS_OR_KEY`). **This changes inherited v4.5.0 behaviour**: it answered `0`
  for such an address. A watch-only address still answers; a foreign Sapling address was already refused.
- **Post-quantum keys are filed in the address book when first paid**: a key of the wallet that receives a
  `TX_PQPKH` output without an address-book entry is filed (purpose `receive`), so a seed-only restore treats its
  receipts as receipts, not change; a fresh YED holder key is filed when drawn. With `-yellowback`,
  `listreceivedbyaddress` and `validateaddress` mark a YED holder key with `"yed_holder": true`.
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

