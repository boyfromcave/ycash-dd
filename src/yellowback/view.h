// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_VIEW_H
#define YCASH_YELLOWBACK_VIEW_H

#include "amount.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "serialize.h"
#include "uint256.h"
#include "yellowback/params.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * The Yellowback state view (plan §3.6, V2): an ordered key/value store with
 * the per-table record types on top. The state machine in state.cpp reads
 * and writes only through StateView, so it runs identically over the
 * in-memory view (unit tests, dry runs, the template filter's overlay) and
 * the LevelDB view (db.cpp).
 *
 * Key layout (§3.6): one prefix byte per table, fixed-width big-endian
 * heights so lexicographic order is height order, outpoints as the raw txid
 * bytes followed by the big-endian index.
 *
 *   T                     Tip
 *   Q<u32 height>         TagRecord         (valid coinbase tags >= START_HEIGHT)
 *   J<u32 height>         Judgement         (REG-4, written at height + PEER_LAG)
 *   C                     Activation        (the carried ACT-2/3 state)
 *   V<outpoint>           VaultRecord
 *   K<outpoint>           TokenRecord
 *   L<txid>               TxLogRecord       (history; not part of the state hash, N7)
 *   S<u32 height>         Snapshot
 *   G                     Totals
 *   X<blockhash>          RejectedRecord    (node-local, V13; not part of the state hash)
 *   P                     ParamsRecord      (the four hashed regtest values, N18)
 *   U<blockhash>          Undo              (not part of the state hash)
 *
 * The federation prototype's A (anchor), Rc/R<u32> (roster), P<u32> (price
 * table) and O (volatility) keys are gone; SCHEMA_VERSION 2 makes a v1
 * database wipe itself on start (index.cpp).
 *
 * Canonical serialisation (§3.6 *State hash*): Ycash's READWRITE encoding —
 * fixed-width little-endian integers, u8 booleans and enumerations,
 * uint160/uint256 as raw bytes, strings and byte vectors CompactSize-
 * prefixed. qa/rpc-tests/test_framework/SERIALISATION.md is the byte-level
 * reference the Python model implements; every record below matches it.
 */
namespace yellowback {

static const uint32_t SCHEMA_VERSION = 2;

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
    bool operator!=(const MemoryStateView& o) const { return map != o.map; }

private:
    std::map<std::string, std::string> map;
};

/**
 * Copy-on-write overlay over another view. Reads fall through to the base
 * unless overridden; writes stay in the overlay until Commit() is called
 * (or are discarded). This is how a block is applied atomically, how a
 * transaction is dry-run without touching the index, and the view the
 * template filter applies candidates to (TPL-1).
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
// Records (plan §3.6)

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

/** Tags[height]: a valid coinbase tag (TAG-1..5). */
struct TagRecord
{
    uint160 payoutKey;
    uint64_t priceMicroUsd;   //!< 0 = signal-only (TAG-3)
    bool signal;
    uint16_t sourceMask;

    TagRecord() : priceMicroUsd(0), signal(false), sourceMask(0) {}

    bool IsQuote() const { return priceMicroUsd != 0; }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(payoutKey);
        READWRITE(priceMicroUsd);
        READWRITE(signal);
        READWRITE(sourceMask);
    }
};

/** Judgements[t]: REG-4 for the quote tag at t, written when block t + PEER_LAG is applied. */
struct Judgement
{
    bool evaluated;   //!< false when fewer than PEER_MIN peers quoted around t
    bool inBand;
    bool penalized;

    Judgement() : evaluated(false), inBand(false), penalized(false) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(evaluated);
        READWRITE(inBand);
        READWRITE(penalized);
    }
};

/** Activation.status, declaration order (ACT-2/3; status never moves backward). */
enum class ActivationStatus : uint8_t { SIGNALING = 0, LOCKED_IN = 1, ACTIVE = 2 };
const char* ActivationStatusName(ActivationStatus s);

struct Activation
{
    uint8_t status;           //!< ActivationStatus
    int32_t lockInHeight;
    int32_t activateHeight;

