# Ycash Yellowback (YED) — user guide

Ycash Yellowback (YED) is an over-collateralised US-dollar token on Ycash: lock YEC in a vault to
mint YED (`1 YED = 1 US dollar`), return the YED to get the YEC back. It is built on the **vault
upgrade**, a proposed Ycash network upgrade (`Consensus::UPGRADE_VAULT`, consensus branch ID
`0x6d5b7a31`, "Vault") that adds vaults and bonded signer sets to Ycash's consensus rules.
Yellowback is the upgrade's one registered rule module (tag `YED\0`): from the activation height
every upgraded node checks its rules as consensus rules, and a node that does not upgrade stops
following the chain there. The same vault feature also carries the wYEC bridge template, a
separate application with no module and nothing to do with YED.

**Status: proposed, not live.** Mainnet and testnet have no `UPGRADE_VAULT` activation height and
no YED attestor set; Yellowback runs on regtest and the one-laptop devnet only.

Below the activation height, and on a network that names no YED attestor set, nothing of
Yellowback applies and the `yed_*` commands do not exist; for everything Yellowback touches the
node behaves as the Ycash v4.5.0 baseline. YED's own records are ordinary transparent Ycash v4
transactions, one `OP_RETURN` payload, and a self-contained index under `<datadir>/yellowback/`.

Where to read further: the vault primitive and its `set_*` / `vault_*` RPCs are `doc/vault-rpc.md`;
the `yed_*` RPC surface is `doc/yellowback-rpc.md`; what a mining pool runs is
`doc/yellowback-mining.md`; what an attestor runs is `doc/yellowback-attestor.md`; how to bring a
local devnet or a regtest node up in a few minutes is `doc/yellowback-devnet.md`. The normative
specification of this line is §15 of the workspace's `docs/plans/yellowback-upgrade-plan.md`
(§15.10 for YED). `doc/yellowback-spec.md` is the generated protocol text of the soft-fork design
(`harden/yellowback`): its MINT, TRANSFER, redemption, price and attestation rules are the ones the
YED module keeps, as amended by §15.10, and its activation, signalling, sunset and abandonment
rules do not apply here. This file is the user-facing guide to the node and its wallet commands.

## How it works

- **A Yellowback is a vault.** `yed_mint` locks YEC in the vault primitive's **V template**, with
  the YED attestor set as its signer set, `CLAIM_DELAY` as its delay, the lock height as its owner
  height and the claim height (lock height + `GRACE`) as its application height. It issues the
  minted YED to the minter as a transparent output carrying a small `OP_RETURN` payload. The
  attestor set never spends a YED vault: its only power over one is to cancel a pending claim.
- **YED moves like YEC.** `yed_send` builds an ordinary transparent transaction whose payload
  assigns cents to outputs; every Yellowback node keeps the same ledger of who holds what
  (`yed_getbalance`, `yed_listunspent`). Spending a YED output with a plain YEC command **burns**
  the YED it carries; the wallet locks its YED outputs so that cannot happen by accident.
- **Redemption burns the debt.** `yed_redeem` at or after the lock height spends the vault on its
  owner branch, burns the vault's minted cents, pays the pool fee and returns the collateral. A
  vault spend without that burn is an invalid transaction.
- **Underwater vaults can be claimed, after a delay.** From the claim height anyone holding enough
  YED can `yed_claim` a vault whose collateral is worth less than its debt at the claim price
  (`yed_listclaimable` lists them). The claim burns the debt and moves the collateral into a
  **pending release** (an intent) paying the claimant; any remainder above the claimant's share
  goes into a second one paying the owner. The vault is `CLAIMING` for `CLAIM_DELAY` blocks
  (1,152, one day, on mainnet; 10 on regtest), during which the YED attestor set can cancel the
  claimant's intent; then anyone releases it (`vault_release`) and the vault is `CLAIMED`.
- **Prices come from pools and attestors.** Every pool running the module tags its coinbase with a
  YEC/USD quote; bonded attestors sign prices off-chain. A mint is sized at the lower of the two
  and a claim opens at the higher (*Where the price comes from*, below). Minting is refused while
  any halt holds (`yed_getstats.mintingAllowed`, `halts`).
- **The rules are consensus.** From the `UPGRADE_VAULT` height, on a network that names a YED
  attestor set, a transaction that breaks a Yellowback rule is refused by every mempool and a block
  carrying one is rejected by every upgraded node, whoever mined it. `yed_getactivation` reports
  the upgrade.
- **Fees pay the pools.** Mints, redemptions and claims pay a **pool fee** (the larger of a flat
  minimum and a few basis points of the collateral) to a pool that published a quote in the
  `payeeWindow` blocks up to the transaction's reference height (100 on mainnet, 10 on regtest);
  the wallet picks the payee, avoiding pools whose quotes strayed from their peers'.

## Where the price comes from

Every rule that matters reads a YEC/USD price: how much collateral a mint needs, when a vault
becomes claimable. Two independent populations supply it.

- **Pools.** Each pool's coinbase tag carries its quote. The pool price for a mint is the minimum
  of three rolling medians (8, 24 and 64 blocks on regtest; longer on mainnet), the claim price the
  maximum of the two longer ones. No price is defined until enough blocks in a window carry quotes.
- **Attestors.** An attestor is a member of the **YED attestor set**, a signer set of the vault
  primitive: it joins with a bonded `SET_JOIN` (`yed_registerattestor` does that with a fresh
  wallet key), stays live with periodic `SET_HEARTBEAT`s, and signs prices with an off-chain agent
  (`contrib/yellowback/attest/`). It needs no domain, no open port and only the node's own wallet.
  The highest-weighted bonds are *seated*; weight is bond size times age, so influence is slow,
  visible and costly to buy. An attestor that signs two different prices for one height is ejected
  and its bond frozen in the set.
