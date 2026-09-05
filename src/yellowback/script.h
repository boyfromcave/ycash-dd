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

/**
 * Yellowback scripts (plan §3.3). Existing opcodes only.
 *
 * Roster script (also the anchor's redeem script):
 *     <k> <Q1> … <Qn> <n> OP_CHECKMULTISIG
 *
 * Vault script (redeem script of the P2SH collateral output):
 *     <lockHeight> OP_CHECKLOCKTIMEVERIFY OP_DROP
 *     <ownerPubKey> OP_CHECKSIGVERIFY
 *     <k> <Q1> … <Qn> <n> OP_CHECKMULTISIG
 *
 * Vault scriptSig:
 *     OP_0 <qsig_1> … <qsig_k> <ownerSig> <vaultScript>
 *
 * DigiByte's vault is a P2TR two-leaf MAST with DD opcodes
 * (ref/digibyte/src/digidollar/scripts.h:112); Ycash has no Taproot and no
 * spare opcode semantics (mapping.md §2), so the burn-on-release invariant is
 * carried by the federation co-signature instead (plan D3).
 */
namespace yellowback {

typedef std::vector<unsigned char> valtype;

/** A parsed roster: threshold and keys in script order. */
struct Roster
{
    unsigned int k;
    std::vector<CPubKey> keys;

    Roster() : k(0) {}
    unsigned int n() const { return keys.size(); }
    bool IsValid() const;
};

/** True iff the key is a 33-byte compressed encoding (plan D7). */
bool IsCompressedKey(const CPubKey& key);

/** Keys sorted ascending by their serialisation, so every party derives the same script. */
std::vector<CPubKey> SortKeys(std::vector<CPubKey> keys);

/**
 * <k> <keys…> <n> OP_CHECKMULTISIG. Keys are used in the order given (call
 * SortKeys first when building a new roster). Returns an empty script if
 * k or n are out of range or any key is not compressed.
 */
CScript RosterScript(unsigned int k, const std::vector<CPubKey>& keys);

/**
 * Parse a roster script: exactly <k> <keys…> <n> OP_CHECKMULTISIG with
 * 1 <= k <= n <= ROSTER_MAX_N and every key 33 bytes. Anything else is not a
 * roster (an anchor spend revealing it still moves custody; §3.7 PRICE-2).
 */
bool ParseRosterScript(const CScript& script, Roster& roster);

/** The vault redeem script for lockHeight, owner and roster. Empty if any argument is invalid. */
CScript VaultScript(uint32_t lockHeight, const CPubKey& owner, const Roster& roster);

/** Convenience: VaultScript from the roster's script bytes. */
CScript VaultScript(uint32_t lockHeight, const CPubKey& owner, const CScript& rosterScript);

/** Parse a vault script back into its parts. */
bool ParseVaultScript(const CScript& script, uint32_t& lockHeight, CPubKey& owner, Roster& roster);

/** P2SH scriptPubKey for a redeem script. */
CScript P2SHScript(const CScript& redeemScript);

/** OP_0 <quorumSigs…> <ownerSig> <vaultScript>. Signatures carry their hashtype byte. */
CScript BuildVaultScriptSig(const std::vector<valtype>& quorumSigs, const valtype& ownerSig, const CScript& vaultScript);

/**
 * Split a vault scriptSig into its pushes: leading OP_0, then zero or more
 * quorum signatures, the owner signature and the vault script. The owner
 * signature is the second-to-last push, the script the last. A scriptSig with
 * only OP_0 and the script (no owner signature yet) is accepted with an empty
 * ownerSig.
 */
bool ParseVaultScriptSig(const CScript& scriptSig, std::vector<valtype>& quorumSigs, valtype& ownerSig, CScript& vaultScript);

/** The last push of a P2SH scriptSig, i.e. the redeem script. False if the scriptSig is not push-only or empty. */
bool ExtractRedeemScript(const CScript& scriptSig, CScript& redeemScript);

/** Serialised size of the vault script for n keys and a lockHeight push of `heightPushBytes` bytes (D3 arithmetic). */
size_t VaultScriptSize(unsigned int n, size_t heightPushBytes);

} // namespace yellowback

#endif // YCASH_YELLOWBACK_SCRIPT_H
