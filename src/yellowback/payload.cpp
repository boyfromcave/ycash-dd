// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/payload.h"

#include "script/script.h"

#include <set>

namespace yellowback {

namespace {

const size_t MINT_BODY_SIZE = 1 + 4 + 4 + 4 + CPubKey::COMPRESSED_PUBLIC_KEY_SIZE; // 46
const size_t PRICE_BODY_SIZE = 8;

/** Bounds-checked little-endian reader. Never throws. */
class Reader
{
public:
    explicit Reader(const std::vector<unsigned char>& d) : data(d), pos(0) {}

    bool U8(uint8_t& v)
    {
        if (pos + 1 > data.size()) return false;
        v = data[pos++];
        return true;
    }
    bool U32(uint32_t& v)
    {
        if (pos + 4 > data.size()) return false;
        v = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) | ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        pos += 4;
        return true;
    }
    bool U64(uint64_t& v)
    {
        uint32_t lo, hi;
        if (!U32(lo) || !U32(hi)) return false;
        v = (uint64_t)lo | ((uint64_t)hi << 32);
        return true;
    }
    bool Bytes(size_t n, std::vector<unsigned char>& v)
    {
        if (pos + n > data.size()) return false;
        v.assign(data.begin() + pos, data.begin() + pos + n);
        pos += n;
        return true;
    }
    bool AtEnd() const { return pos == data.size(); }

private:
    const std::vector<unsigned char>& data;
    size_t pos;
};

void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
    out.push_back(v & 0xff);
    out.push_back((v >> 8) & 0xff);
    out.push_back((v >> 16) & 0xff);
    out.push_back((v >> 24) & 0xff);
}

void PutU64(std::vector<unsigned char>& out, uint64_t v)
{
    PutU32(out, (uint32_t)(v & 0xffffffffULL));
    PutU32(out, (uint32_t)(v >> 32));
}

bool ValidAssignments(const std::vector<Assignment>& assignments)
{
    if (assignments.size() > MAX_ASSIGNMENTS) return false;
    std::set<uint8_t> seen;
    for (const Assignment& a : assignments) {
        if (a.cents == 0) return false;
        if (!seen.insert(a.vout).second) return false;
    }
    return true;
}

} // namespace

Payload Payload::Mint(uint8_t tier, uint32_t cents, uint32_t lockHeight, uint32_t evalHeight, const CPubKey& owner)
{
    Payload p;
    p.type = PayloadType::MINT;
    p.tier = tier;
    p.cents = cents;
    p.lockHeight = lockHeight;
    p.evalHeight = evalHeight;
    p.ownerPubKey = owner;
    return p;
}

Payload Payload::Transfer(const std::vector<Assignment>& assignments)
{
    Payload p;
    p.type = PayloadType::TRANSFER;
    p.assignments = assignments;
    return p;
}

Payload Payload::Redeem(const std::vector<Assignment>& assignments)
{
    Payload p;
    p.type = PayloadType::REDEEM;
    p.assignments = assignments;
    return p;
}

Payload Payload::Price(uint64_t priceMicroUsd)
{
    Payload p;
    p.type = PayloadType::PRICE;
    p.priceMicroUsd = priceMicroUsd;
    return p;
}

int64_t Payload::AssignedCents() const
{
    int64_t sum = 0;
    for (const Assignment& a : assignments) sum += a.cents;
    return sum;
}

bool operator==(const Payload& a, const Payload& b)
{
    if (a.type != b.type) return false;
    switch (a.type) {
    case PayloadType::MINT:
        return a.tier == b.tier && a.cents == b.cents && a.lockHeight == b.lockHeight &&
               a.evalHeight == b.evalHeight && a.ownerPubKey == b.ownerPubKey;
    case PayloadType::TRANSFER:
    case PayloadType::REDEEM:
        return a.assignments == b.assignments;
    case PayloadType::PRICE:
        return a.priceMicroUsd == b.priceMicroUsd;
    }
    return false;
}

std::vector<unsigned char> EncodePayload(const Payload& payload)
{
    std::vector<unsigned char> out;
    out.push_back(PAYLOAD_MAGIC_0);
    out.push_back(PAYLOAD_MAGIC_1);
    out.push_back(PAYLOAD_VERSION);
    out.push_back((unsigned char)payload.type);
    switch (payload.type) {
    case PayloadType::MINT:
        if (!payload.ownerPubKey.IsValid() || !payload.ownerPubKey.IsCompressed()) return {};
        out.push_back(payload.tier);
        PutU32(out, payload.cents);
        PutU32(out, payload.lockHeight);
        PutU32(out, payload.evalHeight);
        out.insert(out.end(), payload.ownerPubKey.begin(), payload.ownerPubKey.end());
        break;
    case PayloadType::TRANSFER:
    case PayloadType::REDEEM:
        if (!ValidAssignments(payload.assignments)) return {};
        out.push_back((unsigned char)payload.assignments.size());
        for (const Assignment& a : payload.assignments) {
            out.push_back(a.vout);
            PutU32(out, a.cents);
        }
        break;
    case PayloadType::PRICE:
        PutU64(out, payload.priceMicroUsd);
        break;
    default:
        return {};
    }
    if (out.size() > MAX_PAYLOAD) return {};
    return out;
}

