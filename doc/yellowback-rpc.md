# Yellowback RPC contract (`yed_*`), rpcversion 1

This file is the interface between the node (`ycash-dd`) and the wallet application
(`yecwallet-dd`). Nothing else crosses that boundary. `yed_getinfo.rpcversion` is bumped on any
incompatible change and the application refuses a version it does not know.

All amounts: YED in integer **cents** (`100` = $1.00), YEC in **zatoshi** fields (`…Zat`) with a
decimal YEC twin where convenient, prices in **micro-USD per YEC** (`1000000` = $1.00). Heights are
block heights; the protocol never speaks wall-clock time.

Every command requires `-experimentalfeatures -yellowback`; without them the node answers JSON-RPC
`-32601` "Method not found". While the index is unhealthy every command except `yed_getinfo` fails
with `-1` "yellowback index unhealthy: <reason>; restart with -reindex-yellowback". Error strings
that begin with a rule id (`RED-3: …`, `SUB-1: …`) or a verdict (`bad-mint-collateral: …`,
`mint-supply-cap: …`) are stable identifiers.

## Node context (no wallet needed)

| Command | Arguments | Result |
|---|---|---|
| `yed_getinfo` | — | `enabled`, `rpcversion` (1), `network` (`main`/`test`/`regtest`), `startHeight`, `genesisAnchor{txid,vout}`, `height` (index; −1 when empty), `blockhash`, `chainHeight`, `synced` (index tip == chain tip), `healthy`, `unhealthyReason`, `anchor{valid,txid,vout,valueZat,address}`, `rosterIndex`, `params{minMintCents,maxMintCents,minOutputCents,maxOutputCents,supplyCapCents,tiers[{tier,blocks,ratioPct}],rosterGrace,volCooldown,priceMaxAge,mintWindow,mintEvalLag,tokenValueZat,feeZat}` |
| `yed_getstatehash` | `[height]` | `height`, `blockhash`, `statehash` (SHA-256 over every table, undo excluded; `height` must equal the index height if given) |
| `yed_getstats` | — | `height`, `supplyCents`, `collateralZat`, `collateral`, `activeVaults`, `voidVaults`, `supplyCapCents` (0 = none), `priceMicroUsd` (null if undefined), `priceHeight`, `priceAge`, `healthPct`, `dcaBps`, `errBps`, `mintFrozen`, `lastBreachHeight`, `mintFrozenUntil` |
| `yed_getprotectionstatus` | — | `height`, `healthPct`, `dca{bps,band}`, `err{bps,active,burnMultiplierBps}`, `volatility{mintFrozen,lastBreachHeight,frozenUntil,cooldownBlocks,priceNow,priceShortWindow,priceLongWindow,shortWindowBlocks,longWindowBlocks,shortThresholdBps,longThresholdBps}`, `mintingAllowed` |
| `yed_getprice` | `[height]` | `height`, `priceMicroUsd` (null if none within 48 blocks), `sourceHeight`, `age` |
| `yed_getroster` | — | `index`, `revealHeight`, `scriptHex`, `address`, `k`, `n`, `pubkeys[]`, `mintableIndices[]`, `previous` (same shape or null) |
| `yed_getvault` | `txid` | `txid`, `vout`, `status` (`ACTIVE`/`VOID`/`CLOSED`), `ownerPubKey`, `ownerKeyId`, `tier`, `lockHeight`, `collateralZat`, `collateral`, `mintedCents`, `mintHeight`, `rosterIndex`, `requiredBurnCents` (open vaults), `voidReason` (VOID), and for CLOSED: `wasActive`, `closeHeight`, `closingTxid`, `burnedCents`, `errBpsAtClose`, `requiredBurnAtClose`, `unbacked`; plus `indexHeight` |
| `yed_listvaults` | `[status] [rosterIndex] [count=100] [skip=0]` | `height`, `total`, `vaults[]` (as `yed_getvault`), `openVaultsPerRoster{index: n}` |
| `yed_gettxinfo` | `txid` | the index record of a confirmed transaction: `height`, `type` (`mint`/`transfer`/`redeem`/`price`/`none`), `verdict`, `yedIn`, `yedOut`, `burned`, `assigned[{txid,vout,cents,address}]`, `spentTokens[…]`, `closedVaults[{txid,vout}]`, `anchorSpend`, `priceRecorded`, `confirmations`, `dryRun` (false); for a mempool transaction the same fields from a dry run with `dryRun: true`, `confirmations: 0` |
| `yed_decodepayload` | `hex` (payload, `OP_RETURN` script or raw transaction) | `type` and the type's fields (`tier`, `cents`, `lockHeight`, `evalHeight`, `ownerPubKey` / `assignments[{vout,cents}]`, `assignedCents` / `priceMicroUsd`) |
| `yed_validaterawtransaction` | `hex` | the dry-run record (as `yed_gettxinfo`), `relevant`, `indexHeight`, `synced`, `payload` |
| `yed_estimatecollateral` | `cents tier [priceMicroUsd]` | `cents`, `tier`, `ratioPct`, `lockBlocks`, `evalHeight`, `dcaBps`, `priceMicroUsd`, `requiredZat`, `required`, `lockHeight`, `unlockHeight` (same value), `expiryHeight`; or `requiredZat: null` with `error: bad-oracle-price` or `error: collateral-out-of-range` |
| `yed_gethistory` | `fromHeight toHeight` (≤ 10,000) | `[{height,blockhash,supplyCents,collateralZat,priceMicroUsd,healthPct,dcaBps,errBps,mintFrozen}]` |
| `yed_createpricetx` | `priceMicroUsd\|"rotate" ["refillTxid:n"] ["newRosterScriptHex"]` | `hex` (unsigned), `prevtxs[]` (for `signrawtransaction`), `anchor{txid,vout,valueZat,unconfirmed}`, `refillValueZat`, `newAnchorValueZat`, `feeZat`, `rotation`, `priceMicroUsd`, `expiryHeight` |

