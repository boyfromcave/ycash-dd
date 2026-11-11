// Copyright (c) 2019 The Zcash developers
// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "asyncrpcoperation_common.h"

#include "core_io.h"
#include "init.h"
#include "rpc/protocol.h"
#include "chainparams.h"
#include "consensus/upgrades.h"
#include "crypto/pq/scheme.h"
#include "main.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "rpc/server.h"
#include "script/sign.h"

extern UniValue signrawtransaction(const UniValue& params, bool fHelp);

UniValue SendTransaction(CTransaction& tx, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode) {
    UniValue o(UniValue::VOBJ);
    // Send the transaction
    if (!testmode) {
        CWalletTx wtx(pwalletMain, tx);
        if (!pwalletMain->CommitTransaction(wtx, reservekey)) {
            // More details in debug.log
            throw JSONRPCError(RPC_WALLET_ERROR, "SendTransaction: CommitTransaction failed");
        }
        o.pushKV("txid", tx.GetHash().ToString());
    } else {
        // Test mode does not send the transaction to the network.
        o.pushKV("test", 1);
        o.pushKV("txid", tx.GetHash().ToString());
        o.pushKV("hex", EncodeHexTx(tx));
    }
    return o;
}

std::pair<CTransaction, UniValue> SignSendRawTransaction(UniValue obj, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode) {
    // Sign the raw transaction
    UniValue rawtxnValue = find_value(obj, "rawtxn");
    if (rawtxnValue.isNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing hex data for raw transaction");
    }
    std::string rawtxn = rawtxnValue.get_str();

    UniValue params = UniValue(UniValue::VARR);
    params.push_back(rawtxn);
    UniValue signResultValue = signrawtransaction(params, false);
    UniValue signResultObject = signResultValue.get_obj();
    UniValue completeValue = find_value(signResultObject, "complete");
    bool complete = completeValue.get_bool();
    if (!complete) {
        // TODO: #1366 Maybe get "errors" and print array vErrors into a string
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Failed to sign transaction");
    }

    UniValue hexValue = find_value(signResultObject, "hex");
    if (hexValue.isNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing hex data for signed transaction");
    }
    std::string signedtxn = hexValue.get_str();
    CDataStream stream(ParseHex(signedtxn), SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;
    stream >> tx;

    UniValue sendResult = SendTransaction(tx, reservekey, testmode);

    return std::make_pair(tx, sendResult);
}

CTransaction PQFeeShape(int nHeight, const std::vector<std::pair<CScript, CAmount>>& tIns,
                        const std::vector<CTxOut>& tOuts, size_t saplingSpends, size_t saplingOutputs)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(pwalletMain->cs_wallet);
    const Consensus::Params& consensus = Params().GetConsensus();
    CMutableTransaction mtx = CreateNewContextualCMutableTransaction(consensus, nHeight);
    const uint32_t branchId = CurrentEpochBranchId(nHeight, consensus);
    const unsigned int flags = STANDARD_SCRIPT_VERIFY_FLAGS | GetVaultScriptFlags(nHeight, consensus);
    mtx.vin.resize(tIns.size());
    for (size_t i = 0; i < tIns.size(); i++) {
        SignatureData sigdata;
        if (!ProduceSignature(DummySignatureCreator(pwalletMain), tIns[i].first, sigdata, branchId, flags))
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot size a transparent input of this transaction");
        UpdateTransaction(mtx, i, sigdata);
    }
    mtx.vout = tOuts;
    mtx.vShieldedSpend.resize(saplingSpends);
    mtx.vShieldedOutput.resize(saplingOutputs);
    return CTransaction(mtx);
}

CAmount PQBuilderFee(CAmount base, const CTransaction& tx)
{
    const size_t nBytes = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
    return std::max(std::min(std::max(base, PQSizeFee(DEFAULT_FEE, nBytes)), PQMaxTxFee()),
                    Zip317ConventionalFee(tx));
}

unsigned int PolicySigOps(const CTransaction& shape, const std::vector<CScript>& prevScripts)
{
    assert(prevScripts.size() == shape.vin.size());
    unsigned int n = GetLegacySigOpCount(shape);
    for (size_t i = 0; i < shape.vin.size(); i++) {
        if (prevScripts[i].IsPayToScriptHash())
            n += prevScripts[i].GetSigOpCount(shape.vin[i].scriptSig);
        n += GetPQSigOpCount(prevScripts[i]);
    }
    return n;
}

void EnsurePQShapeStandard(const CTransaction& shape, const std::vector<CScript>& prevScripts, size_t nPQInputs)
{
    if (nPQInputs > MAX_PQ_INPUTS_PER_TX) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "This send needs %d post-quantum inputs, more than the %d one transaction may carry; "
            "combine the address's coins first with z_mergetoaddress", nPQInputs, MAX_PQ_INPUTS_PER_TX));
    }
    const unsigned int sigops = PolicySigOps(shape, prevScripts);
    if (sigops > MAX_STANDARD_TX_SIGOPS) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "This transaction needs %u signature operations, more than the %u a standard transaction allows; "
            "spend fewer inputs", sigops, MAX_STANDARD_TX_SIGOPS));
    }
}

static void CheckPQInputsStillValid(const std::vector<PQBuilderInput>& ins, uint32_t branchId, unsigned int vaultFlags)
{
    LOCK2(cs_main, pwalletMain->cs_wallet);
    EnsureWalletIsUnlocked();
    const Consensus::Params& consensus = Params().GetConsensus();
    const int next = chainActive.Height() + 1;
    if (CurrentEpochBranchId(next, consensus) != branchId || GetVaultScriptFlags(next, consensus) != vaultFlags) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            "The next block's rules changed while this transaction was built (a reorg or a network upgrade); send it again");
    }
    for (const PQBuilderInput& in : ins) {
        if (pwalletMain->IsSpent(in.outpoint.hash, in.outpoint.n)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "A post-quantum input was spent while this transaction was built; send it again");
        }
    }
}

void SignPQBuilderInputs(CMutableTransaction& mtx, const std::vector<PQBuilderInput>& ins, int nHeight)
{
    if (ins.empty()) return;
    AssertLockNotHeld(cs_main);
    AssertLockNotHeld(pwalletMain->cs_wallet);
    const Consensus::Params& consensus = Params().GetConsensus();
    const uint32_t branchId = CurrentEpochBranchId(nHeight, consensus);
    const unsigned int vaultFlags = GetVaultScriptFlags(nHeight, consensus);
    const unsigned int flags = STANDARD_SCRIPT_VERIFY_FLAGS | vaultFlags;

    CheckPQInputsStillValid(ins, branchId, vaultFlags);
    // No global lock from here: the key comes from the keystore under cs_KeyStore, the signature is made and
    // verified (ProduceSignature) with nothing else held.
    const CTransaction txConst(mtx);
    for (const PQBuilderInput& in : ins) {
        if (in.nIn >= mtx.vin.size() || mtx.vin[in.nIn].prevout != in.outpoint || !IsPQInputScript(in.scriptPubKey))
            throw JSONRPCError(RPC_WALLET_ERROR, "Post-quantum input does not match the built transaction");
        SignatureData sigdata;
        if (!ProduceSignature(TransactionSignatureCreator(pwalletMain, &txConst, in.nIn, in.value, SIGHASH_ALL),
                              in.scriptPubKey, sigdata, branchId, flags))
            throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign a post-quantum input");
        UpdateTransaction(mtx, in.nIn, sigdata);
    }
    CheckPQInputsStillValid(ins, branchId, vaultFlags);
}