- **Arming.** The attestation layer switches on by itself: once seven attestors have matured bonds
  on mainnet (`ATTEST_ARM_MIN`; three on regtest), a one-day countdown starts (`yed_getinfo.attest`
  shows `UNARMED` / `TRIGGERED` / `ARMED`). **Minting requires an armed layer** on mainnet and
  testnet (`MINT_REQUIRES_ARMED`; `yed_getinfo.mintRequiresArmed`): until it arms every mint is
  refused (`mintpol-unarmed`), and a hand-built one is invalid. Redemptions, transfers and claims
  do not wait for it. Regtest mints unarmed unless `-yellowbackmintrequiresarmed` is set.
- **What you see as a minter.** Once armed, `yed_mint` and `yed_claim` are **two transactions**:
  the wallet first publishes a small *carrier* output that commits to the attestations it will
  use, waits one block, then sends the mint or claim that spends it. The wallet does this for you;
  the GUI shows "preparing price proof (1 block)". You pay one extra small output and an attestor
  fee on top of the pool fee — half of it on mainnet (`ATTEST_FEE_BPS` 5,000), a quarter on regtest
  — which goes to an attestor whose signature you used.
- **The combination.** Mints size at the **lower** of the two sources and claims open at the
  **higher**, so each operation is priced against whichever population the transacting party
  controls least. If the two disagree by more than 15 %, minting pauses rather than guessing.
- **If attestations are unavailable**, minting refuses with `bundle-insufficient` and nothing else
  changes: YED still moves, redemptions still work, and existing vaults are untouched.

Attestor `seq`s are a 16-bit counter that never reuses a number: after 65,536 registrations no
further attestor can be registered, so the attestation layer would be exhausted permanently. With
`BOND_MIN` at 20,000 YEC and a one-year lock the concurrent set is bounded near supply / 20,000
(about 1,000), so exhaustion takes decades; the bound is a parameter-review item if `BOND_MIN`
ever drops, not a run-time concern. The per-block scan of every attestor record is linear in that
same count.

## Configuration

No flag turns Yellowback on or off. It is live wherever `UPGRADE_VAULT` has an activation height
and the network names its YED attestor set: on mainnet and testnet both are compiled in by the
release that activates it (neither is set yet); on regtest they are
`-nuparams=6d5b7a31:<h> -yellowbackattestorset=<setid>`. The set has to exist before the second
flag can name it, so a regtest node starts with the vault upgrade alone, creates the set with
`set_create` once the chain is past `<h>`, and restarts with `-yellowbackattestorset`
(`doc/yellowback-devnet.md` §2 walks through it). Without the attestor set the `yed_*` commands
are not registered.

Where Yellowback is live the node refuses `-prune` (the index rebuilds from blocks on disk), and a
node whose index is unhealthy stops rather than validate blocks without it (*Rebuilding the
index*). Options:

- `-reindex-yellowback` — wipe and rebuild the index on startup;
- `-yellowbackfee=<zat>` — the network fee of every Yellowback transaction (minimum 1,000);
- `-yellowbackmintlag=<blocks>` — how far below the tip a mint is evaluated (default 2);
- `-yellowbackpreferredpayee=<s1…>` — where this wallet's own transactions pay their pool fee when
  that pool is eligible; `-yellowbackpayeepenaltyblocks`, `-yellowbackpayeeaccuracywindow` and
  `-yellowbackpayeetiltbps` tune the payee choice;
- `-yellowbackpreferredattestor=<seq>` — the attestor this wallet pays its attestation fee to when
  it is in the bundle;
- `-yellowbackcarriertimeout=<sec>` — how long a waiting `yed_mint` / `yed_claim` blocks for its
  carrier to confirm;
- `-debug=yellowback` (and `-debug=vault` for the primitive).

The mining-side options (`-yellowbackpayoutaddress`, `-yellowbackquotemaxage`) are in
`doc/yellowback-mining.md`. Regtest additionally takes `-yellowbacksigmaref`,
`-yellowbacksupplycapbps`, `-yellowbackattestarmmin`, `-yellowbackmintrequiresarmed`,
`-yellowbackbundlecarrier` and `-yellowbacktestfault`; each is refused on mainnet and testnet.

**Flags from earlier designs.** `-yellowback` is accepted and ignored (`-yellowback=0` only
acknowledges a leftover index, see below), and `-experimentalfeatures` does not gate Yellowback.
`-yellowbackenforce`, `-yellowbacksignal`, `-yellowbacktemplatepolicy` and
`-yellowbackrequirehealthy` are logged and ignored. `-yellowbackstartheight` and
`-yellowbackenforceuntil` stop the node at startup with an error: YED starts at the
`UPGRADE_VAULT` activation height and has no end height.

## Using Yellowback from `ycash-cli`

Every amount is in **cents** (`10000` = $100.00); prices are in micro-USD per YEC (`2000000` =
$2.00). The node must have caught its index up (`yed_getinfo` → `healthy: true`, `height` at the
chain tip) before any of these work.