bool DecodePayload(const std::vector<unsigned char>& data, Payload& out)
{
    if (data.size() < MIN_PAYLOAD || data.size() > MAX_PAYLOAD) return false;
    if (data[0] != PAYLOAD_MAGIC_0 || data[1] != PAYLOAD_MAGIC_1) return false;
    if (data[2] != PAYLOAD_VERSION) return false;

    Reader r(data);
    uint8_t magic0, magic1, version, type;
    r.U8(magic0); r.U8(magic1); r.U8(version); r.U8(type);

    Payload p;
    switch (type) {
    case (uint8_t)PayloadType::MINT: {
        if (data.size() != 4 + MINT_BODY_SIZE) return false;
        p.type = PayloadType::MINT;
        std::vector<unsigned char> key;
        if (!r.U8(p.tier) || !r.U32(p.cents) || !r.U32(p.lockHeight) || !r.U32(p.evalHeight) ||
            !r.Bytes(CPubKey::COMPRESSED_PUBLIC_KEY_SIZE, key)) return false;
        p.ownerPubKey.Set(key.begin(), key.end());
        // A syntactically compressed key (0x02/0x03 prefix, 33 bytes). Curve
        // validity is MINT-3's job; the codec only fixes the shape.
        if (!p.ownerPubKey.IsValid() || !p.ownerPubKey.IsCompressed()) return false;
        break;
    }
    case (uint8_t)PayloadType::TRANSFER:
    case (uint8_t)PayloadType::REDEEM: {
        p.type = (type == (uint8_t)PayloadType::TRANSFER) ? PayloadType::TRANSFER : PayloadType::REDEEM;
        uint8_t count;
        if (!r.U8(count)) return false;
        if (count > MAX_ASSIGNMENTS) return false;
        if (data.size() != 5 + 5 * (size_t)count) return false;
        for (uint8_t i = 0; i < count; i++) {
            Assignment a;
            if (!r.U8(a.vout) || !r.U32(a.cents)) return false;
            p.assignments.push_back(a);
        }
        if (!ValidAssignments(p.assignments)) return false;
        break;
    }
    case (uint8_t)PayloadType::PRICE: {
        if (data.size() != 4 + PRICE_BODY_SIZE) return false;
        p.type = PayloadType::PRICE;
        if (!r.U64(p.priceMicroUsd)) return false;
        break;
    }
    default:
        return false; // unknown type: forward-compatibility rule, non-Yellowback
    }
    if (!r.AtEnd()) return false;
    out = p;
    return true;
}

CScript PayloadScript(const std::vector<unsigned char>& data)
{
    return CScript() << OP_RETURN << data;
}

std::optional<std::vector<unsigned char>> ExtractOpReturnData(const CScript& script)
{
    if (script.size() < 1 || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc = script.begin() + 1;
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    // Only a real data push (OP_PUSHDATA*, or a direct 1..75 byte push) qualifies;
    // OP_0/OP_1..OP_16/OP_1NEGATE and any other opcode return no data.
    if (opcode > OP_PUSHDATA4 || data.empty()) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    if (data.size() < MIN_PAYLOAD || data.size() > MAX_PAYLOAD) return std::nullopt;
    return data;
}

std::optional<unsigned int> FindOpReturn(const CTransaction& tx)
{
    std::optional<unsigned int> found;
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CScript& s = tx.vout[i].scriptPubKey;
        if (s.size() >= 1 && s[0] == OP_RETURN) {
            if (found.has_value()) return std::nullopt; // more than one => non-Yellowback
            found = i;
        }
    }
    return found;
}

std::optional<FoundPayload> FindPayload(const CTransaction& tx)
{
    auto idx = FindOpReturn(tx);
    if (!idx.has_value()) return std::nullopt;
    auto data = ExtractOpReturnData(tx.vout[idx.value()].scriptPubKey);
    if (!data.has_value()) return std::nullopt;
    FoundPayload fp;
    fp.opReturnIndex = idx.value();
    if (!DecodePayload(data.value(), fp.payload)) return std::nullopt;
    for (const Assignment& a : fp.payload.assignments) {
        if (a.vout >= tx.vout.size()) return std::nullopt;
        if (a.vout == fp.opReturnIndex) return std::nullopt;
    }
    return fp;
}

const char* PayloadTypeName(PayloadType type)
{
    switch (type) {
    case PayloadType::MINT: return "mint";
    case PayloadType::TRANSFER: return "transfer";
    case PayloadType::REDEEM: return "redeem";
    case PayloadType::PRICE: return "price";
    }
    return "unknown";
}

} // namespace yellowback
