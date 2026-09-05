// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YDOLLAR_ADDRESS_H
#define YCASH_YDOLLAR_ADDRESS_H

#include "pubkey.h"
#include "ydollar/params.h"

#include <string>

/**
 * YDollar addresses (plan D10): Base58Check(version || 20-byte key hash) with
 * version bytes 0x1FE2 (mainnet, "yd…"), 0x2007 (testnet, "yt…"), 0x2002
 * (regtest, "yr…"), decoding to an ordinary P2PKH destination. A distinct
 * format stops users from sending YDollar to a plain s1… address by accident
 * (an unaware wallet would burn it, plan D2). No chainparams.cpp edit:
 * the version bytes live in ydollar::Params.
 *
 * Mirrors DigiByte's DD/TD/RD prefixes (DIGIDOLLAR_ARCHITECTURE.md §3.2).
 */
namespace ydollar {

std::string EncodeAddress(const CKeyID& keyID, const Params& params);

/** False if the string is not a YDollar address of this network. */
bool DecodeAddress(const std::string& str, const Params& params, CKeyID& keyID);

bool IsValidAddress(const std::string& str, const Params& params);

} // namespace ydollar

#endif // YCASH_YDOLLAR_ADDRESS_H