    Activation() : status((uint8_t)ActivationStatus::SIGNALING), lockInHeight(0), activateHeight(0) {}

    ActivationStatus Status() const { return (ActivationStatus)status; }
    bool IsActive() const { return Status() == ActivationStatus::ACTIVE; }

    friend bool operator==(const Activation& a, const Activation& b)
    {
        return a.status == b.status && a.lockInHeight == b.lockInHeight && a.activateHeight == b.activateHeight;
    }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(status);
        READWRITE(lockInHeight);
        READWRITE(activateHeight);
    }
};

/** Vaults.status, declaration order (§3.6). */
enum class VaultStatus : uint8_t { ACTIVE = 0, VOID = 1, CLOSED = 2, CLAIMED = 3 };
const char* VaultStatusName(VaultStatus s);

struct VaultRecord
{
    std::vector<unsigned char> ownerPubKey;   //!< the 33 payload bytes verbatim (valid or not; a VOID vault may carry an invalid key)
    uint8_t termClass;                        //!< the payload byte (0/1/2 = A/B/C; a VOID vault may carry any value)
    int32_t lockHeight;
    int32_t claimHeight;                      //!< lockHeight + GRACE
    CAmount collateralZat;                    //!< vout[0].nValue
    int64_t mintedCents;
    int32_t mintHeight;
    int32_t refHeight;
    uint8_t status;                           //!< VaultStatus
    std::string voidReason;                   //!< the MINT verdict for a VOID vault, else ""
    int32_t closeHeight;
    uint256 closingTxid;
    int64_t burnedCents;                      //!< IN-3's burn of the closing transaction
    CAmount feePaidZat;                       //!< the mint's fee, rewritten by a passing redeem/claim (0 under FEE-0)
    bool unbacked;                            //!< closed by a spend that failed RED-1..4 with burned < mintedCents

    VaultRecord() : termClass(0), lockHeight(0), claimHeight(0), collateralZat(0), mintedCents(0), mintHeight(0), refHeight(0),
                    status((uint8_t)VaultStatus::VOID), closeHeight(0), burnedCents(0), feePaidZat(0), unbacked(false) {}

    VaultStatus Status() const { return (VaultStatus)status; }
    bool IsOpen() const { return Status() == VaultStatus::ACTIVE || Status() == VaultStatus::VOID; }
    /** The owner key as a CPubKey (invalid when the bytes are not a key). */
    CPubKey OwnerKey() const { return CPubKey(ownerPubKey.begin(), ownerPubKey.end()); }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(ownerPubKey);
        READWRITE(termClass);
        READWRITE(lockHeight);
        READWRITE(claimHeight);
        READWRITE(collateralZat);
        READWRITE(mintedCents);
        READWRITE(mintHeight);
        READWRITE(refHeight);
        READWRITE(status);
        READWRITE(voidReason);
        READWRITE(closeHeight);
        READWRITE(closingTxid);
        READWRITE(burnedCents);
        READWRITE(feePaidZat);
        READWRITE(unbacked);
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

/** TxLog.type: the payload family a transaction was processed as (M3: any ACTIVE-vault spend is REDEEM). */
enum class TxLogType : uint8_t { NONE = 0, MINT = 1, TRANSFER = 2, REDEEM = 3 };
const char* TxLogTypeName(TxLogType t);

/**
 * One entry per transaction that created or spent a Tokens / Vaults entry
 * (N7: never for a payload alone). History only; excluded from the state hash.
 */
struct TxLogRecord
{
    int32_t height;
    uint8_t type;                //!< TxLogType
    std::string path;            //!< "owner" | "claim" | "" (vault spends only, §3.4)
    std::string verdict;         //!< §4.2a verdict string
    int64_t yedIn;
    int64_t yedOut;              //!< cents assigned (XFER/RED) or minted (MINT)
    int64_t burned;
    CAmount feeZat;              //!< the enforcement fee paid (0 under FEE-0 or on failure)
    bool hasPayee;
    uint160 payee;               //!< key hash of the fee output when hasPayee
    std::vector<AssignedOutput> assigned;
    std::vector<AssignedOutput> spentTokens;   //!< Tokens consumed by this tx (scriptPubKey kept for wallet filtering, N21)
    std::vector<COutPoint> closedVaults;

