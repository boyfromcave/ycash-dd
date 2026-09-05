// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YDOLLAR_VIEW_H
#define YCASH_YDOLLAR_VIEW_H

#include "amount.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "serialize.h"
#include "uint256.h"
#include "ydollar/params.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * The YDollar state view (plan §3.5, D19): an ordered key/value store with
 * the per-table record types on top. The state machine in state.cpp reads
 * and writes only through StateView, so it runs identically over the
 * in-memory view (unit tests, dry runs) and the LevelDB view (db.cpp).
 *
 * Key layout: one prefix byte per table, fixed-width big-endian heights so
 * lexicographic order is height order.
 *
 *   T                     Tip
 *   A                     Anchor
 *   Rc                    roster count;  R<u32 index>     RosterRecord
 *   P<u32 height>         price (MicroUsd)
 *   V<outpoint>           VaultRecord
 *   K<outpoint>           TokenRecord
 *   L<txid>               TxLogRecord
 *   S<u32 height>         Snapshot
 *   G                     Totals
 *   O                     Volatility
 *   U<blockhash>          Undo (not part of the state hash)
 */
namespace ydollar {

static const uint32_t SCHEMA_VERSION = 1;

/** Abstract ordered byte-string store. */
class StateView
{
public:
    virtual ~StateView() {}
    virtual bool Read(const std::string& key, std::string& value) const = 0;
    virtual void Write(const std::string& key, const std::string& value) = 0;
    virtual void Erase(const std::string& key) = 0;
    /** Visit every key with the prefix in ascending key order; return false from the callback to stop. */
    virtual void Iterate(const std::string& prefix, const std::function<bool(const std::string&, const std::string&)>& fn) const = 0;

    bool Exists(const std::string& key) const { std::string v; return Read(key, v); }
};

/** std::map-backed view for tests and dry runs. */
class MemoryStateView : public StateView
{
public:
    bool Read(const std::string& key, std::string& value) const override;
    void Write(const std::string& key, const std::string& value) override;
    void Erase(const std::string& key) override;
    void Iterate(const std::string& prefix, const std::function<bool(const std::string&, const std::string&)>& fn) const override;
    const std::map<std::string, std::string>& Map() const { return map; }
    bool operator==(const MemoryStateView& o) const { return map == o.map; }

private:
    std::map<std::string, std::string> map;
};

/**
 * Copy-on-write overlay over another view. Reads fall through to the base
 * unless overridden; writes stay in the overlay until Commit() is called
 * (or are discarded). This is how a block is applied atomically and how a
 * transaction is dry-run without touching the index.
 */
class OverlayStateView : public StateView
{
public:
    explicit OverlayStateView(StateView& base) : base(base) {}
    bool Read(const std::string& key, std::string& value) const override;
    void Write(const std::string& key, const std::string& value) override;
    void Erase(const std::string& key) override;
    void Iterate(const std::string& prefix, const std::function<bool(const std::string&, const std::string&)>& fn) const override;

    /** Pending writes: key -> value, or nullopt for an erase. */
    const std::map<std::string, std::optional<std::string>>& Pending() const { return pending; }
    /** Apply the pending writes to the base and clear them. */
    void Commit();
    void Discard() { pending.clear(); }

private:
    StateView& base;
    std::map<std::string, std::optional<std::string>> pending;
};

// ---------------------------------------------------------------------------
// Records (plan §3.5)

struct TipRecord
{
    int32_t height;
    uint256 blockHash;
    uint32_t schemaVersion;
    std::string network;

    TipRecord() : height(0), schemaVersion(SCHEMA_VERSION) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(height);
        READWRITE(blockHash);
        READWRITE(schemaVersion);
        READWRITE(network);
    }
};

struct AnchorRecord
{
    COutPoint outpoint;
    CAmount nValue;
    CScript scriptPubKey;
    bool valid; //!< false once custody is broken (§3.7 PRICE-2)

    AnchorRecord() : nValue(0), valid(false) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(outpoint);
        READWRITE(nValue);
        READWRITE(*(CScriptBase*)(&scriptPubKey));
        READWRITE(valid);
    }
};

struct RosterRecord
{
    CScript script;
    int32_t revealHeight;