```
ycash-cli yed_getinfo                       # index height, healthy, upgrade, attest, miner, params
ycash-cli yed_getstats                      # supply, collateral, prices, halts, mintingAllowed
ycash-cli yed_getprice                      # the three medians and their fill
ycash-cli yed_getactivation                 # the vault upgrade: status, activationHeight, attestorSetId, claimDelay
ycash-cli yed_getnewaddress                 # a YED address (ye… on mainnet)
ycash-cli yed_getbalance
ycash-cli yed_estimatecollateral 10000 48   # YEC needed now to mint $100.00 with a 48-block lock (class A on regtest)
ycash-cli yed_mint 10000 48                 # mint; back up wallet.dat afterwards
ycash-cli yed_mint 10000 48 ys1…            # the same, funded from that Sapling address in one transaction
ycash-cli yed_listpositions                 # your vaults: status, lockHeight, claimHeight, canRedeem, intents
ycash-cli yed_send ye… 2500                 # send $25.00
ycash-cli yed_redeem <vaultTxid>            # burn the debt, pay the pool fee, take the collateral back
ycash-cli yed_listclaimable                 # underwater vaults past their claim height
ycash-cli yed_claim <vaultTxid>             # claim one with your own YED (a pending release for CLAIM_DELAY)
ycash-cli vault_release <intentTxid> 0      # after CLAIM_DELAY: pay the claimant's intent out
ycash-cli yed_listtransactions
```

A mint commits to a reference height two blocks below the tip; the collateral requirement is fixed
there (it is what `yed_mint` reports). A mint whose reference snapshot is not active, or whose
collateral is short at that snapshot, is an invalid transaction: every mempool refuses it
(`bad-yellowback-<verdict>`) and no block may carry it. The wallet refuses to build such a mint in
the first place (`mintpol-*` identifiers in `doc/yellowback-rpc.md`).

**The supply cap is soft.** YED supply is capped at `SUPPLY_CAP_BPS` (15 %) of YEC's issued market
cap, but reaching the cap is read as a sign that demand for YED is strong relative to YEC, not as
a stop: above it a mint is accepted iff it is class A and the ratio it locks — the class minimum
times the volatility multiplier — is at least `RECAP_RATIO_BPS` (600 % on mainnet; 500 % on
regtest, where class A always qualifies). Every YED minted above the cap locks at least that
multiple of its value in YEC, the buffer wanted if the market cap corrects.
`yed_getinfo.supplyCapReached` says the cap is reached, `yed_getstats.mintableClasses` whether
class A still mints, and the wallet's `mintpol-cap` refusal says so.

**Launch parameters.** On mainnet and testnet only class A (30–90 days) is enabled: classes B and
C carry an empty term range (`minBlocks > maxBlocks` in `yed_getinfo.params.classes`) and every
lock length in them is refused (`mint-bad-lock`; invalid if hand-built). The largest single mint
is $2,500 (`MAX_MINT`), the pool fee 15 bps of the collateral (`FEE_BPS`, minimum 0.5 YEC) with
half of it again to the attestor (`ATTEST_FEE_BPS` 5,000), the global-ratio halt 300 % and the
recapitalisation floor 600 %. Regtest uses 25 bps, 2,500 bps, $10,000 and 250 % / 500 %, with all
three classes.

Never spend a YED output with a plain YEC command: the YED it carries is burned. The wallet locks
every YED output it owns (`listlockunspent` shows them) so `sendtoaddress` and friends cannot pick
them by accident; `lockunspent true` on one of them removes that protection.

## What the consensus rules guarantee

- **Every YED rule is a consensus rule** at every height where `UPGRADE_VAULT` is active on a
  network that names the YED attestor set: a block that spends a YED vault against the rules, or
  mints YED it may not, is invalid on every upgraded node, whoever mined it. The rules hold without
  any pool's participation, in initial block download, during a reindex and at every height after
  activation.
- **The owner branch.** The vault's owner redeems with the V template's owner branch (selector 2)
  from the lock height, burning the debt. If the YED attestor set goes dormant or winds down, the
  primitive treats it as *released* and also opens selector 3 to the owner, at any height; the
  module applies the same redemption rules to it, so the debt is still burned. `yed_redeem` builds
  the selector-2 redemption.
- **The claim branch.** From the claim height a claim (selector 4) burns the debt and moves the
  collateral into the claimant's intent, plus the owner's residual intent when one is due. Until
  `CLAIM_DELAY` has passed, the YED attestor set may cancel the claimant's intent
  (`vault_buildcancel` + `set_signcancel` + `vault_send`): the vault is re-created at the cancel's
  output 0 as the same `ACTIVE` position, and the claim's burn is not refunded. After the delay
  anyone may release it. The owner's residual intent is never cancelled; anyone may release it to
  the owner after the delay.
- **No other way out.** A vault has no anyone-can-spend path: collateral leaves only by the owner
  branch or by a claim that burns the debt.

## Rebuilding the index

The index lives under `<datadir>/yellowback/` and is rebuilt from the blocks on disk when it is
missing, when the node was reindexed, or on `-reindex-yellowback`. `yed_getinfo.healthy: false`
names the reason (`unhealthyReason`) and always means "restart with `-reindex-yellowback`". Every
`yed_*` call except `yed_getinfo`, `yed_getblockverdict`, `yed_gettag`, `yed_decodepayload` and
`yed_setquote` refuses while the index is unhealthy or behind the chain tip. Because the module's
verdict is consensus, a node whose index is unhealthy stops (`AbortNode`) rather than validate
blocks without it; restart with `-reindex-yellowback`.

## How your YED is protected from being spent as plain YEC

A YED output is an ordinary 10,000-zatoshi P2PKH output; the dollars it carries live in the
Yellowback index, not in the output. Spend it with any command that does not know about Yellowback
and the YED is destroyed while the 10,000 zatoshi survive. The wallet therefore:

- **locks every YED outpoint it owns** (before it broadcasts, when it sees the transaction, and
  again after every block and at startup), so `sendtoaddress`, `sendmany` and `z_sendmany` never
  select one. `yed_getinfo.lockedOutputs` says how many are held and `yed_lockcoins` re-runs the
  reconciliation if `yed_listunspent` ever shows more coins than that;
