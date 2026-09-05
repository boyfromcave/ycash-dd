// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "ydollar/state.h"

#include "ydollar/math.h"
#include "ydollar/script.h"

namespace ydollar {

namespace verdict {
const char* const NON_YDOLLAR = "non-ydollar";
const char* const MINT_OK = "mint-registered";
const char* const TRANSFER_OK = "transfer-ok";
const char* const REDEEM_OK = "redeem-ok";
const char* const PRICE_OK = "price-recorded";
const char* const PRICE_NOT_ANCHOR = "price-not-anchor-spend";
const char* const PRICE_ROTATION = "price-rotation";
const char* const PRICE_BAD_RANGE = "bad-oracle-price-range";
const char* const PRICE_SHIELDED = "price-shielded";
const char* const SHIELDED = "yd-shielded";
const char* const COINBASE = "yd-coinbase";
const char* const MINT_NO_VAULT = "mint-invalid-no-vault";
const char* const BAD_MINT_TIER = "bad-mint-tier";
const char* const BAD_MINT_AMOUNT = "bad-mint-amount";
const char* const BAD_MINT_LOCK_HEIGHT = "bad-mint-lock-height";
const char* const BAD_MINT_LOCK_TIER_DURATION = "bad-mint-lock-tier-duration";
const char* const BAD_MINT_EVAL_HEIGHT = "bad-mint-eval-height";
const char* const BAD_MINT_EVAL_SNAPSHOT = "bad-mint-eval-snapshot";
const char* const BAD_MINT_OUTPUTS = "bad-mint-outputs";
const char* const BAD_MINT_OWNER_KEY = "bad-mint-owner-key";
const char* const BAD_MINT_VAULT_SCRIPT = "bad-mint-vault-script";
const char* const BAD_ORACLE_PRICE = "bad-oracle-price";
const char* const MINT_BLOCKED_ERR = "minting-blocked-during-err";
const char* const MINT_FROZEN = "mint-frozen-volatility";
const char* const BAD_MINT_COLLATERAL = "bad-mint-collateral";
const char* const MINT_SUPPLY_CAP = "mint-supply-cap";
const char* const BAD_MINT_TOKEN_OUTPUT = "bad-mint-token-output";
const char* const XFER_NO_INPUT = "xfer-no-ydollar-input";
const char* const BAD_XFER_AMOUNT = "bad-xfer-amount";
const char* const XFER_OVER_ASSIGNED = "xfer-over-assigned";
} // namespace verdict

namespace {

bool IsTransparentOnly(const CTransaction& tx)
{
    return tx.vJoinSplit.empty() && tx.vShieldedSpend.empty() && tx.vShieldedOutput.empty() && tx.valueBalance == 0;
}

/**
 * MINT-2..7. On success fills `vault` (ACTIVE) and returns MINT_OK; on
 * failure returns the first failing rule's reason. `vault` carries what is
 * known either way so a VOID record can be registered.
 */
const char* CheckMint(const State& st, const Params& params, const CTransaction& tx, const Payload& p,
                      unsigned int opReturnIndex, int height, VaultRecord& vault, CAmount& collateral)
{
    vault.ownerPubKey = p.ownerPubKey;
    vault.tier = p.tier;
    vault.lockHeight = p.lockHeight;
    vault.mintedCents = p.cents;
    vault.mintHeight = height;
    vault.rosterIndex = -1;
    collateral = tx.vout.empty() ? 0 : tx.vout[0].nValue;
    vault.collateralZat = collateral;

    // MINT-2
    if (!params.IsValidTier(p.tier)) return verdict::BAD_MINT_TIER;
    if ((Cents)p.cents < params.minMint || (Cents)p.cents > params.maxMint) return verdict::BAD_MINT_AMOUNT;
    if (p.lockHeight >= LOCKTIME_THRESHOLD) return verdict::BAD_MINT_LOCK_HEIGHT;
    const int64_t tierBlocks = params.tierBlocks[p.tier];
    const int64_t lockDelta = (int64_t)p.lockHeight - (int64_t)height;
    if (lockDelta < tierBlocks || lockDelta > tierBlocks + MINT_WINDOW) return verdict::BAD_MINT_LOCK_TIER_DURATION;
    if ((int64_t)p.evalHeight < params.startHeight || (int64_t)p.evalHeight > height ||
        (int64_t)height - (int64_t)p.evalHeight > MINT_WINDOW) return verdict::BAD_MINT_EVAL_HEIGHT;

    // MINT-3
    if (tx.vout.size() < 3) return verdict::BAD_MINT_OUTPUTS;
    if (!tx.vout[0].scriptPubKey.IsPayToScriptHash()) return verdict::BAD_MINT_OUTPUTS;
    if (!p.ownerPubKey.IsFullyValid() || !p.ownerPubKey.IsCompressed()) return verdict::BAD_MINT_OWNER_KEY;
    std::vector<RosterRecord> rosters = st.GetRosters();
    bool matched = false;
    for (uint32_t idx : MintableRosters(rosters, params, height)) {
        CScript vs = VaultScript(p.lockHeight, p.ownerPubKey, rosters[idx].script);
        if (vs.empty()) continue;
        if (P2SHScript(vs) == tx.vout[0].scriptPubKey) {
            vault.rosterIndex = (int32_t)idx;
            matched = true;
            break;
        }
    }
    if (!matched) return verdict::BAD_MINT_VAULT_SCRIPT;

    // MINT-4 (snapshot at evalHeight; Snapshots[H] does not exist yet while H is being applied)
    std::optional<Snapshot> E = st.GetSnapshot(p.evalHeight);
    if (!E.has_value()) return verdict::BAD_MINT_EVAL_SNAPSHOT;
    if (!E->priceDefined) return verdict::BAD_ORACLE_PRICE;
    if (E->healthPct < 100) return verdict::MINT_BLOCKED_ERR;
    if (E->mintFrozen) return verdict::MINT_FROZEN;

    // MINT-5
    std::optional<CAmount> required = RequiredCollateral(p.cents, params.tierRatioPct[p.tier], E->dcaBps, E->price);
    if (!required.has_value() || collateral < required.value()) return verdict::BAD_MINT_COLLATERAL;

    // MINT-6
    if (params.supplyCap != 0 && st.GetTotals().supplyCents + (Cents)p.cents > params.supplyCap) return verdict::MINT_SUPPLY_CAP;

    // MINT-7
    if (opReturnIndex == 1) return verdict::BAD_MINT_TOKEN_OUTPUT;

    return verdict::MINT_OK;
}

} // namespace

std::vector<uint32_t> MintableRosters(const std::vector<RosterRecord>& rosters, const Params& params, int height)
{
    std::vector<uint32_t> out;
    if (rosters.empty()) return out;
    uint32_t back = rosters.size() - 1;
    out.push_back(back);
    if (rosters.size() >= 2 && height <= rosters[back].revealHeight + params.rosterGrace) {
        out.push_back(back - 1);
    }
    return out;
}

TxLogRecord ProcessTx(State& st, const Params& params, const CTransaction& tx, int height, bool& relevant)
{
    TxLogRecord log;
    log.height = height;
    log.verdict = verdict::NON_YDOLLAR;
    relevant = false;

    const uint256 txid = tx.GetHash();
    const bool coinbase = tx.IsCoinBase();
    const bool transparent = IsTransparentOnly(tx);

    // ---- inputs (IN-1, IN-2, PRICE-1 detection)
    AnchorRecord anchor = st.GetAnchor();
    int anchorInput = -1;
    Cents ydIn = 0;
    std::vector<std::pair<COutPoint, VaultRecord>> closing;
    if (!coinbase) {
        for (size_t i = 0; i < tx.vin.size(); i++) {
            const COutPoint& prev = tx.vin[i].prevout;
            if (auto tok = st.GetToken(prev)) {
                ydIn += tok->cents;
                AssignedOutput spent;
                spent.outpoint = prev;
                spent.cents = tok->cents;
                spent.scriptPubKey = tok->scriptPubKey;
                log.spentTokens.push_back(spent);
                st.EraseKey(keys::Token(prev));
                relevant = true;
            }
            if (auto v = st.GetVault(prev)) {
                if (v->IsOpen()) {
                    closing.push_back(std::make_pair(prev, v.value()));
                    relevant = true;
                }
            }
            if (anchor.valid && prev == anchor.outpoint) {
                anchorInput = (int)i;
                relevant = true;
            }
        }
    }
    log.ydIn = ydIn;

    // IN-2: close vaults (before outputs; errBps from the previous block's snapshot, F1)
    if (!closing.empty()) {
        Totals totals = st.GetTotals();
        std::optional<Snapshot> prevSnap = st.GetSnapshot(height - 1);
        int errBps = prevSnap.has_value() ? prevSnap->errBps : 10000;
        for (auto& c : closing) {
            VaultRecord& v = c.second;
            bool wasActive = v.Status() == VaultStatus::ACTIVE;
            v.wasActive = wasActive;
            v.status = (uint8_t)VaultStatus::CLOSED;
            v.closeHeight = height;
            v.closingTxid = txid;
            v.errBpsAtClose = wasActive ? errBps : 0;
            v.requiredBurnAtClose = wasActive ? RequiredBurn(v.mintedCents, errBps) : 0;
            if (wasActive) {
                totals.collateralZat -= v.collateralZat;
                if (totals.activeVaults > 0) totals.activeVaults--;
            } else {
                if (totals.voidVaults > 0) totals.voidVaults--;
            }
            log.closedVaults.push_back(c.first);
        }
        st.Put(keys::Totals(), totals);
    }

    // ---- payload / outputs
    std::optional<FoundPayload> fp = FindPayload(tx);
    Cents ydOut = 0;      // cents assigned to outputs (XFER)
    Cents minted = 0;     // cents created (MINT)
    if (fp.has_value()) {
        relevant = true;
        log.type = (uint8_t)fp->payload.type;
        const Payload& p = fp->payload;
        if (coinbase) {
            log.verdict = verdict::COINBASE;                                   // TX-0
        } else if (!transparent && p.type != PayloadType::PRICE) {
            log.verdict = verdict::SHIELDED;                                   // TX-0
        } else if (p.type == PayloadType::MINT) {
            VaultRecord vault;
            CAmount collateral = 0;
            const char* r = CheckMint(st, params, tx, p, fp->opReturnIndex, height, vault, collateral);
            log.verdict = r;
            const COutPoint vaultOut(txid, 0);
            if (r == verdict::MINT_OK) {
                vault.status = (uint8_t)VaultStatus::ACTIVE;
                st.Put(keys::Vault(vaultOut), vault);
                TokenRecord tok;
                tok.cents = p.cents;
                tok.nValue = tx.vout[1].nValue;
                tok.scriptPubKey = tx.vout[1].scriptPubKey;
                tok.height = height;
                const COutPoint tokOut(txid, 1);
                st.Put(keys::Token(tokOut), tok);
                AssignedOutput a;
                a.outpoint = tokOut;
                a.cents = p.cents;
                a.scriptPubKey = tok.scriptPubKey;
                log.assigned.push_back(a);
                minted = p.cents;
                Totals totals = st.GetTotals();
                totals.supplyCents += minted;
                totals.collateralZat += collateral;
                totals.activeVaults++;
                st.Put(keys::Totals(), totals);
            } else if (!tx.vout.empty() && tx.vout[0].scriptPubKey.IsPayToScriptHash()) {
                vault.status = (uint8_t)VaultStatus::VOID;
                vault.voidReason = r;
                st.Put(keys::Vault(vaultOut), vault);
                Totals totals = st.GetTotals();
                totals.voidVaults++;
                st.Put(keys::Totals(), totals);
            } else {
                log.verdict = verdict::MINT_NO_VAULT;
            }
        } else if (p.type == PayloadType::TRANSFER || p.type == PayloadType::REDEEM) {
            const char* r = nullptr;
            if (ydIn <= 0) {
                r = verdict::XFER_NO_INPUT;                                    // XFER-3
            } else {
                for (const Assignment& a : p.assignments) {                    // XFER-1
                    if ((Cents)a.cents < params.minOutput || (Cents)a.cents > params.maxOutput) {
                        r = verdict::BAD_XFER_AMOUNT;
                        break;
                    }
                }
                if (!r && p.AssignedCents() > ydIn) r = verdict::XFER_OVER_ASSIGNED; // XFER-2
            }
            if (r) {
                log.verdict = r;                                               // everything burns (D18)
            } else {
                for (const Assignment& a : p.assignments) {
                    const COutPoint out(txid, a.vout);
                    TokenRecord tok;
                    tok.cents = a.cents;
                    tok.nValue = tx.vout[a.vout].nValue;
                    tok.scriptPubKey = tx.vout[a.vout].scriptPubKey;
                    tok.height = height;
                    st.Put(keys::Token(out), tok);
                    AssignedOutput ao;
                    ao.outpoint = out;
                    ao.cents = a.cents;
                    ao.scriptPubKey = tok.scriptPubKey;
                    log.assigned.push_back(ao);
                    ydOut += a.cents;
                }
                log.verdict = (p.type == PayloadType::TRANSFER) ? verdict::TRANSFER_OK : verdict::REDEEM_OK;
            }
        } else if (p.type == PayloadType::PRICE) {
            log.verdict = (anchorInput < 0) ? verdict::PRICE_NOT_ANCHOR : verdict::PRICE_BAD_RANGE; // refined below
        }
    }

    // ---- anchor move (PRICE-1, PRICE-2, PRICE-3), independent of the payload
    if (anchorInput >= 0) {
        log.anchorSpend = true;
        if (!tx.vout.empty() && tx.vout[0].scriptPubKey.IsPayToScriptHash()) {
            AnchorRecord next;
            next.outpoint = COutPoint(txid, 0);
            next.nValue = tx.vout[0].nValue;
            next.scriptPubKey = tx.vout[0].scriptPubKey;
            next.valid = true;

            // Roster reveal: the redeem script is the last push of the spending scriptSig.
            CScript redeem;
            Roster roster;
            if (ExtractRedeemScript(tx.vin[anchorInput].scriptSig, redeem) && ParseRosterScript(redeem, roster)) {
                std::vector<RosterRecord> rosters = st.GetRosters();
                if (rosters.empty() || rosters.back().script != redeem) {
                    RosterRecord r;
                    r.script = redeem;
                    r.revealHeight = height;
                    st.AppendRoster(r);
                }
            }

            const bool sameScript = (next.scriptPubKey == anchor.scriptPubKey);
            if (fp.has_value() && fp->payload.type == PayloadType::PRICE) {
                const int64_t price = (int64_t)fp->payload.priceMicroUsd;
                if (!transparent) {
                    log.verdict = verdict::PRICE_SHIELDED;
                } else if (!sameScript) {
                    log.verdict = verdict::PRICE_ROTATION;
                } else if (fp->payload.priceMicroUsd < (uint64_t)PRICE_MIN || fp->payload.priceMicroUsd > (uint64_t)PRICE_MAX) {
                    log.verdict = verdict::PRICE_BAD_RANGE;
                } else {
                    st.Put(keys::Price((uint32_t)height), price);              // last anchor spend in block order wins (B7)
                    log.priceRecorded = true;
                    log.verdict = verdict::PRICE_OK;
                }
            } else if (!fp.has_value() && !sameScript) {
                log.verdict = verdict::PRICE_ROTATION;
            }
            st.Put(keys::Anchor(), next);
        } else {
            // Custody broken: no further prices until a release with a new genesis anchor (§3.7, §8).
            AnchorRecord broken = anchor;
            broken.valid = false;
            st.Put(keys::Anchor(), broken);
        }
    }

    // ---- IN-3
    const Cents burned = ydIn - ydOut;
    log.ydOut = fp.has_value() && fp->payload.type == PayloadType::MINT ? minted : ydOut;
    log.burned = burned;
    if (burned != 0 || !closing.empty()) {
        if (burned != 0) {
            Totals totals = st.GetTotals();
            totals.supplyCents -= burned;
            st.Put(keys::Totals(), totals);
        }
        for (auto& c : closing) {
            c.second.burnedCents = burned;
            st.Put(keys::Vault(c.first), c.second);
        }
    }

    if (relevant) st.Put(keys::TxLog(txid), log);
    return log;
}

Snapshot ComputeSnapshot(const State& st, const Params& params, int height, const uint256& blockHash, Volatility& vol)
{
    Snapshot s;
    s.blockHash = blockHash;
    Totals totals = st.GetTotals();
    s.supplyCents = totals.supplyCents;
    s.collateralZat = totals.collateralZat;
    std::optional<MicroUsd> p0 = st.PriceInEffect((uint32_t)height);
    s.priceDefined = p0.has_value();
    s.price = p0.value_or(0);
    s.healthPct = Health(s.supplyCents, s.collateralZat, p0);
    s.dcaBps = DcaBps(s.healthPct);
    s.errBps = ErrBps(s.healthPct);

    // Volatility (§3.6): windows below startHeight have no prices and are not evaluated.
    std::optional<MicroUsd> p1 = height >= params.volWindowShort ? st.PriceInEffect((uint32_t)(height - params.volWindowShort)) : std::nullopt;
    std::optional<MicroUsd> p24 = height >= params.volWindowLong ? st.PriceInEffect((uint32_t)(height - params.volWindowLong)) : std::nullopt;
    if (VolatilityBreach(p0, p1, VOL_1H_BPS) || VolatilityBreach(p0, p24, VOL_24H_BPS)) {
        vol.lastBreachHeight = height;
    }
    s.mintFrozen = vol.lastBreachHeight >= 0 && height <= vol.lastBreachHeight + params.volCooldown;
    return s;
}

std::optional<std::string> ApplyBlock(StateView& view, const Params& params, const CBlock& block, int height, const uint256& blockHash, UndoRecord& undo)
{
    if (height < params.startHeight) return std::nullopt;
    undo.height = height;
    State st(view, &undo);

    if (height == params.startHeight) {
        // Genesis anchor check (A11, B9): the anchor transaction must be in this block
        // with the expected script. Anchor and Rosters[0] are initialised before any
        // transaction of the block is processed (E6).
        const CScript expected = P2SHScript(params.genesisRosterScript);
        const CTransaction* anchorTx = nullptr;
        for (const CTransaction& tx : block.vtx) {
            if (tx.GetHash() == params.genesisAnchor.hash) { anchorTx = &tx; break; }
        }
        if (!anchorTx) return std::string("genesis anchor transaction not found in block ") + std::to_string(height);
        if (params.genesisAnchor.n >= anchorTx->vout.size()) return "genesis anchor output index out of range";
        if (anchorTx->vout[params.genesisAnchor.n].scriptPubKey != expected) return "genesis anchor scriptPubKey does not match HASH160(genesisRosterScript)";

        AnchorRecord a;
        a.outpoint = params.genesisAnchor;
        a.nValue = anchorTx->vout[params.genesisAnchor.n].nValue;
        a.scriptPubKey = expected;
        a.valid = true;
        st.Put(keys::Anchor(), a);
        RosterRecord r;
        r.script = params.genesisRosterScript;
        r.revealHeight = height;
        st.Put(keys::RosterCount(), (uint32_t)0);
        st.AppendRoster(r);
        st.Put(keys::Totals(), Totals());
        st.Put(keys::Volatility(), Volatility());
    }

    for (const CTransaction& tx : block.vtx) {
        bool relevant = false;
        ProcessTx(st, params, tx, height, relevant);
    }

    // SNAP
    Volatility vol = st.GetVolatility();
    Snapshot snap = ComputeSnapshot(st, params, height, blockHash, vol);
    st.Put(keys::Volatility(), vol);
    st.Put(keys::Snapshot((uint32_t)height), snap);

    TipRecord tip;
    tip.height = height;
    tip.blockHash = blockHash;
    tip.schemaVersion = SCHEMA_VERSION;
    tip.network = params.network;
    st.Put(keys::Tip(), tip);
    return std::nullopt;
}

void UndoBlock(StateView& view, const UndoRecord& undo)
{
    for (const UndoEntry& e : undo.entries) {
        if (e.hadValue) view.Write(e.key, e.value);
        else view.Erase(e.key);
    }
}

} // namespace ydollar
