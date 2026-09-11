# Yellowback RPC contract (`yed_*`), rpcversion 2

This file is the interface between the node (`ycash-dd`) and the wallet application
(`yecwallet-dd`). Nothing else crosses that boundary. It is written *before* the code it
describes (plan §4.5, M7, N27, P7): the node context is Phase 3's gate, the wallet context is
Phase 6's, and `qa/rpc-tests/yellowback_rpc_contract.py` calls every command and asserts every
documented key with its JSON type. `scripts/extract-spec.sh` (`make spec`) turns the fenced
`json` block under each command heading into `doc/yellowback-rpc-contract.json`, copied into both
forks; the wallet's `yellowbackrpc.h` is checked against that copy.

**How to read the `json` blocks.** Each is an *example result*: the field names and nesting are
the contract, the values show the JSON type (`number`, `string`, `boolean`, `null`, object,
array). A list result is written as a one-element array holding one example row. A field whose
example value is `null` is one the text marks *null when …* — the checker accepts `null` or the
documented type for it. A field the text marks **optional** may be absent, and the checker asserts
it only in the state the text names. Nothing else may be absent.

**`rpcversion` rule.** `yed_getinfo.rpcversion` is `2`. Additions (new commands, new fields)
never bump it; a removal or a shape change does. Phase 8's additions (`yed_estimatesend`,
`yed_unlockcoin`, `yed_getinfo.lockedOutputs`/`protectedByIndex`, H3/H5/H10) therefore land under
`rpcversion = 2` and are listed at the end of this file without a shape until then. The wallet's
`YellowbackRpc::RPC_VERSION` goes to `2` in Phase 7b-a's first commit, not before: until Phase 3 the
node reports `1` and the wallet refuses a mismatch (N27).

## Conventions

- **Units.** YED in integer **cents** (`100` = $1.00); YEC in **zatoshi** fields (`…Zat`) with a
  decimal YEC twin (`collateral`) where the prototype had one; prices in **micro-USD per YEC**
  (`1000000` = $1.00); ratios and shares in **basis points** (`10000` = 100 %). Heights are block
  heights. The protocol never speaks wall-clock time; the only clock fields are the miner's own
  (`quoteAgeSeconds`, `receivedAt`) and they are read by no rule.
- **Undefined prices are `null`.** `pFast`, `pMid`, `pSlow`, `pMint`, `pClaim`, `globalRatioBps`
  and `supplyCapCents` are `null` when §3.7 leaves them undefined (no fill, no supply, no cap);
  stored as `0` in the index (§3.6 M1), rendered as `null` here.
- **Outpoints** are objects `{"txid": "<hex>", "vout": n}`; a vault is named by its outpoint
  string `"<txid>:0"` where the prototype did (`vault`).
- **Term classes** are the strings `"A"`, `"B"`, `"C"`; vault statuses `"ACTIVE"`, `"VOID"`,
  `"CLOSED"`, `"CLAIMED"`; activation statuses `"signaling"`, `"locked_in"`, `"active"`; halt-mask
  names `"NOT_ACTIVE"`, `"NO_PRICE"`, `"PARTICIPATION"`, `"GLOBAL_RATIO"`, `"DIVERGENCE"`,
  `"ENFORCEMENT"` (§3.6).
- **Payees** are rendered as the P2PKH address (`s1…` mainnet, `sm…` testnet/regtest) of the key
  hash, `null` when there is none (FEE-0, or a VOID release / sweep).
- **Verdicts** are the §4.2a strings (`ok`, `bad-mint-collateral`, `vault-spend-malformed`, …).
  `type` is `"mint"`, `"transfer"`, `"redeem"` or `"none"`; `path` is `"owner"`, `"claim"` or `""`.
- **Gating.** Every command requires `-experimentalfeatures -yellowback`; without them the node
  answers JSON-RPC `-32601` "Method not found". Every refusal is `RPC_INVALID_PARAMETER` (a bad
  argument), `RPC_WALLET_ERROR` (funds, locking) or `RPC_VERIFY_REJECTED` (a rule), with a
  message that **begins with a stable identifier** from the table in *Error identifiers*; the
  wallet matches the identifier, never the text after it.
- **While the index is unhealthy** (`yed_getinfo.healthy == false`) every command refuses with
  `yellowback-unhealthy: <unhealthyReason>` **except** `yed_getinfo`, `yed_getblockverdict`,
  `yed_gettag`, `yed_decodepayload` and `yed_setquote` — the diagnostic and operator commands the
  runbook (`doc/yellowback-mining.md`) needs (M8). Recovery is `-reindex-yellowback`.

## Address format

A Yellowback address is Base58Check(version ‖ 20-byte key hash) with version bytes `0x1F 0xE4`
(mainnet, `ye…`), `0x20 0x07` (testnet, `yt…`), `0x20 0x02` (regtest, `yr…`); it decodes to an
ordinary P2PKH destination (`src/yellowback/address.cpp`, no `chainparams.cpp` edit). YED is
carried by `TOKEN_VALUE` (10,000 zat) P2PKH outputs assigned cents by a payload; the `ye…` prefix
is the only technical guard against sending YED to software that does not run the overlay (H9).
`yed_send`/`yed_sendmany`/`yed_validateaddress` refuse anything else with
`not-a-yellowback-address`. Sapling `ys1…` addresses are accepted as the *funding* of `yed_mint`
and the *destination* of `yed_redeem`/`yed_claim`/`yed_sweep` (§4.6), never as YED recipients.

---

## Node context (`src/rpc/yellowback.cpp`; no wallet needed)

### `yed_getinfo`

Arguments: none. Never refuses while unhealthy. `height`/`blockhash` are always the tip the index
holds (V2; `height` is `-1` and `blockhash` `""` while the index is empty). `enforcing` is `false`
under the kill switch (`-yellowbackenforce=0`), the valve (`valveTripped`, ACT-7, L7), the sunset
(`sunset`, ACT-5, L8) or an unhealthy index. `rejectedBlocks` counts blocks this node refused
(`Rejected`); `suppressedBlocks` counts rule-breaking blocks accepted by BLK-2 clause 3 because the
network had already built `VALVE_BLOCKS` on them (L11) — an information line, not an alarm.
`abandoned` is the abandonment predicate below (L10). `templatePolicy` is `"strict"` or
`"consensus"`. `miner.quoteKind` is what the next template's tag would be (`"quote"`, `"signal"`,
`"none"`); `miner.quoteAgeSeconds` is `null` when no quote is held; `miner.payoutAddress` is `null`
when the node has no payout key (then `quoteKind` is `"none"`). `params` reports every value the
Mint page derives from; on regtest `startHeight`, `sigmaRefBps`, `supplyCapBps` and
`enforceUntilHeight` are the four hashed values (§3.1, M13; `enforceUntilHeight` `0` = none).
`params.feeZat` is the network fee `YELLOWBACK_FEE`, distinct from the enforcement fee
(`feeMinZat`/`feeBps`). `params.policy.preferredPayee` is `null` unless `-yellowbackpreferredpayee`
is set. `lockedOutputs` (H10) is how many outpoints the Yellowback wallet layer holds locked
(stage (i)–(iii) of §4.6; `0` when the node runs without a wallet) and `protectedByIndex` is
`true` whenever that layer is attached: the GUI treats a mismatch between `lockedOutputs` and the
length of `yed_listunspent` as the trigger for `yed_lockcoins`.

Result of `yed_getinfo`:

