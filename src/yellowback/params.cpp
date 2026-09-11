// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/params.h"

#include <stdexcept>

namespace yellowback {

namespace {

// v1, retained: lock tiers at 75-second blocks; ratios from ref/digibyte/src/consensus/digidollar.h:72-84.
const int MAINNET_TIER_BLOCKS[NUM_TIERS]  = { 48, 34560, 103680, 207360, 420480 };
const int TIER_RATIO_PCT[NUM_TIERS]       = { 1000, 500, 400, 350, 300 };
// Regtest overrides (plan §3.1): only block counts shrink.
const int REGTEST_TIER_BLOCKS[NUM_TIERS]  = { 48, 96, 144, 192, 240 };

/** The §3.1 mainnet/testnet column. Every rule-read value is compiled in (K10). */
void SetCommon(Params& p)
{
    p.minMint   = 10000;      // $100
    p.maxMint   = 1000000;    // $10,000 (param; DigiByte $100,000)
    p.minOutput = 100;        // $1.00 (DigiByte minOutputAmount)
    p.maxOutput = 10000000;   // $100,000 (param; DigiByte's maxMintAmount)
    p.tokenValue = TOKEN_VALUE;
    p.refWindow  = REF_WINDOW;

    p.pFastWindow = 96;   p.pFastMinFill = 48;     // ceil(W/2)
    p.pMidWindow  = 576;  p.pMidMinFill  = 384;    // ceil(2W/3) (L9)
    p.pSlowWindow = 2016; p.pSlowMinFill = 1344;

    p.signalWindow        = 2016;
    p.activationThreshold = 1512;   // 75 %
    p.participationFloor  = 1210;   // 60 %
    p.activationDelay     = 2016;
    p.enforcementFloor    = 1008;   // 50 % (L3)
    p.enforcementResume   = 1210;   // 60 %
    p.valveBlocks         = 6;      // L7
    p.abandonBlocks       = 4032;   // 2 * signalWindow (L10, L12)

    p.nReg            = 576;
    p.peerLag         = 10;
    p.peerMin         = 5;
    p.deviationBps    = 1000;
    p.accuracyBandBps = 300;
    p.payeeWindow     = 100;

    p.feeMin = 50000000;            // 0.5 YEC (V10)
    p.feeBps = 25;

    p.grace              = 34560;   // 30 d
    p.claimThresholdBps  = 11000;
    p.supplyCapBps       = 1500;    // 15 % of market cap (V21)
    p.globalRatioHaltBps = 25000;
    p.divergenceBps      = 2000;
    p.classMin[0] = 34560;  p.classMax[0] = 103680;  p.baseRatioBps[0] = 50000;   // A: 30-90 d
    p.classMin[1] = 103681; p.classMax[1] = 420480;  p.baseRatioBps[1] = 40000;   // B: 90-365 d
    p.classMin[2] = 420481; p.classMax[2] = 2102400; p.baseRatioBps[2] = 30000;   // C: 1-5 y (MAX_LOCK, V19)

    p.volWindow         = 2016;
    p.volStep           = 48;
    p.volPeriodsPerYear = 8760;     // K13: equal on every network
    p.sigmaRefBps       = 10000;
    p.sigmaMultMaxBps   = 30000;

    p.nPenalty       = 288;         // L6 wallet defaults
    p.accuracyWindow = 576;
    p.payeeTiltBps   = 10000;

    p.enforceUntilHeight = 0;       // set per release beside startHeight (L8)

    // v1, retained
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
    : startHeight(0), enforceUntilHeight(0),
      pFastWindow(0), pMidWindow(0), pSlowWindow(0), pFastMinFill(0), pMidMinFill(0), pSlowMinFill(0),
      signalWindow(0), activationThreshold(0), participationFloor(0), activationDelay(0),
      enforcementFloor(0), enforcementResume(0), valveBlocks(0), abandonBlocks(0),
      nReg(0), peerLag(0), peerMin(0), deviationBps(0), accuracyBandBps(0), payeeWindow(0),
      feeMin(0), feeBps(0),
      grace(0), claimThresholdBps(0), supplyCapBps(0), globalRatioHaltBps(0), divergenceBps(0),
      volWindow(0), volStep(0), volPeriodsPerYear(0), sigmaRefBps(0), sigmaMultMaxBps(0),
      minMint(0), maxMint(0), minOutput(0), maxOutput(0), tokenValue(0), refWindow(0),
      nPenalty(0), accuracyWindow(0), payeeTiltBps(0),
      supplyCap(0), rosterGrace(0), volCooldown(0), volWindowShort(0), volWindowLong(0)
{
    for (int i = 0; i < NUM_CLASSES; i++) {
        classMin[i] = classMax[i] = baseRatioBps[i] = 0;
    }
    for (int i = 0; i < NUM_TIERS; i++) {
        tierBlocks[i] = 0;
        tierRatioPct[i] = 0;
    }
}

int Params::ClassForLockBlocks(int64_t lockBlocks) const
{
    for (int i = 0; i < NUM_CLASSES; i++) {
        if (lockBlocks >= classMin[i] && lockBlocks <= classMax[i]) return i;
    }
    return -1;
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

/** The §3.1 regtest column; only the four arguments come from flags (M13). */
Params RegtestParams(int startHeight, int sigmaRefBps, int supplyCapBps, int enforceUntil)
{
    Params r;
    r.network = "regtest";
    SetCommon(r);
    r.addressVersion = { 0x20, 0x02 };       // renders "yr…" (D10)
    r.pFastWindow = 8;  r.pFastMinFill = 4;
    r.pMidWindow  = 24; r.pMidMinFill  = 16;
    r.pSlowWindow = 64; r.pSlowMinFill = 43;
    r.signalWindow        = 64;
    r.activationThreshold = 48;
    r.participationFloor  = 39;
    r.activationDelay     = 64;
    r.enforcementFloor    = 32;
    r.enforcementResume   = 39;
    r.valveBlocks         = 6;
    r.abandonBlocks       = 128;
    r.nReg    = 24;
    r.peerLag = 4;
    r.peerMin = 3;
    r.payeeWindow = 10;
    r.grace = 24;
    r.classMin[0] = 48;  r.classMax[0] = 96;
    r.classMin[1] = 97;  r.classMax[1] = 144;
    r.classMin[2] = 145; r.classMax[2] = 240;
    r.volWindow = 64;
    r.volStep   = 8;
    r.nPenalty       = 12;
    r.accuracyWindow = 24;
    // the four flags
    r.startHeight        = startHeight;
    r.sigmaRefBps        = sigmaRefBps;
    r.supplyCapBps       = supplyCapBps;
    r.enforceUntilHeight = enforceUntil;
    // v1, retained
    for (int i = 0; i < NUM_TIERS; i++) r.tierBlocks[i] = REGTEST_TIER_BLOCKS[i];
    r.rosterGrace = 48; r.volCooldown = 96; r.volWindowShort = 48; r.volWindowLong = 96;
    return r;
}

/** v1, retained. */
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
