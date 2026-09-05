// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_MATH_H
#define YCASH_YELLOWBACK_MATH_H

#include "amount.h"
#include "arith_uint256.h"
#include "yellowback/params.h"

#include <optional>

/**
 * Derived quantities of plan §3.6. Integer only, arith_uint256 for every
 * product (Ycash uses no __int128; plan D12). Nothing here reads the chain,
 * the clock or configuration, so every function is callable from a unit test.
 *
 * Reference tables: ref/digibyte/src/consensus/dca.cpp:53-58 (DCA) and
 * ref/digibyte/src/consensus/err.cpp:38-41,100 (ERR).
 */
namespace yellowback {

/** ceil(a / b) for b > 0. */
inline arith_uint256 CeilDiv(const arith_uint256& a, const arith_uint256& b)
{
    return (a + b - arith_uint256(1)) / b;
}

/** True iff a fits in an int64_t. */
inline bool FitsInt64(const arith_uint256& a)
{
    return a.bits() <= 63;
}

/**
 * System health in percent, capped at HEALTH_CAP:
 *   HEALTH_CAP                                   if supplyCents == 0
 *   0                                            if the price is undefined (fail closed)
 *   min(HEALTH_CAP, C * p / (COIN * S * 100))    otherwise
 */
inline int Health(Cents supplyCents, CAmount collateralZat, std::optional<MicroUsd> price)
{
    if (supplyCents <= 0) return HEALTH_CAP;
    if (!price.has_value() || price.value() <= 0) return 0;
    if (collateralZat <= 0) return 0;
    arith_uint256 num = arith_uint256(collateralZat) * arith_uint256(price.value());
    arith_uint256 den = arith_uint256(COIN) * arith_uint256(supplyCents) * arith_uint256(100);
    arith_uint256 h = num / den;
    if (h >= arith_uint256(HEALTH_CAP)) return HEALTH_CAP;
    return (int)h.GetLow64();
}

/** DCA multiplier in basis points (ref/digibyte/src/consensus/dca.cpp:53-58). */
inline int DcaBps(int healthPct)
{
    if (healthPct >= 150) return 10000;
    if (healthPct >= 120) return 12500;
    if (healthPct >= 110) return 15000;
    return 20000;
}

/** ERR ratio in basis points (ref/digibyte/src/consensus/err.cpp:38-41). 10000 == inactive. */
inline int ErrBps(int healthPct)
{
    if (healthPct >= 100) return 10000;
    if (healthPct >= 95) return 9500;
    if (healthPct >= 90) return 9000;
    if (healthPct >= 85) return 8500;
    return 8000;
}

/**
 * Collateral in zatoshi required to mint `cents` at tier ratio `ratioPct`
 * under DCA multiplier `dcaBps` and price `priceMicroUsd`:
 *   ceil(cents * ratioPct * dcaBps * COIN / (100 * price))
 * (DigiByte's CalculateRequiredCollateral). nullopt if undefined or above MAX_MONEY.
 */
inline std::optional<CAmount> RequiredCollateral(Cents cents, int ratioPct, int dcaBps, MicroUsd priceMicroUsd)
{
    if (cents <= 0 || ratioPct <= 0 || dcaBps <= 0 || priceMicroUsd <= 0) return std::nullopt;
    arith_uint256 num = arith_uint256(cents) * arith_uint256(ratioPct) * arith_uint256(dcaBps) * arith_uint256(COIN);
    arith_uint256 den = arith_uint256(100) * arith_uint256(priceMicroUsd);
    arith_uint256 r = CeilDiv(num, den);
    if (!FitsInt64(r)) return std::nullopt;
    CAmount z = (CAmount)r.GetLow64();
    if (!MoneyRange(z)) return std::nullopt;
    return z;
}

/** Same, rounded up to a multiple of `granularity` zat (the wallet uses 1,000; MINTPOL-1). */
inline std::optional<CAmount> RequiredCollateralRounded(Cents cents, int ratioPct, int dcaBps, MicroUsd priceMicroUsd, CAmount granularity = 1000)
{
    auto r = RequiredCollateral(cents, ratioPct, dcaBps, priceMicroUsd);
    if (!r.has_value() || granularity <= 0) return r;
    CAmount z = r.value();
    CAmount rem = z % granularity;
    if (rem != 0) z += granularity - rem;
    if (!MoneyRange(z)) return std::nullopt;
    return z;
}

/** Yellowback cents that must be burned to release a vault that minted `mintedCents`: ceil(M * 10000 / errBps). */
inline Cents RequiredBurn(Cents mintedCents, int errBps)
{
    if (mintedCents <= 0) return 0;
    if (errBps <= 0) errBps = 8000;
    arith_uint256 r = CeilDiv(arith_uint256(mintedCents) * arith_uint256(10000), arith_uint256(errBps));
    return (Cents)r.GetLow64();
}

/**
 * Volatility breach test (§3.6): |p0 - pRef| * 10000 >= thresholdBps * pRef.
 * Either price undefined => not evaluated (false).
 */
inline bool VolatilityBreach(std::optional<MicroUsd> p0, std::optional<MicroUsd> pRef, int thresholdBps)
{
    if (!p0.has_value() || !pRef.has_value() || pRef.value() <= 0 || p0.value() <= 0) return false;
    MicroUsd a = p0.value(), b = pRef.value();
    arith_uint256 diff = a >= b ? arith_uint256(a - b) : arith_uint256(b - a);
    return diff * arith_uint256(10000) >= arith_uint256(thresholdBps) * arith_uint256(b);
}

} // namespace yellowback

#endif // YCASH_YELLOWBACK_MATH_H
