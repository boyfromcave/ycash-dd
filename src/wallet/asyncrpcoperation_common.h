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
    COutPoint outpoint;      //!< the coin selected; SignPQBuilderInputs refuses a vin[nIn] that spends another
    CScript scriptPubKey;    //!< the TX_PQPKH script of the coin
    CAmount value;
};

/**
 * The most post-quantum inputs one z_sendmany / z_mergetoaddress transaction carries. Measured on an Apple M-series
 * laptop (2026-11-10, regtest, under load): an SLH-DSA-SHA2-128s input signs and verifies in ~0.33 s, an FN-DSA-512
 * input in ~3 ms. 100 SLH-DSA inputs are ~33 s of signing (done with no global lock held, SignPQBuilderInputs) and
 * ~0.8 MB, under half a block; policy alone would allow 199 (MAX_STANDARD_TX_SIGOPS / pq::SIGOP_COST, less the outputs).
 */
static const size_t MAX_PQ_INPUTS_PER_TX = 100;

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

/**
 * The policy signature-operation count of `shape` as the mempool counts it (main.cpp, AcceptToMemoryPool): its legacy
 * sigops, the P2SH sigops of each input whose previous output (prevScripts[i]) is P2SH (from the redeem script in its
 * dummy scriptSig), and pq::SIGOP_COST per OP_CHECKPQSIG in each previous output (GetPQSigOpCount).
 */
unsigned int PolicySigOps(const CTransaction& shape, const std::vector<CScript>& prevScripts);

/**
 * Refuse a builder transaction with more than MAX_PQ_INPUTS_PER_TX post-quantum inputs (RPC_WALLET_ERROR, "This send
 * needs …") or more than MAX_STANDARD_TX_SIGOPS policy sigops (RPC_WALLET_ERROR, "This transaction needs …"), before
 * anything is proved or signed.
 */
void EnsurePQShapeStandard(const CTransaction& shape, const std::vector<CScript>& prevScripts, size_t nPQInputs);

/**
 * Signs the post-quantum inputs of a built transaction under the vault script flags of nHeight (the height the
 * builder was made for; quantum spec §5.2) and that height's branch ID. Must be called with neither cs_main nor
 * pwalletMain->cs_wallet held, and holds neither while signing (an SLH-DSA signature takes ~0.3 s): it takes them
 * briefly before and after the signing loop to check that the wallet is unlocked, that every input is the coin
 * selected and still unspent by the wallet, and that the next block's branch ID and vault flags are still those of
 * nHeight (a reorg or an upgrade boundary while proving or signing). Throws RPC_WALLET_UNLOCK_NEEDED for a locked
 * wallet and RPC_WALLET_ERROR otherwise; the keys come from the keystore under its own lock.
 */
void SignPQBuilderInputs(CMutableTransaction& mtx, const std::vector<PQBuilderInput>& ins, int nHeight);

#endif // ZCASH_WALLET_ASYNCRPCOPERATION_COMMON_H
