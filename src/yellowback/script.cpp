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

} // namespace

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

    // <lockHeight>: a minimal number push (OP_1..OP_16 or a CScriptNum of at most 5 bytes)
    if (!script.GetOp(pc, opcode, data)) return false;
    int64_t height;
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
    if (height <= 0 || height >= LOCKTIME_THRESHOLD) return false;

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
