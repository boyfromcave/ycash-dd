// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_WALLET_H
#define YCASH_YELLOWBACK_WALLET_H

#include "amount.h"
#include "primitives/transaction.h"
#include "sync.h"
#include "yellowback/index.h"
#include "yellowback/view.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

class CWallet;

/**
 * The Yellowback wallet layer (plan §4.5): ownership, three-stage coin
 * locking (B4, C3), balances and positions. Uses only the public CWallet
 * API; nothing is written to wallet.dat. Zero lines in src/wallet/.
 *
 * Lock order: cs_main -> cs_wallet -> cs_yellowback. The notifier-thread
 * hooks (PreLock, Reconcile) take cs_yellowback, release it, then take
 * cs_wallet; they never take cs_main.
 */
namespace yellowback {

struct YedCoin
{
    COutPoint outpoint;
    TokenRecord token;
};

class YellowbackWallet
{
public:
    YellowbackWallet(CWallet* wallet, YellowbackIndex* index);

    /** Register the index hooks (stage ii and iii of coin locking). */
    void Attach();

    // ---- ownership
    bool IsMineScript(const CScript& scriptPubKey) const;
    bool IsMineVault(const VaultRecord& v) const;

    // ---- coins and balances (cs_wallet and cs_yellowback held by the caller)
    /** Confirmed YED outputs that are mine and not spent by an unconfirmed wallet transaction. */
    std::vector<YedCoin> SpendableCoins() const;
    /** Every YED output that is mine (for listing). */
    std::vector<YedCoin> AllCoins() const;
    int64_t ConfirmedCents() const;

    // ---- locking
    /** Stage (i): lock the given outpoints before CommitTransaction (cs_wallet). */
    void LockOwn(const std::vector<COutPoint>& outs);
    /** Stage (ii): pre-lock every output a well-formed payload assigns to a script that is mine. */
    void PreLock(const CTransaction& tx);
    /** Stage (iii): reconcile locks against the index after every applied block and at startup. */
    void Reconcile();
    /** Outpoints this layer has locked. */
    std::set<COutPoint> Locked() const;
    /** How many (H10: yed_getinfo.lockedOutputs). */
    size_t LockedCount() const;
    /** True iff this layer holds `out` locked — what lockunspent refuses to undo (H5). */
    bool IsYellowbackLocked(const COutPoint& out) const;
    /**
     * H5: give one outpoint back to plain YEC coin selection (yed_unlockcoin). Returns whether
     * this layer held it. The YED it carries burns if it is then spent outside the overlay, and
     * the next Reconcile() locks it again.
     */
    bool ReleaseLock(const COutPoint& out);
    /** H5: re-apply every lock this layer holds, after `lockunspent true` unlocked everything. */
    void ReapplyLocks();

    CWallet* Wallet() const { return wallet; }
    YellowbackIndex* Index() const { return index; }

private:
    CWallet* wallet;
    YellowbackIndex* index;
    std::set<COutPoint> ourLocks; //!< cs_wallet
};

/**
 * H7: does this raw transaction destroy YED that belongs to this wallet? True when it spends a
 * `Tokens` outpoint the wallet owns and carries no payload assigning cents to any output — the
 * state machine then burns those cents. `reason` is filled with a human sentence naming the
 * outpoints and the cents at stake. False without -yellowback, without a wallet, while the index
 * is unhealthy, and for any transaction that spends none of this wallet's YED. Takes
 * cs_yellowback and cs_wallet itself; the caller holds cs_main (sendrawtransaction does).
 */
bool YedBurnedByRawTransaction(const CTransaction& tx, std::string& reason);

/** The wallet layer, or nullptr without a wallet or without -yellowback. */
extern YellowbackWallet* g_yellowbackWallet;

} // namespace yellowback

#endif // YCASH_YELLOWBACK_WALLET_H
