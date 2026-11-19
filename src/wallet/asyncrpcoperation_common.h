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

/**
 * Beta2 (the Yellowback second check, review m2, audit L-ii): removes from `coins` every Yellowback outpoint
 * (YellowbackInputRefusal: a token, vault or carrier whose coin lock was removed) and returns the refusal naming them
 * ("" when there were none); the caller spends the remaining plain coins and throws the refusal (RPC_WALLET_ERROR) only
 * when they do not cover the send. A `strict` caller (a post-quantum source) is refused outright while the index is
 * unhealthy (thrown here). cs_main and cs_wallet may be held.
 */
std::string DropYellowbackCoins(std::vector<COutput>& coins, bool strict);

/**
 * Beta2 (audit A-M2): what one more post-quantum input of `scheme` adds to a plain spend's fee at the current rule:
 * max(PlainPQFeeRate() x its spend size, the ZIP-317 marginal fee of its logical actions) (SLH-DSA 27,000 zat, FN-DSA
 * 5,500 zat with no attested price). A coin worth no more than this is never selected by z_sendmany or merged by
 * z_mergetoaddress from a post-quantum address: spending it costs at least what it holds (outsiders can place such
 * outputs on a known address at the relay dust price, far below this).
 */
CAmount PQInputMarginalFee(uint8_t scheme);

/**
 * Beta3 (a post-quantum miner shields its coinbase): the value of the immature coinbase outputs of this wallet that
 * pay `script` (in the main chain, GetBlocksToMaturity() > 0). AvailableCoins never returns them; z_sendmany names the
 * sum when it refuses a send from the address. Beta4: *nextMaturesIn (when given) is set to the confirmations the
 * soonest of them still needs (0 when there is none). cs_main and wallet.cs_wallet held.
 */
CAmount PQImmatureCoinbase(const CWallet& wallet, const CScript& script, int* nextMaturesIn = nullptr);

/**
 * Beta4 (REPORT item 7): the value of the unspent non-coinbase outputs of this wallet that pay `script` with fewer
 * than `minDepth` confirmations (unconfirmed included, conflicted not): what minconf left out of a send.
 * cs_main and wallet.cs_wallet held.
 */
CAmount PQBelowMinconf(const CWallet& wallet, const CScript& script, int minDepth);

/**
 * Beta4: the sentence z_sendmany adds to an insufficient-funds refusal when minconf left `belowMinconf` of the source's
 * funds out (" The address holds <v> YEC with fewer than the <n> confirmations this send requires (minconf): ...");
 * "" when nothing was left out. Character-identical on both C++ lines.
 */
std::string MinconfNote(CAmount belowMinconf, int minDepth);

/** Beta4: what a z_sendmany from a post-quantum address could not use, for its insufficient-funds refusal. */
struct PQFundsLeftOut {
    CAmount coinbase{0};                //!< mature coinbase left out because a recipient is transparent
    size_t coinbaseCoins{0};            //!< how many outputs that is
    CAmount coinbaseSend{0};            //!< the one amount that moves the largest of them to one Sapling address (0: none)
    CAmount coinbaseSelected{0};        //!< the rewards that amount spends
    CAmount coinbaseFee{0};             //!< and its fee
    size_t coinbaseSelectedCoins{0};    //!< how many rewards that amount spends
    CAmount immature{0};                //!< immature coinbase of the address
    int immatureBlocks{0};              //!< the confirmations the soonest of them still needs
    CAmount belowMinconf{0};            //!< ordinary coins of the address that minconf left out
    int minDepth{0};                    //!< the send's minconf
};

/**
 * Beta3, reworded in beta4 (REPORT item 7): the "Insufficient transparent funds" message of z_sendmany from a
 * post-quantum address. `message` is returned unchanged when nothing was left out. Otherwise, when `noCoins` (the
 * address had no ordinary coin to spend) the lead becomes "Insufficient transparent funds: this address has no coins
 * this send can spend.", and the message names the mining rewards (with the exact amount that sends them to Sapling),
 * the immature rewards (with the confirmations still needed) and the coins minconf left out.
 */
std::string PQInsufficientFunds(const std::string& message, bool noCoins, const PQFundsLeftOut& left);

/**
 * Beta4 (REPORT item 7; reworded with item 7a): the sentence z_shieldcoinbase adds to its insufficient-funds refusal
 * when the wallet holds `pqCoinbase` of mature coinbase at post-quantum addresses that this call did not take ("*"
 * takes ECC coins only): it shields them when the post-quantum address is named. "" when that is 0.
 */
std::string PQCoinbaseShieldNote(CAmount pqCoinbase);

/**
 * Beta4 (item 7a): the tail of z_shieldcoinbase's "Could not find any coinbase funds to shield." from a post-quantum
 * address that holds only immature coinbase: its value and the blocks the first of it still needs (yewd's
 * `immature_tail`, character for character); "" when there is none.
 */
std::string PQShieldImmatureTail(CAmount immature, int blocksToGo);

/**
 * Beta3: the refusal of a z_sendmany from a post-quantum address whose selected coins include coinbase and would
 * leave `change` (a coinbase spend has no transparent output where coinbase must be shielded, so no change returns
 * to the address): it names the one amount that spends the selected coins, `selected` less `fee`.
 */
std::string PQCoinbaseChangeRefusal(CAmount change, CAmount selected, CAmount fee);

/**
 * Beta2 (audit H1, review L-1): refuse a fee above PQMaxTxFee() on a post-quantum spend (RPC_INVALID_PARAMETER, "Fee
 * … is above the … cap on a post-quantum spend"), an explicit one before building and a computed one before signing:
 * the commit submits a PQ transaction without the absurd-fee check only within that cap, so a larger fee would be
 * built, signed and then refused at the commit.
 */
void EnsurePQFeeWithinCap(CAmount fee);

/**
 * Beta2 (audit H1): SendTransaction for a transaction with post-quantum inputs built outside the wallet's locks
 * (z_sendmany, z_mergetoaddress). Under cs_main and cs_wallet, held across the commit: the fee is re-checked against
 * PQMaxTxFee() (a price change in a block found while the inputs were signed can lower it; RPC_WALLET_ERROR, nothing
 * recorded); and when the commit fails (the mempool refuses the transaction) the transaction is erased from the
 * wallet again, as the Yellowback commit does (YellowbackWallet::RemoveOrphan), so that no later restart
 * (ReacceptWalletTransactions, which judges the fee by another rule) broadcasts a send the caller was told failed.
 */
UniValue SendPQTransaction(CTransaction& tx, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode);

/**
 * Beta2 (audit H1, review L-2): the erase SendPQTransaction makes when the commit of a transaction the wallet did not
 * hold before fails. cs_main and wallet.cs_wallet held. Returns "" when `txid` is not in the wallet or was erased
 * (CWallet::EraseUnbroadcastTx: the wallet file, mapWallet and every index naming it, review H-1); otherwise the error for the caller, naming the txid: the transaction is in the mempool
 * (never erased: it may already be relayed), or the wallet file refused the erase (the wallet still holds the
 * unbroadcast transaction, which a restart may submit).
 */
std::string EraseRefusedPQSend(CWallet& wallet, const uint256& txid);

#endif // ZCASH_WALLET_ASYNCRPCOPERATION_COMMON_H
