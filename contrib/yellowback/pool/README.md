# Pool-integration kit (plan §5)

What a mining pool does to carry the Yellowback coinbase tag, and the tools to check it.

## The tag in one paragraph

A tag is 36 bytes pushed into the coinbase `scriptSig` as one direct push (`0x24` + 36 bytes)
anywhere **after** the BIP34 height push: magic `YED!`, version `1`, flags (bit 0 = activation
signal), `priceMicroUsd` (8 bytes LE; `0` = signal-only), `sourceMask` (2 bytes) and the 20-byte
Hash160 of the pool's enforcement-fee key (§3.2). The node reads it with a **byte-level scan** for
the 5-byte pattern `24 59 45 44 21` — never a script parse — so raw extranonce bytes before or
after it are harmless (TAG-1, V4). Only the first occurrence counts (TAG-5). No property of a
coinbase ever makes a block invalid (TAG-4).

The scriptSig is bounded to 2..100 bytes by consensus (`ref/ycash/src/main.cpp:1456-1458`). The
height push takes 1–5 bytes and the tag 37, which leaves ≈ 55 bytes for extranonces and pool
text. Keep the pool's own text short.

## The three carriers (V5/V26)

The node keeps the current tag in the never-assigned `COINBASE_FLAGS` global, so it reaches
every coinbase the node builds:

| Carrier | Who uses it | What to do |
|---|---|---|
| **Internal miner / `generate`** | solo, regtest, the devnet | nothing: `IncrementExtraNonce` appends the tag |
| **`getblocktemplate` → `coinbasetxn`** | pools that take the node's coinbase as is | nothing: the tag is in `coinbasetxn.data` |
| **`getblocktemplate` → `coinbaseaux.flags`** | pools that assemble their own coinbase | append the bytes of `coinbaseaux.flags` **verbatim** (they already begin with the `0x24` push opcode) after the height push and before the extranonce and text |

## What an operator does (§5, M9)

1. Run the release `ycashd` with the §4.7 options (`-yellowback -yellowbackpayoutaddress=<s1…>`,
   the signal flag as the runbook says) and the quote agent:
   `yellowback-quote --conf yellowback-quote.toml` (copy `yellowback-quote.toml.sample`; a
   `systemd` unit and a `launchd` plist are in this directory). A pool on the `coinbasetxn` path
   needs nothing else.
2. A pool that assembles its own coinbase appends `coinbaseaux.flags` verbatim, as above, and keeps
   its own text ≤ ~55 bytes.
3. A stratum layer that honours neither `coinbasetxn` nor `coinbaseaux.flags` uses the per-stack
   note below, or switches to the `coinbasetxn` path.
4. Verify with `check-coinbase <height|blockhash> [-- -datadir=…]` (prints `kind: quote|signal|none`
   plus the decoded fields) and watch the node with `monitor-quote.sh` (reads `yed_getinfo.miner`;
   exit 0 only while the node holds a fresh quote and the payout key is eligible).

`check-coinbase` decodes the block's coinbase locally with the same TAG-1..5 rules the node uses
(`../yellowback_price.py`), so it also works against a node without the module; `yed_gettag` on an
enabled node is the cross-check. `check-coinbase --scriptsig <hex>` decodes a raw scriptSig
offline (useful on a pool's coinbase before it is mined).

## Per-stack notes (skeleton — survey §12 Q9 pending)

The survey of which stacks Ycash pools actually run needs the operators (Phase 7, §12 Q9). Each
note below is the expected shape from reading the stack's public source; **every item marked
TODO is unverified against a live pool.**

### node-stratum-pool lineage (s-nomp / z-nomp / zcash-equihash stratum)

- These build the coinbase themselves from `getblocktemplate` (`lib/transactions.js`
  `createGeneration`), so the **`coinbaseaux.flags` path** applies.
- TODO (survey): confirm the fork in use reads `coinbaseaux.flags` at all — stock
  node-stratum-pool ignores it and writes `height push | extranonce1 | extranonce2 | text`.
  Expected patch: after the height serialisation in `createGeneration`, concatenate
  `Buffer.from(rpcData.coinbaseaux.flags, 'hex')` when it is non-empty. One line; verify the
  scriptSig stays ≤ 100 bytes with the pool's `coinbase_text` (the stock text is ≈ 10 bytes).
- TODO (survey): whether the pool refreshes the template every block (it must: the tag's price
  changes as the agent publishes).

### miningcore

- Builds its own coinbase per coin family (`Blockchain/Equihash/EquihashJob.cs`); the
  **`coinbaseaux.flags` path** applies.
- TODO (survey): whether the Equihash job builder honours `coinbaseaux.flags` (the Bitcoin job
  builder has a `coinbaseaux` field; the Equihash one may not) and where the extranonce is placed.
  Expected patch: same shape as above, in the scriptSig assembly of the job builder.

### Solo `getblocktemplate` (own miner software, `coinbasetxn`)

- Nothing to do if the miner submits `coinbasetxn.data` unchanged.
- TODO (survey): which solo miners in use rewrite the coinbase (some replace `vin[0].scriptSig`
  to add their own extranonce); those fall under the `coinbaseaux.flags` path.

### Hosted / closed stacks (TODO: survey)

- Operators of closed stratum software can only choose between the `coinbasetxn` path and asking
  their vendor for the one-line `coinbaseaux.flags` append. Record what each says here.

## Files

| File | Role |
|---|---|
| `yellowback-quote.toml.sample` | the agent's configuration, every key commented |
| `check-coinbase` | TAG-1..5 decoder over `ycash-cli getblock` / `getrawtransaction` (or `--scriptsig <hex>`) |
| `monitor-quote.sh` | `yed_getinfo.miner` check with exit codes for cron/Nagios/Prometheus textfile use |
| `yellowback-quote.service` | systemd unit |
| `org.ycash.yellowback-quote.plist` | launchd unit |
