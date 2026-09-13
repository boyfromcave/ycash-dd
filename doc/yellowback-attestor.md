# Running a Yellowback attestor (draft, Phase A0)

**One hot key, one node — ever.** The attestor hot key (`attestorPubKey`) must be held by exactly
one `ycashd` wallet, and only that node's `yed_signattestation` may sign with it. Two prices signed
for one block height by one `seq` are an equivocation (EQV-1): anyone can put the two attestations
on chain and the attestor is **ejected for good** — the key and the bond outpoint can never
register again, the fee stream stops, and the bond sits idle until its locktime. The node
protects you from yourself on one machine: `yed_signattestation` records every `(seq,
citedHeight, price)` it has ever signed in `<datadir>/yellowback/attest-signed.dat`, fsyncs it
before answering, returns the earlier signature if you ask for the same price again and refuses
(`equivocation-guard`) if you ask for a different one — so an agent restart mid-interval, or two
agents on one node, cannot equivocate. **Two nodes holding one hot key defeat that guard**: a
hot-standby node with a copy of `wallet.dat`, a restored backup started while the original still
runs, a second `yellowback-attest` pointed at a second node. Do not run them. If you must move an
attestor to a new machine, stop the old node first, copy `wallet.dat` *and* `attest-signed.dat`
together, and never start the old one again. Do not delete `attest-signed.dat`.

This guide is written from the v3 plan (`docs/plans/yellowback-v3-development-plan.md` §4.7) and
the proposal (`docs/reference/yellowback-price-attestation.md` §4–§6, §12, §13). It is a draft:
Phase A4 finishes it with the agent's configuration reference and the `systemd`/`launchd` units.
Ycash Yellowback (YED) is the system; YED is the unit.

## What an attestor is

An attestor is a party that has posted a **bond** (a long CLTV time-lock back to its own key,
never custody, never burned) and runs the **attestor agent** `yellowback-attest`, which polls price
sources, has the node sign a price every `k` blocks (`10` on mainnet) and gossips the 74-byte
attestation. Minters and claimants carry those attestations into their transactions; the enforcing
nodes verify the signatures, take a bond-weighted quantile and combine it with the mining pools'
medians (`pMint = min`, `pClaim = max`). An attestor is paid for attestations that are **used** in a
confirmed mint or claim (`ATTEST_FEE_BPS` = 25 % of the pool enforcement fee, to `P2PKH(bondPubKey)`),
never for attestations published.

There is no slashing: the penalty for misbehaviour is ejection and a bond that earns nothing until
it unlocks. That is sufficient because attestors can **grief but not extract** — no direction the
price is pushed in pays them (proposal §10.3) — and it is why the bond can be your own coin.

What you need: a v3 `ycashd` with `-yellowback` (no payout address, no mining), outbound
connectivity, a bond of at least `BOND_MIN` (20,000 YEC on mainnet) you can lock for at least
`BOND_MIN_LOCK` blocks (420,480 ≈ one year), and price sources. No inbound port, no domain, no
funded hot wallet beyond the node's own for the rare `yed_revive`.

## Registration

```
ycashd -yellowback                                  # any v3 node; no payout address, no mining
ycash-cli yed_registerattestor 20000 420480 0       # once; then wait BOND_MATURITY
yellowback-attest attest --conf attest.toml          # forever: polls, yed_signattestation, gossips
```

`yed_registerattestor <bondYec> <lockBlocks> [flags]` builds the one transaction an attestor ever
needs: `vout[0]` the bond — `P2SH(<bondLocktime> OP_CHECKLOCKTIMEVERIFY OP_DROP <bondPubKey>
OP_CHECKSIG)` of `bondYec` — and an `OP_RETURN` payload naming two fresh keys of your wallet: the
**hot key** (`attestorPubKey`, signs attestations) and the **bond key** (`bondPubKey`, owns the
bond and receives fees). `flags` declares your source tier (`0` direct exchange APIs, `1` mixed,
`2` aggregator) and, in bit 2, whether you also operate a mining pool — voluntary, and the
explorer shows it (proposal §7.5). The record is `PENDING` for `BOND_MATURITY` blocks (16,128 ≈ two
weeks), then `ELIGIBLE`; `yed_listattestors` shows your `seq` — the attestor's identity from then
on — once the transaction confirms.

