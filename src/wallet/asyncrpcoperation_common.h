// Copyright (c) 2019 The Zcash developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_WALLET_ASYNCRPCOPERATION_COMMON_H
#define ZCASH_WALLET_ASYNCRPCOPERATION_COMMON_H

#include "primitives/transaction.h"
#include "univalue.h"
#include "wallet.h"

#include <optional>
#include <utility>
#include <vector>

/**
 * Sends a given transaction.
 * 
 * If testmode is false, commit the transaction to the wallet, 
 * return {"txid": tx.GetHash().ToString()}
 * 
 * If testmode is true, do not commit the transaction,
 * return {"test": 1, "txid": tx.GetHash().ToString(), "hex": EncodeHexTx(tx)}
 */
UniValue SendTransaction(CTransaction& tx, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode);

/**
 * Sign and send a raw transaction.
 * Raw transaction as hex string should be in object field "rawtxn"
 * 
 * Returns a pair of (the parsed transaction, and the result of sending)
 */
std::pair<CTransaction, UniValue> SignSendRawTransaction(UniValue obj, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode);

/*
 * Post-quantum (TX_PQPKH) inputs of the z_* TransactionBuilder operations (beta2 D-2, D-6; reverses quantum
 * spec A-15). TransactionBuilder::Build() signs P2PKH inputs only, without the vault script flags, so a PQ
 * coin is added with AddTransparentInputUnsigned(), the Sapling parts are proved with no lock held, and
 * SignPQBuilderInputs() signs the PQ inputs afterwards (scriptSigs are outside the ZIP-243 digest the Sapling
 * spend and binding signatures cover), as the Yellowback mint builder does (FinishSapling, SignBuiltInputs).
 */
struct PQBuilderInput {
    size_t nIn;              //!< the input's index in the built transaction (Build() keeps the order of vin)
    CScript scriptPubKey;    //!< the TX_PQPKH script of the coin
    CAmount value;
};

/**
 * The shape of the transaction a builder operation will produce, for the fee and the sigop check before
 * anything is proved: every transparent input signed with dummy signatures of the real size (the
 * DummySignatureCreator CWallet::CreateTransaction prices with), the transparent outputs, and default-sized
 * Sapling spend and output descriptions. cs_main and pwalletMain->cs_wallet held.
 */
CTransaction PQFeeShape(int nHeight, const std::vector<std::pair<CScript, CAmount>>& tIns,
                        const std::vector<CTxOut>& tOuts, size_t saplingSpends, size_t saplingOutputs);

/**
 * The fee a transaction with a post-quantum input pays, the rule CWallet::CreateTransaction applies
 * (quantum plan §4.5, D-Q-23 (b), D-Q-26; audit 04/N-1): max(min(max(base, PQSizeFee(DEFAULT_FEE, size)),
 * PQMaxTxFee()), the ZIP-317 conventional fee). `base` is the fee the RPC would charge without a PQ input
 * (its default, or the caller's explicit fee); `tx` carries scriptSigs of the real size (PQFeeShape or signed).
 */
CAmount PQBuilderFee(CAmount base, const CTransaction& tx);

/** The most post-quantum inputs one standard transaction carries: each OP_CHECKPQSIG costs pq::SIGOP_COST policy
 *  sigops, MAX_STANDARD_TX_SIGOPS in all, less the legacy sigops of `shape` (its outputs). */
size_t MaxPQInputs(const CTransaction& shape);

/**
 * Signs the post-quantum inputs of a built transaction under the vault script flags of nHeight (the height the
 * builder was made for; quantum spec §5.2) and that height's branch ID. Takes cs_main and pwalletMain->cs_wallet;
 * throws RPC_WALLET_UNLOCK_NEEDED for a locked wallet and RPC_WALLET_ERROR when a signature cannot be made.
 */
void SignPQBuilderInputs(CMutableTransaction& mtx, const std::vector<PQBuilderInput>& ins, int nHeight);

#endif // ZCASH_WALLET_ASYNCRPCOPERATION_COMMON_H
