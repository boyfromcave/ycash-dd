// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_SCRIPT_H
#define YCASH_YELLOWBACK_SCRIPT_H

#include "pubkey.h"
#include "script/script.h"
#include "script/standard.h"
#include "yellowback/params.h"

#include <vector>

#include <optional>

/**
 * Yellowback scripts (plan §3.4, V7). Existing opcodes only.
 *
 * Vault script (redeem script of the P2SH collateral output):
 *     OP_IF
 *       <lockHeight> OP_CHECKLOCKTIMEVERIFY OP_DROP <ownerPubKey> OP_CHECKSIG
 *     OP_ELSE
 *       <claimHeight> OP_CHECKLOCKTIMEVERIFY OP_DROP OP_TRUE
 *     OP_ENDIF
 * with claimHeight = lockHeight + GRACE. Owner-path scriptSig
 * `<ownerSig> OP_1 <vaultScript>`, claim-path scriptSig `OP_0 <vaultScript>`;
 * both leave exactly one element (CLEANSTACK) and are minimal pushes
 * (MINIMALDATA). 51-53 bytes, one sigop.
 *
 * DigiByte's vault is a P2TR two-leaf MAST with DD opcodes
 * (ref/digibyte/src/digidollar/scripts.h:112); Ycash has no Taproot, no
 * MINIMALIF and no spare opcode semantics (mapping.md §2, §13), so the two
 * leaves become the two OP_IF branches and the burn-on-release invariant is
 * enforced by miners on the spending transaction (RED-1..4), not by script.
 * The claim path is anyone-can-spend at the base layer and safe only under
 * enforcement (V7).
 *
 * The federation prototype's k-of-n multisig helpers were deleted in Phase 3
 * (§4.2); IsCompressedKey, P2SHScript and ExtractRedeemScript remain.
 */
namespace yellowback {

typedef std::vector<unsigned char> valtype;

// ---------------------------------------------------------------- v2 (§3.4)

/** The vault redeem script. Empty if lockHeight/claimHeight are not in [1, LOCKTIME_THRESHOLD), claimHeight <= lockHeight, or the key is not compressed. */
CScript VaultScript(uint32_t lockHeight, const CPubKey& owner, uint32_t claimHeight);

/** Parse a v2 vault script back into its parts; false for anything but the exact template with minimal pushes. */
bool ParseVaultScript(const CScript& script, uint32_t& lockHeight, CPubKey& owner, uint32_t& claimHeight);

/** `<ownerSig> OP_1 <vaultScript>`. The signature carries its hashtype byte. */
CScript OwnerScriptSig(const valtype& ownerSig, const CScript& vaultScript);

/** `OP_0 <vaultScript>`. */
CScript ClaimScriptSig(const CScript& vaultScript);

/** The path a vault spend takes, as OP_IF would evaluate its selector (K4). */
struct VaultSpendPath
{
    bool ownerPath;           //!< CastToBool(selector): true = owner branch, false = claim branch
    valtype selector;         //!< the push immediately before the redeem script (empty for OP_0; the number for OP_1..OP_16)
    CScript vaultScript;      //!< the last push
    valtype ownerSig;         //!< the push before the selector, if any (empty otherwise)
    size_t pushes;            //!< number of pushes in the scriptSig
};

/**
 * Path detection for RED-4. Consensus verifies vaults with P2SH | CLTV only
 * (ref/ycash/src/main.cpp:2931), so the selector is anything OP_IF accepts:
 * `OP_2` or a non-minimal `1` is an owner selector, `0x80` (negative zero) a
 * claim selector. nullopt iff the scriptSig is not push-only or has fewer
 * than two pushes (RED-1, M1). Whether the shape is exactly the wallet's
 * `<sig> OP_1 <script>` / `OP_0 <script>` is strict-template policy (TPL-2),
 * not decided here.
 */
std::optional<VaultSpendPath> ParseVaultSpendPath(const CScript& scriptSig);

// ---------------------------------------------------------------- shared helpers

/** True iff the key is a 33-byte compressed encoding (plan D7). */
bool IsCompressedKey(const CPubKey& key);

/** P2SH scriptPubKey for a redeem script. */
CScript P2SHScript(const CScript& redeemScript);

/** The last push of a P2SH scriptSig, i.e. the redeem script. False if the scriptSig is not push-only or empty. */
bool ExtractRedeemScript(const CScript& scriptSig, CScript& redeemScript);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_SCRIPT_H
