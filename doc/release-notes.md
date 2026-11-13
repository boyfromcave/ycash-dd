(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes at release time)

Notable changes
===============

This is the reference node line (ycashd 4.5.0 with the proposed vault upgrade and Ycash Yellowback
(YED)). It is built and tested beside ycash6 (ycashd 6.22.0-beta2, the release line), and gives the
same answers and error strings. No activation height is set on mainnet. One change below is a
consensus rule, enforced at every height: Sapling signatures and coinbase outputs must use canonical
Jubjub encodings, the rule ycash6 and kogany already enforce. The other changes are in the wallet, the
RPCs, the address text and the tests.

Sapling: canonical Jubjub encodings (consensus, ZIP-216)
--------------------------------------------------------

- **Rejected at every height:** a transaction whose Sapling `bindingSig` R or any `spendAuthSig` R is
  one of the two non-canonical Jubjub encodings (u = 0 with the sign bit set), with
  `bad-sapling-bundle-authorization`, and a coinbase Sapling output whose recovered `pk_d` is the
  identity in either encoding, with `bad-cb-output-desc-invalid-ct` (ycash6's reject reasons).
- **Why:** this line links jubjub 0.5.1, which accepted those encodings; ycash6 (jubjub 0.10) and
  kogany reject them at every height. One such transaction could have split the node lines. The rule
  is not tied to an upgrade (the beta2 decision, 2026-11-12), so the lines fail the same way before
  Vault too.
- **No existing block is affected:** mainnet (to height 3,097,335, stale blocks included) and the
  fast testnet contain no such encoding.
- A block carrying one is scored 100; a transaction in the mempool is scored 0, because a node of an
  earlier build relays it in good faith.
- Run this build (or a later one) wherever a node must follow the same chain as ycash6 and kogany.

Post-quantum addresses show their scheme (`rpcversion` 9)
---------------------------------------------------------

- **New prefixes.** A post-quantum address names its signature scheme in its third character:
  `sqf…` (FN-DSA-512) and `sqs…` (SLH-DSA-SHA2-128s) on mainnet, `tqf…`/`tqs…` on testnet,
  `rqf…`/`rqs…` on regtest. The YED form is `ybf…`/`ybs…` (mainnet), `ytf…`/`yts…`, `yrf…`/`yrs…`.
  Still 53 characters. The key, the script it pays and every balance are unchanged.
- **The earlier text form is refused** by `sendtoaddress`, `validateaddress`, `yed_send` and every
  other RPC. Get the new string for a key you hold with `getaddressesbyaccount ""` or
  `listreceivedbyaddress 0 true`. For a key in a wallet dump, `z_importpqkey` with the dump's whole
  `pqseed=` line returns it; it still accepts a dump line whose `addr=`/`yecaddr=` are in the
  earlier form.
- **Wallets rewrite their address book once.** Post-quantum address-book entries written by an
  earlier build are rewritten in the new form at the first start (labels kept; logged once). An
  earlier build cannot read the rewritten entries: it shows those addresses without their labels,
  while keys and coins stay spendable. Back up `wallet.dat` before the first start.
- **`rpcversion` 9.** `yed_getinfo.params.pqAddressVersion` and `.params.pq.plainAddressVersion`
  change from hex strings to objects keyed by scheme number (`{"1": "<hex>", "2": "<hex>"}`), one
  3-byte version each. Clients that build or check post-quantum addresses read them this way or use
  `src/test/data/pq_address_vectors.json`.

Send YEC between every address type (beta2 D-2, D-4, D-6)
---------------------------------------------------------

- **`z_sendmany`** takes an ECC (`s1…`), Sapling (`ys1…`) or post-quantum (either scheme) from
  address, and any mix of ECC, Sapling and post-quantum recipients.
- A post-quantum from address spends only its own coins (largest first, mature only: `minconf`
  applies). Its change returns to the same address. Sapling change stays a Sapling note.
- Its fee is the wallet's rule for a post-quantum input: max(the default fee, `-pqfeerate` × size,
  the ZIP-317 conventional fee). That is about 27,000 zat for one SLH-DSA input and 5,500 zat for one
  FN-DSA input. The default fee is raised to it; an explicit fee below it is refused. Change below
  the post-quantum dust threshold goes to the fee.