**A bond cannot be topped up.** To change it, register a new identity (new keys, new `seq`, age
from zero) and let the old one go dormant and withdraw at its locktime. **Back up `wallet.dat`
right after registering** — both keys came from the keypool, and a backup taken before the
registration does not contain them (Ycash 4.5 transparent keys are not HD).

Registration is open from `START_HEIGHT`; it never makes a block invalid and needs no miner's
approval (proposal §5.3). Duplicate hot keys are refused (REG-A1) unless the earlier record is
`WITHDRAWN`; an `EJECTED` key is barred for good.

## Bond key hygiene

The registering wallet holds both keys. The hot key must stay in it — `yed_signattestation` uses
it every ten blocks. The bond key is used by exactly one command, `yed_withdrawbond`, a year or
more later; until then it is worth `bondYec` to whoever holds it, and it collects your fees. Move
it cold:

1. After the registration confirms, `dumpprivkey <bondAddress>` (the `bondAddress` from
   `yed_registerattestor`/`yed_listattestors`) and store the WIF offline, twice.
2. Verify the offline copy imports into an offline wallet and yields the same `bondAddress`.
3. Only then remove it from the hot wallet. Ycash 4.5 has no key deletion; the practical form is a
   fresh wallet: `yed_registerattestor` from a wallet created for the purpose, export the bond key,
   then keep that wallet as the attestor wallet knowing the bond key it still contains is a copy
   of an offline original — or, stricter, register from a wallet that holds only the bond key
   afterwards and `importprivkey` the hot key into the attestor node. Documented, not enforced
   (plan §4.6).
4. Fees accrue to `bondAddress` as ordinary transparent YEC; spending them needs the bond key, so
   collect them from the cold side, not the attestor node.

The bond output itself is **not** `IsMine` to the wallet (its script is non-standard to `Solver`):
`listunspent` will not show it, `sendtoaddress` cannot reach it, and nothing about it is in
`wallet.dat` except the key. The wallet finds it through the chain's `Attestors` table by
`bondPubKey`, which is how `yed_withdrawbond` works after a restore (`importprivkey` +
`-rescan`).

## The agent

`yellowback-attest attest --conf attest.toml` runs forever beside the node. `attest.toml`:

```
[node]      rpc_url, rpc_cookie                     # this node's RPC; the hot key lives here
[attest]    every_blocks = 10, fail_polls = 2       # k; after two failed polls publish nothing
[[sources]] …                                       # as the quote agent (yellowback_price.py presets)
[transport] kind = "iroh" | "dir", relays = [...], topic
```

Every `every_blocks` it aggregates the sources exactly as the quote agent does (prefer a median
across venues over a VWAP from one deep book: on thin markets venue diversity beats volume), calls
`yed_signattestation <seq> <price> <tip − REF_LAG>`, and publishes the 74 bytes. After `fail_polls`
failures it publishes nothing — a missing attestation is the correct report of a broken feed, and
costs you only that interval's fees. It never holds a key: the node signs. Prices are integer
micro-USD per YEC inside `[PRICE_MIN, PRICE_MAX]`.

Correlation is the cost of aggregators: eight attestors reading one aggregator are one
observation. Declare your tier honestly and prefer direct APIs where you can.

Monitoring: `yed_getinfo.attest` (arming state, pool health) and `yed_listattestors` (your
`status`, `seated`, `pinned`, `lastBundleHeight`, `poolFresh`). Sample `systemd`/`launchd` units
will live in `contrib/yellowback/attest/` (Phase A4).

## Arming, and the day's notice

