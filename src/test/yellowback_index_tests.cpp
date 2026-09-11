// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// The synchronous index and its hooks (plan Phase 3, §4.2a, §4.3, §8.4 items
// 3-8): fault injection at each hook (BLK-3), the tip-mismatch guards, the
// K5 null-hash shape of TestBlockValidity, the K6 re-verification no-op, the
// BLK-2 suppressions (kill switch, IBD/reindex, catch-up, the tripped valve,
// the sunset), the ACT-7 work valve with the P1 warning and the P2 bounds,
// MP-1 and the mempool sweep, MINER-1..3, and the MempoolCheck half of the
// N6 benchmark.
//
// The blocks are synthetic: real CBlock objects (whose hashes the index keys
// on) chained through fake CBlockIndex entries that hang off the regtest
// genesis, driven through CheckConnect/CommitConnect/UndoDisconnect exactly
// as ConnectBlock/DisconnectBlock would. chainActive stays at genesis, so a
// fake index is never "contained" (the re-verification case uses a real
// 100-block chain instead). Nothing here starts a node or the network.

#include "yellowback/index.h"
#include "yellowback/math.h"
#include "yellowback/payload.h"
#include "yellowback/policy.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/tag.h"
#include "yellowback/view.h"

#include "chain.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "key.h"
#include "main.h"
#include "pow.h"
#include "primitives/block.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"
#include "txmempool.h"
#include "utiltime.h"
#include "warnings.h"

#include <boost/test/unit_test.hpp>

#include <deque>

using namespace yellowback;

namespace {

uint160 KeyOf(int i)
{
    std::vector<unsigned char> v(20, (unsigned char)(0x10 + i));
    return uint160(v);
}

/**
 * A regtest TestingSetup whose tip is never "in initial block download": the
 * regtest genesis is years old, so nMaxTipAge is raised for the suite (the
 * IsInitialBlockDownload latch then stays false for the process, which is the
 * state every other suite runs in after TestChain100Setup anyway). Also
 * insulates the miner configuration read by the hooks.
 */
struct IndexSetup : public TestingSetup
{
    int64_t savedMaxTipAge;
    IndexSetup() : TestingSetup(CBaseChainParams::REGTEST), savedMaxTipAge(nMaxTipAge)
    {
        nMaxTipAge = 1LL << 40;
    }
    ~IndexSetup()
    {
        nMaxTipAge = savedMaxTipAge;
        // Fake entries a case inserted into mapBlockIndex are erased by the case's MapGuard.
    }
};

/** Inserts fake CBlockIndex entries into mapBlockIndex for a case and erases them again (they are owned by the case). */
struct MapGuard
{
    std::vector<uint256> inserted;
    void Insert(CBlockIndex* idx)
    {
        LOCK(cs_main);
        mapBlockIndex[idx->GetBlockHash()] = idx;
        inserted.push_back(idx->GetBlockHash());
    }
    ~MapGuard()
    {
        LOCK(cs_main);
        for (const uint256& h : inserted) mapBlockIndex.erase(h);
    }
};

/** A synthetic chain: real blocks, fake CBlockIndex entries hanging off the regtest genesis. */
struct Chain
{
    struct Node
    {
        CBlock block;
        uint256 hash;
        std::unique_ptr<CBlockIndex> idx;
    };
    std::deque<Node> nodes;

    CBlockIndex* Tip() { return nodes.empty() ? chainActive.Genesis() : nodes.back().idx.get(); }

    /** Append `block` on `parent` (default: the chain tip); the index entry carries real work arithmetic. */
    Node& Add(const CBlock& block, CBlockIndex* parent = nullptr)
    {
        if (!parent) parent = Tip();
        nodes.emplace_back();
        Node& n = nodes.back();
        n.block = block;
        if (parent->phashBlock) n.block.hashPrevBlock = parent->GetBlockHash();
        n.block.hashMerkleRoot = n.block.BuildMerkleTree();     // the hash must depend on the transactions
        n.hash = n.block.GetHash();
        n.idx.reset(new CBlockIndex());
        n.idx->pprev = parent;
        n.idx->nHeight = parent->nHeight + 1;
        n.idx->phashBlock = &n.hash;
        n.idx->nBits = chainActive.Genesis()->nBits;
        n.idx->nTime = (uint32_t)GetTime();
        n.idx->nChainWork = parent->nChainWork + GetBlockProof(*n.idx);
        n.idx->BuildSkip();
        return n;
    }
};

/** Block and transaction builders (the state fixture's shapes, plan §3.4-3.5) over a live index. */
struct Builder
{
    yellowback::Params P;
    YellowbackIndex& index;
    CKey ownerKey, userKey;
    int fakeCounter;

    Builder(const yellowback::Params& p, YellowbackIndex& i) : P(p), index(i), fakeCounter(0)
    {
        ownerKey.MakeNewKey(true);
        userKey.MakeNewKey(true);
    }

    COutPoint FakeInput()
    {
        std::vector<unsigned char> v(32, 0x77);
        v[0] = fakeCounter & 0xff;
        v[1] = (fakeCounter >> 8) & 0xff;
        fakeCounter++;
        return COutPoint(uint256(v), 0);
    }

    static CoinbaseTag Quote(MicroUsd price, int key, bool signal = true, uint16_t mask = 7)
    {
        CoinbaseTag t;
        t.flags = signal ? 1 : 0;
        t.priceMicroUsd = price;
        t.sourceMask = mask;
        t.payoutKey = KeyOf(key);
        return t;
    }

    static CMutableTransaction Coinbase(int height, const std::optional<CoinbaseTag>& tag, uint32_t nonce = 0)
    {
        CMutableTransaction cb;
        CScript sig = CScript() << height;
        if (tag.has_value()) sig += TagPush(tag.value());
        if (nonce) sig << nonce;
        cb.vin.push_back(CTxIn(COutPoint(), sig));
        cb.vout.push_back(CTxOut(625000000, GetScriptForDestination(CKeyID(KeyOf(0)))));
        return cb;
    }

    static CBlock Block(int height, const std::optional<CoinbaseTag>& tag, std::vector<CMutableTransaction> txs = {}, uint32_t nonce = 0)
    {
        CBlock block;
        block.nTime = (uint32_t)GetTime();
        block.vtx.push_back(CTransaction(Coinbase(height, tag, nonce)));
        for (auto& m : txs) block.vtx.push_back(CTransaction(m));
        return block;
    }

    Snapshot Snap(int h) const
    {
        LOCK(index.cs_yellowback);
        std::optional<Snapshot> s = State(index.View()).GetSnapshot((uint32_t)h);
        BOOST_REQUIRE(s.has_value());
        return s.value();
    }

    std::optional<VaultRecord> Vault(const uint256& txid) const
    {
        LOCK(index.cs_yellowback);
        return State(index.View()).GetVault(COutPoint(txid, 0));
    }

    CAmount Required(Cents cents, int termClass, int refHeight) const
    {
        Snapshot s = Snap(refHeight);
        auto r = RequiredCollateralRounded(cents, MinRatioBps(P.baseRatioBps[termClass], s.sigmaMultBps), s.PMint().value());
        BOOST_REQUIRE(r.has_value());
        return r.value();
    }