```json
{
  "rpcversion": 2,
  "enabled": true,
  "network": "regtest",
  "height": 331,
  "blockhash": "0f3a9c1e5b7d2a4c6e8f0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f",
  "chainHeight": 331,
  "startHeight": 1,
  "healthy": true,
  "unhealthyReason": "",
  "enforcing": true,
  "valveTripped": false,
  "sunset": false,
  "rejectedBlocks": 0,
  "suppressedBlocks": 0,
  "templatePolicy": "strict",
  "abandoned": false,
  "lockedOutputs": 2,
  "protectedByIndex": true,
  "activation": {
    "status": "active",
    "lockInHeight": 129,
    "activateHeight": 193,
    "signalCount": 64,
    "window": 64
  },
  "miner": {
    "payoutAddress": "smQvTmAz2ExamplePayoutAddress1111111",
    "signal": true,
    "quoteKind": "quote",
    "quoteAgeSeconds": 12,
    "registered": true,
    "eligible": true
  },
  "params": {
    "startHeight": 1,
    "enforceUntilHeight": 0,
    "sigmaRefBps": 0,
    "supplyCapBps": 0,
    "refLag": 2,
    "refWindow": 40,
    "grace": 24,
    "payeeWindow": 10,
    "feeMinZat": 50000000,
    "feeBps": 25,
    "tokenValueZat": 10000,
    "feeZat": 1000,
    "valveBlocks": 6,
    "abandonBlocks": 128,
    "windows": { "fast": 8, "mid": 24, "slow": 64, "signal": 64 },
    "minFill": { "fast": 4, "mid": 16, "slow": 43 },
    "classes": [
      { "class": "A", "minBlocks": 48, "maxBlocks": 96, "baseRatioBps": 50000 }
    ],
    "policy": {
      "penaltyBlocks": 12,
      "accuracyWindow": 24,
      "tiltBps": 10000,
      "preferredPayee": null
    }
  }
}
```

`classes` always has three rows (A, B, C) in class order; the example shows one. Every value in
`params` other than the four hashed regtest values is compiled into the network's parameter set
(§3.1) — a wallet may display them but must not treat them as configurable.

### `yed_getstatehash [height]`

Arguments: `height` (number, optional) — must equal the index height when given (the index holds
one state). Unchanged from v1: `statehash` is SHA-256 over the §3.6 preimage (`Tip`, `Tags`,
`Judgements`, `Activation`, `Vaults`, `Tokens`, `Totals`, `Snapshots`, `Params`; `TxLog`,
`Rejected` and `Undo` excluded, N18). Two nodes on the same chain with the same parameters
return the same hash; `yellowback_model.py` recomputes it from `yed_gethistory`.

Result of `yed_getstatehash`:

```json
{
  "height": 331,
  "blockhash": "0f3a9c1e5b7d2a4c6e8f0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f",
  "statehash": "9b4e2f1c8d7a6b5c4d3e2f1a0b9c8d7e6f5a4b3c2d1e0f9a8b7c6d5e4f3a2b1c"
}
```

### `yed_getstats`

Arguments: none. `Totals` plus the tip snapshot. `pClaim` is `null` when undefined (fewer than
the minimum fill on the mid or slow window); `globalRatioBps` is `null` when there is no supply or
no `pMint`; `supplyCapCents` is `null` when there is no cap (`supplyCapBps == 0` or no price).
`haltMask` is the decoded tip `haltMask` as an array of names (empty when minting is open);
`mintingAllowed` is `activation == active && haltMask == [] && cap has room` — the MINTPOL-1
gate `yed_mint` applies.

Result of `yed_getstats`:

```json
{
  "height": 331,
  "supplyCents": 250000,
  "collateralZat": 62500000000,
  "activeVaults": 3,
  "voidVaults": 1,
  "closedVaults": 2,
  "claimedVaults": 0,
  "unbackedCents": 0,
  "issuedZat": 1656250000000,
  "pFast": 2000000,
  "pMid": 2000000,
  "pSlow": 1990000,
  "pMint": 1990000,
  "pClaim": 2000000,
  "sigmaMultBps": 10000,
  "globalRatioBps": 49750,
  "supplyCapCents": null,
  "haltMask": [],
  "mintingAllowed": true
}
```

### `yed_getprice [height]`

Arguments: `height` (number, optional; default the index tip; must be `≥ startHeight` and
`≤ tip`). The snapshot prices at that height with the window fills that produced them and the
tag of that block (`tag` has exactly the shape of `yed_gettag`). Prices `null` when undefined.

Result of `yed_getprice`:

```json
{
  "height": 331,
  "pFast": 2000000,
  "pMid": 2000000,
  "pSlow": 1990000,
  "pMint": 1990000,
  "pClaim": 2000000,
  "fill": {
    "fast": { "quoteTags": 8, "window": 8, "minFill": 4 },
    "mid": { "quoteTags": 24, "window": 24, "minFill": 16 },
    "slow": { "quoteTags": 60, "window": 64, "minFill": 43 }
  },
  "tag": {
    "found": true,
    "kind": "quote",
    "version": 1,
    "signal": true,
    "priceMicroUsd": 2000000,
    "sourceMask": 3,
    "payoutAddress": "smQvTmAz2ExamplePayoutAddress1111111"
  }
}
```

### `yed_getactivation`

Arguments: none. The ACT-1..7 state at the tip. `window`, `threshold`, `participationFloor`,
`enforcementFloor`, `enforcementResume` are the network constants (`SIGNAL_WINDOW`,
`ACTIVATION_THRESHOLD`, `PARTICIPATION_FLOOR`, `ENFORCEMENT_FLOOR`, `ENFORCEMENT_RESUME`);
`signalCount` is the tip's; `lockInHeight`/`activateHeight` are `0` until reached. `mintHalted`
is the tip's `PARTICIPATION` bit (ACT-4), `enforcementSuspended` its `ENFORCEMENT` bit (ACT-6).
`enforcing`, `valveTripped`, `sunset` are as `yed_getinfo`; `enforceUntilHeight` is
`ENFORCE_UNTIL_HEIGHT` (`0` = none). `history` samples `signalCount` every `SIGNAL_WINDOW / 8`
blocks ending at the tip (8 rows, oldest first; fewer when the chain is shorter than a window).

Result of `yed_getactivation`:

```json
{
  "status": "active",
  "lockInHeight": 129,
  "activateHeight": 193,
  "signalCount": 64,
  "window": 64,
  "threshold": 48,
  "participationFloor": 39,
  "enforcementFloor": 32,
  "enforcementResume": 39,
  "mintHalted": false,
  "enforcementSuspended": false,
  "enforcing": true,
  "valveTripped": false,
  "sunset": false,
  "enforceUntilHeight": 0,
  "history": [
    { "height": 331, "signalCount": 64 }
  ]
}
```

### `yed_listminers [height] [window]`

Arguments: `height` (number, optional; default the tip), `window` (number, optional; default
`PAYEE_WINDOW`; the launch bar uses `2016`, L4). One row per `payoutKey` that carries a quote tag
in `(height − window, height]`, in descending `lastTagHeight` order. `share` is the row's share
of the quote-tagged blocks in the window in bps; `registered` is REG-1 at `height`, `eligible`
membership in `E(height)` (FEE-2); `penalizedUntil` is the height the REG-2 penalty ends, `0`
when not penalised; `accuracyBps` is REG-3 over the node's accuracy window, `null` when no tag of
this key has been judged in it; `quoted` and `inBand` are the judged quote tags of this key in
that window and how many of them REG-4 found within `ACCURACY_BAND_BPS`.

Result of `yed_listminers`:

```json
[
  {
    "payoutAddress": "smQvTmAz2ExamplePayoutAddress1111111",
    "lastTagHeight": 331,
    "lastQuote": 2000000,
    "quoteTags": 4,
    "share": 4000,
    "registered": true,
    "eligible": true,
    "penalizedUntil": 0,
    "accuracyBps": 9800,
    "quoted": 8,
    "inBand": 8
  }
]
```

### `yed_gettag <height|blockhash>`

Arguments: one **string** (no `client.cpp` conversion): all digits ⇒ a height on the active
chain, otherwise a block hash of a stored block. Decodes the coinbase tag (TAG-1..5) of that
block. When `found` is `false` (no tag, or a height below `startHeight`) `kind` is `"none"` and
the other fields are absent (**optional**). `kind` is `"quote"` or `"signal"`; a signal-only tag
has `priceMicroUsd: 0`. Allowed while unhealthy.

Result of `yed_gettag`:

