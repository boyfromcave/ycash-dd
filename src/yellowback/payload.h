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
 * Yellowback payload codec, version 2 (plan §3.3, V23).
 *
 *   magic   2 bytes   0x59 0x42 ("YB")
 *   version 1 byte    0x02
 *   type    1 byte    0x01 MINT | 0x02 TRANSFER | 0x03 REDEEM
 *   body    per type; total <= 80 bytes; trailing bytes => malformed
 *
 *   MINT      termClass u8, cents u32, lockHeight u32, refHeight u32, ownerPubKey 33, feeVout u8   (51)
 *   TRANSFER  count u8, count x (vout u8, cents u32)                                               (5 + 5n, n <= 15)
 *   REDEEM    refHeight u32, feeVout u8, count u8, count x (vout u8, cents u32)                   (10 + 5n, n <= 14)
 *
 * Types 0x10-0x1F (the prototype's; 0x10 PRICE retired) and 0x20-0xFF are
 * reserved: unknown type or version => non-Yellowback (the forward-
 * compatibility rule). All multi-byte integers are fixed-width little-endian
 * (no CompactSize, no VARINT). The decoder is a bounds-checked reader over a
 * byte vector; it never uses CDataStream and never throws. feeVout = 0xFF
 * means "no enforcement-fee output"; the codec does not range-check feeVout
 * (MINT-8 / RED-3 do, K11).
 *
 * DigiByte packs the equivalent information into nVersion bits and an
 * OP_RETURN (ref/digibyte/src/digidollar/txbuilder.cpp:407-418, 807-818);
 * Ycash pins nVersion == 4, so the type lives in the payload (mapping.md §5).
 *
 * The MINT owner key is carried as its 33 raw bytes (`ownerKeyBytes`) as
 * well as a CPubKey: the codec fixes only the shape (33 bytes), MINT-3
 * decides validity (`bad-mint-owner-key`), and a VOID vault records the
 * bytes verbatim so every implementation serialises the same record
 * (SERIALISATION.md §3 C). Version 1 is non-Yellowback (V23).
 */
namespace yellowback {

enum class PayloadType : uint8_t {
    MINT     = 0x01,
    TRANSFER = 0x02,
    REDEEM   = 0x03,
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

/** TRANSFER: 5 + 5*count <= 80 => count <= 15. */
static const size_t MAX_ASSIGNMENTS = 15;
/** REDEEM: 10 + 5*count <= 80 => count <= 14. */
static const size_t MAX_REDEEM_ASSIGNMENTS = 14;

struct Payload
{
    uint8_t version;          //!< PAYLOAD_VERSION (2)
    PayloadType type;

    // MINT
    uint8_t termClass;        //!< 0 = A, 1 = B, 2 = C (V19); any byte decodes, MINT-2 checks it
    uint32_t cents;
    uint32_t lockHeight;
    uint32_t refHeight;       //!< MINT and REDEEM (V11)
    CPubKey ownerPubKey;      //!< from ownerKeyBytes; invalid (size 0) when the bytes are not a key encoding
    std::vector<unsigned char> ownerKeyBytes;   //!< the 33 payload bytes verbatim
    uint8_t feeVout;          //!< MINT and REDEEM; FEE_VOUT_NONE = no fee output

    // TRANSFER / REDEEM
    std::vector<Assignment> assignments;

    Payload() : version(PAYLOAD_VERSION), type(PayloadType::MINT), termClass(0), cents(0), lockHeight(0), refHeight(0),
                feeVout(FEE_VOUT_NONE) {}

    static Payload Mint(uint8_t termClass, uint32_t cents, uint32_t lockHeight, uint32_t refHeight, const CPubKey& owner, uint8_t feeVout);
    static Payload Transfer(const std::vector<Assignment>& assignments);
    static Payload Redeem(uint32_t refHeight, uint8_t feeVout, const std::vector<Assignment>& assignments);

    /** Sum of assigned cents (TRANSFER/REDEEM); 0 otherwise. Fits int64 (15 x 2^32). */
    int64_t AssignedCents() const;

    friend bool operator==(const Payload& a, const Payload& b);
};

/** Serialise; the result is the data push of the OP_RETURN output. Empty if the payload is not encodable. */
std::vector<unsigned char> EncodePayload(const Payload& payload);

/**
 * Parse a payload. Returns false for every malformed case of §3.3: bad magic,
 * version or type, short or long body, count > 15 (TRANSFER) / 14 (REDEEM),
 * duplicate vout, cents == 0. The owner key is any 33 bytes (MINT-3 judges
 * it). Range checks that need the transaction (vout exists, vout is not the
 * OP_RETURN) are done by FindPayload. Version 1 is non-Yellowback (V23).
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