    /** The §3.5 MINT of `cents`, class A, lockHeight = refHeight + lockBlocks, fee to the payee of the tag at refHeight. */
    CMutableTransaction MintTx(Cents cents, int lockBlocks, int refHeight)
    {
        CPubKey owner = ownerKey.GetPubKey();
        const uint32_t lock = (uint32_t)(refHeight + lockBlocks);
        CScript vs = VaultScript(lock, owner, (uint32_t)(lock + P.grace));
        CAmount collateral = Required(cents, 0, refHeight);
        CMutableTransaction m;
        m.vin.push_back(CTxIn(FakeInput()));
        m.vout.push_back(CTxOut(collateral, P2SHScript(vs)));
        m.vout.push_back(CTxOut(TOKEN_VALUE, GetScriptForDestination(owner.GetID())));
        Payload p = Payload::Mint(0, (uint32_t)cents, lock, (uint32_t)refHeight, owner, 3);
        m.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(p))));
        m.vout.push_back(CTxOut(FeeZat(collateral, P.feeMin, P.feeBps), GetScriptForDestination(CKeyID(KeyOf(refHeight % 3)))));
        return m;
    }

    /**
     * A vault spend: the owner path with no burn and no payload (RED-1 fails: vault-spend-malformed)
     * when `wellFormed` is false; with `wellFormed` a REDEEM payload, the fee and the burn of `yed`
     * so that RED-1..4 pass (the burn covers the debt when `yed` carries it).
     */
    CMutableTransaction SpendTx(const uint256& vaultTxid, int refHeight, bool wellFormed, const std::vector<COutPoint>& yed = {}, uint32_t expiry = 0)
    {
        std::optional<VaultRecord> v = Vault(vaultTxid);
        BOOST_REQUIRE(v.has_value());
        CScript vs = VaultScript((uint32_t)v->lockHeight, v->OwnerKey(), (uint32_t)v->claimHeight);
        CMutableTransaction m;
        m.nLockTime = v->lockHeight;
        m.nExpiryHeight = expiry;
        m.vin.push_back(CTxIn(COutPoint(vaultTxid, 0), OwnerScriptSig(valtype(71, 0x30), vs), 0xFFFFFFFE));
        for (const COutPoint& o : yed) m.vin.push_back(CTxIn(o));
        m.vout.push_back(CTxOut(v->collateralZat - 1000, GetScriptForDestination(userKey.GetPubKey().GetID())));
        m.vout.push_back(CTxOut(FeeZat(v->collateralZat, P.feeMin, P.feeBps), GetScriptForDestination(CKeyID(KeyOf(refHeight % 3)))));
        if (wellFormed) {
            m.vout.push_back(CTxOut(0, PayloadScript(EncodePayload(Payload::Redeem((uint32_t)refHeight, 1, {})))));
        } else {
            m.vout.push_back(CTxOut(0, GetScriptForDestination(userKey.GetPubKey().GetID())));
        }
        return m;
    }
};

/** ConnectBlock's two calls for one synthetic block; returns CheckConnect's verdict (commits only when accepted). */
std::optional<std::string> Connect(YellowbackIndex& index, Chain::Node& n, bool fJustCheck = false)
{
    LOCK(cs_main);
    std::optional<std::string> bad = index.CheckConnect(n.block, n.idx.get(), fJustCheck);
    if (!bad.has_value() && !fJustCheck) index.CommitConnect(n.block, n.idx.get());
    return bad;
}

int TipHeightOf(YellowbackIndex& index)
{
    LOCK(index.cs_yellowback);
    return index.TipHeight();
}

uint256 HashOf(YellowbackIndex& index)
{
    LOCK(index.cs_yellowback);
    return index.GetStateHash();
}

/**
 * An index over a synthetic chain brought to ACTIVE with one ACTIVE vault
 * (quote+signal tags from three rotating pools to startHeight + 2 *
 * signalWindow + 2, then a mint), the shape every rejection case starts from.
 */
struct Live
{
    yellowback::Params P;
    std::unique_ptr<YellowbackIndex> index;
    std::unique_ptr<Builder> b;
    Chain chain;
    CBlockIndex* tip;      //!< the last accepted block (a rejected block never advances it)
    uint256 vault;
    int vaultRef;

    explicit Live(const fs::path& dir, int until = 0) : P(RegtestParams(1, 0, 0, until)), tip(chainActive.Genesis())
    {
        index.reset(new YellowbackIndex(P, dir, 1 << 20, true));
        BOOST_REQUIRE(index->SyncToChain());
        b.reset(new Builder(P, *index));
        MinerConfig cfg;
        cfg.enforce = true;
        cfg.signal = true;
        cfg.payoutKey = CKeyID(KeyOf(0));
        index->SetMinerConfig(cfg);
    }

    int Tip() { return tip->nHeight; }

    /** Mine one quote+signal block from pool (height % 3); asserts acceptance. */
    Chain::Node& MineQuote(MicroUsd price = 50000, bool signal = true)
    {
        const int h = Tip() + 1;
        Chain::Node& n = chain.Add(Builder::Block(h, Builder::Quote(price, h % 3, signal)), tip);
        BOOST_REQUIRE(!Connect(*index, n).has_value());
        BOOST_REQUIRE_EQUAL(TipHeightOf(*index), h);
        tip = n.idx.get();
        return n;
    }

    void Activate()
    {
        while (Tip() < P.startHeight + 2 * P.signalWindow + 2) MineQuote();
        BOOST_REQUIRE(b->Snap(Tip()).activation.IsActive());
        BOOST_REQUIRE_EQUAL(b->Snap(Tip()).haltMask, 0u);
    }

    /** Mint an ACTIVE vault at tip + 1 (refHeight = tip - 1). */
    void MintActive()
    {
        vaultRef = Tip() - 1;
        CMutableTransaction m = b->MintTx(10000, 48, vaultRef);
        const int h = Tip() + 1;
        Chain::Node& n = chain.Add(Builder::Block(h, Builder::Quote(50000, h % 3), { m }), tip);
        BOOST_REQUIRE(!Connect(*index, n).has_value());
        tip = n.idx.get();
        vault = CTransaction(m).GetHash();
        std::optional<VaultRecord> v = b->Vault(vault);
        BOOST_REQUIRE(v.has_value());
        BOOST_REQUIRE_MESSAGE(v->Status() == VaultStatus::ACTIVE, v->voidReason);
    }

    /** A block at tip + 1 carrying a malformed owner-path spend of the vault (BLK-1 fails: RED-1). */
    Chain::Node& BadBlock(uint32_t nonce = 0)
    {
        const int h = Tip() + 1;
        CMutableTransaction s = b->SpendTx(vault, h - 2, false);
        return chain.Add(Builder::Block(h, std::nullopt, { s }, nonce), tip);
    }
};

CBlockHeader HeaderOn(const uint256& prev, uint32_t nonce, uint32_t nBits)
{
    CBlockHeader h;
    h.nVersion = 4;
    h.hashPrevBlock = prev;
    h.nTime = (uint32_t)GetTime();
    h.nBits = nBits;
    std::vector<unsigned char> n(32, 0);
    n[0] = nonce & 0xff;
    n[1] = (nonce >> 8) & 0xff;
    h.nNonce = uint256(n);
    return h;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_index_tests, IndexSetup)