- An explicit fee above the post-quantum cap answers `Fee … is above the … cap on
  a post-quantum spend (…)` even when the fee is also above the sum of the outputs (the inherited
  "greater than the sum of outputs" check used to answer first), as ycash6 does.
- At most 100 post-quantum inputs (`MAX_PQ_INPUTS_PER_TX`) fit one transaction.
- The post-quantum inputs are signed with no global lock held (an SLH-DSA signature takes about
  0.3 s). The wallet checks before and after that the inputs are still unspent and the next block's
  rules unchanged.
- `ANY_TADDR` still selects ECC coins only.
- **`z_mergetoaddress`** merges a named post-quantum source (at most 100 inputs per call, fewer if
  the standard sigop limit needs it; the rest are reported as remaining) and pays a post-quantum
  destination. Post-quantum coins are never merged with Sprout notes or into a Sprout address.
- `z_shieldcoinbase` still refuses a post-quantum source.
- This line has no privacy-policy argument.
- The error strings (the same on every line) are in `doc/yellowback-rpc.md`.

`sendtoaddress`, `sendmany`, `sendfrom` and `fundrawtransaction` spend ECC coins only
--------------------------------------------------------------------------------------------------------

The beta2 decision, confirmed by the Yellowback lead architect on 2026-11-12: the stock transparent
sends are the ECC calls, and `z_sendmany` is the road for every coin type.

- `sendtoaddress`, `sendmany` and `sendfrom` never select a post-quantum coin. Before, they did, and
  sent its change to a new ECC key. Spend a post-quantum coin with `z_sendmany` from its address,
  which returns the change to it. Funds spread over several post-quantum addresses: merge them first
  with `z_mergetoaddress`, or send from each in turn.
- `fundrawtransaction` adds ECC coins only. A post-quantum input stays only when the transaction
  already names it; `-sendchangeback=1` applies to such a named input.
- When the ECC coins do not cover the amount and the wallet holds post-quantum coins, the call is
  refused with `Insufficient funds in ordinary transparent addresses; this wallet also holds
  <amount> YEC at post-quantum addresses: send from one with z_sendmany, which returns the change
  to it`. The amount counts only coins `z_sendmany` would spend. A coinbase shortfall is added:
  `; its coinbase funds can only be spent after they have been sent to a zaddr`.
- Not changed by this rule: `z_sendmany`/`z_mergetoaddress` from a post-quantum address,
  `vault_lock "from"` a post-quantum address, the `yed_*` YEC funding (which can still select
  post-quantum coins), and inputs a raw transaction names.

Fund a vault from any address type (beta2 D-3)
-----------------------------------------------

- `vault_lock` takes an optional `"from"`: an ECC, post-quantum or Sapling address of this wallet.
  Change returns to it. Without `"from"` the funding is unchanged.
- `yed_mint`'s funding address is unchanged (ECC or Sapling).

Balances and listings by address type
-------------------------------------

- `z_gettotalbalance` adds `transparent_ecc`, `transparent_pq` and `pq_by_scheme`.
- `getwalletinfo` adds `balance_ecc`, `balance_pq`, `unconfirmed_balance_ecc`,
  `unconfirmed_balance_pq` and `pq_by_scheme`. Existing fields keep their meaning.
- `listreceivedbyaddress` adds `pqscheme` and `pqschemename` to a post-quantum address.
- **`z_getbalance`** takes a post-quantum address of either scheme. It **refuses a transparent
  address the wallet does not hold** (ECC or post-quantum, no key and no watch-only script) with
  `From address does not belong to this node, transparent or post-quantum key or watch-only script
  not found.` (-5, `RPC_INVALID_ADDRESS_OR_KEY`). **This changes inherited v4.5.0 behaviour**, which
  answered `0`. A watch-only address still answers.