- **refuses `lockunspent`** for one of those outpoints (`yed-locked-outpoint`), and re-applies its
  own locks after `lockunspent true` unlocks everything. The deliberate way out is
  `yed_unlockcoin "<txid>" <n> "I understand this burns YED"`;
- **refuses `sendrawtransaction`** for a raw transaction that spends one of them without a payload
  that reassigns it, unless you pass the third argument `allowyedburn` as `true`;
- **refuses to start with a wallet where Yellowback is not live** when the datadir holds a
  Yellowback index — for example a regtest node restarted without `-yellowbackattestorset`. Start
  it with the network's attestor set to keep those outputs locked, or with `-yellowback=0` to say
  you accept that the YED outputs in this wallet are spendable as plain YEC for this run
  (`-disablewallet` needs neither);
- **re-locks after an import**: `importprivkey`, `importaddress`, `importwallet` and `z_importkey`
  reconcile once their rescan is done, so YED that has just become yours is locked at once.

**What is not protected (documented, not enforced).** These are real ways to lose YED and no
software here can prevent them:

- **Keys used elsewhere.** A private key exported from this wallet and imported into any other
  Ycash wallet, a hardware signer or a script: that software has no Yellowback index, its coin
  selection sees an ordinary 10,000-zatoshi output, and the first transaction it builds burns the
  YED.
- **Other Yellowback nodes.** Another node that holds the same keys but is not this wallet locks
  nothing of yours until it reconciles; two wallets sharing keys can each build a spend the other
  does not know about.
- **Sending YED to someone whose wallet does not know Yellowback.** The `ye…`/`yt…`/`yr…` address
  prefix is the only technical guard: an address that decodes to the same key hash spells the
  same output. If the recipient's wallet does not run Yellowback, the YED you sent is theirs to
  burn by accident. Ask before sending.
- **Restoring an old `wallet.dat`.** A backup taken before a mint does not contain that vault's
  owner key, and the collateral cannot be redeemed without it (see *Backups*).

The rule of thumb: YED lives in the node that owns the keys **and** runs Yellowback. Keep it in one
place, and treat any export of a key as an export of the dollars with it.

## Sending an amount the wallet refuses

`yed_send` refuses (`change-floor`) when no selection of your YED coins leaves change of either
nothing at all or at least $1.00 — the minimum a payload can assign to an output. The message
names the nearest amounts that do work, below and above, and `yed_estimatesend <cents>` shows the
same thing before you commit to it, along with the inputs it would spend and the change it would
leave. A redemption or a claim does not refuse: it burns the sub-dollar remainder instead
(at most $0.99, reported as `extraBurnCents`) rather than leave the vault stranded.

## Backups

Ycash transparent keys are a random keypool, not derived from a seed. The vault owner key of every
mint lives only in `wallet.dat`. **Back up `wallet.dat` after every mint.** Wallet encryption in
Ycash is experimental; protect the file with full-disk encryption, keep RPC on localhost and hold an
offline copy.

## History

Earlier designs: Yellowback began as a federation prototype, being replaced from 2026-09-10 by a
design in which mining pools enforce the vault rules as a soft fork; the branch
`feature/digidollar` keeps the prototype's record and is never built on. The `harden/yellowback`
branch keeps that soft-fork version, which needs no network upgrade. The design history is in the
project workspace's plans.

> **Note on the trust statement below.** It is the CI-checked, byte-identical copy of
> `doc/yellowback-spec.md` §8.1 and describes the soft-fork design of `harden/yellowback`
> (pool enforcement, signalling, the sunset, abandonment and `yed_sweep`), **not** this line. This
> line's trust statement is §10 of the workspace's `docs/plans/yellowback-upgrade-plan.md`; the
> copy below changes when the spec generator takes its text from there.

## Trust statement

Yellowback v2 is a miner-enforced, over-collateralised stablecoin overlay on Ycash.

- Consensus-enforced (by every Ycash node, upgraded or not): collateral cannot leave a vault
  before its lock height; before the claim height only the minter's key can spend it.
- Enforced by every Yellowback-aware node deterministically: Yellowback accounting (conservation,
  supply, collateral totals, vault status, prices, activation, halts).
- Enforced by the mining pools that run the Yellowback module, and effective for the whole
  network once a supermajority of blocks signal: collateral is released only against the burn of
  the vault's debt; an underwater, abandoned vault can be claimed only by burning that debt; both
  pay a fee to a pool that published a price quote in the 100 blocks up to the transaction's
  reference height.
- Prices are the medians of the quotes pools publish in their own blocks; moving them needs a
  majority of *quote-tagged* blocks over a window, which is a majority of hashpower only when
  most blocks carry quotes — so the windows that decide claims are undefined until two-thirds of
  their blocks carry quotes, and the mint window until half do. A pool whose quotes stray from
  its peers' loses the fee income that wallets' default payee choice would otherwise send it.
- **Therefore:** a majority of hashpower that runs the module and follows it makes the rules
  hold; a majority that does not — or a minority that the trailing signal count mistakes for a
  majority, since the count is self-reported — could release collateral without burns or, with
  enough quote-tagged blocks, move the price; the same majority could already reorganise the
  chain. No operator, committee or key other than the minter's can move collateral before the
  claim height; after it, only a burn of the vault's debt can. No pool can move a user's YED or
  take collateral before the grace period; a pool can create YED only by first moving the mint
  price with a majority of quote-tagged blocks.