// Rule: BLK-3
BOOST_AUTO_TEST_CASE(exception_boundary)
{
    yellowback::Params params = RegtestParams(1, 0, 0, 0);
    YellowbackIndex index(params, pathTemp / "yellowback-test", 1 << 20, true);
    BOOST_CHECK(index.SyncToChain());        // chain at genesis (< startHeight): empty and healthy
    BOOST_CHECK(index.IsHealthy());
    {
        LOCK(cs_main);
        LOCK(index.cs_yellowback);
        BOOST_CHECK(!index.GetTip().has_value());
        BOOST_CHECK(index.IsSynced());
    }

    Chain chain;
    Chain::Node& n1 = chain.Add(Builder::Block(1, std::nullopt));

    // Fault injection: an exception in CheckConnect is caught, marks the index unhealthy, does not propagate.
    index.testBeforeApply = [] { throw std::runtime_error("injected fault"); };
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index.CheckConnect(n1.block, n1.idx.get(), false));
    }
    BOOST_CHECK(!index.IsHealthy());
    BOOST_CHECK(index.UnhealthyReason().find("injected fault") != std::string::npos);
    // Further deliveries are ignored while unhealthy.
    index.testBeforeApply = nullptr;
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index.CheckConnect(n1.block, n1.idx.get(), false));
        BOOST_CHECK(!index.CommitConnect(n1.block, n1.idx.get()));
        BOOST_CHECK(!index.MempoolCheckReason(CTransaction()).has_value());   // unhealthy: enforces nothing
    }
    BOOST_CHECK(!index.IsHealthy());

    // Fresh index: the start block applies (v2 has no genesis anchor; an empty block is a valid start).
    YellowbackIndex index2(params, pathTemp / "yellowback-test2", 1 << 20, true);
    BOOST_CHECK(index2.SyncToChain());
    BOOST_CHECK(!Connect(index2, n1).has_value());
    BOOST_CHECK(index2.IsHealthy());
    {
        LOCK(index2.cs_yellowback);
        BOOST_REQUIRE(index2.GetTip().has_value());
        BOOST_CHECK_EQUAL(index2.GetTip()->height, 1);
        BOOST_CHECK_EQUAL(index2.GetTip()->blockHash.ToString(), n1.hash.ToString());
    }

    // Stop(): later deliveries return at once and change nothing (D2).
    YellowbackIndex index3(params, pathTemp / "yellowback-test3", 1 << 20, true);
    BOOST_CHECK(index3.SyncToChain());
    index3.Stop();
    BOOST_CHECK(index3.IsStopped());
    index3.testBeforeApply = [] { throw std::runtime_error("must not run"); };
    {
        LOCK(cs_main);
        BOOST_CHECK_NO_THROW(index3.CheckConnect(n1.block, n1.idx.get(), false));
    }
    BOOST_CHECK(index3.IsHealthy());

    // A disconnect below startHeight is ignored.
    YellowbackIndex index4(params, pathTemp / "yellowback-test4", 1 << 20, true);
    BOOST_CHECK(index4.SyncToChain());
    {
        LOCK(cs_main);
        BOOST_CHECK(index4.UndoDisconnect(chainActive.Genesis()));
    }
    BOOST_CHECK(index4.IsHealthy());

    // An unconfigured network refuses to start (mainnet placeholders until the ceremony).
    YellowbackIndex index5(MainParams(), pathTemp / "yellowback-test5", 1 << 20, true);
    BOOST_CHECK(!index5.SyncToChain());
    BOOST_CHECK(!index5.IsHealthy());
}

// Rule: BLK-3
// Rule: BLK-2
BOOST_AUTO_TEST_CASE(check_storage_fault_accepts_and_sets_unhealthy)
{
    // -yellowbacktestfault=storage:check: the fault fires once in CheckConnect; a rule-breaking
    // block is accepted (nullopt), the index is unhealthy, nothing is written to Rejected.
    Live live(pathTemp / "yb-check-fault");
    live.Activate();
    live.MintActive();
    BOOST_CHECK(!live.index->SetTestFault("storage:check").has_value());
    Chain::Node& bad = live.BadBlock();
    const int hashBefore = TipHeightOf(*live.index);
    BOOST_CHECK(!Connect(*live.index, bad).has_value());
    BOOST_CHECK(!live.index->IsHealthy());
    BOOST_CHECK(live.index->UnhealthyReason().find("CheckConnect") != std::string::npos);
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), hashBefore);
    BOOST_CHECK(!live.index->IsEnforcing());
    // A bad spec is refused at once (init refuses to start).
    BOOST_CHECK(live.index->SetTestFault("storage:bogus").has_value());
    BOOST_CHECK(live.index->SetTestFault("storage:check:x:y").has_value());
    BOOST_CHECK(live.index->SetTestFault("nonsense").has_value());
}

// Rule: BLK-3
BOOST_AUTO_TEST_CASE(commit_storage_fault_sets_unhealthy)
{
    // storage:commit:<height>: fires only at that height; CommitConnect returns false, the index is
    // unhealthy, and the block still connects (nothing here can stop ConnectBlock).
    Live live(pathTemp / "yb-commit-fault");
    live.Activate();
    const int target = live.Tip() + 2;
    BOOST_CHECK(!live.index->SetTestFault(strprintf("storage:commit:%d", target)).has_value());
    live.MineQuote();                                  // tip + 1: not the fault height
    BOOST_CHECK(live.index->IsHealthy());
    const int h = live.Tip() + 1;
    Chain::Node& n = live.chain.Add(Builder::Block(h, Builder::Quote(50000, h % 3)));
    {
        LOCK(cs_main);
        BOOST_CHECK(!live.index->CheckConnect(n.block, n.idx.get(), false).has_value());
        BOOST_CHECK(!live.index->CommitConnect(n.block, n.idx.get()));
    }
    BOOST_CHECK(!live.index->IsHealthy());
    BOOST_CHECK(live.index->UnhealthyReason().find("CommitConnect") != std::string::npos);
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), h - 1);   // the batch was discarded
    BOOST_CHECK(!live.index->GetTestFault().armed);        // consumed
}

// Rule: BLK-3
// Rule: UNDO
BOOST_AUTO_TEST_CASE(undo_tip_mismatch_refuses)
{
    Live live(pathTemp / "yb-undo");
    live.Activate();
    Chain::Node& below = live.chain.nodes[live.chain.nodes.size() - 2];
    const uint256 before = HashOf(*live.index);
    {
        LOCK(cs_main);
        // Undo of a block that is not the index tip: refused, unhealthy.
        BOOST_CHECK(!live.index->UndoDisconnect(below.idx.get()));
    }
    BOOST_CHECK(!live.index->IsHealthy());
    BOOST_CHECK(live.index->UnhealthyReason().find("tip-mismatch") != std::string::npos);
    BOOST_CHECK(HashOf(*live.index) == before);

    // A healthy index: undo of the tip succeeds and restores the previous hash; storage:undo fails open.
    Live live2(pathTemp / "yb-undo2");
    live2.Activate();
    const uint256 h1 = HashOf(*live2.index);
    Chain::Node& n = live2.MineQuote();
    {
        LOCK(cs_main);
        BOOST_CHECK(live2.index->UndoDisconnect(n.idx.get()));
    }
    BOOST_CHECK(HashOf(*live2.index) == h1);
    BOOST_CHECK(live2.index->IsHealthy());
    // A sibling of the undone block on the same parent connects (the index tip is the parent again).
    Chain::Node& n2 = live2.chain.Add(Builder::Block(n.idx->nHeight, Builder::Quote(50000, 1), {}, 7), n.idx->pprev);
    BOOST_CHECK(!Connect(*live2.index, n2).has_value());
    BOOST_CHECK(!live2.index->SetTestFault("storage:undo").has_value());
    {
        LOCK(cs_main);
        BOOST_CHECK(!live2.index->UndoDisconnect(n2.idx.get()));
    }
    BOOST_CHECK(!live2.index->IsHealthy());
    BOOST_CHECK(live2.index->UnhealthyReason().find("UndoDisconnect") != std::string::npos);
}

