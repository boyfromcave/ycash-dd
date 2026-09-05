// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_PAYLOAD_H
#define YCASH_YELLOWBACK_PAYLOAD_H

#include "primitives/transaction.h"
#include "pubkey.h"
#include "yellowback/params.h"

#include <cstdint>
#include <optional>
#include <vector>

/**
 * Yellowback payload codec (plan §3.2).
 *
 *   magic   2 bytes   0x59 0x42 ("YB")
 *   version 1 byte    0x01
 *   type    1 byte    0x01 MINT | 0x02 TRANSFER | 0x03 REDEEM | 0x10 PRICE
 *   body    per type; total <= 80 bytes; trailing bytes => malformed
 *
 * All multi-byte integers are fixed-width little-endian (no CompactSize, no
 * VARINT, plan B11). The decoder is a bounds-checked reader over a byte
 * vector; it never uses CDataStream and never throws (plan §3.2).
 *
 * DigiByte packs the equivalent information into nVersion bits and an
 * OP_RETURN (ref/digibyte/src/digidollar/txbuilder.cpp:407-418, 807-818);
 * Ycash pins nVersion == 4, so the type lives in the payload (mapping.md §5).
 */
namespace yellowback {

enum class PayloadType : uint8_t {
    MINT     = 0x01,
    TRANSFER = 0x02,
    REDEEM   = 0x03,
    PRICE    = 0x10,
};

/** One (vout, cents) assignment of a TRANSFER or REDEEM body. */
struct Assignment
{
    uint8_t vout;
    uint32_t cents;

    Assignment() : vout(0), cents(0) {}
    Assignment(uint8_t v, uint32_t c) : vout(v), cents(c) {}
    friend bool operator==(const Assignment& a, const Assignment& b) { return a.vout == b.vout && a.cents == b.cents; }
};

/** TRANSFER/REDEEM: 5 + 5*count <= 80 => count <= 15. */
static const size_t MAX_ASSIGNMENTS = 15;

struct Payload
{
    PayloadType type;

    // MINT
    uint8_t tier;
    uint32_t cents;
    uint32_t lockHeight;
    uint32_t evalHeight;
    CPubKey ownerPubKey;

    // TRANSFER / REDEEM
    std::vector<Assignment> assignments;

    // PRICE
    uint64_t priceMicroUsd;

    Payload() : type(PayloadType::MINT), tier(0), cents(0), lockHeight(0), evalHeight(0), priceMicroUsd(0) {}

    static Payload Mint(uint8_t tier, uint32_t cents, uint32_t lockHeight, uint32_t evalHeight, const CPubKey& owner);
    static Payload Transfer(const std::vector<Assignment>& assignments);
    static Payload Redeem(const std::vector<Assignment>& assignments);
    static Payload Price(uint64_t priceMicroUsd);

    /** Sum of assigned cents (TRANSFER/REDEEM); 0 otherwise. Fits int64 (15 x 2^32). */
    int64_t AssignedCents() const;

    friend bool operator==(const Payload& a, const Payload& b);
};

/** Serialise; the result is the data push of the OP_RETURN output. Empty if the payload is not encodable. */
std::vector<unsigned char> EncodePayload(const Payload& payload);

/**
 * Parse a payload. Returns false for every malformed case of §3.2: bad magic,
 * version or type, short or long body, count > 15, duplicate vout, cents == 0,
 * non-compressed owner key. Range checks that need the transaction (vout
 * exists, vout is not the OP_RETURN) are done by FindPayload.
 */
bool DecodePayload(const std::vector<unsigned char>& data, Payload& out);

/** Build the OP_RETURN output script for a payload: OP_RETURN <push>. */
CScript PayloadScript(const std::vector<unsigned char>& data);

/**
 * If `script` is exactly `OP_RETURN <one data push>`, return the pushed bytes.
 * Any other shape (no push, two pushes, an OP_N, trailing bytes) => nullopt.
 */
std::optional<std::vector<unsigned char>> ExtractOpReturnData(const CScript& script);

/** Index of the transaction's OP_RETURN output if it has exactly one; nullopt for zero or more than one (§3.2). */
std::optional<unsigned int> FindOpReturn(const CTransaction& tx);

struct FoundPayload
{
    Payload payload;
    unsigned int opReturnIndex;
};

/**
 * The transaction's Yellowback payload, if it has one: exactly one OP_RETURN
 * output, of the required shape, that decodes, whose assigned vouts all exist
 * and none of which is the OP_RETURN itself. Otherwise nullopt: the
 * transaction is non-Yellowback for outputs (inputs still follow IN-1..3).
 */
std::optional<FoundPayload> FindPayload(const CTransaction& tx);

/** Name of a type for RPC output and logs. */
const char* PayloadTypeName(PayloadType type);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_PAYLOAD_H
