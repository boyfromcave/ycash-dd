// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_SCRIPT_SIGCACHE_H
#define BITCOIN_SCRIPT_SIGCACHE_H

#include "script/interpreter.h"

#include <vector>

// DoS prevention: limit cache size to less than 40MB (over 500000
// entries on 64-bit systems).
static const unsigned int DEFAULT_MAX_SIG_CACHE_SIZE = 40;

class CPubKey;

class CachingTransactionSignatureChecker : public TransactionSignatureChecker
{
private:
    bool store;

public:
    CachingTransactionSignatureChecker(const CTransaction* txToIn, unsigned int nInIn, const CAmount& amount, bool storeIn, PrecomputedTransactionData& txdataIn) : TransactionSignatureChecker(txToIn, nInIn, amount, txdataIn), store(storeIn) {}

    bool VerifySignature(const std::vector<unsigned char>& vchSig, const CPubKey& vchPubKey, const uint256& sighash) const;
    // OP_CHECKPQSIG (quantum plan §6): memoizes pq::Verify in the same cache, under the same store
    // rule, keyed on its whole input (scheme, key, signature and sighash) in a separate key space.
    bool VerifyPQSignature(uint8_t scheme, const std::vector<unsigned char>& vchSig, const std::vector<unsigned char>& vchPubKey, const uint256& sighash) const;
};

// The number of OP_CHECKPQSIG verifications answered from the signature cache since startup
// (unit tests; it never affects a verdict).
uint64_t GetPQSigCacheHits();

#endif // BITCOIN_SCRIPT_SIGCACHE_H
