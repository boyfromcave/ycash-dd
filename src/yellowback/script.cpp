// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/script.h"

#include <algorithm>

namespace yellowback {

namespace {

bool IsSmallInt(opcodetype op)
{
    return op == OP_0 || (op >= OP_1 && op <= OP_16);
}

/** Read the pushes of a push-only script. False on any non-push opcode. */
bool GetPushes(const CScript& script, std::vector<valtype>& pushes, std::vector<opcodetype>* opcodes = nullptr)
{
    pushes.clear();
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    valtype data;
    while (pc < script.end()) {
        if (!script.GetOp(pc, opcode, data)) return false;
        if (opcode > OP_16) return false;
        pushes.push_back(data);
        if (opcodes) opcodes->push_back(opcode);
    }
    return true;
}

/** Parse "<k> <keys…> <n> OP_CHECKMULTISIG" starting at pc; pc must end at script end. */
bool ParseMultisigTail(const CScript& script, CScript::const_iterator pc, Roster& roster)
{
    opcodetype opcode;
    valtype data;
    Roster r;
    if (!script.GetOp(pc, opcode, data) || !IsSmallInt(opcode)) return false;
    r.k = CScript::DecodeOP_N(opcode);
    while (true) {
        if (!script.GetOp(pc, opcode, data)) return false;
        if (IsSmallInt(opcode)) break;
        if (data.size() != CPubKey::COMPRESSED_PUBLIC_KEY_SIZE) return false;
        CPubKey key(data);
        if (!key.IsValid() || !key.IsCompressed()) return false;
        r.keys.push_back(key);
    }
    unsigned int n = CScript::DecodeOP_N(opcode);
    if (n != r.keys.size()) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKMULTISIG) return false;
    if (pc != script.end()) return false;
    if (!r.IsValid()) return false;
    roster = r;
    return true;
}

/**
 * Read a minimally encoded height push at pc: OP_1..OP_16 or a CScriptNum of
 * at most 5 bytes (the interpreter's limit under MINIMALDATA). Range (1, LOCKTIME_THRESHOLD).
 */
bool ReadHeightPush(const CScript& script, CScript::const_iterator& pc, int64_t& height)
{
    opcodetype opcode;
    valtype data;
    if (!script.GetOp(pc, opcode, data)) return false;
    if (opcode >= OP_1 && opcode <= OP_16) {
        height = CScript::DecodeOP_N(opcode);
    } else if (opcode <= OP_PUSHDATA4 && !data.empty() && data.size() <= 5) {
        try {
            height = CScriptNum(data, true, 5).getint();
        } catch (const scriptnum_error&) {
            return false;
        }
    } else {
        return false;
    }
    return height > 0 && height < LOCKTIME_THRESHOLD;
}

/** The interpreter's truth test (ref/ycash/src/script/interpreter.cpp:41): any non-zero byte, except negative zero. */
bool CastToBoolLikeInterpreter(const valtype& vch)
{
    for (size_t i = 0; i < vch.size(); i++) {
        if (vch[i] != 0) {
            if (i == vch.size() - 1 && vch[i] == 0x80) return false;
            return true;
        }
    }
    return false;
}

/** What EvalScript pushes for a push opcode: the data, or the small number for OP_1NEGATE / OP_1..OP_16 (interpreter.cpp:334-340). */
valtype StackElementFor(opcodetype opcode, const valtype& data)
{
    if (opcode == OP_1NEGATE || (opcode >= OP_1 && opcode <= OP_16)) {
        return CScriptNum((int)opcode - (int)(OP_1 - 1)).getvch();
    }
    return data;
}

} // namespace

// ---------------------------------------------------------------- v2 (§3.4)