Nothing reads attestations until the layer **arms** (proposal §6.4, D-4). At the first snapshot
with `ATTEST_ARM_MIN` (5) `ELIGIBLE` attestors the state becomes `TRIGGERED` with `armHeight =
triggerHeight + ATTEST_ARM_DELAY` (1,152 blocks ≈ one day); at `armHeight` it becomes `ARMED` and
every mint and claim from then on must carry a valid bundle. The trigger is a public fact for that
day: wallets show "attestation layer arms at height …", `yed_getinfo.attest` reports
`TRIGGERED`/`armHeight`, and an operator can hold a mint until the layer is live. Arming never
reverses by itself; a release can switch the layer off with `ATTEST_REQUIRED = false` (W15).

Everyone registered on or before `triggerHeight + FOUNDING_WINDOW` (8,064 blocks ≈ a week after
the trigger) is a **founding member** and accrues age from `triggerHeight`, so nobody gains by
having registered a block earlier than the rest. Later registrants age from their own
`registerHeight`.

**Seating.** At every snapshot the `N_SLOTS` (9) eligible attestors with the greatest
`weight = bondZat · min(age, AGE_CAP)` are **seated**; a transaction's bundle draws `M_SELECT +
K_SLACK` (4 + 2) of the seated by a hash of the reference block, and at least `M_SELECT` of the
selected must be in it. Being seated, bonded and reachable is what earns; latency does not. An
unseated attestor keeps publishing, keeps its age, and takes the lowest seat when its weight passes
the holder's.

## Dormancy and revival

An attestor is **DORMANT** when it was seated for the whole of the last `DORMANCY_BLOCKS` (16,128 ≈
two weeks), was *selected* by at least `DORMANCY_MIN_BUNDLES` (20) confirmed bundles in that
window, and appeared in none of them — selected that often and never delivered (proposal §12).
During a halt or a quiet period there are no bundles and nobody goes dormant for it. A dormant
attestor is not seated and earns nothing; its age is kept.

Revival is one transaction of your own: `yed_revive <seq> <priceMicroUsd>` signs one fresh
attestation with the hot key (through the same equivocation guard), puts it in an `OP_RETURN`
(`ATTESTOR_REVIVE`), and REV-1 sets the record `ELIGIBLE` again — the only on-chain act asked of an
attestor after registration, and only after a fortnight of being selected and absent. Fix the
outage first (`poolFresh` in `yed_listattestors` on a subscriber's node tells you whether your
attestations arrive), then revive.

**Ejection** (`EJECTED`) has one cause: equivocation, the first paragraph of this document. It is
terminal for the key and the outpoint. A stolen hot key can only grief (sign wrong prices; it is
bounded like a colluding attestor) or get your seat ejected by equivocating; the bond key is
separate and stays safe if it stayed cold.

## Withdrawal

After `bondLocktime`, `yed_withdrawbond <seq> [to]` spends the bond to any address; the record
becomes `WITHDRAWN` (an ejected record stays `EJECTED`, the spend only recorded). Before
`bondLocktime` the command refuses (`bond-locked`); the coin is time-locked by consensus, not by
the overlay, so nothing shorter exists. The hot key is then free to be discarded; a withdrawn
attestor's hot key may register again.

## Quick reference

| I want to… | Command | Refusals to expect |
|---|---|---|
| register | `yed_registerattestor <bondYec> <lockBlocks> [flags]` | `bond-below-min`, `lock-below-min` |
| see my status, seat, weight | `yed_listattestors` | — |
| sign (the agent does this) | `yed_signattestation <seq> <priceMicroUsd> [citedHeight]` | `attest-key-not-held`, `equivocation-guard`, `attest-range`, `attest-stale` |
| return from dormancy | `yed_revive <seq> <priceMicroUsd>` | `not-dormant`, `equivocation-guard` |
| report someone's equivocation | `yed_reportequivocation <hexA> <hexB>` | `not-equivocation` |
| withdraw the bond | `yed_withdrawbond <seq> [to]` | `bond-locked`, `bond-spent`, `attest-key-not-held` |

Every identifier is defined in `doc/yellowback-rpc.md`.