```json
{
  "found": true,
  "kind": "quote",
  "version": 1,
  "signal": true,
  "priceMicroUsd": 2000000,
  "sourceMask": 3,
  "payoutAddress": "smQvTmAz2ExamplePayoutAddress1111111"
}
```

### `yed_setquote <priceMicroUsd> <sourceMask>`

Arguments: `priceMicroUsd` (number; `0` clears the quote so the next tag is signal-only or
none), `sourceMask` (number, 16-bit; the §5 source-bit registry). Miner control (MINER-1): stores
`{quote, sourceMask, receivedAt = GetTime()}`; requires RPC auth like every command. Allowed
while unhealthy (the tag is not emitted then, MINER-3, but the quote is kept). `nextTag` is what
the next template would carry given the stored quote, `-yellowbackquotemaxage`,
`-yellowbacksignal`, `-yellowbackenforce`, the valve and the sunset (`kind` `"quote"`,
`"signal"` or `"none"`; `payoutAddress` `null` when there is no payout key — but then the command
has already refused with `no-payout-address`). `receivedAt` is Unix seconds on the node's clock.
The quote agent (`contrib/yellowback/yellowback-quote`) calls this every `poll_seconds` and
`yed_setquote 0 0` after `fail_polls` failed aggregates (L5).

Result of `yed_setquote`:

```json
{
  "priceMicroUsd": 2000000,
  "sourceMask": 3,
  "receivedAt": 1789000000,
  "nextTag": {
    "kind": "quote",
    "signal": true,
    "payoutAddress": "smQvTmAz2ExamplePayoutAddress1111111"
  }
}
```

### `yed_getfeepayee <refHeight> <collateralZat> [selectorHex]`

Arguments: `refHeight` (number), `collateralZat` (number), `selectorHex` (string, optional; the
33-byte owner key of a MINT or the 36-byte serialised vault outpoint of a REDEEM). For external
transaction builders: `eligible` is `E(refHeight)` (FEE-2, height order, deduplicated), `feeZat`
is FEE-1 for that collateral, `default` is the FEE-W choice for the selector with its weight
(`10⁴ + tiltBps · accuracyBps / 10⁴`; when no selector is given the choice for an all-zero
selector), `preferred` (**optional**) is present only when `-yellowbackpreferredpayee` is
configured and in `E(refHeight)` — it then replaces `default` for the wallet — and `policy` is
the L6 values the node used. Refuses with `fee-no-eligible-payee` when `E(refHeight)` is empty
(FEE-0; the wallet then omits the fee output and never calls this).

Result of `yed_getfeepayee`:

```json
{
  "eligible": [
    "smQvTmAz2ExamplePayoutAddress1111111"
  ],
  "feeZat": 50000000,
  "default": {
    "payoutAddress": "smQvTmAz2ExamplePayoutAddress1111111",
    "weight": 19800
  },
  "preferred": "smQvTmAz2ExamplePayoutAddress1111111",
  "policy": { "penaltyBlocks": 12, "accuracyWindow": 24, "tiltBps": 10000 }
}
```

### `yed_getvault <txid>`

Arguments: `txid` (string; vault outpoints are always `vout 0`). The `Vaults` record plus
derived fields. `collateral` is the decimal-YEC twin of `collateralZat`. `voidReason` is the
verdict of the MINT rule that failed, `""` for a vault that was ever ACTIVE. `closeHeight`,
`closingTxid`, `burnedCents` are `null`/`""`/`0` until the vault is CLOSED or CLAIMED
(`closingTxid` `""`, `closeHeight` `null`). `claimable` is true for an ACTIVE vault at or past
`claimHeight` that is underwater at the tip snapshot (RED-4 would pass); `underwaterAt` is the
`pClaim` (µUSD) below which `collateralZat · pClaim < mintedCents · CLAIM_THRESHOLD_BPS`, i.e. the
price at which the vault becomes claimable (`null` for a VOID vault, which has no debt). `unbacked`
is true for a vault closed without its burn (a sweep, IN-2). **`sweepBefore`** (**optional**,
= `claimHeight`) is present on every VOID vault (its claim path is anyone-can-spend after
`claimHeight`, K3) and on every ACTIVE vault while abandonment holds (L10); absent otherwise.
The prototype's `tier`, `rosterIndex`, `errBpsAtClose`, `requiredBurnCents`, `indexHeight` are
gone. Refuses with `vault-not-found`.

Result of `yed_getvault`:

```json
{
  "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "vout": 0,
  "status": "ACTIVE",
  "ownerPubKey": "02a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9",
  "ownerKeyId": "1f2e3d4c5b6a79880706050403020100f1e2d3c4",
  "ownerAddress": "yrExampleOwnerAddress111111111111111",
  "termClass": "A",
  "lockHeight": 380,
  "claimHeight": 404,
  "collateralZat": 25125628141,
  "collateral": 251.25628141,
  "mintedCents": 100000,
  "mintHeight": 332,
  "refHeight": 329,
  "feePaidZat": 62814071,
  "closeHeight": null,
  "closingTxid": "",
  "burnedCents": 0,
  "unbacked": false,
  "claimable": false,
  "underwaterAt": 437800,
  "voidReason": "",
  "sweepBefore": 404
}
```

### `yed_listvaults [status] [count] [skip]`

Arguments: `status` (string, optional; `""` or absent = every status, else one of `ACTIVE`,
`VOID`, `CLOSED`, `CLAIMED`), `count` (number, default `100`), `skip` (number, default `0`).
Paged, in ascending outpoint order; the result is a **list** of `yed_getvault` rows (an empty
list past the end). The prototype's `rosterIndex` argument and its `{height, total, vaults}`
envelope are gone (the `client.cpp` row is now `1,2`).

Result of `yed_listvaults`:

```json
[
  {
    "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
    "vout": 0,
    "status": "ACTIVE",
    "ownerPubKey": "02a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9",
    "ownerKeyId": "1f2e3d4c5b6a79880706050403020100f1e2d3c4",
    "ownerAddress": "yrExampleOwnerAddress111111111111111",
    "termClass": "A",
    "lockHeight": 380,
    "claimHeight": 404,
    "collateralZat": 25125628141,
    "collateral": 251.25628141,
    "mintedCents": 100000,
    "mintHeight": 332,
    "refHeight": 329,
    "feePaidZat": 62814071,
    "closeHeight": null,
    "closingTxid": "",
    "burnedCents": 0,
    "unbacked": false,
    "claimable": false,
    "underwaterAt": 437800,
    "voidReason": "",
    "sweepBefore": 404
  }
]
```

### `yed_listclaimable`

Arguments: none. ACTIVE vaults past `claimHeight` that are underwater at the tip snapshot — the
source of the wallet's Claim page. `mintedCents` is the burn a claim must carry (RED-2), `feeZat`
the FEE-1 fee it pays from the collateral, `pClaim` the tip's claim price (never `null` here: an
undefined `pClaim` makes RED-4 false, so nothing is claimable). Empty list when nothing is.

Result of `yed_listclaimable`:

```json
[
  {
    "vault": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8:0",
    "ownerAddress": "yrExampleOwnerAddress111111111111111",
    "collateralZat": 25125628141,
    "mintedCents": 100000,
    "feeZat": 62814071,
    "claimHeight": 404,
    "underwaterAt": 437800,
    "pClaim": 400000
  }
]
```

### `yed_gettxinfo <txid>`

Arguments: `txid` (string). The `TxLog` record of a confirmed transaction that created or spent
`Tokens`/`Vaults` (N7; `-txindex` is not required). `assigned` lists the cents the payload
assigned per output, `spentTokens` the `Tokens` outpoints consumed (IN-1), `closedVaults` the
vaults closed (IN-2), `payee` the fee output's address or `null`. `expired` is true — with
`height: -1` and `verdict: "expired"` — for a wallet transaction past its `nExpiryHeight` that is
in neither `TxLog` nor the mempool (§4.6; only meaningful on a node with a wallet). A txid the index
does not know and the wallet does not hold is refused with `tx-not-found`.

Result of `yed_gettxinfo`:

