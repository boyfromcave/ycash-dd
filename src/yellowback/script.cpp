// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "yellowback/script.h"

#include <algorithm>

namespace yellowback {

namespace {

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

// ---------------------------------------------------------------- shared helpers

bool IsCompressedKey(const CPubKey& key)
{
    return key.IsValid() && key.IsCompressed();
}

CScript P2SHScript(const CScript& redeemScript)
{
    return GetScriptForDestination(CScriptID(redeemScript));
}

bool ExtractRedeemScript(const CScript& scriptSig, CScript& redeemScript)
{
    std::vector<valtype> pushes;
    if (!GetPushes(scriptSig, pushes) || pushes.empty()) return false;
    redeemScript = CScript(pushes.back().begin(), pushes.back().end());
    return true;
}

} // namespace yellowback
