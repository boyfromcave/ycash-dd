// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/params.h"

#include <stdexcept>

namespace yellowback {

namespace {

// Lock tiers at 75-second blocks; ratios from ref/digibyte/src/consensus/digidollar.h:72-84
// (tiers 5-9 are deferred, plan D11).
const int MAINNET_TIER_BLOCKS[NUM_TIERS]  = { 48, 34560, 103680, 207360, 420480 };
const int TIER_RATIO_PCT[NUM_TIERS]       = { 1000, 500, 400, 350, 300 };
// Regtest overrides (plan §3.1): only block counts shrink.
const int REGTEST_TIER_BLOCKS[NUM_TIERS]  = { 48, 96, 144, 192, 240 };

void SetCommon(Params& p)
{
    p.minMint   = 10000;      // $100
    p.maxMint   = 1000000;    // $10,000 (param; DigiByte $100,000)
    p.minOutput = 100;        // $1.00 (DigiByte minOutputAmount)
    p.maxOutput = 10000000;   // $100,000 (param; DigiByte's maxMintAmount, plan C17)
    for (int i = 0; i < NUM_TIERS; i++) {
        p.tierBlocks[i]   = MAINNET_TIER_BLOCKS[i];
        p.tierRatioPct[i] = TIER_RATIO_PCT[i];
    }
    p.rosterGrace    = BLOCKS_PER_DAY;   // 1,152 blocks (plan B21)
    p.volCooldown    = 1728;             // 36 h, DigiByte's COOLDOWN_BLOCKS in Ycash blocks
    p.volWindowShort = BLOCKS_PER_HOUR;
    p.volWindowLong  = BLOCKS_PER_DAY;
}

} // namespace

Params::Params()
    : startHeight(0),
      minMint(0), maxMint(0), minOutput(0), maxOutput(0), supplyCap(0),
      rosterGrace(0), volCooldown(0), volWindowShort(0), volWindowLong(0)
{
    for (int i = 0; i < NUM_TIERS; i++) {
        tierBlocks[i] = 0;
        tierRatioPct[i] = 0;
    }
}

const Params& MainParams()
{
    static Params p = [] {
        Params m;
        m.network = "main";
        SetCommon(m);
        m.addressVersion = { 0x1F, 0xE4 };   // renders "ye…" (D10)
        m.supplyCap = 100000000;             // $1,000,000 v1 cap
        // Genesis anchor: filled by the mainnet key ceremony (plan Phase 8).
        m.startHeight = 0;
        return m;
    }();
    return p;
}

const Params& TestParams()
{
    static Params p = [] {
        Params t;
        t.network = "test";
        SetCommon(t);
        t.addressVersion = { 0x20, 0x07 };   // renders "yt…" (D10)
        t.supplyCap = 100000000;
        // Genesis anchor: filled by the testnet key ceremony (plan Phase 7).
        t.startHeight = 0;
        return t;
    }();
    return p;
}

Params RegtestParams(int startHeight, const COutPoint& genesisAnchor, const CScript& genesisRosterScript, Cents supplyCap)
{
    Params r;
    r.network = "regtest";
    SetCommon(r);
    r.addressVersion = { 0x20, 0x02 };       // renders "yr…" (D10)
    for (int i = 0; i < NUM_TIERS; i++) {
        r.tierBlocks[i] = REGTEST_TIER_BLOCKS[i];
    }
    r.rosterGrace    = 48;
    r.volCooldown    = 96;
    r.volWindowShort = 48;
    r.volWindowLong  = 96;                   // plan G5
    r.supplyCap      = supplyCap;
    r.startHeight    = startHeight;
    r.genesisAnchor  = genesisAnchor;
    r.genesisRosterScript = genesisRosterScript;
    return r;
}

const Params& ParamsForNetwork(const std::string& networkId)
{
    if (networkId == "main") return MainParams();
    if (networkId == "test") return TestParams();
    if (networkId == "regtest") {
        static Params r = RegtestParams(0, COutPoint(), CScript(), 0);
        return r;
    }
    throw std::runtime_error("yellowback: unknown network " + networkId);
}

} // namespace yellowback