## Wallet context

| Command | Arguments | Result |
|---|---|---|
| `yed_getnewaddress` | — | the address string (`ye…` mainnet, `yt…` testnet, `yr…` regtest) |
| `yed_validateaddress` | `address` | `isvalid`, `address`, `keyid`, `ismine`, `transparentAddress` |
| `yed_getbalance` | — | `confirmedCents` (index), `unconfirmedCents` (mempool assignments to this wallet), `height` |
| `yed_listunspent` | — | `[{txid,vout,cents,valueZat,address,height,confirmations,reserved,spentUnconfirmed,locked}]` |
| `yed_mint` | `cents tier` | `txid`, `vault` (`txid:0`), `lockHeight`, `evalHeight`, `expiryHeight`, `collateralZat`, `collateral`, `ownerKeyId`, `warning`. Refusals: `bad-mint-amount`, `bad-mint-tier`, `bad-oracle-price`, `minting-blocked-during-err`, `mint-frozen-volatility`, `mint-supply-cap`, `insufficient YEC`, `walletpassphrase` (locked wallet) |
| `yed_send` | `yedaddress cents` | `txid`, `changeCents`, `expiryHeight`. Refusals: `not a Yellowback address`, `bad-xfer-amount`, `insufficient YED`, change floor `C20` (message names the workable amounts) |
| `yed_sendmany` | `{yedaddress: cents, …}` (≤ 14) | as `yed_send` |
| `yed_redeem` | `vaultTxid` | `hex` (owner-signed), `vault`, `roster{index,k,n,pubkeys[]}`, `requiredBurnCents`, `burnCents`, `changeCents`, `expiryHeight`, `deadlineHeight` (submit by this height). Refusals: `locked until height`, `already pending`, `insufficient YED`, `RED-6` (no price) |
| `yed_submitredeem` | `hex` | `txid`, `quorumSignatures`. Refusals: `no pending redemption`, `SUB-1: …`, `C22` (before lockHeight), `expiring too soon` |
| `yed_abortredeem` | `vaultTxid` | `aborted` (bool) |
| `yed_cosignredeem` | `hex` | `hex` (with this node's signature), `quorumSignatures`, `k`, `complete`, `check{burned,requiredBurn,fee,indexHeight}`. Refusals carry the rule id (`RED-0` … `RED-8`), and `(transient)` when a retry after the next block may succeed |
| `yed_listpositions` | `[status]` | `[{vaultTxid,status,mintedCents,collateralZat,lockHeight,unlockHeight,tier,mintHeight,rosterIndex,ownerKeyId,requiredBurnCents,canRedeem,pending, voidReason \| closeHeight,closingTxid,burnedCents}]` |
| `yed_listtransactions` | `[count=100] [skip=0]` | newest first: `[{txid,height,confirmations,type (mint\|send\|receive\|burn\|redeem),verdict,yedIn,yedOut,burned,amountCents,expired}]`; expired wallet transactions have `height: -1`, `verdict: "expired"`, `expired: true` |
| `yed_lockcoins` | — | `[{txid,vout}]` locked after re-running coin locking |

## Operator co-sign endpoint (coordinator, not the node)

`POST /cosign` with JSON `{"hex": "<owner-signed or partially co-signed hex>"}`; success `2xx`
`{"hex": "<hex with one more signature>", "quorumSignatures": n, "k": k, "complete": bool}`;
refusal `4xx` `{"error": "<node error string>", "transient": bool}`.
