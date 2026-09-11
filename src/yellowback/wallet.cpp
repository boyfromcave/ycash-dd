// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/wallet.h"

#include "script/ismine.h"
#include "util.h"
#include "wallet/wallet.h"
#include "yellowback/payload.h"
#include "yellowback/state.h"

namespace yellowback {

YellowbackWallet* g_yellowbackWallet = nullptr;

YellowbackWallet::YellowbackWallet(CWallet* wallet, YellowbackIndex* index) : wallet(wallet), index(index) {}

void YellowbackWallet::Attach()
{
    index->onSyncTransaction = [this](const CTransaction& tx) { PreLock(tx); };
    index->onReconcile = [this]() { Reconcile(); };
}

bool YellowbackWallet::IsMineScript(const CScript& scriptPubKey) const
{
    return (::IsMine(*wallet, scriptPubKey) & ISMINE_SPENDABLE) != 0;
}

bool YellowbackWallet::IsMineVault(const VaultRecord& v) const
{
    const CPubKey owner = v.OwnerKey();
    return owner.IsValid() && wallet->HaveKey(owner.GetID());
}

std::vector<YedCoin> YellowbackWallet::AllCoins() const
{
    AssertLockHeld(index->cs_yellowback);
    std::vector<YedCoin> out;
    index->View().Iterate("K", [&](const std::string& k, const std::string& raw) {
        TokenRecord t;
        if (!DeserializeRecord(raw, t)) return true;
        if (!IsMineScript(t.scriptPubKey)) return true;
        YedCoin c;
        c.outpoint = COutPoint(uint256(std::vector<unsigned char>(k.begin() + 1, k.begin() + 33)),
                               ((uint32_t)(unsigned char)k[33] << 24) | ((uint32_t)(unsigned char)k[34] << 16) |
                               ((uint32_t)(unsigned char)k[35] << 8) | (uint32_t)(unsigned char)k[36]);
        c.token = t;
        out.push_back(c);
        return true;
    });
    return out;
}

std::vector<YedCoin> YellowbackWallet::SpendableCoins() const
{
    AssertLockHeld(wallet->cs_wallet);
    std::vector<YedCoin> out;
    for (const YedCoin& c : AllCoins()) {
        if (wallet->IsSpent(c.outpoint.hash, c.outpoint.n)) continue; // spent by one of our unconfirmed transactions (D3)
        out.push_back(c);
    }
    return out;
}

int64_t YellowbackWallet::ConfirmedCents() const
{
    int64_t sum = 0;
    for (const YedCoin& c : AllCoins()) sum += c.token.cents;
    return sum;
}

void YellowbackWallet::LockOwn(const std::vector<COutPoint>& outs)
{
    LOCK(wallet->cs_wallet);
    for (COutPoint o : outs) {
        wallet->LockCoin(o);
        ourLocks.insert(o);
    }
}

void YellowbackWallet::PreLock(const CTransaction& tx)
{
    std::optional<FoundPayload> fp = FindPayload(tx);
    if (!fp.has_value()) return;
    std::vector<COutPoint> mine;
    const uint256 txid = tx.GetHash();
    if (fp->payload.type == PayloadType::MINT) {
        if (tx.vout.size() > 1 && IsMineScript(tx.vout[1].scriptPubKey)) mine.push_back(COutPoint(txid, 1));
    } else if (fp->payload.type == PayloadType::TRANSFER || fp->payload.type == PayloadType::REDEEM) {
        for (const Assignment& a : fp->payload.assignments) {
            if (IsMineScript(tx.vout[a.vout].scriptPubKey)) mine.push_back(COutPoint(txid, a.vout));
        }
    }
    if (!mine.empty()) {
        LogPrint("yellowback", "pre-locking %u output(s) of %s\n", mine.size(), txid.ToString());
        LockOwn(mine);
    }
}

void YellowbackWallet::Reconcile()
{
    // Collect under cs_yellowback, then act under cs_wallet (B15).
    std::set<COutPoint> tokensMine;
    std::set<COutPoint> release;
    int tipHeight = -1;
    {
        LOCK(index->cs_yellowback);
        if (!index->IsHealthy()) return;
        State st(index->View());
        std::optional<TipRecord> tip = st.GetTip();
        if (tip.has_value()) tipHeight = tip->height;
        for (const YedCoin& c : AllCoins()) tokensMine.insert(c.outpoint);
        std::set<COutPoint> ours;
        {
            LOCK(wallet->cs_wallet);
            ours = ourLocks;
        }
        for (const COutPoint& o : ours) {
            if (tokensMine.count(o)) continue;
            // Release only when the outpoint's transaction is confirmed in the index and assigned it no cents
            // (a VOID mint's token output, a spent token, an output that turned out non-Yellowback); never on
            // disconnect (C3). Unconfirmed or expired transactions keep their (harmless) lock.
            if (st.GetTxLog(o.hash).has_value()) release.insert(o);
        }
    }
    {
        LOCK(wallet->cs_wallet);
        for (COutPoint o : tokensMine) {
            if (!wallet->IsLockedCoin(o.hash, o.n)) wallet->LockCoin(o);
            ourLocks.insert(o);
        }
        for (COutPoint o : release) {
            wallet->UnlockCoin(o);
            ourLocks.erase(o);
        }
    }
}

size_t YellowbackWallet::LockedCount() const
{
    LOCK(wallet->cs_wallet);
    return ourLocks.size();
}

bool YellowbackWallet::IsYellowbackLocked(const COutPoint& out) const
{
    LOCK(wallet->cs_wallet);
    return ourLocks.count(out) != 0;
}

bool YellowbackWallet::ReleaseLock(const COutPoint& out)
{
    LOCK(wallet->cs_wallet);
    const bool was = ourLocks.erase(out) != 0;
    COutPoint o = out;
    wallet->UnlockCoin(o);
    return was;
}

void YellowbackWallet::ReapplyLocks()
{
    LOCK(wallet->cs_wallet);
    for (COutPoint o : ourLocks) {
        if (!wallet->IsLockedCoin(o.hash, o.n)) wallet->LockCoin(o);
    }
}

std::set<COutPoint> YellowbackWallet::Locked() const
{
    LOCK(wallet->cs_wallet);
    return ourLocks;
}

bool YedBurnedByRawTransaction(const CTransaction& tx, std::string& reason)
{
    if (!g_yellowbackWallet) return false;
    YellowbackWallet& yw = *g_yellowbackWallet;
    YellowbackIndex* index = yw.Index();
    if (!index) return false;

    // A payload that assigns cents to an output reassigns the YED; a REDEEM's burn is its own
    // rule (RED-2) and its change assignment is an assignment like any other. No payload at all,
    // or a payload with no assignments, means the YED simply disappears.
    std::optional<FoundPayload> fp = FindPayload(tx);
    const bool reassigns = fp.has_value() &&
                           (fp->payload.type == PayloadType::TRANSFER || fp->payload.type == PayloadType::REDEEM) &&
                           !fp->payload.assignments.empty();
    if (reassigns) return false;

    int64_t cents = 0;
    std::string outpoints;
    {
        LOCK(index->cs_yellowback);
        if (!index->IsHealthy()) return false;
        State st(index->View());
        LOCK(yw.Wallet()->cs_wallet);
        for (const CTxIn& in : tx.vin) {
            std::optional<TokenRecord> t = st.GetToken(in.prevout);
            if (!t.has_value()) continue;
            if (!yw.IsMineScript(t->scriptPubKey)) continue;
            cents += t->cents;
            if (!outpoints.empty()) outpoints += ", ";
            outpoints += in.prevout.ToString();
        }
    }
    if (cents <= 0) return false;
    reason = strprintf("this transaction spends %d cents of this wallet's YED (%s) and its payload reassigns none of it,"
                       " so that YED would be destroyed.", cents, outpoints);
    return true;
}

} // namespace yellowback