    TxLogRecord() : height(0), type((uint8_t)TxLogType::NONE), yedIn(0), yedOut(0), burned(0), feeZat(0), hasPayee(false) {}

    TxLogType Type() const { return (TxLogType)type; }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(height);
        READWRITE(type);
        READWRITE(path);
        READWRITE(verdict);
        READWRITE(yedIn);
        READWRITE(yedOut);
        READWRITE(burned);
        READWRITE(feeZat);
        READWRITE(hasPayee);
        READWRITE(payee);
        READWRITE(assigned);
        READWRITE(spentTokens);
        READWRITE(closedVaults);
    }
};

struct Totals
{
    int64_t supplyCents;
    CAmount collateralZat;       //!< ACTIVE vaults only (a VOID vault's collateral never enters)
    uint32_t activeVaults;
    uint32_t voidVaults;
    uint32_t closedVaults;
    uint32_t claimedVaults;
    int64_t unbackedCents;

    Totals() : supplyCents(0), collateralZat(0), activeVaults(0), voidVaults(0), closedVaults(0), claimedVaults(0), unbackedCents(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(supplyCents);
        READWRITE(collateralZat);
        READWRITE(activeVaults);
        READWRITE(voidVaults);
        READWRITE(closedVaults);
        READWRITE(claimedVaults);
        READWRITE(unbackedCents);
    }
};

/** haltMask bits, declaration order (§3.6). */
enum HaltBit : uint32_t {
    HALT_NOT_ACTIVE    = 1 << 0,
    HALT_NO_PRICE      = 1 << 1,
    HALT_PARTICIPATION = 1 << 2,
    HALT_GLOBAL_RATIO  = 1 << 3,
    HALT_DIVERGENCE    = 1 << 4,
    HALT_ENFORCEMENT   = 1 << 5,
};
/** The names of the set bits in declaration order (RPC rendering). */
std::vector<std::string> HaltMaskNames(uint32_t mask);

/**
 * Snapshots[H] (§3.6). Undefined prices and ratios are stored as 0
 * (PRICE_MIN = 100, so 0 is never a value; M1). sigmaMultBps is always
 * defined. The virtual snapshot below START_HEIGHT is Snapshot::Virtual().
 */
struct Snapshot
{
    uint256 blockHash;
    bool tagged;
    bool quote;
    uint32_t signalCount;
    Activation activation;
    int64_t pFast;
    int64_t pMid;
    int64_t pSlow;
    int64_t pMint;
    int64_t pClaim;
    int32_t sigmaMultBps;
    CAmount issuedZat;
    int64_t supplyCents;
    CAmount collateralZat;
    int64_t globalRatioBps;
    uint32_t haltMask;

    Snapshot() : tagged(false), quote(false), signalCount(0), pFast(0), pMid(0), pSlow(0), pMint(0), pClaim(0),
                 sigmaMultBps(10000), issuedZat(0), supplyCents(0), collateralZat(0), globalRatioBps(0), haltMask(0) {}

    /** The virtual snapshot every rule sees below START_HEIGHT (§3.6). */
    static Snapshot Virtual()
    {
        Snapshot s;
        s.haltMask = HALT_NOT_ACTIVE | HALT_NO_PRICE;
        return s;
    }