- An enforcing node that finds itself on the minority side of a split — a rejected chain that
  outruns its own by six blocks — stops enforcing for the session, rejoins the network's chain,
  raises an alert and waits for its operator; it is never stranded for more than six blocks, and
  it never bans the peers that relayed the other chain, neither for the rejected block nor for
  its descendants. A node that catches up after an outage *usually* does not reject a block the
  network has already built six blocks on: when the network's headers reach it before the block
  does — the ordinary case, since headers lead blocks — it accepts the block, records that it
  did, and keeps enforcing. When the block arrives first it can still reject it, and the work
  valve above is then what bounds the consequence: the node rejoins within six blocks. The valve,
  not catch-up suppression, is the guarantee. Enforcement means "majority in fact", not "majority
  by count".
- Every release enforces only until a sunset height about a year past its start; past it the
  node keeps publishing quotes and accounting but rejects nothing until upgraded, so two
  releases with different rules can never both be enforcing.
- As with any soft fork, every pool — participating or not — should run the module at least in
  filter-only mode, or its blocks can be orphaned by rule-breaking transactions it cannot see.
- Signalling is announced only once the pools running the module are diverse enough that no one
  of them decides alone (at least three independent pools, none above 40 % of quoting blocks,
  measured and published before the announcement).
- If pools stop participating: below 60 % of blocks signalling, minting pauses; below 50 %, block
  rejection pauses as well, and while it is paused neither the owner path nor the claim path is
  policed — collateral can leave a vault without its burn and YED so unbacked stays in
  circulation; vaults untouched during the pause are protected again when it ends. Minting resumes
  at 75 % and rejection at 60 %. Existing YED always remains redeemable by a minter who holds it.
- If the module is abandoned — rejection paused continuously for `ABANDON_BLOCKS` (about 30
  days on mainnet, v3 W21), which is also where a sunset with no successor release ends up —
  every vault's claim path becomes spendable by
  anyone at its claim height: owners must sweep their collateral before that height
  (`yed_sweep`, which every node of every release offers under that one same condition, and
  whose transaction every node then relays and mines like any other) or lose it to whoever
  claims first; the claim path stays open to everyone, so the race is fair, but the YED minted
  against a swept or claimed vault is unbacked from then on. A failed mint's collateral (a VOID
  vault, which never carried a debt) is released by its owner with `yed_redeem` at its lock
  height at any time, abandonment or not.


v2's paragraph "price honesty rests on the honest-majority-hashpower assumption" becomes:

> Prices come from two populations that cannot forge each other: mining pools, weighted by
> blocks, and bonded attestors, weighted by bond and age. A mint is sized at the lower of the
> two; a claim opens at the higher. Moving a price in the direction that pays therefore needs a
> majority of hashpower and a bond-weighted majority of the selected attestors at once. A
> hashpower majority alone keeps exactly the powers it has today — it can halt minting, delay or
> censor transactions, and reorganise the chain — and gains none. A captured attestor set alone
> can halt minting or force an early liquidation at an honest price with the remainder returned
> to the owner; it cannot take collateral. Attestors are not slashed: their penalty is ejection
> and a bond that earns nothing until it unlocks. Attestations travel outside the chain; if that
> transport fails, minting pauses and nothing else changes. Every YEC/USD price is bounded by the
> depth of the markets it is read from.
## Build and test baseline

Everything below runs from `ycash-dd/` on the branch of record, `feature/yellowback-price-attest`
(plan §6.0 item 0). The frozen-file zero-delta check measures against the tag
`yellowback-v3-baseline` (re-tagged 2026-10-02 at the security-audit merge, superseding `9da72131e`,
to carry the two reviewed frozen-file changes: A-1, the MP-1 hook after script verification; A-7, the
template tag decoded from the served coinbase), line budgets against `ycash-legacy`.
`feature/yellowback-sf` is the superseded v2 fork, kept as a record and never a comparison base;
the recorded numbers further down were measured on it. Python is always the workspace
venv (`../.venv/bin/python`), never the system interpreter.

```
# host conditions (macOS, Apple Silicon; Linux CI needs none of the three exports)
export PATH="/opt/homebrew/opt/libtool/libexec/gnubin:/opt/homebrew/opt/coreutils/libexec/gnubin:/opt/homebrew/bin:$PATH"
export CARGO_TARGET_DIR="$PWD/target"          # the Makefile links target/<triple>/release/librustzcash.a relative to the repo
LIBTOOLIZE=glibtoolize BUILD_STAGE=depends ./zcutil/build.sh -j8    # once (~25 min): the depends tree
./zcutil/build.sh -j8                                              # ycashd, ycash-cli, ycash-tx, src/test/test_bitcoin
./zcutil/fetch-params.sh                                           # needs GNU sha256sum
# after a path change (workspace rename): configure bakes absolute depends paths, so reconfigure, then incremental
CONFIG_SITE="$PWD/depends/$(ls depends | grep -m1 darwin)/share/config.site" ./configure && make -C src -j8 test/test_bitcoin ycashd ycash-cli
make -C src -j8 ycash-cli                                          # after EVERY src/rpc/client.cpp change (conversions live in the CLI)

# unit tests
src/test/test_bitcoin --run_test='yellowback_*'
# one functional script (distinct --portseed per concurrent run; --nocleanup --noshutdown keeps the playground)
BITCOIND="$PWD/src/ycashd" ../.venv/bin/python -u qa/rpc-tests/yellowback_index.py --srcdir="$PWD/src" --tmpdir=/tmp/yb-index --portseed=11
# the suite by name (the runner does not glob; scripts are registered in BASE_SCRIPTS / EXTENDED_SCRIPTS)
qa/pull-tester/rpc-tests.py -j4 --nozmq yellowback_index yellowback_activation
# the Python owner-path signer (build_vault_spend_raw) loads libcrypto through ctypes; on macOS it aborts (exit 134) without:
export DYLD_LIBRARY_PATH="$(brew --prefix openssl@3)/lib"
# devnet (§5)
../.venv/bin/python contrib/yellowback/devnet/yellowback-devnet up && ../.venv/bin/python contrib/yellowback/devnet/yellowback-devnet check

# fuzz (Ycash's own harness: --enable-fuzz-main replaces main(); zcutil/clean.sh removes src/fuzz.cpp, confirming the layout)
# fuzzer-no-link at configure time: -fsanitize=fuzzer links libFuzzer's main(), and configure's own "C++ compiler works" program has one ("cannot create executables")
CONFIG_SITE="$PWD/depends/<triple>/share/config.site" ./configure --enable-fuzz-main CC=clang CXX=clang++ CXXFLAGS='-fsanitize=fuzzer-no-link,address' LDFLAGS='-fsanitize=address'
ln -sf fuzzing/YellowbackEvaluate/fuzz.cpp src/fuzz.cpp && make -C src -j8 ycashd ycashd_LDFLAGS='$(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS) -fsanitize=fuzzer,address'
src/ycashd src/fuzzing/YellowbackEvaluate/output src/fuzzing/YellowbackEvaluate/input -max_len=4096   # libFuzzer; corpus in, findings out
../.venv/bin/python src/test/gen_yellowback_corpus.py --check                                          # embedded C++ corpus == input/ files
```