```json
{
  "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "height": 332,
  "type": "mint",
  "path": "",
  "verdict": "ok",
  "yedIn": 0,
  "yedOut": 100000,
  "burned": 0,
  "feeZat": 62814071,
  "payee": "smQvTmAz2ExamplePayoutAddress1111111",
  "assigned": [
    { "vout": 1, "cents": 100000 }
  ],
  "spentTokens": [
    { "txid": "3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8091a2b", "vout": 1 }
  ],
  "closedVaults": [
    { "txid": "3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8091a2b", "vout": 0 }
  ],
  "expired": false
}
```

### `yed_decodepayload <hex>`

Arguments: `hex` (string): a payload, an `OP_RETURN` script or a raw transaction. Decodes the
version-2 payload (§3.3) without touching state; allowed while unhealthy. `valid` is false — with
`type: "none"` and `reason` naming the defect — for anything §3.3 calls malformed or unknown.
Type-specific fields: MINT `termClass`, `cents`, `lockHeight`, `refHeight`, `ownerPubKey`,
`feeVout` (`255` = none); TRANSFER `assignments: [{vout, cents}]`, `assignedCents`; REDEEM
`refHeight`, `feeVout`, `assignments`, `assignedCents`. Fields of the other types are absent
(**optional** — the checker decodes a MINT payload, the example).

Result of `yed_decodepayload`:

```json
{
  "valid": true,
  "version": 2,
  "type": "mint",
  "reason": "",
  "termClass": "A",
  "cents": 100000,
  "lockHeight": 380,
  "refHeight": 329,
  "ownerPubKey": "02a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9",
  "feeVout": 3
}
```

### `yed_validaterawtransaction <hex>`