// Rule: BLK-1
// Rule: BLK-2
BOOST_AUTO_TEST_CASE(check_null_hash)
{
    // K5: under TestBlockValidity pindex is indexDummy with a null phashBlock. CheckConnect keys on
    // block.GetHash() and pprev only; fJustCheck never writes Rejected and never commits (§8.4 item 7).
    Live live(pathTemp / "yb-nullhash");
    live.Activate();
    live.MintActive();
    const uint256 before = HashOf(*live.index);
    const int tipBefore = TipHeightOf(*live.index);

    CBlockIndex dummy;
    dummy.pprev = live.chain.Tip();
    dummy.nHeight = dummy.pprev->nHeight + 1;
    dummy.phashBlock = nullptr;
    CBlock good = Builder::Block(dummy.nHeight, Builder::Quote(50000, 0));
    good.hashMerkleRoot = good.BuildMerkleTree();
    {
        LOCK(cs_main);
        BOOST_CHECK(!live.index->CheckConnect(good, &dummy, true).has_value());
    }
    CMutableTransaction s = live.b->SpendTx(live.vault, dummy.nHeight - 2, false);
    CBlock badBlock = Builder::Block(dummy.nHeight, std::nullopt, { s });
    badBlock.hashMerkleRoot = badBlock.BuildMerkleTree();
    {
        LOCK(cs_main);
        std::optional<std::string> bad = live.index->CheckConnect(badBlock, &dummy, true);
        BOOST_REQUIRE(bad.has_value());
        BOOST_CHECK(bad->find("vault-spend-malformed") != std::string::npos);
    }
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);          // fJustCheck: never recorded
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), tipBefore);     // never committed
    BOOST_CHECK(HashOf(*live.index) == before);
    BOOST_CHECK(live.index->IsHealthy());
}

// Rule: BLK-1
// Rule: SNAP
BOOST_FIXTURE_TEST_CASE(check_reverify, TestChain100Setup)
{
    // K6: a block already in chainActive (VerifyDB level 4, verifychain) is a silent no-op for both
    // hooks; the tip and the hash are unchanged and the index stays healthy.
    yellowback::Params params = RegtestParams(1, 0, 0, 0);
    YellowbackIndex index(params, pathTemp / "yb-reverify", 1 << 20, true);
    BOOST_REQUIRE(index.SyncToChain());
    BOOST_REQUIRE_EQUAL(TipHeightOf(index), 100);
    const uint256 before = HashOf(index);
    for (int h : { 1, 50, 100 }) {
        LOCK(cs_main);
        CBlockIndex* pindex = chainActive[h];
        CBlock block;
        BOOST_REQUIRE(ReadBlockFromDisk(block, pindex, ::Params().GetConsensus()));
        BOOST_CHECK(!index.CheckConnect(block, pindex, false).has_value());
        BOOST_CHECK(index.CommitConnect(block, pindex));
    }
    BOOST_CHECK_EQUAL(TipHeightOf(index), 100);
    BOOST_CHECK(HashOf(index) == before);
    BOOST_CHECK(index.IsHealthy());
    // The real disconnect of the tip undoes, the re-connect re-applies to the same hash (apply/undo identity).
    {
        LOCK(cs_main);
        CBlockIndex* tip = chainActive[100];
        CBlock block;
        BOOST_REQUIRE(ReadBlockFromDisk(block, tip, ::Params().GetConsensus()));
        BOOST_CHECK(index.UndoDisconnect(tip));
        BOOST_CHECK_EQUAL(index.TipHeight(), 99);
        // A block whose parent is the tip but which is not in chainActive[100]'s slot: a fake sibling entry.
        CBlockIndex sibling = *tip;
        sibling.phashBlock = tip->phashBlock;
        BOOST_CHECK(!index.CheckConnect(block, &sibling, false).has_value());
        BOOST_CHECK(index.CommitConnect(block, &sibling));
    }
    BOOST_CHECK_EQUAL(TipHeightOf(index), 100);
    BOOST_CHECK(HashOf(index) == before);
}

// Rule: BLK-2
BOOST_AUTO_TEST_CASE(check_no_enforce)
{
    // -yellowbackenforce=0: the same evaluation is logged and the block is accepted; nothing in Rejected.
    Live live(pathTemp / "yb-noenforce");
    live.Activate();
    live.MintActive();
    MinerConfig cfg = live.index->GetMinerConfig();
    cfg.enforce = false;
    live.index->SetMinerConfig(cfg);
    BOOST_CHECK(!live.index->IsEnforcing());
    Chain::Node& bad = live.BadBlock();
    BOOST_CHECK(!Connect(*live.index, bad).has_value());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), bad.idx->nHeight);   // committed: the vault is now CLOSED by a failing spend
    BOOST_CHECK(live.index->IsHealthy());
    BOOST_CHECK(!live.index->GetMinerStatus(0).signal);               // MINER-1: no signal bit without enforce
}

// Rule: BLK-3
// Rule: BLK-2
BOOST_AUTO_TEST_CASE(check_unhealthy)
{
    Live live(pathTemp / "yb-unhealthy");
    live.Activate();
    live.MintActive();
    live.index->SetUnhealthy("test");
    Chain::Node& bad = live.BadBlock();
    BOOST_CHECK(!Connect(*live.index, bad).has_value());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK(policy::BuildTagScript(*live.index, 0).empty());     // MINER-3: no tag while unhealthy
}

// Rule: BLK-3
BOOST_AUTO_TEST_CASE(check_tip_mismatch)
{
    // A block whose parent is not the index tip (and that is not a re-verification) marks the index
    // unhealthy and is accepted; the storage is untouched.
    Live live(pathTemp / "yb-tipmismatch");
    live.Activate();
    Chain::Node& top = live.chain.nodes.back();
    const uint256 before = HashOf(*live.index);
    Chain::Node& stray = live.chain.Add(Builder::Block(top.idx->nHeight, Builder::Quote(50000, 2), {}, 99), top.idx->pprev);
    BOOST_CHECK(!Connect(*live.index, stray).has_value());
    BOOST_CHECK(!live.index->IsHealthy());
    BOOST_CHECK(live.index->UnhealthyReason().find("tip-mismatch") != std::string::npos);
    BOOST_CHECK(HashOf(*live.index) == before);
    // An empty index refuses a first block that is not at startHeight the same way.
    yellowback::Params p5 = RegtestParams(5, 0, 0, 0);
    YellowbackIndex index5(p5, pathTemp / "yb-tipmismatch5", 1 << 20, true);
    BOOST_REQUIRE(index5.SyncToChain());
    {
        LOCK(cs_main);
        BOOST_CHECK(!index5.CheckConnect(top.block, top.idx.get(), false).has_value());
    }
    BOOST_CHECK(!index5.IsHealthy());
}

