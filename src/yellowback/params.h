// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_PARAMS_H
#define YCASH_YELLOWBACK_PARAMS_H

#include "amount.h"
#include "primitives/transaction.h"
#include "script/script.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * Yellowback: a miner-enforced, over-collateralised USD stablecoin overlay on
 * Ycash (Ycash Yellowback, YED).
 *
 * No opcode and no network upgrade: the overlay reads coinbase tags and vault
 * spends, and enforcing miners reject blocks whose vault spends break RED-1..4
 * (a soft fork among the enforcing subset). The protocol is
 * docs/plans/yellowback-v2-development-plan.md §3; the constants below are
 * §3.1. Rule identifiers in comments (TAG-1, MINT-2, RED-3, …) refer to it.
 *
 * DigiByte's DigiDollar (ref/digibyte/src/consensus/digidollar.h) is the
 * behavioural reference for the ratios and the oracle bundle; the v2 tag,
 * medians and volatility are the Ycash adaptation (mapping.md §13).
 *
 * The federation prototype's symbols (tier tables, health/volatility
 * thresholds, the genesis anchor and the signer set) were deleted in Phases
 * 2 and 3; nothing v1 remains.
 */
namespace yellowback {

/** Yellowback amounts are integer US cents; 100 == $1.00. */
typedef int64_t Cents;
/** Prices are integer micro-USD per YEC; 1,000,000 == $1.00. */
typedef int64_t MicroUsd;

/** Payload magic ("YB") and version (§3.3). */
static const unsigned char PAYLOAD_MAGIC_0 = 0x59;
static const unsigned char PAYLOAD_MAGIC_1 = 0x42;
static const unsigned char PAYLOAD_VERSION = 0x02;
/** Largest payload: Ycash nMaxDatacarrierBytes (83) minus OP_RETURN and the push opcode. */
static const size_t MAX_PAYLOAD = 80;
static const size_t MIN_PAYLOAD = 4;

/** Coinbase tag (§3.2): magic "YED!", version 1, 36 bytes. */
static const unsigned char TAG_MAGIC[4] = { 0x59, 0x45, 0x44, 0x21 };
static const unsigned char TAG_VERSION = 0x01;
static const size_t TAG_SIZE = 36;

/** Blocks per hour / day / year at the 75-second post-Blossom spacing (consensus/params.h:212). */
static const int BLOCKS_PER_HOUR = 48;
static const int BLOCKS_PER_DAY = 1152;
static const int BLOCKS_PER_YEAR = 420480;

/** Price bounds in micro-USD per YEC: $0.0001 .. $100 (DigiByte's bounds, primitives/oracle.h:23-24). */
static const MicroUsd PRICE_MIN = 100;
static const MicroUsd PRICE_MAX = 100000000;

/**
 * Protocol constant equal to DEFAULT_POST_BLOSSOM_TX_EXPIRY_DELTA (main.h) but
 * deliberately not derived from it: -txexpirydelta must not change the rule.
 * MINT-2: H - REF_WINDOW <= refHeight <= H - 1.
 */
static const int REF_WINDOW = 40;
/** Wallet-side only: refHeight = indexTip - REF_LAG (-yellowbackmintlag, V11; the 36 bound is §3.5). */
static const int DEFAULT_REF_LAG = 2;
static const int MAX_REF_LAG = 36;

/** YEC carried by every Yellowback output: 10,000 zat, >= 100x the dust floor. */
static const CAmount TOKEN_VALUE = 10000;
/** Flat network fee; equals policy DEFAULT_FEE (policy/fees.h). -yellowbackfee may not go below it. */
static const CAmount DEFAULT_YELLOWBACK_FEE = 1000;

/** Term classes (V19): A = 30-90 d, B = 90-365 d, C = 1-5 y. */
static const int NUM_CLASSES = 3;
/** A signal-only tag carries this price (V9). */
static const uint64_t TAG_PRICE_SIGNAL_ONLY = 0;
/** feeVout value meaning "no enforcement-fee output" (§3.3). */
static const uint8_t FEE_VOUT_NONE = 0xFF;

/**
 * Per-network parameters (§3.1, field list §4.2a). Built once per network.
 * Regtest takes startHeight, sigmaRefBps, supplyCapBps and enforceUntilHeight
 * from the four regtest-only flags (parsed in index.cpp, never here: this file
 * is libbitcoin_common and §3.10 forbids GetArg); every other regtest value is
 * compiled in.
 */
struct Params
{
    std::string network;                 //!< "main", "test" or "regtest" (CChainParams::NetworkIDString)

    int startHeight;                     //!< first height whose tags count; 0 = not configured; >= 1 (M2)
    int enforceUntilHeight;              //!< ACT-5 sunset; 0 = none (regtest); past it the node rejects nothing
    std::vector<unsigned char> addressVersion; //!< Base58Check version bytes of Yellowback addresses (D10)