- **`z_validateaddress`** answers for a post-quantum address. It said
  `isvalid: false`; it now gives `type` `"pq"`, `ismine`, and `validateaddress`'s `pqscheme`,
  `pqschemename` and `pqkeyhash`.

`getnewaddress` takes the type first
------------------------------------

- `getnewaddress pq`, `getnewaddress pqslh`, `getnewaddress p2pkh`. `getnewaddress "" pq` still
  works.
- Any other first string is refused with `Accounts are unsupported; the first argument is the
  address type, "p2pkh", "pq" or "pqslh" (lower case), or "" followed by the type` (-11, the code it
  had before; the message was `Accounts are unsupported`).
- The help no longer says that `z_sendmany`, `z_shieldcoinbase` and
  `z_mergetoaddress` never pay or spend a post-quantum address. The first help line of
  `getnewaddress`, `signrawtransaction` (`branchid`), `stop` (`detach`, ignored), `yed_getbalance`
  and `yed_listunspent`, `z_listoperationids` (`status`) and `zcbenchmark` names every argument the
  call accepts.

Keeping YED from being spent as plain YEC
-----------------------------------------

Yellowback is **live** where the vault upgrade and a YED attestor set are configured for the network
(regtest: `-yellowbackattestorset`). `-yellowback=0` does not turn a configured Yellowback off; it
only acknowledges a leftover index or a wallet that has held YED.

- Where Yellowback is live, every send that selects coins itself (`z_sendmany`, `z_mergetoaddress`,
  `sendtoaddress`, `sendmany`, `vault_lock "from"`, the vault fee funding) leaves out a coin that
  the Yellowback index knows as a YED token, vault or carrier, also when its coin lock was removed
  with `yed_unlockcoin`. While the index is unhealthy, a send from a post-quantum address is refused.
- **The has-held-YED guard.** A wallet that has held YED, or issued a YED holder key, carries a mark
  in `wallet.dat` (`getwalletinfo` `yed_held`). Where Yellowback is not live, it refuses to load:
  `This wallet has held YED, but Yellowback is not live with these options, so its YED outputs
  cannot be locked. Start with the network's attestor set (regtest -yellowbackattestorset) to keep
  those outputs locked, or with -yellowback=0 to acknowledge that they are spendable as plain YEC.`
  With `-yellowback=0` it loads with a warning (also in `getinfo` `errors`), and `getwalletinfo`
  `yed_unlocked` is true. Do not leave `-yellowback=0` in a configuration file.
- **Outputs the index has not judged yet.** An output of this wallet in a
  transaction the index has not judged (unconfirmed, its block above the index tip, a disconnected
  block, or any transaction while the index is unhealthy) is read by the transaction's own payload
  (a MINT's vault and token, the outputs a TRANSFER or REDEEM assigns, a claim cancel's re-created
  vault and every output that can be its refund token). It is locked and refused with `refusing to
  spend an output that may carry YED as plain YEC before the Yellowback index judges its
  transaction: <txid>:<n>[, …] (its transaction's payload may give it YED, which would burn; wait
  for its block to be judged, or name the outpoint in a raw transaction sent with allowyedburn)`.
  Once the block is judged, the index's records decide: an output it gave YED stays locked, every
  other output is released.
- **Blocks below the start height count as judged.** A transaction confirmed
  below the `UPGRADE_VAULT` activation height, which the index never evaluates, counts as judged with
  no YED.
- **Where Yellowback is not live**, no coin-selecting send spends an output its
  transaction's payload may give YED, whether or not the wallet carries the mark: `sendtoaddress`,
  `sendmany`, `sendfrom` and the coins `fundrawtransaction` adds leave it out and, short of funds,
  say `Insufficient funds; this wallet also holds <n> output(s) that may carry YED, which no send
  spends as plain YEC while Yellowback is not live: <txid>:<n>[, …] (its transaction's payload may
  give it YED, which would burn; to spend it deliberately, name the outpoint in a raw
  transaction)`; `z_sendmany`, `z_mergetoaddress`, the vault fee funding and `vault_lock "from"`
  refuse with `refusing to spend an output that may carry YED as plain YEC while Yellowback is not
  live: <txid>:<n>[, …] (its transaction's payload may give it YED, which would burn; to spend it
  deliberately, name the outpoint in a raw transaction)`. Conservative: a claim cancel's own YEC
  change, or a payment in a transaction carrying a copied payload, stays out of every coin-selecting
  send there; spend it with a raw transaction (or `fundrawtransaction`) that names it.