// Rule: ACT-5
// Rule: ACT-6
// Rule: BLK-2
BOOST_AUTO_TEST_CASE(check_suspended)
{
    // Below ENFORCEMENT_FLOOR signals the ENFORCEMENT bit is set at H - 1 and BLK-2 rejects nothing,
    // whatever -yellowbackenforce says; the verdict is still computed and logged.
    Live live(pathTemp / "yb-suspended");
    live.Activate();
    live.MintActive();
    while (live.b->Snap(live.Tip()).signalCount >= (uint32_t)live.P.enforcementFloor) live.MineQuote(50000, false);
    BOOST_CHECK(live.b->Snap(live.Tip()).haltMask & HALT_ENFORCEMENT);
    BOOST_CHECK(live.index->IsEnforcing());                            // the node-side flags are all on ...
    Chain::Node& bad = live.BadBlock();
    BOOST_CHECK(!Connect(*live.index, bad).has_value());               // ... but ACT-5 is off at H
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK(live.index->IsHealthy());
}

// Rule: BLK-2
BOOST_AUTO_TEST_CASE(check_ibd_suppresses_reject)
{
    // N2: with fReindex set a healthy index evaluates, commits and returns nullopt; Rejected stays
    // empty and enforcement stays on. The same for fImporting.
    Live live(pathTemp / "yb-ibd");
    live.Activate();
    live.MintActive();
    fReindex = true;
    Chain::Node& bad = live.BadBlock();
    BOOST_CHECK(!Connect(*live.index, bad).has_value());
    fReindex = false;
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), bad.idx->nHeight);
    BOOST_CHECK(live.index->IsEnforcing());
    BOOST_CHECK_EQUAL(live.index->SuppressedCount(), 0);
    std::optional<VaultRecord> v = live.b->Vault(live.vault);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK(v->Status() == VaultStatus::CLOSED);                   // the failing spend closed it (IN-2)
    // Undo it again and check that fImporting suppresses the same way.
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->UndoDisconnect(bad.idx.get()));
    }
    fImporting = true;
    Chain::Node& bad2 = live.BadBlock(1);
    BOOST_CHECK(!Connect(*live.index, bad2).has_value());
    fImporting = false;
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    // And without either flag the same block is rejected.
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->UndoDisconnect(bad2.idx.get()));
    }
    Chain::Node& bad3 = live.BadBlock(2);
    std::optional<std::string> reason = Connect(*live.index, bad3);
    BOOST_REQUIRE(reason.has_value());
    BOOST_CHECK(reason->find("vault-spend-malformed") != std::string::npos);
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 1);
    BOOST_CHECK(live.index->GetRejected(bad3.hash).has_value());
    BOOST_CHECK_EQUAL(live.index->GetRejected(bad3.hash)->height, bad3.idx->nHeight);
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), bad3.idx->nHeight - 1);   // never committed
}

// Rule: BLK-2
BOOST_AUTO_TEST_CASE(catchup_suppresses_reject)
{
    // L11: with pindexBestHeader a descendant of the block carrying VALVE_BLOCKS of work above the
    // tip, CheckConnect accepts, counts suppressedBlocks, writes nothing to Rejected and leaves
    // enforcing true; at five blocks of work it rejects.
    Live live(pathTemp / "yb-catchup");
    live.Activate();
    live.MintActive();
    Chain::Node& bad = live.BadBlock();
    // The node's tip is the regtest genesis (chainActive); the fake bad block sits at height H on
    // the synthetic chain. The network's headers descend from it.
    CBlockIndex* tip = chainActive.Tip();
    const arith_uint256 proof = GetBlockProof(*tip);
    std::deque<CBlockIndex> heads;
    CBlockIndex* prev = bad.idx.get();
    bad.idx->nChainWork = tip->nChainWork + proof;
    for (int k = 1; k <= 5; k++) {
        heads.emplace_back();
        CBlockIndex& d = heads.back();
        d.pprev = prev;
        d.nHeight = prev->nHeight + 1;
        d.nBits = tip->nBits;
        d.nChainWork = tip->nChainWork + proof * (k + 1);
        d.BuildSkip();
        prev = &d;
    }
    CBlockIndex* savedBest = pindexBestHeader;
    pindexBestHeader = &heads[4];                                      // tip + 6 blocks of work
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->NetworkAlreadyBuiltOn(bad.idx.get()));
    }
    BOOST_CHECK(!Connect(*live.index, bad).has_value());
    BOOST_CHECK_EQUAL(live.index->SuppressedCount(), 1);
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK(live.index->IsEnforcing());
    BOOST_CHECK(!live.index->ValveTripped());
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), bad.idx->nHeight);   // accepted and committed
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->UndoDisconnect(bad.idx.get()));
    }
    pindexBestHeader = &heads[3];                                      // tip + 5 blocks of work: rejected as usual
    {
        LOCK(cs_main);
        BOOST_CHECK(!live.index->NetworkAlreadyBuiltOn(bad.idx.get()));
    }
    std::optional<std::string> reason = Connect(*live.index, bad);
    BOOST_REQUIRE(reason.has_value());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 1);
    BOOST_CHECK_EQUAL(live.index->SuppressedCount(), 1);
    pindexBestHeader = savedBest;
}

// Rule: ACT-7
// Rule: BLK-2
BOOST_AUTO_TEST_CASE(valve_trips_at_six_blocks)
{
    // Six noted headers of one block of work each above the tip trip the valve; five do not. The
    // note map is cleared, a Rejected hash absent from mapBlockIndex is skipped, the P1 text is in
    // GetMiscWarning(), and afterwards nothing is rejected (check_tripped_valve_never_rejects).
    Live live(pathTemp / "yb-valve");
    live.Activate();
    live.MintActive();
    Chain::Node& bad = live.BadBlock();
    Chain::Node& bad2 = live.BadBlock(1);                          // a second rejected block, never indexed
    BOOST_REQUIRE(Connect(*live.index, bad).has_value());
    BOOST_REQUIRE(Connect(*live.index, bad2).has_value());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 2);
    CBlockIndex* tip = chainActive.Tip();
    bad.idx->nStatus |= BLOCK_FAILED_VALID;
    bad.idx->nChainWork = tip->nChainWork;                         // a sibling of the tip: no work above it
    MapGuard guard;
    guard.Insert(bad.idx.get());
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->IsRejectedAncestor(bad.idx.get()));
        BOOST_CHECK(!live.index->IsRejectedAncestor(tip));
    }
    // A header whose parent is unknown or not rejected is not the clause's business.
    {
        LOCK(cs_main);
        BOOST_CHECK(!live.index->NoteHeaderOnRejectedChain(HeaderOn(uint256S("ab"), 1, tip->nBits)));
        BOOST_CHECK(!live.index->NoteHeaderOnRejectedChain(HeaderOn(tip->GetBlockHash(), 1, tip->nBits)));
    }
    uint256 prev = bad.hash;
    for (int k = 1; k <= 5; k++) {
        CBlockHeader h = HeaderOn(prev, k, tip->nBits);
        LOCK(cs_main);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(h));
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(h));      // answered again, noted once
        BOOST_CHECK(!live.index->ValveTripped());
        BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), (size_t)k);
        prev = h.GetHash();
    }
    BOOST_CHECK(live.index->IsEnforcing());
    {
        CBlockHeader h6 = HeaderOn(prev, 6, tip->nBits);
        LOCK(cs_main);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(h6));    // the tripping header is answered DoS 0 too
        BOOST_CHECK(live.index->ValveTripped());
        BOOST_CHECK(!live.index->NoteHeaderOnRejectedChain(HeaderOn(h6.GetHash(), 7, tip->nBits)));   // P3: inert from now on
    }
    BOOST_CHECK(!live.index->IsEnforcing());
    BOOST_CHECK(live.index->IsHealthy());                          // unhealthyReason untouched
    BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), 0u);
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);             // cleared, bad2 skipped (not in mapBlockIndex)
    BOOST_CHECK(!(bad.idx->nStatus & BLOCK_FAILED_MASK));          // ReconsiderBlock ran
    const std::string warning = GetMiscWarning().first;
    BOOST_CHECK_MESSAGE(warning.find(strprintf("Yellowback: work valve tripped at height %d (rejected root %s); enforcement off until restart", tip->nHeight, bad.hash.ToString())) != std::string::npos, warning);
    BOOST_CHECK(!live.index->GetMinerStatus(0).signal);            // MINER-1: the signal bit is dropped
    // check_tripped_valve_never_rejects: the same rule-breaking block is accepted now.
    Chain::Node& bad3 = live.BadBlock(2);
    BOOST_CHECK(!Connect(*live.index, bad3).has_value());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
    BOOST_CHECK_EQUAL(TipHeightOf(*live.index), bad3.idx->nHeight);
}

