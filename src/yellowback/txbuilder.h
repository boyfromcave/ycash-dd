// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_TXBUILDER_H
#define YCASH_YELLOWBACK_TXBUILDER_H

#include "amount.h"
#include "key.h"
#include "keystore.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "transaction_builder.h"
#include "yellowback/script.h"
#include "yellowback/wallet.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CReserveKey;

/**
 * Yellowback transaction builder (plan D9, §3.4, §4.5). Built by hand from
 * public CWallet primitives, as z_sendmany and the atomic-swap RPCs do:
 * CreateNewContextualCMutableTransaction for version/group/expiry, a flat
 * fee (B14), smallest-first confirmed inputs (F3), SignSignature for P2PKH
 * inputs and a manual sighash for the vault input. Every builder requires
 * cs_main, cs_wallet and cs_yellowback held by the caller, in that order.
 * Failures throw std::runtime_error with the stable reason strings the RPCs
 * pass through.
 *
 * Sapling shapes (plan I2): a mint funded from a ys1… address and a
 * redemption paying its collateral to one are assembled in Ycash's
 * TransactionBuilder (proofs, binding signature) and returned unbuilt in
 * BuiltTx::builder. The caller runs FinishSapling() with NO lock held (one
 * spend proof per note, seconds each), then re-locks: a mint is complete, a
 * redemption still needs SignRedeem(). The transparent shapes never touch
 * the builder.
 */
namespace yellowback {

struct BuiltTx
{
    CMutableTransaction tx;
    std::vector<COutPoint> ownYedOutputs;   //!< outputs to lock before CommitTransaction (stage i)
    std::set<COutPoint> yedInputs;          //!< YED outpoints consumed
    CPubKey freshKey;                       //!< the key drawn for this transaction (owner/token/collateral)
    std::string warning;
    // Sapling shape (I2): set by BuildMint/BuildRedeem, consumed by FinishSapling
    std::optional<TransactionBuilder> builder;
    bool isMint;
    std::string fundedFrom;                 //!< MINT: "transparent" | "sapling"
    std::string collateralTo;               //!< REDEEM: the destination address as given or drawn
    // MINT
    int evalHeight;
    uint32_t lockHeight;
    CAmount collateralZat;
    // REDEEM
    int64_t requiredBurn;
    int64_t burnCents;
    int64_t changeCents;
    int changeVout;                                        //!< index of the YED change output, -1 if none
    CScript vaultScript;                                   //!< signing material for SignRedeem
    CAmount vaultValue;
    CPubKey ownerPubKey;
    std::vector<std::pair<CScript, CAmount>> yedPrevs;     //!< scriptPubKey/value of vin[1..] (YED inputs)

    BuiltTx() : isMint(false), evalHeight(0), lockHeight(0), collateralZat(0), requiredBurn(0), burnCents(0), changeCents(0), changeVout(-1), vaultValue(0) {}
    bool NeedsProving() const { return builder.has_value(); }
};

/**
 * `from` (I2): "" = any confirmed transparent output; an s1… address = that address's
 * outputs only; a ys1… address = its Sapling notes in the same transaction (Sapling shape).
 */
/** Phase 2 shim: throws until Phase 6 lands the v2 builder (the v1 builder read deleted tier tables and rosters). */
BuiltTx BuildMint(YellowbackWallet& yw, int64_t cents, int lockBlocks, CReserveKey& reservekey, const std::string& from = "");

/** recipients: P2PKH script -> cents. At most 14 recipients (one assignment slot is kept for change). */
BuiltTx BuildTransfer(YellowbackWallet& yw, const std::vector<std::pair<CScript, int64_t>>& recipients, CReserveKey& reservekey);

/**
 * `to` (I2): "" = a fresh transparent key; an s1… address; or a ys1… address (Sapling shape).
 * Returns the transaction UNSIGNED: call FinishSapling() first for the Sapling shape (no lock
 * held), then SignRedeem() under cs_main + cs_wallet for either shape.
 */
/** Phase 2 shim: throws until Phase 6 lands the v2 builder (owner path, burn = debt, fee output, VOID release). */
BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to = "");

/** Sapling shape: run TransactionBuilder::Build() (proofs, binding signature). No lock may be held. */
void FinishSapling(BuiltTx& out);

/** REDEEM: owner-sign vin[0] over the vault script and SignSignature every YED input. Requires cs_main and cs_wallet. */
void SignRedeem(BuiltTx& out, CWallet& wallet, uint32_t branchId);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_TXBUILDER_H
