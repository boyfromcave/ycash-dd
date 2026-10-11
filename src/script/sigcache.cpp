// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "sigcache.h"

#include "crypto/common.h"
#include "crypto/sha256.h"
#include "memusage.h"
#include "pubkey.h"
#include "random.h"
#include "uint256.h"
#include "util.h"

#include <boost/thread.hpp>
#include <boost/unordered_set.hpp>

#include <atomic>

namespace {

/**
 * We're hashing a nonce into the entries themselves, so we don't need extra
 * blinding in the set hash computation.
 */
class CSignatureCacheHasher
{
public:
    size_t operator()(const uint256& key) const {
        return key.GetCheapHash();
    }
};

/**
 * Valid signature cache, to avoid doing expensive ECDSA signature checking
 * twice for every transaction (once when accepted into memory pool, and
 * again when accepted into the block chain)
 */
class CSignatureCache
{
private:
     //! Entries are SHA256(nonce || signature hash || public key || signature):
    uint256 nonce;
    //! Post-quantum (OP_CHECKPQSIG) entries share the set and its size bound but use a second,
    //! independent nonce, so a PQ preimage and an ECDSA preimage differ in their first 32 bytes
    //! and no PQ entry can equal an ECDSA entry (short of a SHA256 collision). They are
    //! SHA256(pqNonce || scheme || signature hash || len(public key) || public key ||
    //! len(signature) || signature), lengths as 4-byte little-endian: the whole input of
    //! pq::Verify, so an entry vouches for exactly one (scheme, key, signature, message).
    uint256 pqNonce;
    typedef boost::unordered_set<uint256, CSignatureCacheHasher> map_type;
    map_type setValid;
    boost::shared_mutex cs_sigcache;


public:
    CSignatureCache()
    {
        GetRandBytes(nonce.begin(), 32);
        GetRandBytes(pqNonce.begin(), 32);
    }

    void
    ComputeEntry(uint256& entry, const uint256 &hash, const std::vector<unsigned char>& vchSig, const CPubKey& pubkey)
    {
        CSHA256().Write(nonce.begin(), 32).Write(hash.begin(), 32).Write(&pubkey[0], pubkey.size()).Write(&vchSig[0], vchSig.size()).Finalize(entry.begin());
    }

    void
    ComputePQEntry(uint256& entry, uint8_t scheme, const uint256 &hash, const std::vector<unsigned char>& vchSig, const std::vector<unsigned char>& pk)
    {
        unsigned char pkLen[4], sigLen[4];
        WriteLE32(pkLen, (uint32_t)pk.size());
        WriteLE32(sigLen, (uint32_t)vchSig.size());
        CSHA256().Write(pqNonce.begin(), 32).Write(&scheme, 1).Write(hash.begin(), 32).Write(pkLen, 4).Write(pk.data(), pk.size()).Write(sigLen, 4).Write(vchSig.data(), vchSig.size()).Finalize(entry.begin());
    }

    bool
    Get(const uint256& entry)
    {
        boost::shared_lock<boost::shared_mutex> lock(cs_sigcache);
        return setValid.count(entry);
    }

    void Erase(const uint256& entry)
    {
        boost::unique_lock<boost::shared_mutex> lock(cs_sigcache);
        setValid.erase(entry);
    }

    void Set(const uint256& entry)
    {
        size_t nMaxCacheSize = GetArg("-maxsigcachesize", DEFAULT_MAX_SIG_CACHE_SIZE) * ((size_t) 1 << 20);
        if (nMaxCacheSize <= 0) return;

        boost::unique_lock<boost::shared_mutex> lock(cs_sigcache);
        while (memusage::DynamicUsage(setValid) > nMaxCacheSize)
        {
            map_type::size_type s = GetRand(setValid.bucket_count());
            map_type::local_iterator it = setValid.begin(s);
            if (it != setValid.end(s)) {
                setValid.erase(*it);
            }
        }

        setValid.insert(entry);
    }
};

// One cache for ECDSA and PQ entries (it was a function-local static of VerifySignature).
CSignatureCache& SignatureCache()
{
    static CSignatureCache signatureCache;
    return signatureCache;
}

std::atomic<uint64_t> pqSigCacheHits{0};

}

bool CachingTransactionSignatureChecker::VerifySignature(const std::vector<unsigned char>& vchSig, const CPubKey& pubkey, const uint256& sighash) const
{
    CSignatureCache& signatureCache = SignatureCache();

    uint256 entry;
    signatureCache.ComputeEntry(entry, sighash, vchSig, pubkey);

    if (signatureCache.Get(entry)) {
        if (!store) {
            signatureCache.Erase(entry);
        }
        return true;
    }

    if (!TransactionSignatureChecker::VerifySignature(vchSig, pubkey, sighash))
        return false;

    if (store) {
        signatureCache.Set(entry);
    }
    return true;
}

bool CachingTransactionSignatureChecker::VerifyPQSignature(uint8_t scheme, const std::vector<unsigned char>& vchSig, const std::vector<unsigned char>& vchPubKey, const uint256& sighash) const
{
    // As VerifySignature: a hit is a verification that already succeeded on these exact bytes;
    // only a success is stored, and only when the caller allows it (store).
    CSignatureCache& signatureCache = SignatureCache();

    uint256 entry;
    signatureCache.ComputePQEntry(entry, scheme, sighash, vchSig, vchPubKey);

    if (signatureCache.Get(entry)) {
        if (!store) {
            signatureCache.Erase(entry);
        }
        pqSigCacheHits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (!TransactionSignatureChecker::VerifyPQSignature(scheme, vchSig, vchPubKey, sighash))
        return false;

    if (store) {
        signatureCache.Set(entry);
    }
    return true;
}

uint64_t GetPQSigCacheHits()
{
    return pqSigCacheHits.load(std::memory_order_relaxed);
}