Arguments: `hex` (string; a complete, normally signed transaction). Dry run of §3.8 at the tip
(`EvaluateBlock` over a one-transaction pseudo-block at `tip + 1`, exactly `MempoolCheck`'s
predicate) plus `VerifyAllInputs` for the scripts (`valid`). For a vault spend `blockValid` is
RED-1..4 (BLK-1's condition, independent of activation) and `wouldBeRejected` says whether an
enforcing miner refuses it — `!blockValid`, or the MP-1 expiry bound (`mempoolExpiryOk` false:
`nExpiryHeight == 0` or `> refHeight + REF_WINDOW`) — *unless* abandonment holds (L13), when it
is false. `unconfirmedInputs` lists inputs the index does not know (unconfirmed parents; the
verdict cannot see them, M13). Never commits anything.

Result of `yed_validaterawtransaction`:

```json
{
  "valid": true,
  "verdict": "ok",
  "type": "redeem",
  "path": "owner",
  "yedIn": 100000,
  "yedOut": 0,
  "burned": 100000,
  "feeZat": 62814071,
  "payee": "smQvTmAz2ExamplePayoutAddress1111111",
  "blockValid": true,
  "wouldBeRejected": false,
  "mempoolExpiryOk": true,
  "unconfirmedInputs": [
    { "txid": "3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8091a2b", "vout": 1 }
  ]
}
```

### `yed_getblockverdict <blockhash>`

Arguments: `blockhash` (string; a block the node has on disk). Runs `EvaluateBlock` on the
stored block and explains a rejection: `blockInvalid` is BLK-1's condition, `enforcementOn` ACT-5
at that height (from `Snapshots[H − 1]`, incl. the sunset), `reason` is `"<verdict>:<txid>"` of
the first failing vault spend or `""`. `transactions` lists every Yellowback-relevant
transaction in block order with its verdict; `closedVaults` as `yed_gettxinfo`. Allowed while
unhealthy. **Precondition (N12):** refuses with `verdict-parent-not-tip` unless the block's
parent is the index tip (a candidate child, or the block the node just rejected) or the block is
in `Rejected` with the current tip as its parent — the state at any other parent would need an
undo replay.

Result of `yed_getblockverdict`:

```json
{
  "blockInvalid": true,
  "reason": "vault-spend-short-burn:6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "enforcementOn": true,
  "transactions": [
    {
      "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
      "type": "redeem",
      "path": "owner",
      "verdict": "vault-spend-short-burn",
      "yedIn": 50000,
      "yedOut": 0,
      "feeZat": 62814071,
      "payee": "smQvTmAz2ExamplePayoutAddress1111111",
      "closedVaults": [
        { "txid": "3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8091a2b", "vout": 0 }
      ]
    }
  ]
}
```

### `yed_estimatecollateral <cents> <lockBlocks> [priceMicroUsd]`

Arguments: `cents` (number), `lockBlocks` (number; the class follows from it, V19),
`priceMicroUsd` (number, optional; replaces the snapshot's `pMint`). At the current reference
snapshot `R = tip − REF_LAG`: `termClass`, `baseRatioBps` for the class, `sigmaMultBps`
(SIGMA-1), `minRatioBps = MinRatioBps(base, sigma)`, `pMint` (`null` when undefined and no price
was given — then `requiredZat` is `null` too), `requiredZat` rounded up to 1,000 zat,
`lockHeight = R + lockBlocks`, `claimHeight = lockHeight + GRACE`, `refHeight = R`. Refuses with
`mint-bad-lock` (`lockBlocks` outside every class, or `lockHeight + GRACE ≥ LOCKTIME_THRESHOLD`)
and `mint-unsatisfiable` (`requiredZat > MAX_MONEY`, K14). It does not apply MINTPOL-1: a halted
gate still estimates, so the Mint page can show the figure next to the halt reason.

Result of `yed_estimatecollateral`:

```json
{
  "requiredZat": 25125628141,
  "termClass": "A",
  "lockHeight": 380,
  "claimHeight": 404,
  "minRatioBps": 50000,
  "baseRatioBps": 50000,
  "sigmaMultBps": 10000,
  "pMint": 1990000,
  "refHeight": 329
}
```

### `yed_estimatefee <collateralZat>`

Arguments: `collateralZat` (number). FEE-1: `max(FEE_MIN, collateralZat · FEE_BPS / 10⁴)`.

Result of `yed_estimatefee`:

```json
{
  "feeZat": 62814071
}
```

### `yed_gethistory <from> <to>`

Arguments: `from`, `to` (numbers; `startHeight ≤ from ≤ to ≤ tip`, at most 2,016 rows per call,
else `RPC_INVALID_PARAMETER`). The `Snapshots` records (§3.6) for `from ≤ h ≤ to`, ascending,
`haltMask` decoded to names, `activation` as an object, undefined prices and ratio `null`.
`tagged` is whether the block carried a valid tag, `quote` whether that tag carried a quote
(`priceMicroUsd ≠ 0`). `yellowback_model.py` compares every field of every row.

Result of `yed_gethistory`:

```json
[
  {
    "height": 331,
    "blockHash": "0f3a9c1e5b7d2a4c6e8f0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f",
    "tagged": true,
    "quote": true,
    "signalCount": 64,
    "activation": { "status": "active", "lockInHeight": 129, "activateHeight": 193 },
    "pFast": 2000000,
    "pMid": 2000000,
    "pSlow": 1990000,
    "pMint": 1990000,
    "pClaim": 2000000,
    "sigmaMultBps": 10000,
    "issuedZat": 1656250000000,
    "supplyCents": 250000,
    "collateralZat": 62500000000,
    "globalRatioBps": 49750,
    "haltMask": []
  }
]
```

### `getblocktemplate` (stock command; the `yellowback` object)

On a node with `-yellowback` — and only then; otherwise the response is v4.5.0's key for key
(N10, `yellowback_stockparity.py`) — `getblocktemplate` returns `coinbasetxn` with the tag in its
scriptSig, `coinbaseaux.flags` = the tag push in hex (it begins with the `0x24` push opcode; a
pool that assembles its own coinbase appends these bytes verbatim after the BIP34 height push,
§5), `"coinbase/append"` in `mutable`, and `TemplateInfo()`:

```
"yellowback": { "tag": "<hex>", "kind": "quote"|"signal"|"none", "priceMicroUsd": n,
                "quoteAgeSeconds": n, "signal": bool, "payoutAddress": "s1…",
                "registered": bool, "eligible": bool, "activation": "signaling"|"locked_in"|"active",
                "signalCount": n, "enforcing": bool, "valveTripped": bool, "sunset": bool,
                "healthy": bool, "templatePolicy": "strict" }
```

With `-yellowbackrequirehealthy` the command refuses outright while the index is unhealthy
(K24); without it the template is unpoliced and untagged (MINER-3). Regtest `generate` goes
through the same `CreateNewBlock`, so a regtest node with a payout address and a quote mines
tagged blocks with no further code (V25).

---

## Wallet context (`src/rpc/yellowbackwallet.cpp`)

### The abandonment predicate (shared by every wallet command that reads it)

The chain shows **abandonment** when `Snapshots[tip].haltMask.ENFORCEMENT` has been set
continuously for at least `ABANDON_BLOCKS` (= 2 · `SIGNAL_WINDOW`: 4,032 on mainnet, 128 on
regtest) — two full windows in which fewer than half of blocks signalled. That is the whole
predicate (`YellowbackIndex::IsAbandoned()`, computed from `Snapshots` alone, so every node of
every release answers alike, L12). It is **never** gated on the node's own `-yellowbackenforce`,
its valve state, its health or a passed sunset. While it holds: `yed_getinfo.abandoned` is true;
`yed_getvault` and `yed_listpositions` show `sweepBefore` on every ACTIVE vault and
`yed_listpositions.canSweep`; `yed_sweep` builds; MP-1 and TPL-1/2 stand down for vault spends
(L13), so `yed_validaterawtransaction.wouldBeRejected` is false for a sweep. Otherwise
`yed_sweep` refuses with `sweep-not-abandoned`.

### Coin locking (three stages; carried over from the prototype)

A wallet's own 0-conf outputs are trusted and spendable at once, so a plain `sendtoaddress`
issued right after a Yellowback command could burn fresh YED. Therefore (i) `yed_mint`,
`yed_send`, `yed_sendmany`, `yed_redeem`, `yed_claim` call `LockCoin` on every YED output of the
transaction they are about to commit **before** `CommitTransaction`; (ii) the index's
`SyncTransaction` pre-locks every output a well-formed payload assigns to a script that `IsMine`,
for mempool and block transactions alike; (iii) after every applied block and at startup the
wallet reconciles against the state: every `Tokens` outpoint that is mine is locked, a pre-lock
is released only when its transaction is confirmed and the applied verdict assigned it no cents,
spent outpoints are pruned. Nothing is unlocked on disconnect. Locks are in-memory
(`setLockedCoins`) and re-applied at startup; `yed_lockcoins` re-runs the reconciliation on
demand. Vault outputs need no lock (a P2SH the wallet cannot `IsMine`). Every input of every
Yellowback transaction is confirmed (YED from the index, YEC via `AvailableCoins(nMinDepth = 1)`).
Phase 8 (H5, H10) makes the locks protected and reports them in `yed_getinfo`.

### The `wallet.dat` backup rule

Ycash 4.5 transparent keys are a random keypool, not HD, so a vault owner key lives only in
`wallet.dat`: **back up `wallet.dat` after minting** — a backup taken before the keypool was
consumed does not contain the vault key. `yed_mint.warning` nags when the keypool is low. Ycash's
wallet encryption is experimental (`-developerencryptwallet`) and must not be presented as
protection for vault keys; the custody rule is full-disk encryption, RPC on localhost only, and
an offline copy of `wallet.dat`.

### `yed_getnewaddress`

Arguments: none. A fresh keypool key as a Yellowback address string (`ye…` mainnet, `yt…`
testnet, `yr…` regtest). The result is a bare string.

Result of `yed_getnewaddress`:

```json
"yrExampleOwnerAddress111111111111111"
```

### `yed_validateaddress <address>`

Arguments: `address` (string). `isvalid` false — and the other fields absent (**optional**) —
with `not-a-yellowback-address` in `reason` when the string is not a `ye…`/`yt…`/`yr…` address of
this network (the command itself does not throw, so the wallet can validate as the user types;
`yed_send` throws the same identifier). `transparentAddress` is the same key hash as an `s1…`/
`sm…` address.

Result of `yed_validateaddress`:

```json
{
  "isvalid": true,
  "address": "yrExampleOwnerAddress111111111111111",
  "keyid": "1f2e3d4c5b6a79880706050403020100f1e2d3c4",
  "ismine": true,
  "transparentAddress": "smExampleTransparentTwin111111111111",
  "reason": ""
}
```

### `yed_getbalance`

Arguments: none. `confirmedCents` from the index's `Tokens` that are mine, `unconfirmedCents`
the mempool's payload assignments to this wallet (a dry run per transaction), `height` the index
tip the confirmed figure was read at.

Result of `yed_getbalance`:

```json
{
  "confirmedCents": 100000,
  "unconfirmedCents": 0,
  "height": 331
}
```

### `yed_listunspent`

Arguments: none. Every `Tokens` outpoint that is mine. `spentUnconfirmed` is `CWallet::IsSpent`
(spent by an own unconfirmed transaction; skipped by the selectors), `locked` whether stage (iii)
holds it (always true after reconciliation; a false is the H10 mismatch that triggers
`yed_lockcoins`).

Result of `yed_listunspent`:

```json
[
  {
    "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
    "vout": 1,
    "cents": 100000,
    "valueZat": 10000,
    "address": "yrExampleOwnerAddress111111111111111",
    "height": 332,
    "confirmations": 1,
    "spentUnconfirmed": false,
    "locked": true
  }
]
```

### `yed_lockcoins`

Arguments: none. Re-runs stage (iii) and returns the outpoints now locked.

Result of `yed_lockcoins`:

```json
[
  { "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8", "vout": 1 }
]
```

### `yed_mint <cents> <lockBlocks> [from]`

Arguments: `cents` (number, `MIN_MINT ≤ cents ≤ MAX_MINT`), `lockBlocks` (number; the class
follows from it, V19), `from` (string, optional; a `ys1…` Sapling address to fund the collateral
from that address's confirmed notes, largest-first — blocks for the proving time; default:
transparent YEC via `AvailableCoins`). Builds the §3.5 MINT at `R = tip − REF_LAG` after the
MINTPOL-1 gate (`Snapshots[R].activation == ACTIVE`, `haltMask == 0`, the cap has room), draws
one fresh key for the vault owner and the token output, locks the token output, commits. `vault`
is `"<txid>:0"`; `payee` is the FEE-W choice (or the configured preference) and `null` under
FEE-0 (then `feeZat` is `0`); `fundedFrom` is `"transparent"` or `"sapling"`; `warning` is the
keypool-low nag, `""` when there is none. Refusals: `mintpol-not-active`, `mintpol-no-price`,
`mintpol-participation`, `mintpol-global-ratio`, `mintpol-divergence`, `mintpol-cap`,
`mint-unsatisfiable`, `mint-bad-lock`, `RPC_INVALID_PARAMETER` for `cents` out of range,
`RPC_WALLET_ERROR` for insufficient YEC or a locked wallet.

Result of `yed_mint`:

```json
{
  "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "vault": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8:0",
  "termClass": "A",
  "lockHeight": 380,
  "claimHeight": 404,
  "collateralZat": 25125628141,
  "feeZat": 62814071,
  "payee": "smQvTmAz2ExamplePayoutAddress1111111",
  "fundedFrom": "transparent",
  "warning": ""
}
```

### `yed_send <yedaddress> <cents>` and `yed_sendmany <{yedaddress: cents, …}>`

Arguments: `yed_send`: `yedaddress` (string), `cents` (number). `yed_sendmany`: one object of
at most 14 recipients. Builds the §3.5 TRANSFER from confirmed YED inputs (index) and confirmed
YEC fee inputs, locks the outputs, commits. The YED inputs are chosen by the **floor-aware
selector** (H1), which is deterministic — the same wallet state and the same amount always give
the same inputs — and tries, in order: an exact match (change `0`), a single input whose change is
valid, greedy smallest-first with extension while the change is unworkable, then a bounded search.
A TRANSFER **never burns** (H2): when no selection leaves change of `0` or `≥ MIN_OUTPUT` the
command refuses with `change-floor` rather than build one. `yed_estimatesend` runs the same
selector without signing or locking. Refusals: `not-a-yellowback-address`, `insufficient-yed`,
`change-floor` (YED change would lie in `(0, MIN_OUTPUT)`; the message carries the nearest
workable amounts below and above, H2), `too-many-inputs`,
`RPC_INVALID_PARAMETER` for an amount outside `[MIN_OUTPUT, MAX_OUTPUT]`.

Result of `yed_send`:

```json
{
  "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "changeCents": 50000,
  "expiryHeight": 371
}
```

`yed_sendmany` returns the same shape.

Result of `yed_sendmany`:

```json
{
  "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "changeCents": 50000,
  "expiryHeight": 371
}
```

### `yed_estimatesend <{yedaddress: cents, …}|cents>`

Arguments: one object of at most 14 recipients (as `yed_sendmany`), **or** a single number — the
total cents, when the recipients are not yet known (the GUI calls this while the user types).
A dry run of `yed_send`/`yed_sendmany` (H3): it runs the same floor-aware selector (H1) over the
same confirmed YED coins, signs nothing, locks nothing and commits nothing. It mirrors
`yed_estimatecollateral`: a halted gate or an unworkable amount still returns a figure to show.

`workable` is whether a selection exists whose change is `0` or `≥ MIN_OUTPUT`. When it is
`true`, `inputs` are the outpoints the selector would spend (in the order it would spend them),
`selectedCents` their total, `changeCents` the YED change (`0` for an exact match), `stage` the
selector stage that produced it (`"exact"`, `"single"`, `"greedy"` or `"search"`), and
`spendableCents` the wallet's confirmed, unspent YED. When it is `false`, `inputs` is empty,
`changeCents` is `0`, `stage` is `"none"`, `error` is the identifier that `yed_send` would throw
(`change-floor` or `insufficient-yed`, `""` when `workable`) and `alternatives` names the nearest
workable amounts: `below` the largest workable amount strictly below the request and `above` the
smallest strictly above it, each `null` when there is none. `alternatives` is `null` when
`workable`. The command never refuses for the amount itself; `RPC_INVALID_PARAMETER` only for a
malformed argument, an amount outside `[MIN_OUTPUT, MAX_OUTPUT]`, or more than 14 recipients.

Result of `yed_estimatesend`:

```json
{
  "amountCents": 4000,
  "recipients": 1,
  "workable": true,
  "stage": "greedy",
  "inputs": [
    { "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8", "vout": 1, "cents": 10000 }
  ],
  "selectedCents": 10000,
  "changeCents": 6000,
  "spendableCents": 10000,
  "error": "",
  "alternatives": null
}
```

### `yed_unlockcoin <txid> <n> <acknowledgement>`

Arguments: `txid` (string), `n` (number), `acknowledgement` (string; must be exactly
`I understand this burns YED`). The deliberate escape hatch of H5: `lockunspent` refuses to unlock
an outpoint the Yellowback wallet layer holds (`yed-locked-outpoint`) and `lockunspent true`
without an argument re-applies those locks after unlocking everything else, so this command is the
only way to hand a YED outpoint back to plain YEC coin selection. The YED it carries is burned by
the first transaction that spends it outside the overlay, and the next `yed_lockcoins`,
reconciliation or restart locks it again — unlock it and spend it in the same session.

Refusals: `unlock-acknowledgement-missing` (the third argument is not the exact string),
`RPC_INVALID_PARAMETER` for a bad txid or a negative `n`. Unlocking an outpoint the layer does not
hold succeeds and reports `wasYellowbackLocked: false`.

Result of `yed_unlockcoin`:

```json
{
  "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
  "vout": 1,
  "unlocked": true,
  "wasYellowbackLocked": true,
  "cents": 10000
}
```

### `yed_redeem <vaultTxid> [to]`

Arguments: `vaultTxid` (string), `to` (string, optional; a transparent or `ys1…` destination for
the collateral; default a fresh own transparent address). One step (V24):

- On an **ACTIVE** vault at or past `lockHeight`: the §3.5 owner-path REDEEM — burns
  `mintedCents` of the wallet's YED, pays the FEE-1 fee to `payee(R, vaultOutpoint)`, sends the
  rest of the collateral to `to`. Runs `MempoolCheck` first and refuses (`mempool-check-failed:
  <verdict>`) rather than commit anything MP-1 would refuse (K7); on success `CommitTransaction`
  puts it in the node's own mempool.
- On a **VOID** vault at or past `lockHeight` (L14): the §3.5 VOID RELEASE — owner path, no
  burn, no fee, no payload; an ordinary spend no rule polices (K3). Returns `burnedCents: 0`,
  `feeZat: 0`, `payee: null`. The GUI calls this **Release**.

`collateralOut` is the zat paid to `to`. `extraBurnCents` (H4) is a sub-dollar YED remainder the
selector burned rather than refuse: it is `0` whenever a selection with change of `0` or
`≥ MIN_OUTPUT` exists (those are always preferred) and otherwise lies in `[1, MIN_OUTPUT − 1]`,
i.e. at most $0.99. `burnedCents` stays the vault's debt; `extraBurnCents` is burned on top of it.
Refusals: `vault-not-found`, `vault-not-owned`,
`vault-not-active` (CLOSED or CLAIMED only), `vault-locked` (tip below `lockHeight`, ACTIVE and
VOID alike), `insufficient-yed`, `change-floor`, `mempool-check-failed:<verdict>`,
`RPC_WALLET_ERROR` for a locked wallet.

Result of `yed_redeem`:

```json
{
  "txid": "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d",
  "burnedCents": 100000,
  "feeZat": 62814071,
  "payee": "smQvTmAz2ExamplePayoutAddress1111111",
  "collateralOut": 25062804070,
  "to": "smExampleTransparentTwin111111111111",
  "extraBurnCents": 0
}
```

### `yed_claim <vaultTxid> [to]`

Arguments as `yed_redeem`. The §3.5 CLAIM of somebody else's underwater vault (from
`yed_listclaimable`): claim-path scriptSig, `nLockTime = claimHeight`, burns `mintedCents` of the
claimant's own YED, pays the fee from the collateral, collateral to `to`. Same return shape as
`yed_redeem` (including `extraBurnCents`, H4). Refusals: `vault-not-found`, `vault-not-active`, `claim-not-yet` (tip below
`claimHeight`), `claim-not-underwater` (RED-4 would fail at the reference snapshot),
`insufficient-yed`, `change-floor`, `mempool-check-failed:<verdict>`.

Result of `yed_claim`:

```json
{
  "txid": "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d",
  "burnedCents": 100000,
  "feeZat": 62814071,
  "payee": "smQvTmAz2ExamplePayoutAddress1111111",
  "collateralOut": 25062804070,
  "to": "smExampleTransparentTwin111111111111",
  "extraBurnCents": 0
}
```

### `yed_sweep <vaultTxid> <acknowledgement> [to]`

Arguments: `vaultTxid` (string), `acknowledgement` (string; must be exactly
`I understand this leaves YED unbacked`), `to` (string, optional, as `yed_redeem`). L10: builds
the §3.5 SWEEP of an own ACTIVE vault — an owner-path spend with no burn and no fee — **only
while the abandonment predicate holds**; signs it and commits it through the node's own mempool,
which admits it under abandonment (L13), and returns the raw `hex` so the owner can submit it to
any other node as well. The transaction is a rule-breaking vault spend by design (it fails RED-1):
an enforcing node — one on a chain that does not show abandonment — never mines it; on the
abandoned chain every node admits, relays and mines it like any other transaction. The vault is
then `CLOSED, unbacked = true`, `Totals.unbackedCents` grows by the debt (`unbackedCents` here),
and `yed_listtransactions` shows `type: "sweep"`. What the owner is told (§4.6): after
`claimHeight` the claim path is anyone-can-spend and, with nobody enforcing RED-4, whoever mines
first takes the collateral — sweep before `claimHeight` (`sweepBefore`) or lose it. Refusals:
`sweep-not-abandoned`, `sweep-acknowledgement-missing`, `vault-not-found`, `vault-not-owned`,
`vault-not-active`, `vault-locked`.

Result of `yed_sweep`:

```json
{
  "txid": "9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f5e4d3c2b1a0f9e8d",
  "hex": "0400008085202f8901…",
  "collateralOut": 25125627141,
  "to": "smExampleTransparentTwin111111111111",
  "unbackedCents": 100000
}
```

### `yed_listpositions [status]`

Arguments: `status` (string, optional; as `yed_listvaults`). The wallet's own vaults (a vault is
mine iff `HaveKey(ownerPubKey)`): every `yed_getvault` field plus `canRedeem` (true for an
ACTIVE or VOID vault at or past `lockHeight` — for VOID it is the Release, L14), `canClaim`
(true when `claimable` and the wallet holds `≥ mintedCents`) and `canSweep` (true for an ACTIVE
vault while abandonment holds). `sweepBefore` as `yed_getvault` (**optional**).