    RosterRecord() : revealHeight(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(*(CScriptBase*)(&script));
        READWRITE(revealHeight);
    }
};

enum class VaultStatus : uint8_t { ACTIVE = 1, VOID = 2, CLOSED = 3 };
const char* VaultStatusName(VaultStatus s);

struct VaultRecord
{
    CPubKey ownerPubKey;
    uint8_t tier;
    uint32_t lockHeight;
    CAmount collateralZat;
    int64_t mintedCents;
    int32_t mintHeight;
    uint8_t status;              //!< VaultStatus
    bool wasActive;              //!< status before CLOSED (ACTIVE or VOID)
    int32_t closeHeight;
    uint256 closingTxid;
    int64_t burnedCents;
    int32_t errBpsAtClose;
    int64_t requiredBurnAtClose;
    int32_t rosterIndex;         //!< -1 if the vault script matched no roster (VOID)
    std::string voidReason;      //!< verdict reason for a VOID vault

    VaultRecord() : tier(0), lockHeight(0), collateralZat(0), mintedCents(0), mintHeight(0),
                    status((uint8_t)VaultStatus::VOID), wasActive(false), closeHeight(0), burnedCents(0),
                    errBpsAtClose(0), requiredBurnAtClose(0), rosterIndex(-1) {}

    VaultStatus Status() const { return (VaultStatus)status; }
    bool IsOpen() const { return Status() == VaultStatus::ACTIVE || Status() == VaultStatus::VOID; }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(ownerPubKey);
        READWRITE(tier);
        READWRITE(lockHeight);
        READWRITE(collateralZat);
        READWRITE(mintedCents);
        READWRITE(mintHeight);
        READWRITE(status);
        READWRITE(wasActive);
        READWRITE(closeHeight);
        READWRITE(closingTxid);
        READWRITE(burnedCents);
        READWRITE(errBpsAtClose);
        READWRITE(requiredBurnAtClose);
        READWRITE(rosterIndex);
        READWRITE(voidReason);
    }
};

struct TokenRecord
{
    int64_t cents;
    CAmount nValue;
    CScript scriptPubKey;
    int32_t height;

    TokenRecord() : cents(0), nValue(0), height(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(cents);
        READWRITE(nValue);
        READWRITE(*(CScriptBase*)(&scriptPubKey));
        READWRITE(height);
    }
};

struct AssignedOutput
{
    COutPoint outpoint;
    int64_t cents;
    CScript scriptPubKey;

    AssignedOutput() : cents(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(outpoint);
        READWRITE(cents);
        READWRITE(*(CScriptBase*)(&scriptPubKey));
    }
};

/** One entry per transaction that carried a payload or spent a Tokens / Vaults / Anchor outpoint (plan E1). */
struct TxLogRecord
{
    int32_t height;
    uint8_t type;                //!< PayloadType, or 0 for no (well-formed) payload
    std::string verdict;         //!< stable reason string (§3.7 / Phase 2 list)
    int64_t ydIn;
    int64_t ydOut;               //!< cents assigned (XFER) or minted (MINT)
    int64_t burned;
    std::vector<AssignedOutput> assigned;
    std::vector<AssignedOutput> spentTokens;   //!< Tokens consumed by this tx (scriptPubKey kept for wallet filtering)
    std::vector<COutPoint> closedVaults;
    bool anchorSpend;
    bool priceRecorded;

    TxLogRecord() : height(0), type(0), ydIn(0), ydOut(0), burned(0), anchorSpend(false), priceRecorded(false) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(height);
        READWRITE(type);
        READWRITE(verdict);
        READWRITE(ydIn);
        READWRITE(ydOut);
        READWRITE(burned);
        READWRITE(assigned);
        READWRITE(spentTokens);
        READWRITE(closedVaults);
        READWRITE(anchorSpend);
        READWRITE(priceRecorded);
    }
};

struct Totals
{
    int64_t supplyCents;
    CAmount collateralZat;
    uint32_t activeVaults;
    uint32_t voidVaults;

    Totals() : supplyCents(0), collateralZat(0), activeVaults(0), voidVaults(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(supplyCents);
        READWRITE(collateralZat);
        READWRITE(activeVaults);
        READWRITE(voidVaults);
    }
};