    static std::optional<MicroUsd> Price(int64_t v) { return v > 0 ? std::optional<MicroUsd>(v) : std::nullopt; }
    std::optional<MicroUsd> PFast() const { return Price(pFast); }
    std::optional<MicroUsd> PMid() const { return Price(pMid); }
    std::optional<MicroUsd> PSlow() const { return Price(pSlow); }
    std::optional<MicroUsd> PMint() const { return Price(pMint); }
    std::optional<MicroUsd> PClaim() const { return Price(pClaim); }
    std::optional<int64_t> GlobalRatioBps() const { return globalRatioBps > 0 ? std::optional<int64_t>(globalRatioBps) : std::nullopt; }

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(blockHash);
        READWRITE(tagged);
        READWRITE(quote);
        READWRITE(signalCount);
        READWRITE(activation);
        READWRITE(pFast);
        READWRITE(pMid);
        READWRITE(pSlow);
        READWRITE(pMint);
        READWRITE(pClaim);
        READWRITE(sigmaMultBps);
        READWRITE(issuedZat);
        READWRITE(supplyCents);
        READWRITE(collateralZat);
        READWRITE(globalRatioBps);
        READWRITE(haltMask);
    }
};

/** Rejected[blockHash]: a block this node refused (BLK-2; node-local, never a state input, V13). */
struct RejectedRecord
{
    int32_t height;
    std::string reason;

    RejectedRecord() : height(0) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(height);
        READWRITE(reason);
    }
};

/** Params: the four regtest values in the state-hash preimage (M13, N18); written by the first applied block. */
struct ParamsRecord
{
    int32_t startHeight;
    int32_t sigmaRefBps;
    int32_t supplyCapBps;
    int32_t enforceUntil;

    ParamsRecord() : startHeight(0), sigmaRefBps(0), supplyCapBps(0), enforceUntil(0) {}
    explicit ParamsRecord(const Params& p)
        : startHeight(p.startHeight), sigmaRefBps(p.sigmaRefBps), supplyCapBps(p.supplyCapBps), enforceUntil(p.enforceUntilHeight) {}

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(startHeight);
        READWRITE(sigmaRefBps);
        READWRITE(supplyCapBps);
        READWRITE(enforceUntil);
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
std::string Tag(uint32_t height);
std::string Judgement(uint32_t height);
std::string Activation();
std::string Vault(const COutPoint& out);
std::string Token(const COutPoint& out);
std::string TxLog(const uint256& txid);
std::string Snapshot(uint32_t height);
std::string Totals();
std::string Rejected(const uint256& blockHash);
std::string Params();
std::string Undo(const uint256& blockHash);
extern const char PREFIX_UNDO;
extern const char PREFIX_REJECTED;
extern const char PREFIX_TXLOG;
/** The txid of a vault/token key (the 32 bytes after the prefix); zero if the key is too short. */
uint256 OutPointHashOf(const std::string& key);
uint32_t OutPointIndexOf(const std::string& key);
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
    std::optional<TagRecord> GetTag(uint32_t height) const;
    std::optional<Judgement> GetJudgement(uint32_t height) const;
    Activation GetActivation() const;                                   //!< SIGNALING/0/0 when absent
    std::optional<VaultRecord> GetVault(const COutPoint& out) const;
    std::optional<TokenRecord> GetToken(const COutPoint& out) const;
    std::optional<TxLogRecord> GetTxLog(const uint256& txid) const;
    std::optional<Snapshot> GetSnapshot(uint32_t height) const;
    Totals GetTotals() const;
    std::optional<RejectedRecord> GetRejected(const uint256& blockHash) const;
    std::optional<ParamsRecord> GetParamsRecord() const;

    StateView& View() { return view; }
    const StateView& View() const { return view; }

private:
    void WriteRaw(const std::string& key, const std::string& value);
    void RecordUndo(const std::string& key);

    StateView& view;
    UndoRecord* undo;
};

/**
 * The state hash (§3.6 *State hash*, N18): SHA-256 over the concatenation,
 * in this order, of key ‖ value for Tip, every Tags record (ascending
 * height), every Judgements record, Activation, every Vaults record
 * (ascending outpoint), every Tokens record, Totals, every Snapshots record
 * and the Params record. TxLog (L), Rejected (X) and Undo (U) are excluded.
 * When no Tip has been written yet the preimage carries a zero tip with
 * `network` as given (the Python model's `{height 0, zero hash}`).
 */
uint256 StateHash(const StateView& view, const std::string& network = "");

} // namespace yellowback

#endif // YCASH_YELLOWBACK_VIEW_H
