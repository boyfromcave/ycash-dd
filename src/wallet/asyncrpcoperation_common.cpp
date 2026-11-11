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
#include "utilmoneystr.h"

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

std::string DropYellowbackCoins(std::vector<COutput>& coins, bool strict)
{
    std::vector<COutPoint> candidates;
    for (const COutput& c : coins) candidates.emplace_back(c.tx->GetHash(), c.i);
    std::vector<COutPoint> refused;
    const std::string refusal = YellowbackInputRefusal(candidates, strict, &refused);
    if (refusal.empty()) return "";
    if (refused.empty()) throw JSONRPCError(RPC_WALLET_ERROR, refusal);     // the unhealthy-index refusal
    coins.erase(std::remove_if(coins.begin(), coins.end(), [&](const COutput& c) {
        return std::find(refused.begin(), refused.end(), COutPoint(c.tx->GetHash(), c.i)) != refused.end();
    }), coins.end());
    return refusal;
}

CAmount PQInputMarginalFee(uint8_t scheme)
{
    const size_t n = PQSpendInputSize(scheme);
    return std::max(PlainPQFeeRate().GetFee(n), ZIP317_MARGINAL_FEE * (CAmount)((n + ZIP317_INPUT_SIZE - 1) / ZIP317_INPUT_SIZE));
}

void EnsurePQFeeWithinCap(CAmount fee)
{
    const CAmount cap = PQMaxTxFee();
    if (fee > cap) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
            "Fee %s is above the %s cap on a post-quantum spend (-maxtxfee, or $2 at the attested price, at most -yellowbackmaxtxfee)",
            FormatMoney(fee), FormatMoney(cap)));
    }
}

UniValue SendPQTransaction(CTransaction& tx, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode)
{
    if (testmode) return SendTransaction(tx, reservekey, testmode);
    LOCK2(cs_main, pwalletMain->cs_wallet);
    if (std::optional<CAmount> over = YedFeeOverCap(tx)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
            "The post-quantum fee cap fell to %s while this transaction was signed, below its fee %s; nothing was sent, send it again",
            FormatMoney(PQMaxTxFee()), FormatMoney(over.value())));
    }
    const uint256 txid = tx.GetHash();
    const bool known = pwalletMain->mapWallet.count(txid) != 0;
    try {
        return SendTransaction(tx, reservekey, testmode);
    } catch (...) {
        // CommitTransaction records the transaction before the mempool judges it; a refused one must not stay at
        // depth -1, where a restart's ReacceptWalletTransactions would submit it (by its own fee rule) after the
        // caller was told it failed and may have sent again.
        if (!known) {
            const std::string kept = EraseRefusedPQSend(*pwalletMain, txid);
            if (!kept.empty()) throw JSONRPCError(RPC_WALLET_ERROR, kept);
        }
        throw;
    }
}

std::string EraseRefusedPQSend(CWallet& wallet, const uint256& txid)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(wallet.cs_wallet);
    if (!wallet.mapWallet.count(txid)) return "";
    if (mempool.exists(txid)) {
        // Accepted after all (relay follows acceptance): never erase a transaction that may already be relayed.
        LogPrintf("SendPQTransaction: %s is in the mempool although its commit reported an error; kept\n", txid.ToString());
        return strprintf("Transaction %s is in the mempool although its commit reported an error; it is kept in the "
                         "wallet and will be relayed: do not send it again", txid.GetHex());
    }
    LogPrintf("SendPQTransaction: erasing %s, recorded by CommitTransaction and refused by the mempool\n", txid.ToString());
    if (wallet.fFileBacked && !CWalletDB(wallet.strWalletFile).EraseTx(txid)) {
        // Kept in mapWallet too, so the wallet shows what its file holds (and a restart would submit).
        LogPrintf("ERROR: SendPQTransaction: could not erase %s from the wallet file\n", txid.ToString());
        return strprintf("The mempool refused transaction %s, but it could not be erased from the wallet file: the "
                         "wallet still holds this unbroadcast transaction and may submit it at the next start; check "
                         "it with gettransaction before you send again", txid.GetHex());
    }
    wallet.mapWallet.erase(txid);   // mapTxSpends entries are ignored once it is gone (IsSpent looks it up)
    return "";
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