- **The raw-send check.** `sendrawtransaction` without `allowyedburn` also
  counts an output of this wallet that the index has not judged and whose payload may give it YED;
  when its figure is unknown (a claim cancel's refund) it refuses with `yed-burn-refused: this
  transaction spends output(s) of this wallet whose transaction the Yellowback index has not judged
  yet and whose payload may give them YED (<outpoints>); whatever YED they carry would be destroyed.
  Pass allowyedburn = true to send it anyway.` It also acts while the index is unhealthy (before, it
  then let every raw transaction through).
- `vault_send` and `set_sendact` refuse a coin named in `allowinputs` that is a
  YED token, vault or carrier of this wallet; a deliberate burn goes through
  `sendrawtransaction … allowyedburn`.

Vault funding and coin locks
----------------------------

- **No lock outlives a vault transaction.** The vault RPCs record an accepted transaction in the
  wallet at once, as a send does, and keep no coin lock for it: its inputs count as spent while it
  is in the mempool or the chain, and are spendable again when it leaves the mempool unmined (before,
  `vault_lock "from"` a transparent or post-quantum address kept them locked until a restart). The
  same holds for the default funding (`vault_lock` without `"from"`, `set_*`, `vault_release`, …).
  The YEC inputs the Yellowback wallet locks for a YED transaction are handed back within about a
  second once it is mined or has left the mempool unmined.
- `vault_lock "from"` (new in beta2): `from: not an address of this wallet` and `from: an FN-DSA-512 (scheme 2)
  address cannot be spent before Falcon is active at the next block` answer -5
  (`RPC_INVALID_ADDRESS_OR_KEY`), as `z_sendmany` does.
- The default vault funding skips coinbase that must be shielded and releases the coins it locked
  when the call fails for any reason. A coin locked with `lockunspent` is never released. The
  coinbase refusal names the route: `z_shieldcoinbase, then z_sendmany to a transparent address, or
  vault_lock "from" the shielded address`.
- `yed_mint` checks the whole mint (collateral, fees, carrier) before its carrier is broadcast:
  `insufficient-yec: the mint needs <total> YEC (collateral <c>, fees <f> with its carrier's),
  checked before its carrier is broadcast; nothing was sent: need …, have …`. It used to broadcast
  the carrier and fail at the MINT. `yed_mint` and its carrier lock the Sapling notes they spend
  until they commit or fail.

Defects fixed
-------------

- **The node could stop after an erased send.** Erasing a wallet transaction (the Yellowback orphan
  removal) left its entries in the spend index; a later send of the same coins created an empty
  wallet transaction, and the next re-broadcast stopped ycashd with `Assertion failed: (pwallet)`.
  No funds were at risk. One erase function now clears every index.
- **A refused post-quantum send could be paid twice.** A post-quantum send whose fee was above the
  mempool's cap was written to the wallet, refused by the mempool, and broadcast again at the next
  start, after the user had sent again. The fee cap is now checked before signing and again under
  the node's locks, and a send the mempool refuses is erased from the wallet.
- **An unconfirmed claim-cancel refund could be spent as plain YEC** (the
  wallet's pre-lock did not cover it), and where Yellowback is not live a coin-selecting send could
  burn a cancel's refund token.
- The Windows build of the unit tests and of the `-yellowbacktestfault` crash path (no `SIGKILL` on
  Windows; locals named after `windows.h` macros), kept identical to ycash6 (#175).

Plain-YEC post-quantum addresses: notes carried from beta 1
-----------------------------------------------------------

- **Inputs spending post-quantum coins carry no address** in `getblockdeltas`, `getspentinfo` and
  `getrawtransaction` (verbose `vin[].address`): the insight index keys a spent output by its
  Hash160, and a `TX_PQPKH` key hash is 32 bytes. `getaddressdeltas`, `getaddressutxos`,
  `getaddressbalance` and `getaddresstxids` name them as requested.
- **`-reindex` once** on a node with `-insightexplorer` or `-lightwalletd` whose address index was
  built by a build before 6.22.0-beta1: the index of earlier blocks holds no `TX_PQPKH` outputs
  until it is rebuilt. D-9 needs no reindex: the index is keyed by the key hash, not the text.
- **`createrawtransaction` takes a post-quantum output** without the wallet's payability checks
  (Falcon activation, the post-quantum dust threshold); relay policy still refuses a non-standard or
  dust output.
- **`yed_getnewaddress "pq"` files holder keys in the address book** under their plain-YEC form, so
  the YEC their token outputs carry is not taken for change. Holder keys issued by an earlier build
  are not added retroactively.
- **Post-quantum keys are filed in the address book when first paid** (purpose `receive`), so a
  seed-only restore treats their receipts as receipts, not change. A vault, intent or channel owner
  key stays out. Keys paid before this release are filed once when the wallet loads. Where
  Yellowback is live, `listreceivedbyaddress` and `validateaddress` mark a YED
  holder key with `"yed_holder": true`.
- **Seed-only restore (`-hdseedfile`)** restores the seed's first Sapling address and its
  post-quantum keys up to a gap of 20 unused keys per scheme (`PQ_KEY_LOOKAHEAD`). Recover keys
  beyond the gap by issuing addresses past it (`getnewaddress pq`, `z_getnewaddress`) and running
  `rescanblockchain`. On an existing wallet the option is ignored when the seed matches and refused
  when it does not.
- **`-sendchangeback=1`** returns a post-quantum input's change to its address; the change's dust
  check uses the post-quantum spend size.
- **`z_importpqkey`** takes a dump's whole `pqseed=` line and refuses it when its
  `owner=`/`addr=`/`yecaddr=` do not name the key the seed makes. **`vault_release`** refuses a
  66-hex pqkeyid as the recipient (pass the address or the script).

Known limitations
-----------------

- **A seed-only restore recovers no transparent coins on this line, by design.** Its transparent
  keys are random, not derived from the seed (`-hdseedfile` help: "Transparent keypool keys and
  imported keys are not seed-derived"). Back up `wallet.dat`. ycash6 derives them from the seed.
- **A seed restore does not restore the has-held-YED mark.** A wallet restored on a node where
  Yellowback is not live loads without the guard. Coin-selecting sends leave its YED outputs out,
  but a raw transaction that names one spends it as plain YEC and burns its YED. Restore a wallet
  that has held YED only on a node where Yellowback is live. The mark also does not cover a rescan
  or key import done where Yellowback is not live, nor a wallet last used by a build before the
  mark existed.
- **A raw spend in the first moment after a YED payment arrives** is not counted by the raw-send
  check: the wallet records the incoming transaction a moment (normally
  under a second) after the mempool accepts it.
- **The mint pre-check's figure is for the current coins**: topping the address
  up with a new coin adds one input and its fee, so funding exactly the named `need` can be refused
  once more; add a margin.
- **The `yed_*` YEC funding can still spend post-quantum coins**; the ECC-only
  rule covers the stock sends.
- **No YED attestor set on any public network**: Yellowback is live only on regtest with
  `-yellowbackattestorset`.
- **One known consensus difference, regtest and devnet only:** a pre-Canopy shielded coinbase output
  with lead byte 0x01 and an ephemeral key inconsistent with its `esk` is rejected here and accepted by
  ycash6 and kogany (recorded in #180; it arises only on regtest and devnet chains).
- **Wallet encryption is experimental** (`-experimentalfeatures -developerencryptwallet`).