Result of `yed_listpositions`:

```json
[
  {
    "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
    "vout": 0,
    "status": "ACTIVE",
    "ownerPubKey": "02a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9",
    "ownerKeyId": "1f2e3d4c5b6a79880706050403020100f1e2d3c4",
    "ownerAddress": "yrExampleOwnerAddress111111111111111",
    "termClass": "A",
    "lockHeight": 380,
    "claimHeight": 404,
    "collateralZat": 25125628141,
    "collateral": 251.25628141,
    "mintedCents": 100000,
    "mintHeight": 332,
    "refHeight": 329,
    "feePaidZat": 62814071,
    "closeHeight": null,
    "closingTxid": "",
    "burnedCents": 0,
    "unbacked": false,
    "claimable": false,
    "underwaterAt": 437800,
    "voidReason": "",
    "sweepBefore": 404,
    "canRedeem": false,
    "canClaim": false,
    "canSweep": false
  }
]
```

### `yed_listtransactions [count] [skip]`

Arguments: `count` (number, default `100`), `skip` (number, default `0`). Newest first; every
wallet transaction that is in `TxLog` with an own token or vault involved (N21: filtered on
`spentTokens`/`assigned`/`closedVaults` that are mine), plus own unconfirmed and expired
Yellowback transactions. `type` is the wallet's view: `mint`, `send`, `receive`, `burn` (a
transfer that burned), `redeem` (own owner-path redemption), `claim` (this wallet claimed),
`claimed` (an own vault was claimed by someone else), `sweep` (an own vault swept under
abandonment). `amountCents` is the signed effect on this wallet. `unbacked` is true on a row that
closed an own vault without its burn; `expired` rows have `height: -1`, `confirmations: 0`,
`verdict: "expired"`. `payee` `null` where there is no fee output.

