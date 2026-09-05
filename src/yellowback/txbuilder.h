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
#include "yellowback/script.h"
#include "yellowback/wallet.h"

#include <map>
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
 */
namespace yellowback {

struct BuiltTx
{
    CMutableTransaction tx;
    std::vector<COutPoint> ownYedOutputs;   //!< outputs to lock before CommitTransaction (stage i)
    std::set<COutPoint> yedInputs;          //!< YED outpoints consumed
    CPubKey freshKey;                       //!< the key drawn for this transaction (owner/token/collateral)
    std::string warning;
    // MINT
    int evalHeight;
    uint32_t lockHeight;
    CAmount collateralZat;
    // REDEEM
    int64_t requiredBurn;
    int64_t burnCents;
    int64_t changeCents;

    BuiltTx() : evalHeight(0), lockHeight(0), collateralZat(0), requiredBurn(0), burnCents(0), changeCents(0) {}
};

BuiltTx BuildMint(YellowbackWallet& yw, int64_t cents, int tier, CReserveKey& reservekey);

/** recipients: P2PKH script -> cents. At most 14 recipients (one assignment slot is kept for change). */
BuiltTx BuildTransfer(YellowbackWallet& yw, const std::vector<std::pair<CScript, int64_t>>& recipients, CReserveKey& reservekey);

BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid);

/**
 * Add this node's roster signature to vin[0] of a redemption, in roster
 * order (§3.3). `vaultScript` and `vaultValue` come from the index, never
 * from the transaction (C5). Returns the number of quorum signatures now
 * present; throws if the wallet holds no roster key or already signed.
 */
unsigned int AddCosignature(CMutableTransaction& tx, const CScript& vaultScript, CAmount vaultValue, const Roster& roster,
                            const CKeyStore& keystore, uint32_t branchId);

/** Number of quorum signatures in vin[0]. */
unsigned int CountQuorumSignatures(const CTransaction& tx);

/**
 * SUB-1: `returned` equals `original` except for additional quorum
 * signature pushes in vin[0].scriptSig.
 */
bool SameExceptSignatures(const CTransaction& original, const CTransaction& returned, std::string& why);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_TXBUILDER_H