    // Prices (PRICE-1..2, V16, L9)
    int pFastWindow, pMidWindow, pSlowWindow;          //!< 96 / 576 / 2,016
    int pFastMinFill, pMidMinFill, pSlowMinFill;       //!< ceil(W/2), ceil(2W/3), ceil(2W/3)

    // Activation (ACT-1..7)
    int signalWindow;                    //!< 2,016
    int activationThreshold;             //!< 1,512 (75 %)
    int participationFloor;              //!< 1,210 (60 %): below it minting halts
    int activationDelay;                 //!< 2,016 blocks from lock-in to ACTIVE
    int enforcementFloor;                //!< 1,008 (50 %): below it block rejection suspends
    int enforcementResume;               //!< 1,210 (60 %)
    int valveBlocks;                     //!< 6 (node-local, never a state input)
    int abandonBlocks;                   //!< 4,032 (L10, L12)

    // Miners (REG-1..4, FEE-2)
    int nReg;                            //!< 576 (informational)
    int peerLag;                         //!< 10
    int peerMin;                         //!< 5
    int deviationBps;                    //!< 1,000
    int accuracyBandBps;                 //!< 300
    int payeeWindow;                     //!< 100

    // Enforcement fee (FEE-1)
    CAmount feeMin;                      //!< 0.5 YEC
    int feeBps;                          //!< 25

    // Vaults
    int grace;                           //!< claimHeight = lockHeight + grace
    int claimThresholdBps;               //!< 11,000
    int supplyCapBps;                    //!< 1,500; 0 = no cap
    int globalRatioHaltBps;              //!< 25,000
    int divergenceBps;                   //!< 2,000
    int classMin[NUM_CLASSES];           //!< lock length range per class (blocks), inclusive
    int classMax[NUM_CLASSES];
    int baseRatioBps[NUM_CLASSES];       //!< 50,000 / 40,000 / 30,000

    // Volatility (SIGMA-1, V17)
    int volWindow;                       //!< 2,016
    int volStep;                         //!< 48
    int volPeriodsPerYear;               //!< 8,760 on every network (K13)
    int sigmaRefBps;                     //!< 10,000; 0 = multiplier fixed at 1
    int sigmaMultMaxBps;                 //!< 30,000

    // Amounts
    Cents minMint;                       //!< MINT-2
    Cents maxMint;                       //!< MINT-2
    Cents minOutput;                     //!< XFER-1
    Cents maxOutput;                     //!< XFER-1
    CAmount tokenValue;                  //!< TOKEN_VALUE
    int refWindow;                       //!< REF_WINDOW

    // L6 wallet defaults (never hashed; node overrides -yellowbackpayee*)
    int nPenalty;                        //!< 288
    int accuracyWindow;                  //!< 576
    int payeeTiltBps;                    //!< 10,000

    Params();

    /** v2: configured iff the start height is known (§4.2). */
    bool IsConfigured() const { return startHeight > 0; }
    /** Class index (0..2) for a lock length in blocks; -1 if in no class (V19). */
    int ClassForLockBlocks(int64_t lockBlocks) const;
    bool IsValidClass(int termClass) const { return termClass >= 0 && termClass < NUM_CLASSES; }
    /** WINDOW_MIN_FILL of the three price windows (PRICE-1, L9). */
    int MinFill(int window) const
    {
        if (window == pFastWindow) return pFastMinFill;
        if (window == pMidWindow) return pMidMinFill;
        return pSlowMinFill;
    }
};

/** Mainnet and testnet parameters; startHeight is set per release (§3.1, K10). */
const Params& MainParams();
const Params& TestParams();

/**
 * v2 regtest parameters (§3.1 regtest column). The four arguments are the
 * regtest-only flags -yellowbackstartheight, -yellowbacksigmaref (0 = multiplier
 * fixed at 1), -yellowbacksupplycapbps (0 = no cap) and -yellowbackenforceuntil
 * (0 = no sunset); they are the only values hashed into the state hash (M13).
 */
Params RegtestParams(int startHeight, int sigmaRefBps, int supplyCapBps, int enforceUntil);

/**
 * Parameter-set selection by height (§3.1 *Parameter versioning*, K10): the
 * set with the greatest startHeight <= height, or the first set when none
 * qualifies (every rule below a set's start reads the virtual snapshot
 * anyway). `sets` must be non-empty. Pure; the index passes its release's
 * sets and EvaluateBlock receives the one selected for H.
 */
const Params& SelectParams(const std::vector<Params>& sets, int height);

/** Parameters for a network id as returned by CChainParams::NetworkIDString(); regtest returns unconfigured defaults. */
const Params& ParamsForNetwork(const std::string& networkId);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_PARAMS_H