// Rule: ACT-7
BOOST_AUTO_TEST_CASE(check_tripped_valve_never_rejects)
{
    // Named for §8.4 item 3; the tripped-valve acceptance is asserted at the end of valve_trips_at_six_blocks
    // and here again from a valve tripped by the smallest chain: a root that is the tip's sibling.
    Live live(pathTemp / "yb-tripped");
    live.Activate();
    live.MintActive();
    Chain::Node& bad = live.BadBlock();
    BOOST_REQUIRE(Connect(*live.index, bad).has_value());
    CBlockIndex* tip = chainActive.Tip();
    bad.idx->nStatus |= BLOCK_FAILED_VALID;
    bad.idx->nChainWork = tip->nChainWork;
    MapGuard guard;
    guard.Insert(bad.idx.get());
    uint256 prev = bad.hash;
    for (int k = 1; k <= 6; k++) {
        CBlockHeader h = HeaderOn(prev, k, tip->nBits);
        LOCK(cs_main);
        live.index->NoteHeaderOnRejectedChain(h);
        prev = h.GetHash();
    }
    BOOST_REQUIRE(live.index->ValveTripped());
    for (int i = 0; i < 3; i++) {
        Chain::Node& b = live.BadBlock(10 + i);
        BOOST_CHECK(!Connect(*live.index, b).has_value());
        BOOST_CHECK_EQUAL(TipHeightOf(*live.index), b.idx->nHeight);
        LOCK(cs_main);
        BOOST_CHECK(live.index->UndoDisconnect(b.idx.get()));
    }
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
}

// Rule: ACT-7
// Rule: BLK-2
BOOST_AUTO_TEST_CASE(valve_trips_through_failed_child)
{
    // L11: the rejected root has two indexed BLOCK_FAILED_CHILD descendants; a header whose parent is
    // the second is noted with that parent's real nChainWork, so the odometer starts at three blocks
    // and trips three headers later.
    Live live(pathTemp / "yb-valve-child");
    live.Activate();
    live.MintActive();
    Chain::Node& bad = live.BadBlock();
    BOOST_REQUIRE(Connect(*live.index, bad).has_value());
    CBlockIndex* tip = chainActive.Tip();
    const arith_uint256 proof = GetBlockProof(*tip);
    bad.idx->nStatus |= BLOCK_FAILED_VALID;
    bad.idx->nChainWork = tip->nChainWork;
    Chain::Node& c1 = live.chain.Add(Builder::Block(bad.idx->nHeight + 1, std::nullopt, {}, 1), bad.idx.get());
    Chain::Node& c2 = live.chain.Add(Builder::Block(bad.idx->nHeight + 2, std::nullopt, {}, 2), c1.idx.get());
    c1.idx->nStatus |= BLOCK_FAILED_CHILD;
    c2.idx->nStatus |= BLOCK_FAILED_CHILD;
    c1.idx->nChainWork = tip->nChainWork + proof;
    c2.idx->nChainWork = tip->nChainWork + proof * 2;
    MapGuard guard;
    guard.Insert(bad.idx.get());
    guard.Insert(c1.idx.get());
    guard.Insert(c2.idx.get());
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->IsRejectedAncestor(c2.idx.get()));  // walks through the _CHILD marks
    }
    uint256 prev = c2.hash;
    for (int k = 3; k <= 5; k++) {                                   // noted at 3p, 4p, 5p: no trip
        CBlockHeader h = HeaderOn(prev, k, tip->nBits);
        LOCK(cs_main);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(h));
        BOOST_CHECK(!live.index->ValveTripped());
        prev = h.GetHash();
    }
    BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), 3u);
    {
        CBlockHeader h = HeaderOn(prev, 6, tip->nBits);              // 6p: trips
        LOCK(cs_main);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(h));
    }
    BOOST_CHECK(live.index->ValveTripped());
    BOOST_CHECK(!(c2.idx->nStatus & BLOCK_FAILED_MASK));            // ReconsiderBlock cleared the descendants too
}

// Rule: ACT-7
BOOST_AUTO_TEST_CASE(valve_note_map_bounded)
{
    // P2: the 65th header on one root is answered (true) but not noted; the sum stops growing. The
    // valve itself is disabled (-yellowbacktestfault=novalve) so the chain can grow past six.
    Live live(pathTemp / "yb-valve-cap");
    live.Activate();
    live.MintActive();
    Chain::Node& bad = live.BadBlock();
    BOOST_REQUIRE(Connect(*live.index, bad).has_value());
    BOOST_CHECK(!live.index->SetTestFault("novalve").has_value());
    CBlockIndex* tip = chainActive.Tip();
    bad.idx->nStatus |= BLOCK_FAILED_VALID;
    bad.idx->nChainWork = tip->nChainWork;
    MapGuard guard;
    guard.Insert(bad.idx.get());
    uint256 prev = bad.hash;
    for (int k = 1; k <= VALVE_NOTE_CAP + 1; k++) {
        CBlockHeader h = HeaderOn(prev, k, tip->nBits);
        LOCK(cs_main);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(h));
        BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), (size_t)std::min(k, VALVE_NOTE_CAP));
        prev = h.GetHash();
    }
    BOOST_CHECK(!live.index->ValveTripped());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 1);
    BOOST_CHECK(live.index->IsEnforcing());
}