Result of `yed_listtransactions`:

```json
[
  {
    "txid": "6a1f2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8",
    "height": 332,
    "confirmations": 1,
    "type": "mint",
    "verdict": "ok",
    "path": "",
    "yedIn": 0,
    "yedOut": 100000,
    "burned": 0,
    "amountCents": 100000,
    "feeZat": 62814071,
    "payee": "smQvTmAz2ExamplePayoutAddress1111111",
    "unbacked": false,
    "expired": false
  }
]
```

---

## Error identifiers (M8)

The identifier is the first token of the message, followed by `: ` and free text. Provocations are
what `yellowback_rpc_contract.py` uses.

| Identifier | Raised by | When (provocation) |
|---|---|---|
| `yellowback-unhealthy` | every gated command | the index is unhealthy (`unhealthyReason` follows); provoke with `-yellowbacktestfault=storage:commit` then any non-allow-listed command |
| `mintpol-not-active`, `mintpol-no-price`, `mintpol-participation`, `mintpol-global-ratio`, `mintpol-divergence`, `mintpol-cap` | `yed_mint` | MINTPOL-1, one per halt bit and the cap: mint before activation; with no quote tags in the windows; after fewer than `PARTICIPATION_FLOOR` signals in a window; with the global ratio above `GLOBAL_RATIO_HALT_BPS`; with `P_fast`/`P_slow` diverging by more than `DIVERGENCE_BPS`; with `-yellowbacksupplycapbps` low and supply at the cap |
| `mint-unsatisfiable` | `yed_mint`, `yed_estimatecollateral` | `requiredZat > MAX_MONEY` (K14): `MAX_MINT` cents at `priceMicroUsd = PRICE_MIN` |
| `mint-bad-lock` | `yed_mint`, `yed_estimatecollateral` | `lockBlocks` outside every class, or `lockHeight + GRACE ≥ LOCKTIME_THRESHOLD` |
| `vault-not-found`, `vault-not-active`, `vault-not-owned` | `yed_redeem`, `yed_claim`, `yed_sweep`, `yed_getvault` | an unknown txid; a CLOSED or CLAIMED vault (a VOID vault is releasable by `yed_redeem`, L14); another wallet's vault |
| `vault-locked` | `yed_redeem`, `yed_sweep` | tip below `lockHeight` (ACTIVE and VOID alike) |
| `claim-not-yet` | `yed_claim` | tip below `claimHeight` |
| `claim-not-underwater` | `yed_claim` | RED-4 would fail at the reference snapshot (the price did not fall) |
| `sweep-not-abandoned` | `yed_sweep` | the abandonment predicate is false: enforcement on, or suspended for less than `ABANDON_BLOCKS` (L10, L12 — a passed sunset alone is not abandonment) |
| `sweep-acknowledgement-missing` | `yed_sweep` | the second argument is not the exact acknowledgement string |
| `change-floor` | `yed_send`, `yed_sendmany`, `yed_redeem`, `yed_claim` | no selection leaves YED change of `0` or `≥ MIN_OUTPUT` (H2; a REDEEM or CLAIM burns a sub-dollar remainder instead, H4, so it reaches this only when even that is impossible): send `cents − 50` from a single `cents` output. The message is structured and always has this shape: `change-floor: <requested> cents cannot be sent from these coins without change below the $1.00 minimum output; nearest workable amounts: below <n\|none>, above <n\|none>` — the GUI reads the two numbers with that grammar and `yed_estimatesend.alternatives` returns them as fields |
| `unlock-acknowledgement-missing` | `yed_unlockcoin` | the third argument is not exactly `I understand this burns YED` |
| `yed-locked-outpoint` | `lockunspent` (the stock RPC) | `lockunspent false\|true [{txid,vout}]` naming an outpoint the Yellowback wallet layer holds: use `yed_unlockcoin` |
| `yed-burn-refused` | `sendrawtransaction` (the stock RPC) | with `-yellowback`, a raw transaction that spends a `Tokens` outpoint of this wallet without a payload that reassigns it: pass `allowyedburn = true` to send it anyway |
| `not-a-yellowback-address` | `yed_send`, `yed_sendmany`, `yed_validateaddress` (in `reason`, no throw) | the recipient is not a `ye…`/`yt…`/`yr…` address of this network: pass an `s1…`/`sm…` address |
| `verdict-parent-not-tip` | `yed_getblockverdict` | the block's parent is not the index tip and the block is not a rejected child of it (N12): pass the tip's grandparent |
| `insufficient-yed` | `yed_redeem`, `yed_claim`, `yed_send`, `yed_sendmany` | wallet YED below the burn or amount |
| `mempool-check-failed:<verdict>` | `yed_redeem`, `yed_claim` | `MempoolCheck` returned the named RED verdict (K7); provoke with a redemption whose reference snapshot has no `pClaim` where the claim path is chosen (`vault-claim-not-underwater`) |
| `fee-no-eligible-payee` | `yed_getfeepayee` | `E(refHeight)` empty (FEE-0; not an error for the wallet, which omits the output): a `refHeight` below the first quote tag |
| `quote-out-of-range` | `yed_setquote` | price outside `[PRICE_MIN, PRICE_MAX]` and not `0` |
| `no-payout-address` | `yed_setquote` | the node has no payout key and so can emit no tag (no `-yellowbackpayoutaddress` and no P2PKH `-mineraddress`) |

