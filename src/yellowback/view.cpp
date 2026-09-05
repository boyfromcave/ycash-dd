// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/view.h"

#include "clientversion.h"
#include "crypto/sha256.h"
#include "streams.h"

namespace yellowback {

// ---------------------------------------------------------------------------
// MemoryStateView

bool MemoryStateView::Read(const std::string& key, std::string& value) const
{
    auto it = map.find(key);
    if (it == map.end()) return false;
    value = it->second;
    return true;
}

void MemoryStateView::Write(const std::string& key, const std::string& value) { map[key] = value; }
void MemoryStateView::Erase(const std::string& key) { map.erase(key); }

void MemoryStateView::Iterate(const std::string& prefix, const std::function<bool(const std::string&, const std::string&)>& fn) const
{
    for (auto it = map.lower_bound(prefix); it != map.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) break;
        if (!fn(it->first, it->second)) break;
    }
}

// ---------------------------------------------------------------------------
// OverlayStateView

bool OverlayStateView::Read(const std::string& key, std::string& value) const
{
    auto it = pending.find(key);
    if (it != pending.end()) {
        if (!it->second.has_value()) return false;
        value = it->second.value();
        return true;
    }
    return base.Read(key, value);
}

void OverlayStateView::Write(const std::string& key, const std::string& value) { pending[key] = value; }
void OverlayStateView::Erase(const std::string& key) { pending[key] = std::nullopt; }

void OverlayStateView::Iterate(const std::string& prefix, const std::function<bool(const std::string&, const std::string&)>& fn) const
{
    // Merge the base's keys with the pending overrides, in key order.
    std::map<std::string, std::string> merged;
    base.Iterate(prefix, [&](const std::string& k, const std::string& v) { merged[k] = v; return true; });
    for (auto it = pending.lower_bound(prefix); it != pending.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) break;
        if (it->second.has_value()) merged[it->first] = it->second.value();
        else merged.erase(it->first);
    }
    for (const auto& kv : merged) {
        if (!fn(kv.first, kv.second)) break;
    }
}

void OverlayStateView::Commit()
{
    for (const auto& kv : pending) {
        if (kv.second.has_value()) base.Write(kv.first, kv.second.value());
        else base.Erase(kv.first);
    }
    pending.clear();
}

// ---------------------------------------------------------------------------
// Keys

namespace keys {

const char PREFIX_UNDO = 'U';

static std::string U32BE(uint32_t v)
{
    std::string s(4, '\0');
    s[0] = (char)((v >> 24) & 0xff);
    s[1] = (char)((v >> 16) & 0xff);
    s[2] = (char)((v >> 8) & 0xff);
    s[3] = (char)(v & 0xff);
    return s;
}

static std::string OutPointKey(char prefix, const COutPoint& out)
{
    std::string s(1, prefix);
    s.append((const char*)out.hash.begin(), 32);
    s.append(U32BE(out.n));
    return s;
}

std::string Tip() { return "T"; }
std::string Anchor() { return "A"; }
std::string RosterCount() { return "Rc"; }
std::string Roster(uint32_t index) { return "R" + U32BE(index); }
std::string Price(uint32_t height) { return "P" + U32BE(height); }
std::string Vault(const COutPoint& out) { return OutPointKey('V', out); }
std::string Token(const COutPoint& out) { return OutPointKey('K', out); }
std::string TxLog(const uint256& txid) { return "L" + std::string((const char*)txid.begin(), 32); }
std::string Snapshot(uint32_t height) { return "S" + U32BE(height); }
std::string Totals() { return "G"; }
std::string Volatility() { return "O"; }
std::string Undo(const uint256& blockHash) { return std::string(1, PREFIX_UNDO) + std::string((const char*)blockHash.begin(), 32); }

} // namespace keys

// ---------------------------------------------------------------------------
// Serialization

template <typename T>
std::string SerializeRecord(const T& t)
{
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << t;
    return ss.str();
}