struct Volatility
{
    int32_t lastBreachHeight; //!< -1 = never

    Volatility() : lastBreachHeight(-1) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(lastBreachHeight);
    }
};

struct Snapshot
{
    uint256 blockHash;
    int64_t supplyCents;
    CAmount collateralZat;
    bool priceDefined;
    int64_t price;
    int32_t healthPct;
    int32_t dcaBps;
    int32_t errBps;
    bool mintFrozen;

    Snapshot() : supplyCents(0), collateralZat(0), priceDefined(false), price(0), healthPct(0), dcaBps(0), errBps(0), mintFrozen(false) {}

    std::optional<MicroUsd> Price() const { return priceDefined ? std::optional<MicroUsd>(price) : std::nullopt; }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(blockHash);
        READWRITE(supplyCents);
        READWRITE(collateralZat);
        READWRITE(priceDefined);
        READWRITE(price);
        READWRITE(healthPct);
        READWRITE(dcaBps);
        READWRITE(errBps);
        READWRITE(mintFrozen);
    }
};

/** One undo entry: the value a key had before the block (or none). Restoring every entry restores the state byte for byte. */
struct UndoEntry
{
    std::string key;
    bool hadValue;
    std::string value;

    UndoEntry() : hadValue(false) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(key);
        READWRITE(hadValue);
        READWRITE(value);
    }
};

struct UndoRecord
{
    int32_t height;
    std::vector<UndoEntry> entries;

    UndoRecord() : height(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(height);
        READWRITE(entries);
    }
};

// ---------------------------------------------------------------------------
// Keys

namespace keys {
std::string Tip();
std::string Anchor();
std::string RosterCount();
std::string Roster(uint32_t index);
std::string Price(uint32_t height);
std::string Vault(const COutPoint& out);
std::string Token(const COutPoint& out);
std::string TxLog(const uint256& txid);
std::string Snapshot(uint32_t height);
std::string Totals();
std::string Volatility();
std::string Undo(const uint256& blockHash);
extern const char PREFIX_UNDO;
} // namespace keys

// ---------------------------------------------------------------------------
// Typed access

template <typename T>
std::string SerializeRecord(const T& t);
template <typename T>
bool DeserializeRecord(const std::string& s, T& t);

/**
 * Typed reads and writes over a StateView. Writes are optionally recorded
 * into an UndoRecord (the first pre-image of each key wins).
 */
class State
{
public:
    explicit State(StateView& view, UndoRecord* undo = nullptr) : view(view), undo(undo) {}

    template <typename T> bool Get(const std::string& key, T& out) const
    {
        std::string raw;
        if (!view.Read(key, raw)) return false;
        return DeserializeRecord(raw, out);
    }
    template <typename T> void Put(const std::string& key, const T& t) { WriteRaw(key, SerializeRecord(t)); }
    void EraseKey(const std::string& key);
    bool Has(const std::string& key) const { return view.Exists(key); }

    std::optional<TipRecord> GetTip() const;
    AnchorRecord GetAnchor() const;
    std::vector<RosterRecord> GetRosters() const;
    void AppendRoster(const RosterRecord& r);
    std::optional<MicroUsd> GetPriceAt(uint32_t height) const;          //!< the attestation recorded at exactly this height
    std::optional<MicroUsd> PriceInEffect(uint32_t height) const;       //!< price(H) of §3.6
    std::optional<uint32_t> PriceSourceHeight(uint32_t height) const;
    std::optional<VaultRecord> GetVault(const COutPoint& out) const;
    std::optional<TokenRecord> GetToken(const COutPoint& out) const;
    std::optional<TxLogRecord> GetTxLog(const uint256& txid) const;
    std::optional<Snapshot> GetSnapshot(uint32_t height) const;
    Totals GetTotals() const;
    Volatility GetVolatility() const;

    StateView& View() { return view; }
    const StateView& View() const { return view; }

private:
    void WriteRaw(const std::string& key, const std::string& value);
    void RecordUndo(const std::string& key);

    StateView& view;
    UndoRecord* undo;
};

/** SHA-256 over every (key, value) in key order, excluding undo records. */
uint256 StateHash(const StateView& view);

} // namespace ydollar

#endif // YCASH_YDOLLAR_VIEW_H