// Rule: ACT-7
BOOST_AUTO_TEST_CASE(valve_ignores_lowdiff_headers)
{
    // P2: a header whose target exceeds 132/100 of its parent's is answered but not noted; one within
    // the loosening is noted.
    Live live(pathTemp / "yb-valve-lowdiff");
    live.Activate();
    live.MintActive();
    Chain::Node& bad = live.BadBlock();
    BOOST_REQUIRE(Connect(*live.index, bad).has_value());
    CBlockIndex* tip = chainActive.Tip();
    bad.idx->nStatus |= BLOCK_FAILED_VALID;
    bad.idx->nChainWork = tip->nChainWork;
    MapGuard guard;
    guard.Insert(bad.idx.get());
    arith_uint256 parentTarget;
    parentTarget.SetCompact(tip->nBits);
    const uint32_t tooEasy = arith_uint256(parentTarget / 100 * 140).GetCompact();
    const uint32_t withinBound = arith_uint256(parentTarget / 100 * 125).GetCompact();
    {
        LOCK(cs_main);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(HeaderOn(bad.hash, 1, tooEasy)));
        BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), 0u);
        CBlockHeader ok = HeaderOn(bad.hash, 2, withinBound);
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(ok));
        BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), 1u);
        // A header on the unnoted easy one: its parent is neither a note nor indexed, so it is not answered here.
        BOOST_CHECK(!live.index->NoteHeaderOnRejectedChain(HeaderOn(HeaderOn(bad.hash, 1, tooEasy).GetHash(), 3, tip->nBits)));
        // Chained on the noted one, the loosening is measured against the note's own target.
        BOOST_CHECK(live.index->NoteHeaderOnRejectedChain(HeaderOn(ok.GetHash(), 4, arith_uint256(parentTarget / 100 * 125 / 100 * 140).GetCompact())));
        BOOST_CHECK_EQUAL(live.index->ValveNoteCount(), 1u);
    }
    BOOST_CHECK(!live.index->ValveTripped());
}

// Rule: ACT-5
// Rule: MINER-1
BOOST_AUTO_TEST_CASE(sunset_flag_stops_rejection_and_signal)
{
    // L8: past ENFORCE_UNTIL_HEIGHT the node tags and evaluates, rejects nothing, drops the signal
    // bit and reports sunset; H == ENFORCE_UNTIL_HEIGHT is still enforced.
    const int until = 1 + 2 * 64 + 2 + 3;
    Live live(pathTemp / "yb-sunset", until);
    live.Activate();
    live.MintActive();
    live.index->SetQuote(2000000, 1, 0);
    BOOST_CHECK(!live.index->IsSunset());
    BOOST_CHECK(live.index->GetMinerStatus(0).signal);
    while (live.Tip() < until) live.MineQuote();
    BOOST_CHECK(live.index->IsSunset());
    BOOST_CHECK(!live.index->IsEnforcing());
    BOOST_CHECK(!live.index->GetMinerStatus(0).signal);
    BOOST_CHECK_EQUAL(live.index->GetMinerStatus(0).kind, "quote");   // still tags
    Chain::Node& bad = live.BadBlock();
    BOOST_CHECK(!Connect(*live.index, bad).has_value());
    BOOST_CHECK_EQUAL(live.index->RejectedCount(), 0);
}

// Rule: MP-1
BOOST_AUTO_TEST_CASE(mempoolcheck_bench)
{
    // The MempoolCheck half of N6: 10,000 plain transactions in under a second (O(inputs) lookups,
    // no SNAP); a well-formed vault spend is admitted, a malformed one refused with its verdict, one
    // without the expiry bound refused with mempool-expiry, a mint never refused; the ConnectTip
    // sweep drops the failing spend and keeps the rest.
    Live live(pathTemp / "yb-mp1");
    live.Activate();
    live.MintActive();
    std::vector<CTransaction> plain;
    for (int i = 0; i < 10000; i++) {
        CMutableTransaction m;
        m.vin.push_back(CTxIn(live.b->FakeInput()));
        m.vin.push_back(CTxIn(live.b->FakeInput()));
        m.vout.push_back(CTxOut(1000 + i, GetScriptForDestination(live.b->userKey.GetPubKey().GetID())));
        plain.push_back(CTransaction(m));
    }
    const int64_t t0 = GetTimeMicros();
    for (const CTransaction& tx : plain) BOOST_CHECK(live.index->MempoolCheck(tx));
    const int64_t elapsed = GetTimeMicros() - t0;
    BOOST_TEST_MESSAGE(strprintf("MempoolCheck over 10000 plain transactions: %d us", (int)elapsed));
    BOOST_CHECK(elapsed < 1000000);

    const int ref = live.Tip() - 1;
    CMutableTransaction good = live.b->SpendTx(live.vault, ref, true, { COutPoint(live.vault, 1) }, (uint32_t)(ref + live.P.refWindow));
    CMutableTransaction malformed = live.b->SpendTx(live.vault, ref, false, {}, (uint32_t)(ref + live.P.refWindow));
    CMutableTransaction noExpiry = live.b->SpendTx(live.vault, ref, true, { COutPoint(live.vault, 1) }, 0);
    CMutableTransaction lateExpiry = live.b->SpendTx(live.vault, ref, true, { COutPoint(live.vault, 1) }, (uint32_t)(ref + live.P.refWindow + 1));
    CMutableTransaction mint = live.b->MintTx(10000, 48, ref);
    BOOST_CHECK(live.index->MempoolCheck(CTransaction(good)));
    BOOST_CHECK(!live.index->MempoolCheck(CTransaction(malformed)));
    BOOST_CHECK_EQUAL(live.index->MempoolCheckReason(CTransaction(malformed)).value_or(""), "vault-spend-malformed");
    BOOST_CHECK_EQUAL(live.index->MempoolCheckReason(CTransaction(noExpiry)).value_or(""), "mempool-expiry");
    BOOST_CHECK_EQUAL(live.index->MempoolCheckReason(CTransaction(lateExpiry)).value_or(""), "mempool-expiry");
    BOOST_CHECK(live.index->MempoolCheck(CTransaction(mint)));
    BOOST_CHECK(live.index->MempoolCheck(CTransaction(plain[0])));
    BOOST_CHECK(!live.index->IsAbandoned());

    // The sweep (N5): a pool holding the malformed spend, a plain transaction and the good spend.
    CTxMemPool pool(CFeeRate(0));
    TestMemPoolEntryHelper entry;
    CMutableTransaction plain0(plain[0]);
    pool.addUnchecked(malformed.GetHash(), entry.FromTx(malformed));
    pool.addUnchecked(plain0.GetHash(), entry.FromTx(plain0));
    pool.addUnchecked(good.GetHash(), entry.FromTx(good));
    BOOST_CHECK_EQUAL(pool.size(), 3u);
    {
        LOCK(cs_main);
        live.index->RemoveInvalidVaultSpends(pool);
    }
    BOOST_CHECK_EQUAL(pool.size(), 2u);
    BOOST_CHECK(!pool.exists(malformed.GetHash()));
    BOOST_CHECK(pool.exists(plain0.GetHash()));
    BOOST_CHECK(pool.exists(good.GetHash()));
}