The wallet builder's own refusals (`src/yellowback/txbuilder.cpp`; Phase 6) carry identifiers
the same way, so the GUI can match them too. They are not rule refusals: `RPC_INVALID_PARAMETER`
for a bad argument, `RPC_WALLET_ERROR` otherwise.

| Identifier | Raised by | When (provocation) |
|---|---|---|
| `bad-address` | `yed_mint` (`from`), `yed_redeem`, `yed_claim`, `yed_sweep` (`to`) | the address is not an `s1…`/`sm…` or `ys1…` address of this network (a `ye…` YED address, a Sprout address, nonsense), or its Sapling spending key is not in this wallet: pass a `yed_getnewaddress` result as `from` |
| `bad-mint-amount` | `yed_mint` | `cents` outside `[MIN_MINT, MAX_MINT]` |
| `bad-xfer-amount` | `yed_send`, `yed_sendmany` | an amount outside `[MIN_OUTPUT, MAX_OUTPUT]` |
| `insufficient-yec` | `yed_mint` | the wallet (or the named `from` address) cannot cover collateral + fees from confirmed, unlocked outputs or notes: `yed_mint … <an empty s1… address>` |
| `wallet-locked` | every signing command | the wallet is encrypted and locked (`walletpassphrase` first); the stock `EnsureWalletIsUnlocked` message may precede it |
| `keypool-empty` | `yed_mint`, `yed_send`, `yed_redeem`, `yed_claim` | no fresh key could be drawn (`keypoolrefill`) |
| `too-many-inputs`, `too-many-notes` | `yed_send`, `yed_redeem`, `yed_claim` / `yed_mint` | more than 250 YED inputs / more than 20 Sapling notes would be spent: consolidate first |
| `expiring-too-soon`, `index-below-start` | every builder | the index is far enough behind the chain that `R + REF_WINDOW` would expire the transaction at once, or the index has not reached `startHeight + REF_LAG` |
| `vault-value-too-small` | `yed_redeem`, `yed_claim`, `yed_sweep` | the vault does not cover the network fee plus the enforcement fee (cannot happen for a vault MINT-5 accepted) |

## `rpc/client.cpp` conversion rows (Phase 3)

The CLI converts positional arguments by index, so every numeric argument is listed and every
string argument is not. **Rebuild `ycash-cli` after every `client.cpp` change** — a stale CLI
passes a number as a string and the node answers `RPC_INVALID_PARAMETER` (N27).

| Command | Converted indices |
|---|---|
| `yed_getstatehash` | 0 |
| `yed_getprice` | 0 |
| `yed_listminers` | 0, 1 |
| `yed_setquote` | 0, 1 |
| `yed_getfeepayee` | 0, 1 |
| `yed_listvaults` | 1, 2 (was 1, 2, 3) |
| `yed_estimatecollateral` | 0, 1, 2 |
| `yed_estimatefee` | 0 |
| `yed_gethistory` | 0, 1 |
| `yed_mint` | 0, 1 |
| `yed_listtransactions` | 0, 1 (unchanged) |
| `yed_send` | 1 (unchanged) |
| `yed_sendmany` | 0 (unchanged; the object) |
| `yed_estimatesend` | 0 (Phase 8; the recipients object or the plain cents number) |
| `yed_unlockcoin` | 1 (Phase 8; the vout) |
| `yed_gettag`, `yed_getvault`, `yed_gettxinfo`, `yed_decodepayload`, `yed_validaterawtransaction`, `yed_getblockverdict`, `yed_validateaddress`, `yed_redeem`, `yed_claim`, `yed_sweep`, `yed_listpositions` | none (all strings) |

## Configuration the contract depends on

`-yellowback`; `-yellowbackenforce` (default 1); `-yellowbackpayoutaddress=<s1…>` (P2PKH; defaults
to a P2PKH `-mineraddress`); `-yellowbacksignal` (default 0 on mainnet, 1 on testnet and regtest,
L4; effective only with `-yellowbackenforce=1`, L3); `-yellowbackquotemaxage=<sec>` (default
1800); `-yellowbacktemplatepolicy=strict|consensus`; `-yellowbackrequirehealthy` (default 0);
`-yellowbackpreferredpayee=<s1…>`; `-yellowbackpayeepenaltyblocks`, `-yellowbackpayeeaccuracywindow`,
`-yellowbackpayeetiltbps` (FEE-W overrides, L6; reported in `yed_getinfo.params.policy` and
`yed_getfeepayee.policy`); `-reindex-yellowback` (also clears `Rejected`); `-yellowbackfee`
(clamped to `≥ DEFAULT_FEE`); `-yellowbackmintlag` (`REF_LAG`, 0..36); `-debug=yellowback`;
regtest only: `-yellowbackstartheight`, `-yellowbacksigmaref`, `-yellowbacksupplycapbps`,
`-yellowbackenforceuntil` (all four in `yed_getinfo.params` and the state hash); test only:
`-yellowbacktestfault=storage:<check|commit|undo>[:<height>]|template|novalve`. `-prune` is refused
with `-yellowback`.

## Phase 8 additions (delivered; `rpcversion` stays 2)

Every item below is implemented and has its shape above; the `rpcversion` rule holds (additions
only, nothing removed and nothing reshaped).

- `yed_estimatesend` (H3): the dry run of `yed_send`/`yed_sendmany` — the selection, the change
  and, when the amount is unworkable, the nearest workable amounts; no signing, no locking.
- `yed_unlockcoin <txid> <n> "I understand this burns YED"` (H5): the deliberate escape hatch now
  that `lockunspent` refuses to unlock a Yellowback-held outpoint.
- `yed_getinfo.lockedOutputs` (number) and `protectedByIndex` (boolean) (H10).
- `yed_redeem.extraBurnCents` and `yed_claim.extraBurnCents` (H4): a sub-dollar remainder the
  selector burned rather than refuse, `0` whenever a selection with valid change exists.

### Stock RPCs the overlay changes (H5, H7, H8; wallet tier, no consensus effect)

These are Ycash's own commands. Each change is additive and inert without `-yellowback`:

| Command | Change |
|---|---|
| `lockunspent` | With `-yellowback` and a wallet, `lockunspent false\|true [{txid,vout},…]` refuses (`yed-locked-outpoint`, `RPC_WALLET_ERROR`) when any named outpoint is held by the Yellowback wallet layer, and nothing in the call is applied. `lockunspent true` with no second argument still unlocks everything, then **re-applies** the Yellowback locks before returning, so it can never leave YED spendable as plain YEC. |
| `sendrawtransaction` | Third parameter `allowyedburn` (boolean, default `false`), the same shape as `allowhighfees`: with `-yellowback` and a wallet, a raw transaction that spends a `Tokens` outpoint that is mine and carries no payload assigning cents to an output is refused with `yed-burn-refused` (`RPC_WALLET_ERROR`) unless it is `true`. Without `-yellowback`, without a wallet, or for a transaction that spends no YED of this wallet, the parameter changes nothing. |
| `importprivkey`, `importaddress`, `importwallet`, `z_importkey` | After their rescan, each calls the Yellowback wallet layer's `Reconcile()` (H8), so YED that has just become mine is locked before the next block rather than at the next reconciliation. No return-value change. |

### Startup (H6)

A datadir that holds a Yellowback index refuses to start without `-yellowback`: `init` fails with
*"This datadir holds a Yellowback index … start with -yellowback, or with -yellowback=0 to
acknowledge that any YED outputs in this wallet are spendable as plain YEC."* Passing
`-yellowback=0` explicitly is that acknowledgement and starts normally (with the warning logged);
the check is skipped with `-disablewallet`, which cannot burn anything.