CScript VaultScript(uint32_t lockHeight, const CPubKey& owner, uint32_t claimHeight)
{
    if (lockHeight == 0 || lockHeight >= LOCKTIME_THRESHOLD) return CScript();
    if (claimHeight <= lockHeight || claimHeight >= LOCKTIME_THRESHOLD) return CScript();
    if (!IsCompressedKey(owner)) return CScript();
    CScript script;
    // CScript << int64_t pushes a minimal CScriptNum (OP_1..OP_16 for 1..16).
    script << OP_IF;
    script << (int64_t)lockHeight << OP_CHECKLOCKTIMEVERIFY << OP_DROP;
    script << valtype(owner.begin(), owner.end()) << OP_CHECKSIG;
    script << OP_ELSE;
    script << (int64_t)claimHeight << OP_CHECKLOCKTIMEVERIFY << OP_DROP << OP_TRUE;
    script << OP_ENDIF;
    return script;
}

bool ParseVaultScript(const CScript& script, uint32_t& lockHeight, CPubKey& owner, uint32_t& claimHeight)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    valtype data;
    int64_t lock, claim;

    if (!script.GetOp(pc, opcode, data) || opcode != OP_IF) return false;
    if (!ReadHeightPush(script, pc, lock)) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKLOCKTIMEVERIFY) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return false;
    if (!script.GetOp(pc, opcode, data) || data.size() != CPubKey::COMPRESSED_PUBLIC_KEY_SIZE) return false;
    CPubKey key(data);
    if (!IsCompressedKey(key)) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSIG) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_ELSE) return false;
    if (!ReadHeightPush(script, pc, claim)) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKLOCKTIMEVERIFY) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_TRUE) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_ENDIF) return false;
    if (pc != script.end()) return false;
    if (claim <= lock) return false;

    lockHeight = (uint32_t)lock;
    owner = key;
    claimHeight = (uint32_t)claim;
    return true;
}

CScript OwnerScriptSig(const valtype& ownerSig, const CScript& vaultScript)
{
    return CScript() << ownerSig << OP_1 << valtype(vaultScript.begin(), vaultScript.end());
}

CScript ClaimScriptSig(const CScript& vaultScript)
{
    return CScript() << OP_0 << valtype(vaultScript.begin(), vaultScript.end());
}

std::optional<VaultSpendPath> ParseVaultSpendPath(const CScript& scriptSig)
{
    std::vector<valtype> pushes;
    std::vector<opcodetype> opcodes;
    if (!GetPushes(scriptSig, pushes, &opcodes)) return std::nullopt;
    if (pushes.size() < 2) return std::nullopt;
    VaultSpendPath path;
    path.pushes = pushes.size();
    path.vaultScript = CScript(pushes.back().begin(), pushes.back().end());
    const size_t sel = pushes.size() - 2;
    path.selector = StackElementFor(opcodes[sel], pushes[sel]);
    path.ownerPath = CastToBoolLikeInterpreter(path.selector);
    if (pushes.size() >= 3) path.ownerSig = StackElementFor(opcodes[sel - 1], pushes[sel - 1]);
    return path;
}

// ---------------------------------------------------------------- v1, retained

bool Roster::IsValid() const
{
    if (k < 1 || keys.size() < k || keys.size() > ROSTER_MAX_N) return false;
    for (const CPubKey& key : keys) {
        if (!IsCompressedKey(key)) return false;
    }
    return true;
}

bool IsCompressedKey(const CPubKey& key)
{
    return key.IsValid() && key.IsCompressed();
}

std::vector<CPubKey> SortKeys(std::vector<CPubKey> keys)
{
    std::sort(keys.begin(), keys.end(), [](const CPubKey& a, const CPubKey& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    });
    return keys;
}

CScript RosterScript(unsigned int k, const std::vector<CPubKey>& keys)
{
    Roster r;
    r.k = k;
    r.keys = keys;
    if (!r.IsValid()) return CScript();
    CScript script;
    script << CScript::EncodeOP_N(k);
    for (const CPubKey& key : keys) {
        script << valtype(key.begin(), key.end());
    }
    script << CScript::EncodeOP_N(keys.size()) << OP_CHECKMULTISIG;
    return script;
}

bool ParseRosterScript(const CScript& script, Roster& roster)
{
    return ParseMultisigTail(script, script.begin(), roster);
}

