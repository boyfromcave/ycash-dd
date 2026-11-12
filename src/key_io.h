// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2016-2018 The Zcash developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_KEY_IO_H
#define BITCOIN_KEY_IO_H

#include <chainparams.h>
#include <key.h>
#include <pubkey.h>
#include <script/standard.h>
#include <zcash/Address.hpp>

#include <map>
#include <vector>
#include <string>

/** A post-quantum address's Base58Check versions (D-9): one 3-byte version per registered scheme, so the
 *  prefix names the scheme. The address is Base58Check(version || 32-byte keyHash), 53 characters. */
typedef std::map<uint8_t, std::vector<unsigned char>> PQAddressVersions;

/** The plain-YEC PQ address versions (D-Q-20, D-9) of a network ID: main 4ddd04 "sqf…" (FN-DSA-512) and
 *  4dde6a "sqs…" (SLH-DSA-SHA2-128s), test 4f6519 "tqf…" / 4f667f "tqs…", regtest 4c54ef "rqf…" / 4c5655
 *  "rqs…" (quantum spec §4); empty for any other network. */
const PQAddressVersions& PlainPQAddressVersions(const std::string& networkID);

/** Base58Check(versions[id.scheme] || id.hash); "" for a scheme without a version or outside the registry. */
std::string EncodePQAddress(const CPQKeyID& id, const PQAddressVersions& versions);

/** True, and id set, iff data (a decoded Base58Check payload) is versions[s] || 32-byte keyHash for a
 *  registered scheme s. Any other version, length or scheme is refused, the pre-D-9 form among them. */
bool DecodePQAddressData(const std::vector<unsigned char>& data, const PQAddressVersions& versions, CPQKeyID& id);
bool DecodePQAddress(const std::string& str, const PQAddressVersions& versions, CPQKeyID& id);

/** The pre-D-9 form (2-byte version || scheme || keyHash: "sq…"/"tq…"/"rq…" plain, "yb…"/"yt…"/"yr…" YED),
 *  never produced and never accepted as input. Only the wallet reads it, to load an address-book record
 *  written before D-9 and to check a key-dump line from before it. */
bool DecodeLegacyPQAddress(const std::string& str, const std::string& networkID, bool yed, CPQKeyID& id);

class KeyIO {
private:
    const KeyConstants& keyConstants;

public:
    KeyIO(const KeyConstants& keyConstants): keyConstants(keyConstants) { }

    CKey DecodeSecret(const std::string& str);
    std::string EncodeSecret(const CKey& key);

    CExtKey DecodeExtKey(const std::string& str);
    std::string EncodeExtKey(const CExtKey& extkey);
    CExtPubKey DecodeExtPubKey(const std::string& str);
    std::string EncodeExtPubKey(const CExtPubKey& extpubkey);

    std::string EncodeDestination(const CTxDestination& dest);
    CTxDestination DecodeDestination(const std::string& str);

    bool IsValidDestinationString(const std::string& str);

    std::string ZecToYec(const std::string& str);
    std::string ZecToYecShielded(const std::string& str);

    std::string EncodePaymentAddress(const libzcash::PaymentAddress& zaddr);
    libzcash::PaymentAddress DecodePaymentAddress(const std::string& str);
    bool IsValidPaymentAddressString(const std::string& str);

    std::string EncodeViewingKey(const libzcash::ViewingKey& vk);
    libzcash::ViewingKey DecodeViewingKey(const std::string& str);

    std::string EncodeIVK(const libzcash::SaplingIncomingViewingKey& ivk);
    libzcash::SaplingIncomingViewingKey DecodeIVK(const std::string& str);

    std::string EncodeSpendingKey(const libzcash::SpendingKey& zkey);
    libzcash::SpendingKey DecodeSpendingKey(const std::string& str);
};

#endif // BITCOIN_KEY_IO_H
