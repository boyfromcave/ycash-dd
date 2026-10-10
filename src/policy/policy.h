// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_POLICY_POLICY_H
#define BITCOIN_POLICY_POLICY_H

#include "amount.h"
#include "consensus/consensus.h"
#include "script/interpreter.h"
#include "script/standard.h"

#include <optional>
#include <string>

class CChainParams;
class CCoinsViewCache;
class CFeeRate;
class CTxOut;
namespace Consensus { struct Params; }

/** Default for -blockmaxsize and -blockminsize, which control the range of sizes the mining code will create **/
static const unsigned int DEFAULT_BLOCK_MAX_SIZE = MAX_BLOCK_SIZE;
static const unsigned int DEFAULT_BLOCK_MIN_SIZE = 0;
/** Default for -blockprioritysize, maximum space for zero/low-fee transactions **/
static const unsigned int DEFAULT_BLOCK_PRIORITY_SIZE = DEFAULT_BLOCK_MAX_SIZE / 2;
/** Maximum number of signature check operations in an IsStandard() P2SH script */
static const unsigned int MAX_P2SH_SIGOPS = 15;
/** The maximum number of sigops we're willing to relay/mine in a single tx */
static const unsigned int MAX_STANDARD_TX_SIGOPS = MAX_BLOCK_SIGOPS/5;
/** The largest `YV` act OP_RETURN scriptPubKey relayed once UPGRADE_VAULT is active (plan §15.5). */
static const unsigned int MAX_VAULT_ACT_BYTES = 1200;
/** The largest standard scriptSig of an input spending TX_PQPKH or a vault template once UPGRADE_VAULT
 *  is active (quantum spec §0, §2.3, F-6): an SLH-DSA spend is 7,939 bytes. */
static const unsigned int MAX_STANDARD_PQ_SCRIPTSIG = 9000;
/** The largest standard scriptSig of every other input (Bitcoin's 15-of-15 P2SH multisig bound). */
static const unsigned int MAX_STANDARD_SCRIPTSIG = 1650;
/**
 * Standard script verification flags that standard transactions will comply
 * with. However scripts violating these flags may still be present in valid
 * blocks and we must accept those blocks.
 */
static const unsigned int STANDARD_SCRIPT_VERIFY_FLAGS = MANDATORY_SCRIPT_VERIFY_FLAGS |
                                                         // SCRIPT_VERIFY_DERSIG is always enforced
                                                         SCRIPT_VERIFY_STRICTENC |
                                                         SCRIPT_VERIFY_MINIMALDATA |
                                                         SCRIPT_VERIFY_NULLDUMMY |
                                                         SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS |
                                                         SCRIPT_VERIFY_CLEANSTACK |
                                                         SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
                                                         SCRIPT_VERIFY_LOW_S;

/** For convenience, standard but not mandatory verify flags. */
static const unsigned int STANDARD_NOT_MANDATORY_VERIFY_FLAGS = STANDARD_SCRIPT_VERIFY_FLAGS & ~MANDATORY_SCRIPT_VERIFY_FLAGS;

// Sanity check the magic numbers when we change them
static_assert(DEFAULT_BLOCK_MAX_SIZE <= MAX_BLOCK_SIZE);
static_assert(DEFAULT_BLOCK_PRIORITY_SIZE <= DEFAULT_BLOCK_MAX_SIZE);

bool IsStandard(const CScript& scriptPubKey, txnouttype& whichType);

/**
 * The post-quantum owner/holder scheme of a TX_PQPKH, V or I scriptPubKey (quantum spec §1, §2), or
 * nullopt for any other script.
 */
std::optional<uint8_t> PQScriptScheme(const CScript& scriptPubKey, txnouttype whichType);

/** The serialized size of the input that spends a TX_PQPKH output of `scheme` (outpoint, sequence and
 *  the chunked scriptSig: SLH-DSA 7,938 + 43 bytes, Falcon 1,577 + 43); 0 for an unknown scheme. */
size_t PQSpendInputSize(uint8_t scheme);

/** The dust threshold of a TX_PQPKH output, priced with its real spend size (review A F8); any other
 *  output's CTxOut::GetDustThreshold. */
CAmount GetPQDustThreshold(const CTxOut& txout, const CFeeRate& minRelayTxFee);
    /**
     * Check for standard transaction types
     * @return True if all outputs (scriptPubKeys) use only standard transaction forms
     */
bool IsStandardTx(const CTransaction& tx, std::string& reason, const CChainParams& chainparams, int nHeight = 0);
    /**
     * Check for standard transaction types
     * @param[in] mapInputs    Map of previous transactions that have outputs we're spending
     * @return True if all inputs (scriptSigs) use only standard transaction forms
     */
bool AreInputsStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs, uint32_t consensusBranchId);

/**
 * Policy sigops of OP_CHECKPQSIG (docs/plans/yellowback-quantum-plan.md §4.1, D-Q-7):
 * pq::SIGOP_COST for each OP_CHECKPQSIG in what a spend executes (each input's scriptSig, its
 * previous output's scriptPubKey and, for P2SH, the redeem script). Not a consensus count
 * (CScript::GetSigOpCount does not see 0xc2, as it does not see OP_CHECKSETSIG); the mempool and
 * the block template add it to their sigop budgets (getblocktemplate's per-transaction "sigops"
 * includes it), and AreInputsStandard to a P2SH redeem script's (R-B2).
 */
unsigned int GetPQSigOpCount(const CTransaction& tx, const CCoinsViewCache& mapInputs);
/** The same count over one script: pq::SIGOP_COST per OP_CHECKPQSIG opcode (not inside pushes). */
unsigned int GetPQSigOpCount(const CScript& script);

/**
 * The fee rate wallets charge a transaction that spends a post-quantum input (TX_PQPKH, or a V/I
 * owner path), -pqfeerate (quantum plan §4.5, quantum briefing fee rules): the default is 1x the
 * per-kB relay floor (-minrelaytxfee); the release multiplier is the owner's decision (Q8). Not a
 * relay or consensus rule: relay stays minRelayTxFee per byte.
 */
extern CFeeRate pqFeeRate;

/** max(floor, pqFeeRate x nBytes): the fee of a transaction of nBytes with a PQ input (ycash-dd:
 *  floor = DEFAULT_FEE for the wallet, VAULT_RPC_FEE for the vault RPCs). */
CAmount PQSizeFee(CAmount floor, size_t nBytes);

#endif // BITCOIN_POLICY_POLICY_H