CScript VaultScript(uint32_t lockHeight, const CPubKey& owner, const Roster& roster)
{
    if (lockHeight == 0 || lockHeight >= LOCKTIME_THRESHOLD) return CScript();
    if (!IsCompressedKey(owner) || !roster.IsValid()) return CScript();
    CScript script;
    // CScript << int64_t pushes a minimal CScriptNum (OP_1..OP_16 for 1..16).
    script << (int64_t)lockHeight << OP_CHECKLOCKTIMEVERIFY << OP_DROP;
    script << valtype(owner.begin(), owner.end()) << OP_CHECKSIGVERIFY;
    CScript tail = RosterScript(roster.k, roster.keys);
    if (tail.empty()) return CScript();
    script += tail;
    return script;
}

CScript VaultScript(uint32_t lockHeight, const CPubKey& owner, const CScript& rosterScript)
{
    Roster r;
    if (!ParseRosterScript(rosterScript, r)) return CScript();
    return VaultScript(lockHeight, owner, r);
}

bool ParseVaultScript(const CScript& script, uint32_t& lockHeight, CPubKey& owner, Roster& roster)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    valtype data;

    int64_t height;
    if (!ReadHeightPush(script, pc, height)) return false;

    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKLOCKTIMEVERIFY) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_DROP) return false;

    if (!script.GetOp(pc, opcode, data) || data.size() != CPubKey::COMPRESSED_PUBLIC_KEY_SIZE) return false;
    CPubKey key(data);
    if (!IsCompressedKey(key)) return false;
    if (!script.GetOp(pc, opcode, data) || opcode != OP_CHECKSIGVERIFY) return false;

    Roster r;
    if (!ParseMultisigTail(script, pc, r)) return false;

    lockHeight = (uint32_t)height;
    owner = key;
    roster = r;
    return true;
}

CScript P2SHScript(const CScript& redeemScript)
{
    return GetScriptForDestination(CScriptID(redeemScript));
}

CScript BuildVaultScriptSig(const std::vector<valtype>& quorumSigs, const valtype& ownerSig, const CScript& vaultScript)
{
    CScript sig;
    sig << OP_0; // CHECKMULTISIG dummy (NULLDUMMY)
    for (const valtype& s : quorumSigs) sig << s;
    if (!ownerSig.empty()) sig << ownerSig;
    sig << valtype(vaultScript.begin(), vaultScript.end());
    return sig;
}

bool ParseVaultScriptSig(const CScript& scriptSig, std::vector<valtype>& quorumSigs, valtype& ownerSig, CScript& vaultScript)
{
    std::vector<valtype> pushes;
    std::vector<opcodetype> opcodes;
    if (!GetPushes(scriptSig, pushes, &opcodes)) return false;
    if (pushes.size() < 2) return false;
    if (opcodes[0] != OP_0) return false;
    vaultScript = CScript(pushes.back().begin(), pushes.back().end());
    quorumSigs.clear();
    ownerSig.clear();
    if (pushes.size() == 2) return true; // OP_0 <script>: nothing signed yet
    ownerSig = pushes[pushes.size() - 2];
    for (size_t i = 1; i + 2 < pushes.size(); i++) {
        if (pushes[i].empty()) return false;
        quorumSigs.push_back(pushes[i]);
    }
    return true;
}

bool ExtractRedeemScript(const CScript& scriptSig, CScript& redeemScript)
{
    std::vector<valtype> pushes;
    if (!GetPushes(scriptSig, pushes) || pushes.empty()) return false;
    redeemScript = CScript(pushes.back().begin(), pushes.back().end());
    return true;
}

size_t VaultScriptSize(unsigned int n, size_t heightPushBytes)
{
    // <push op + height bytes> CLTV DROP <33-byte key push (34)> CHECKSIGVERIFY <k> n*(34) <n> CHECKMULTISIG
    return (1 + heightPushBytes) + 1 + 1 + 34 + 1 + 1 + 34 * (size_t)n + 1 + 1;
}

} // namespace yellowback