// Rule: MINER-1
BOOST_AUTO_TEST_CASE(coinbase_flags_empty_without_flag)
{
    // Plan §8.4 item 2, the central claim of §1 (a): the two `+ COINBASE_FLAGS` appends in
    // miner.cpp (CreateCoinbaseTransaction :328 and IncrementExtraNonce :725) are the only
    // unguarded behaviour-bearing insertions in the mining path, and without -yellowback they
    // must be byte-level no-ops, so a node without the flag builds v4.5.0's coinbase exactly.
    //
    // COINBASE_FLAGS is assigned only at miner.cpp:370, from the module when g_yellowback is
    // non-null and from a default-constructed CScript otherwise; in this binary nothing ever
    // sets the module, which is the without-the-flag configuration.
    BOOST_CHECK(g_yellowback == nullptr);
    BOOST_CHECK(COINBASE_FLAGS.empty());

    // The BIP34 height push changes width at these boundaries (CScriptNum encoding), so the
    // append is checked against each: a wider push must not make the concatenation differ.
    const int heights[] = {1, 16, 17, 127, 128, 65535, 65536, 16777215, 16777216};
    for (int nHeight : heights) {
        const CScript stockCreate = CScript() << nHeight << OP_0;
        const CScript forkCreate = (CScript() << nHeight << OP_0) + COINBASE_FLAGS;
        BOOST_CHECK_MESSAGE(forkCreate == stockCreate,
                            "CreateCoinbaseTransaction scriptSig differs at height " << nHeight);
        BOOST_CHECK_EQUAL(forkCreate.size(), stockCreate.size());

        for (unsigned int nExtraNonce : {0u, 1u, 0xffffu}) {
            const CScript stockIncr = CScript() << nHeight << CScriptNum(nExtraNonce);
            const CScript forkIncr = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
            BOOST_CHECK_MESSAGE(forkIncr == stockIncr,
                                "IncrementExtraNonce scriptSig differs at height " << nHeight);
            // The coinbase length limit of main.cpp:1456 is 100 bytes; the stock form is far
            // below it and the empty append cannot move it.
            BOOST_CHECK(forkIncr.size() <= 100u);
        }
    }

    // An empty COINBASE_FLAGS also carries no tag, so a stock-configured node's coinbase is not
    // merely byte-identical but invisible to the tag reader (TAG-1).
    BOOST_CHECK(!FindTag((CScript() << 200 << OP_0) + COINBASE_FLAGS, 200).has_value());
}

// Rule: MINER-1
// Rule: MINER-2
// Rule: MINER-3
BOOST_AUTO_TEST_CASE(miner_tag_script)
{
    // The tag a template carries: a quote tag while the quote is younger than quoteMaxAge, else a
    // signal-only tag iff -yellowbacksignal, else nothing; the signal bit iff signal and enforce;
    // no payout key means no tag; and COINBASE_FLAGS is empty without the flag (§8.4 item 2).
    BOOST_CHECK(g_yellowback == nullptr);
    BOOST_CHECK(COINBASE_FLAGS.empty());
    Live live(pathTemp / "yb-miner");
    live.Activate();
    MinerConfig cfg = live.index->GetMinerConfig();
    cfg.quoteMaxAge = 100;
    live.index->SetMinerConfig(cfg);
    live.index->SetQuote(2000000, 5, 1000);
    const int h = live.Tip() + 1;
    {
        CScript s = policy::BuildTagScript(*live.index, 1050);
        std::optional<CoinbaseTag> t = FindTag(CScript() << h << OP_0, h);
        BOOST_CHECK(!t.has_value());
        t = FindTag((CScript() << h << OP_0) + s, h);
        BOOST_REQUIRE(t.has_value());
        BOOST_CHECK(t->IsQuote());
        BOOST_CHECK_EQUAL(t->priceMicroUsd, 2000000u);
        BOOST_CHECK_EQUAL(t->sourceMask, 5);
        BOOST_CHECK(t->Signal());
        BOOST_CHECK(t->payoutKey == KeyOf(0));
        MinerStatus st = live.index->GetMinerStatus(1050);
        BOOST_CHECK_EQUAL(st.kind, "quote");
        BOOST_CHECK_EQUAL(st.quoteAgeSeconds.value_or(-1), 50);
        BOOST_CHECK(st.registered);                                  // REG-1: KeyOf(0) quoted within N_REG
        BOOST_CHECK(st.eligible);
    }
    {
        // Stale quote: signal-only.
        std::optional<CoinbaseTag> t = FindTag((CScript() << h << OP_0) + policy::BuildTagScript(*live.index, 1101), h);
        BOOST_REQUIRE(t.has_value());
        BOOST_CHECK(!t->IsQuote());
        BOOST_CHECK(t->Signal());
        BOOST_CHECK_EQUAL(live.index->GetMinerStatus(1101).kind, "signal");
    }
    {
        // No signal flag and a stale quote: no tag at all; a fresh quote still tags without the bit.
        cfg.signal = false;
        live.index->SetMinerConfig(cfg);
        BOOST_CHECK(policy::BuildTagScript(*live.index, 1101).empty());
        BOOST_CHECK_EQUAL(live.index->GetMinerStatus(1101).kind, "none");
        std::optional<CoinbaseTag> t = FindTag((CScript() << h << OP_0) + policy::BuildTagScript(*live.index, 1050), h);
        BOOST_REQUIRE(t.has_value());
        BOOST_CHECK(!t->Signal());
        // Signal without enforce: no bit (L3).
        cfg.signal = true;
        cfg.enforce = false;
        live.index->SetMinerConfig(cfg);
        t = FindTag((CScript() << h << OP_0) + policy::BuildTagScript(*live.index, 1050), h);
        BOOST_REQUIRE(t.has_value());
        BOOST_CHECK(!t->Signal());
        // No payout key: no tag of either kind (MINER-2).
        cfg.enforce = true;
        cfg.payoutKey = std::nullopt;
        live.index->SetMinerConfig(cfg);
        BOOST_CHECK(policy::BuildTagScript(*live.index, 1050).empty());
        BOOST_CHECK_EQUAL(live.index->GetMinerStatus(1050).kind, "none");
    }
}

// Rule: SNAP
BOOST_FIXTURE_TEST_CASE(params_change_wipes_on_start, TestChain100Setup)
{
    // A node restarted with different hashed parameters (the four regtest values) rebuilds:
    // SyncToChain wipes when the stored Params record differs, so params_mismatch_fails_loudly sees
    // the new record in the hash and index_start_height_above_tip sees no rows below the new start.
    yellowback::Params original = RegtestParams(1, 0, 0, 0);
    uint256 h0;
    {
        YellowbackIndex index(original, pathTemp / "yb-params", 1 << 20, true);
        BOOST_REQUIRE(index.SyncToChain());
        BOOST_REQUIRE_EQUAL(TipHeightOf(index), 100);
        h0 = HashOf(index);
    }
    {
        YellowbackIndex reopened(RegtestParams(1, 1, 0, 0), pathTemp / "yb-params", 1 << 20, false);
        BOOST_REQUIRE(reopened.SyncToChain());
        BOOST_CHECK_EQUAL(TipHeightOf(reopened), 100);
        LOCK(reopened.cs_yellowback);
        std::optional<ParamsRecord> rec = State(reopened.View()).GetParamsRecord();
        BOOST_REQUIRE(rec.has_value());
        BOOST_CHECK_EQUAL(rec->sigmaRefBps, 1);
        BOOST_CHECK(reopened.GetStateHash() != h0);
    }
    {
        YellowbackIndex back(RegtestParams(50, 0, 0, 0), pathTemp / "yb-params", 1 << 20, false);
        BOOST_REQUIRE(back.SyncToChain());
        BOOST_CHECK_EQUAL(TipHeightOf(back), 100);
        LOCK(back.cs_yellowback);
        BOOST_CHECK(!State(back.View()).GetSnapshot(49).has_value());   // no rows below the new start
        BOOST_CHECK(State(back.View()).GetSnapshot(50).has_value());
    }
    {
        YellowbackIndex same(original, pathTemp / "yb-params", 1 << 20, false);
        BOOST_REQUIRE(same.SyncToChain());
        BOOST_CHECK(HashOf(same) == h0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