A failed `make` leaves the old `test_bitcoin` in place, so "tests pass" after a failed build means
nothing: check the make exit code. The `build_vault_spend_raw`, `yellowback_activation`, devnet
`check` and fuzz lines name Phase 1–5 artefacts; on the Phase 0 tree the Python signer that needs
`DYLD_LIBRARY_PATH` is `cosign_and_submit`, and the fuzz targets are the prototype's
`src/fuzzing/Yellowback{Payload,Script}/`.

### Host notes (macOS, Apple Silicon)

Host: macOS 26 (Darwin 25.0.0), Apple clang 17, GNU make 3.81; depends toolchain
`aarch64-apple-darwin` (clang 18.1.8, rust, boost, libevent, zeromq, libsodium, utfcpp,
googletest, bdb) builds from `depends/` unchanged. `zcutil/build.sh` needs three host-side
conditions that are not fork changes: Homebrew `automake` and GNU `libtool` on the PATH
(`LIBTOOLIZE=glibtoolize`; libevent's `autoreconf` needs them), GNU coreutils' `sha256sum` for
`zcutil/fetch-params.sh` (macOS ships a BSD `sha256sum` whose flags differ), and
`CARGO_TARGET_DIR` pointed at `<repo>/target` when the shell sets a global cargo target directory.
Python 3.12+: the inherited `test_framework/mininode.py` imports `asyncore` (removed in 3.12) and
`pyblake2` (unmaintained); the workspace venv carries `pyasyncore` and a one-line `pyblake2` shim
over `hashlib.blake2b`; no framework file is changed. `grep` on this host is ugrep.

### Recorded baseline (Phase 0, 2026-09-10, node side)

Recorded from this tree (commits `6f4380028`, `e394ce7a7`, `bbb132630` on `feature/yellowback-sf`),
incremental `make -C src -j8 test/test_bitcoin ycashd ycash-cli` on the host above.

- `src/test/test_bitcoin --run_test='yellowback_*'`: **31 cases, all green** (33 before Phase 0;
  `genesis_and_prices` and `rotation_and_custody` are behind `#if 0` in
  `yellowback_state_tests.cpp` until Phase 2).
- The whole `src/test/test_bitcoin`: **453 cases, 2 failures, both pre-existing at the pin** in
  files the fork does not touch — `main_tests/subsidy_limit_test` (`nSum` off by one halving:
  `2099999981520000 != 2099999990760000`) and `rpc_wallet_tests/rpc_z_sendmany_internals`
  (two change outputs hash to the same address). 121 s. The CI `main` job runs the whole suite;
  these two must be looked at (or excluded by name) before `main` can be green there.
- Yellowback functional scripts (run one process each, `--portseed` 11–15, `BITCOIND` set,
  `DYLD_LIBRARY_PATH` for the Python co-signer): `yellowback_index`, `yellowback_lifecycle`,
  `yellowback_void_mint`, `yellowback_wallet_restore`, `yellowback_sapling` — **all five green**.
  `yellowback_reorg_stress` was still the v1 federation script and was never run by CI; it was
  deleted on 2026-10-01 (audit I-1; the ycash6 plan's P-7 retired it there first). Reorgs are
  exercised by index, lifecycle, enforcement, mining, void_mint, activation, stock_node and the
  three attest scripts.
- `DEBUG_LOCKORDER` (`--enable-debug`) aborts Ycash v4.5.0 itself on the first peer connection:
  `getpeerinfo` takes `cs_main` > `cs_vNodes` > `cs_vSend` (`rpc/net.cpp:117,68`, `net.cpp:687`)
  while `SendMessages` takes `TRY cs_vSend` > `cs_main` (`net.cpp:1741`), and `sync.cpp:132`
  asserts. All four files are byte-identical to `ycash-legacy` at those sites; every functional
  script with a peer fails the same way (only `reindex.py` passes). Reproduced locally
  2026-09-23. So the CI `lockorder` job builds with `CPPFLAGS=-DDEBUG_LOCKORDER_LOGONLY`
  (owner decision, 2026-09-23): `sync.cpp` still detects and logs every inversion, the process
  no longer aborts, and the job fails on any report that names `cs_yellowback`, a `yellowback/`
  site or a `yed_` RPC. The switch is dead code in every other build.
- What the armed detector then found (2026-09-23), all fixed in the same commit: (1) two
  fork-side inversions of the order the fork documents (N25: `cs_main` > `cs_wallet` >
  `mempool.cs` > `cs_yellowback`) — `YellowbackWallet::Reconcile` and the raw-transaction burn
  check took `cs_wallet` under `cs_yellowback`, and the wallet RPCs held `cs_yellowback` while
  building or committing, which reaches `mempool.cs` through `FetchInputs`,
  `CommitTransaction` and `AcceptToMemoryPool`; every RPC site now takes `mempool.cs` first, the
  two wallet sites take `cs_wallet` first, and `Bonds`/`HotKeys` (no callers) demand both from
  the caller. (2) An **inherited missing lock**: `AsyncRPCOperation_sendmany::find_utxos`
  (`src/wallet/asyncrpcoperation_sendmany.cpp`, not frozen) calls `CWallet::AvailableCoins`,
  which asserts `cs_wallet`, on the async worker thread with no lock at all — a wallet-map data
  race in every Ycash v4.5.0 `z_sendmany`, and under `DEBUG_LOCKORDER` a segfault (the worker's
  lock stack is NULL; `AssertLockHeldInternal` dereferenced it). `find_utxos` now takes
  `LOCK2(cs_main, cs_wallet)` as later Zcash releases do, and `sync.cpp`'s assertion helpers
  treat a NULL stack as "nothing held". Surfaced by the Sapling-funded mint in
  `yellowback_attest_wallet`, which is a `z_sendmany` underneath. (3) With those gone, two more
  pairs surfaced: `yed_getinfo` read the wallet's locked-output count (`cs_wallet`) under
  `cs_yellowback` — it now reads it under `cs_main` alone, first; and the miner's `TemplateView`
  holds `cs_yellowback` across `TestBlockValidity` (script-check queue `ControlMutex`) while
  `ConnectTip` takes `ControlMutex` before `CheckConnect`'s `cs_yellowback`. Both paths hold
  `cs_main` from their first line, so that pair cannot deadlock; `qa/yellowback-lockorder-check.py`
  allow-lists exactly it with the proof, and the structural fix (drop the view before
  `TestBlockValidity`, one line in the frozen `miner.cpp`) is an open item. The CI job runs the
  checker over every node's `debug.log`.
- Variant builds and `config.site`: the depends `config.site` assigns `CC`/`CXX` *after*
  autoconf has read the command line and nothing restores them, so `./configure CC=clang` under
  `CONFIG_SITE` silently builds with depends' clang 18 (`-target x86_64-pc-linux-gnu`,
  `-stdlib=libc++`). That clang keeps its compiler-rt runtimes under the LLVM tarball's
  `x86_64-unknown-linux-gnu` per-target directory (the mismatch `native_clang.mk` already patches
  for libc++'s `__config_site`), so `-fsanitize=…`, `--coverage` and `-fsanitize=fuzzer` link
  tests fail in configure ("linker did not accept requested flags", "Cannot enable RELRO",
  "cannot create executables"). CI links the host-triple directory to it before configuring.
- Inherited stock baseline, `qa/pull-tester/rpc-tests.py -j4 --nozmq` over the eleven scripts of
  plan §6.0 item 6, against the fork binary without `-yellowback`: **7 pass** — `mempool_reorg`,
  `mempool_tx_expiry`, `reorg_limit`, `reindex`, `wallet`, `rawtransactions`, `txn_doublespend`
  (these are the CI `STOCK_BASELINE`); **4 fail at the pin**: `getblocktemplate_proposals`,
  `getblocktemplate_longpoll` and `invalidateblock` crash `ycashd` with `SIGABRT` in
  `CChainParams::GetFoundersRewardAddressAtHeight` under `getblocktemplate`/`generate` (a stock
  regtest node activates no Ycash upgrade; every Yellowback script passes the six `-nuparams` at
  height 1 and never hits it), and `p2p-acceptblock` fails "Unrequested block from whitelisted
  peer not accepted" (Bitcoin behaviour Zcash/Ycash does not have). The four scripts, the test
  framework and `main.cpp`/`net.cpp`/`miner.cpp`/`rpc/mining.cpp`/`chainparams.cpp` are
  byte-identical to `ycash-legacy`, and the failure reproduces with the one framework fix of this
  phase reverted, so they are the pin's, not the fork's.
- `make check` cannot pass at the pin (found by the CI nightly, 2026-09-22): the inherited
  `src/test/bitcoin-util-test.py` executes `./zcash-tx`, which Ycash v4.5.0 renamed to `ycash-tx`
  (`src/Makefile.am`) without touching `src/test/data/bitcoin-util-test.json`, and with the name
  linked ten of its 22 cases still expect Zcash `t1…` addresses where Ycash prints `s1…`
  (`chainparams.cpp` `PUBKEY_ADDRESS` = 0x1C,0x28). Both files are byte-identical to
  `ycash-legacy`. CI runs the rest of `make check` by hand: `make -C src check-TESTS`
  (`test_bitcoin`, `ycash-gtest`), then `secp256k1` and `univalue` `check`.
- `python3 -m unittest contrib/yellowback/test_yellowback_price.py contrib/yellowback/test_yellowback_quote.py`: 52 tests OK (Phase 7 replaced `test_yellowback_fed.py`). `pyflakes` over
  `qa/rpc-tests/yellowback_*.py` and `yellowback_util.py`: clean.
- Consensus set (`src/consensus`, `src/script`, `src/primitives`, `src/pow`, `chainparams.cpp`,
  `wallet/wallet.{h,cpp}`, `txdb.*`, `configure.ac`): zero lines changed vs `ycash-legacy`;
  `main.cpp`, `miner.cpp`, `rpc/mining.cpp`: 0 of 40/35/35.
- The runner (`rpc-tests.py`) execs each script through `#!/usr/bin/env python3`: put the
  workspace venv's `bin` first on `PATH` or the scripts start under the system interpreter and
  fail on `import simplejson`.
- The same exec through `/usr/bin/env` (a SIP-protected binary) **drops `DYLD_LIBRARY_PATH`**, so
  under the runner the Python signer (`build_vault_spend_raw`'s `CECKey`) loads the system
  libcrypto and macOS aborts the script ("is loading libcrypto in an unsafe way"). On this host run
  a script that signs vault spends directly (`../.venv/bin/python -u qa/rpc-tests/<script>.py
  --srcdir=... --tmpdir=... --portseed=...`, as the Phase 4 record below does); the Linux CI runner
  is unaffected. Verified: `/usr/bin/env python3 -c 'import os; print(os.environ.get("DYLD_LIBRARY_PATH"))'`
  prints `None` with the variable exported.

### Recorded baseline (Phase 2, 2026-09-10, node side)

Recorded from this tree on `feature/yellowback-sf` after the Phase 2 commits (state machine v2,
view schema 2, fuzz targets), same host and build recipe as above.

- **The four wallet-flow scripts left the CI `main` job's list at Phase 2's first commit**
  (`yellowback_lifecycle`, `yellowback_void_mint`, `yellowback_wallet_restore`,
  `yellowback_sapling`; plan §6 preamble, N26): the payload is now version 2 and the state
  machine follows the v2 rules while `yed_mint`/`yed_redeem` still build v1 transactions
  (`BuildMint`/`BuildRedeem` throw "lands in Phase 6" until then), so they cannot pass. They
  return at Phase 6. `YELLOWBACK_SCRIPTS` is `yellowback_index` alone until Phases 3–5 add
  `yellowback_activation`, `yellowback_mining` and `yellowback_enforcement`.
- `src/test/test_bitcoin --run_test='yellowback_*'`: all green — `yellowback_state_tests` was
  rewritten (53 cases, every rule of plan §3.7–3.9 tagged `// Rule:`), including
  `statehash_golden_vector`, which replays the Python model's 224-block vector
  (`src/test/data/yellowback_golden.json`, a copy of `qa/rpc-tests/test_framework/
  yellowback_golden.json` that `gen_yellowback_corpus.py --check` keeps equal) and reproduces the
  pinned hash `6eb05394…8682`; `yellowback_fuzz_tests` gained `evaluate_corpus_replay` and
  `payee_corpus_replay` over the two new targets' corpora.
- Fuzz targets: `src/fuzzing/YellowbackEvaluate` (the prefix grammar of
  `src/test/yellowback_fuzz_harness.h`, four properties: apply/undo identity, overlay
  equivalence, `blockInvalid ⇒ enforcementOn` computed at `H`, `supplyCents == Σ Tokens`) and
  `src/fuzzing/YellowbackPayee` (the FEE-W pick is in `E(R)`, nullopt iff `E(R)` is empty).
  Corpora 30 + 24 seeds; the CI `nightly` job runs every target 10 minutes under libFuzzer and
  `weekly-fuzz` two hours with corpus minimisation. **The 8 CPU-hour `YellowbackEvaluate` run of
  Phase 2's exit was not run on this host** (Apple clang ships no libFuzzer runtime,
  `docs/mapping.md` §13.1); it runs on the Linux jobs.
- Removed with the prototype (§4.2, N20, N21): the v1 payload codec (version 1 is non-Yellowback,
  V23), `Payload::Price`, the tier tables, `Health`/`DcaBps`/`ErrBps`/`RequiredBurn`/
  `VolatilityBreach`, the genesis anchor and roster parameters and their regtest flags
  (`-yellowbackgenesisanchor`, `-yellowbackgenesisroster`, `-yellowbacksupplycap`; the four v2
  flags are `-yellowbackstartheight`, `-yellowbacksigmaref`, `-yellowbacksupplycapbps`,
  `-yellowbackenforceuntil`), `yed_getprice` and `yed_getprotectionstatus`. The node RPCs render
  the v2 records in a transitional shape; Phase 3 rewrites them per plan §4.5.

### Open items carried over from the prototype's Phase 0

- The `YCASH_WR=1` build (the Ycash-specific build variant) has still not been run on this host.
- The inherited `qa/pull-tester/rpc-tests.py` baseline: the eleven scripts of plan §6.0 item 6
  are recorded above (7 pass, 4 fail at the pin); the rest of `BASE_SCRIPTS` is still unrun. The
  four that fail need the six `-nuparams` on every node (the Phase 3 `qa/yellowback-wrapped-ycashd.sh`
  wrapper, or a `regtest`-wide default) before they can join the CI list.
- The two pre-existing `test_bitcoin` failures (`subsidy_limit_test`, `rpc_z_sendmany_internals`)
  make the whole-suite step of the CI `main` job red until they are fixed or excluded by name.

## Releases and continuity

A Yellowback parameter set is consensus from the `UPGRADE_VAULT` height and has no end height:
there is no sunset, no renewal release and no freeze. Changing a value is a network upgrade like
any other, coordinated through a new consensus branch ID. A defect found in the meantime is
contained by the rules themselves: a claim at a wrong price is cancelled by the YED attestor set
within `CLAIM_DELAY`, and an owner can always redeem by the owner branch.
