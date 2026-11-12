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
- **Nothing prints the earlier form.** A wallet or data directory carried over from an earlier build shows its
  post-quantum addresses only in the new form, in every RPC result and in the address book; the keys, coins,
  balances and the address index are untouched (they are keyed by the key hash, not by the text). Going back to an
  earlier build after the first start is not supported for the address book: that build cannot read the rewritten
  entries, so it shows the post-quantum addresses (in the earlier form) without their labels, while keys and coins
  stay spendable. Keep a backup of `wallet.dat` from before the upgrade if you may need to go back.
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
- **`z_sendmany` is the any-to-any call for plain YEC (beta2 D-2; quantum spec A-15 reversed).** From an
  ECC (`s1…`), Sapling (`ys1…`) or post-quantum (`sq…`, either scheme) address to any mix of ECC, Sapling
  and PQ recipients. A PQ from address spends that address's own coins (largest first, mature only:
  `minconf` applies); its inputs are signed after the Sapling proofs are made, with no wallet lock held
  while proving, under the vault script flags of the next block; its change returns to the same `sq…`
  address (D-4; Sapling change stays a Sapling note, ECC change is unchanged). Its fee follows the rule
  `sendtoaddress` uses for a PQ input: max(the default fee, `-pqfeerate` × size, the ZIP-317 conventional
  fee), about 27,000 zat for one SLH-DSA input and 5,500 for one FN-DSA input; the default fee is raised to
  it, an explicit fee below it is refused, change below the PQ dust threshold goes to the fee, and at most
  100 PQ inputs (`MAX_PQ_INPUTS_PER_TX`) fit one transaction, within the standard sigop limit. The PQ inputs are
  signed with no global lock held (an SLH-DSA signature takes ~0.3 s); the wallet re-checks before and after that
  the inputs are still unspent and the next block's rules unchanged. A PQ recipient amount below the PQ dust
  threshold (priced on the spend size of its scheme) is refused. `ANY_TADDR` still selects ECC coins only.
- **`z_mergetoaddress` follows (D-6):** a named `sq…` source merges its coins (at most 100 per call, and
  only as many inputs as keep the policy sigops, P2SH redeem scripts included, within the standard limit; the
  rest are reported as remaining), an `sq…` destination receives the merge, with the same fee rule. PQ coins
  are never merged with Sprout notes or into a Sprout address. `z_shieldcoinbase` still refuses a PQ source (D-7).
- The error strings of these paths (the same on every line):
  `post-quantum addresses can be spent from only once the vault upgrade is active at the next block`;
  `an FN-DSA-512 (scheme 2) post-quantum address can be spent from only once Falcon is active at the next block`;
  `From address does not belong to this node, post-quantum key not found.`;
  `Error: Please enter the wallet passphrase with walletpassphrase first.` (a locked wallet);
  `Invalid parameter, amount <x> is below the dust threshold <y> of a post-quantum output`;
  `Fee <x> is below the <y> this post-quantum spend needs`;
  `This send needs <n> post-quantum inputs, more than the <m> one transaction may carry; combine the address's coins first with z_mergetoaddress`;
  `This transaction needs <n> signature operations, more than the <m> a standard transaction allows; spend fewer inputs`;
  `A post-quantum from address cannot pay a Sprout address`;
  `Post-quantum coins cannot be merged with Sprout notes or into a Sprout address`;
  `The next block's rules changed while this transaction was built (a reorg or a network upgrade); send it again`;
  `A post-quantum input was spent while this transaction was built; send it again`. A PQ recipient before the vault upgrade or
  Falcon keeps its `… can be paid only once …` strings.
- **`z_getbalance` refuses a transparent address the wallet does not hold** (ECC or post-quantum: no key and no
  watch-only script) with `From address does not belong to this node, transparent or post-quantum key or watch-only
  script not found.` (`RPC_INVALID_ADDRESS_OR_KEY`). **This changes inherited v4.5.0 behaviour**: it answered `0`
  for such an address. A watch-only address still answers; a foreign Sapling address was already refused.
- **Post-quantum keys are filed in the address book when first paid**: a key of the wallet that receives a
  `TX_PQPKH` output without an address-book entry is filed (purpose `receive`), so a seed-only restore treats its
  receipts as receipts, not change (a vault, intent or channel owner key stays out, even when paid a `TX_PQPKH`
  output); keys paid before this release are filed once when the wallet loads; a fresh YED holder key is filed when
  drawn. Where Yellowback is live (the network has a vault height and a YED attestor set),
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

