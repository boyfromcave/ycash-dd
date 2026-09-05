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
 * Yellowback: a federated, over-collateralised USD stablecoin overlay on Ycash.
 *
 * Everything in this namespace is a Tier-0 overlay: no consensus rule, no
 * policy rule, no opcode and no network upgrade. The protocol is specified in
 * the workspace document docs/spec/yellowback-adaptation-spec.md (plan §3); the
 * constants below are §3.1. Rule identifiers in comments (MINT-2, XFER-1, …)
 * refer to that document.
 *
 * DigiByte's DigiDollar (ref/digibyte/src/consensus/digidollar.h) is the
 * behavioural reference for the ratios, tiers and protection tables.
 */
namespace yellowback {

/** Yellowback amounts are integer US cents; 100 == $1.00. */
typedef int64_t Cents;
/** Prices are integer micro-USD per YEC; 1,000,000 == $1.00. */
typedef int64_t MicroUsd;

/** Payload magic ("YB") and version (§3.2). */
static const unsigned char PAYLOAD_MAGIC_0 = 0x59;
static const unsigned char PAYLOAD_MAGIC_1 = 0x42;
static const unsigned char PAYLOAD_VERSION = 0x01;
/** Largest payload: Ycash nMaxDatacarrierBytes (83) minus OP_RETURN and the push opcode. */
static const size_t MAX_PAYLOAD = 80;
static const size_t MIN_PAYLOAD = 4;

/** Blocks per hour / day at the 75-second post-Blossom spacing. */
static const int BLOCKS_PER_HOUR = 48;
static const int BLOCKS_PER_DAY = 1152;

/** An attestation older than this is not a price (§3.6). */
static const int PRICE_MAX_AGE = 48;
/** Price bounds in micro-USD per YEC: $0.0001 .. $100 (DigiByte's bounds). */
static const MicroUsd PRICE_MIN = 100;
static const MicroUsd PRICE_MAX = 100000000;

/**
 * Protocol constant equal to DEFAULT_POST_BLOSSOM_TX_EXPIRY_DELTA (main.h) but
 * deliberately not derived from it: -txexpirydelta must not change the rule.
 */
static const int MINT_WINDOW = 40;
/** Wallet-side only: evalHeight = indexTip - MINT_EVAL_LAG (plan C4). */
static const int DEFAULT_MINT_EVAL_LAG = 2;
static const int MAX_MINT_EVAL_LAG = 36;
/** Co-signer slack on RED-8 (plan E2). */
static const int RED_SKEW = 2;

/** YEC carried by every Yellowback output: 10,000 zat, >= 100x the dust floor. */
static const CAmount TOKEN_VALUE = 10000;
/** Flat fee; equals policy DEFAULT_FEE (policy/fees.h). -yellowbackfee may not go below it (C16). */
static const CAmount DEFAULT_YELLOWBACK_FEE = 1000;

/** Health is a percentage capped as DigiByte does. */
static const int HEALTH_CAP = 30000;
/** Volatility thresholds in basis points: 20 % / 1 h, 30 % / 24 h. */
static const int VOL_1H_BPS = 2000;
static const int VOL_24H_BPS = 3000;

/** Roster bound from the 520-byte push / 15-sigop limits (plan D3). */
static const unsigned int ROSTER_MAX_N = 13;

static const int NUM_TIERS = 5;

/** Per-network parameters (§3.1). Built once per network; regtest values come from init arguments. */
struct Params
{
    std::string network;                 //!< "main", "test" or "regtest" (CChainParams::NetworkIDString)

    int startHeight;                     //!< first block the index applies; 0 = not configured
    COutPoint genesisAnchor;             //!< the federation's first anchor UTXO
    CScript genesisRosterScript;         //!< k-of-n CHECKMULTISIG redeem script of that anchor

    std::vector<unsigned char> addressVersion; //!< Base58Check version bytes of Yellowback addresses (D10)

    Cents minMint;                       //!< MINT-2
    Cents maxMint;                       //!< MINT-2
    Cents minOutput;                     //!< XFER-1
    Cents maxOutput;                     //!< XFER-1
    Cents supplyCap;                     //!< MINT-6; 0 = no cap

    int tierBlocks[NUM_TIERS];           //!< lock period per tier
    int tierRatioPct[NUM_TIERS];         //!< collateral ratio per tier, percent

    int rosterGrace;                     //!< MINT-3 grace for the previous roster after a reveal
    int volCooldown;                     //!< mint freeze length after a volatility breach
    int volWindowShort;                  //!< volatility window "1 hour" in blocks
    int volWindowLong;                   //!< volatility window "24 hours" in blocks

    Params();

    bool IsConfigured() const { return startHeight > 0 && !genesisRosterScript.empty() && !genesisAnchor.IsNull(); }
    bool IsValidTier(int tier) const { return tier >= 0 && tier < NUM_TIERS; }
};

/** Mainnet and testnet parameters. Genesis anchor fields are placeholders until the key ceremony (plan §11). */
const Params& MainParams();
const Params& TestParams();

/**
 * Regtest parameters. The three genesis values are supplied by the test through
 * -yellowbackstartheight / -yellowbackgenesisanchor / -yellowbackgenesisroster (plan C2);
 * supplyCap 0 means no cap (-yellowbacksupplycap, plan G5).
 */
Params RegtestParams(int startHeight, const COutPoint& genesisAnchor, const CScript& genesisRosterScript, Cents supplyCap = 0);

/** Parameters for a network id as returned by CChainParams::NetworkIDString(); regtest returns unconfigured defaults. */
const Params& ParamsForNetwork(const std::string& networkId);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_PARAMS_H
