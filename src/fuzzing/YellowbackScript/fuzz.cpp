// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// Fuzz target for the Yellowback script parsers (plan C14, §6 Phase 6):
// ParseRosterScript, ParseVaultScript, ParseVaultScriptSig and
// ExtractRedeemScript over arbitrary bytes. They must never throw.

#include "yellowback/script.h"

#include "script/script.h"

#include <cstdint>
#include <cstdio>
#include <vector>

int fuzz_YellowbackScript(const std::vector<unsigned char>& data)
{
    CScript script(data.begin(), data.end());
    yellowback::Roster roster;
    if (yellowback::ParseRosterScript(script, roster)) {
        if (yellowback::RosterScript(roster.k, roster.keys) != script) return -2;
    }
    uint32_t lock;
    CPubKey owner;
    yellowback::Roster r2;
    if (yellowback::ParseVaultScript(script, lock, owner, r2)) {
        if (yellowback::VaultScript(lock, owner, r2) != script) return -3;
    }
    std::vector<yellowback::valtype> sigs;
    yellowback::valtype ownerSig;
    CScript vault;
    yellowback::ParseVaultScriptSig(script, sigs, ownerSig, vault);
    CScript redeem;
    yellowback::ExtractRedeemScript(script, redeem);
    return 0;
}

#ifdef FUZZ_WITH_AFL

int main(int argc, char* argv[])
{
    FILE* f = fopen(argv[1], "rb");
    if (!f) return -1;
    std::vector<unsigned char> data;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    return fuzz_YellowbackScript(data);
}

#endif // FUZZ_WITH_AFL
#ifdef FUZZ_WITH_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size)
{
    fuzz_YellowbackScript(std::vector<unsigned char>(Data, Data + Size));
    return 0;
}

#endif
