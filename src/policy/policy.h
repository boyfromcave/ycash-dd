// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_POLICY_POLICY_H
#define BITCOIN_POLICY_POLICY_H

#include "consensus/consensus.h"
#include "script/interpreter.h"
#include "script/standard.h"

#include <string>

class CChainParams;
class CCoinsViewCache;

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

#endif // BITCOIN_POLICY_POLICY_H