template <typename T>
bool DeserializeRecord(const std::string& s, T& t)
{
    try {
        CDataStream ss(s.data(), s.data() + s.size(), SER_DISK, CLIENT_VERSION);
        ss >> t;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Explicit instantiations for every record type.
#define YD_RECORD(T) \
    template std::string SerializeRecord<T>(const T&); \
    template bool DeserializeRecord<T>(const std::string&, T&);
YD_RECORD(TipRecord)
YD_RECORD(AnchorRecord)
YD_RECORD(RosterRecord)
YD_RECORD(VaultRecord)
YD_RECORD(TokenRecord)
YD_RECORD(TxLogRecord)
YD_RECORD(Totals)
YD_RECORD(Volatility)
YD_RECORD(Snapshot)
YD_RECORD(UndoRecord)
YD_RECORD(int64_t)
YD_RECORD(uint32_t)
#undef YD_RECORD

const char* VaultStatusName(VaultStatus s)
{
    switch (s) {
    case VaultStatus::ACTIVE: return "ACTIVE";
    case VaultStatus::VOID: return "VOID";
    case VaultStatus::CLOSED: return "CLOSED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// State

void State::RecordUndo(const std::string& key)
{
    if (!undo) return;
    for (const UndoEntry& e : undo->entries) {
        if (e.key == key) return; // first pre-image wins
    }
    UndoEntry e;
    e.key = key;
    e.hadValue = view.Read(key, e.value);
    undo->entries.push_back(e);
}

void State::WriteRaw(const std::string& key, const std::string& value)
{
    RecordUndo(key);
    view.Write(key, value);
}

void State::EraseKey(const std::string& key)
{
    RecordUndo(key);
    view.Erase(key);
}

std::optional<TipRecord> State::GetTip() const
{
    TipRecord t;
    if (!Get(keys::Tip(), t)) return std::nullopt;
    return t;
}

AnchorRecord State::GetAnchor() const
{
    AnchorRecord a;
    if (!Get(keys::Anchor(), a)) return AnchorRecord();
    return a;
}

std::vector<RosterRecord> State::GetRosters() const
{
    std::vector<RosterRecord> out;
    uint32_t count = 0;
    if (!Get(keys::RosterCount(), count)) return out;
    for (uint32_t i = 0; i < count; i++) {
        RosterRecord r;
        if (Get(keys::Roster(i), r)) out.push_back(r);
    }
    return out;
}

void State::AppendRoster(const RosterRecord& r)
{
    uint32_t count = 0;
    Get(keys::RosterCount(), count);
    Put(keys::Roster(count), r);
    Put(keys::RosterCount(), (uint32_t)(count + 1));
}

std::optional<MicroUsd> State::GetPriceAt(uint32_t height) const
{
    int64_t p;
    if (!Get(keys::Price(height), p)) return std::nullopt;
    return p;
}

std::optional<uint32_t> State::PriceSourceHeight(uint32_t height) const
{
    for (uint32_t h = height;; h--) {
        if (Has(keys::Price(h))) return h;
        if (h == 0 || height - h >= (uint32_t)PRICE_MAX_AGE) break;
    }
    return std::nullopt;
}

std::optional<MicroUsd> State::PriceInEffect(uint32_t height) const
{
    auto h = PriceSourceHeight(height);
    if (!h.has_value()) return std::nullopt;
    return GetPriceAt(h.value());
}

std::optional<VaultRecord> State::GetVault(const COutPoint& out) const
{
    VaultRecord v;
    if (!Get(keys::Vault(out), v)) return std::nullopt;
    return v;
}

std::optional<TokenRecord> State::GetToken(const COutPoint& out) const
{
    TokenRecord t;
    if (!Get(keys::Token(out), t)) return std::nullopt;
    return t;
}

std::optional<TxLogRecord> State::GetTxLog(const uint256& txid) const
{
    TxLogRecord l;
    if (!Get(keys::TxLog(txid), l)) return std::nullopt;
    return l;
}

std::optional<Snapshot> State::GetSnapshot(uint32_t height) const
{
    Snapshot s;
    if (!Get(keys::Snapshot(height), s)) return std::nullopt;
    return s;
}

Totals State::GetTotals() const
{
    Totals t;
    Get(keys::Totals(), t);
    return t;
}

Volatility State::GetVolatility() const
{
    Volatility v;
    Get(keys::Volatility(), v);
    return v;
}

uint256 StateHash(const StateView& view)
{
    CSHA256 hasher;
    view.Iterate("", [&](const std::string& k, const std::string& v) {
        if (!k.empty() && k[0] == keys::PREFIX_UNDO) return true;
        uint32_t kl = k.size(), vl = v.size();
        hasher.Write((const unsigned char*)&kl, 4);
        hasher.Write((const unsigned char*)k.data(), k.size());
        hasher.Write((const unsigned char*)&vl, 4);
        hasher.Write((const unsigned char*)v.data(), v.size());
        return true;
    });
    uint256 out;
    hasher.Finalize(out.begin());
    return out;
}

} // namespace yellowback
