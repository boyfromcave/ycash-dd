// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef YCASH_YELLOWBACK_TXBUILDER_H
#define YCASH_YELLOWBACK_TXBUILDER_H

#include "amount.h"
#include "coins.h"
#include "key.h"
#include "keystore.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "transaction_builder.h"
#include "yellowback/script.h"
#include "yellowback/state.h"
#include "yellowback/wallet.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CReserveKey;

/**
 * Yellowback v2 transaction builder (plan §3.5, §4.2a, §4.6). Built by hand
 * from public CWallet primitives, as z_sendmany and the atomic-swap RPCs do:
 * CreateNewContextualCMutableTransaction for version/group/expiry, a flat
 * network fee (YELLOWBACK_FEE), smallest-first confirmed inputs, SignSignature
 * for P2PKH inputs and a manual ZIP-243 sighash for the vault input
 * (SignVaultSpend). Every Build* requires cs_main, cs_wallet and
 * cs_yellowback held by the caller, in that order. Failures throw
 * std::runtime_error whose message begins with a stable identifier of
 * doc/yellowback-rpc.md (`insufficient-yed`, `change-floor`, `vault-locked`,
 * …); the RPC layer maps the identifier to an RPC error code.
 *
 * Two layers: the pure shape functions (MintOutputs, PlanVaultSpend,
 * SignVaultSpend) take explicit inputs and are unit-tested without a wallet
 * (yellowback_txbuilder_tests.cpp); the Build* functions select coins,
 * draw keys and choose the payee from the wallet and the index.
 *
 * Sapling shapes (§4.6): a mint funded from a ys1… address and a vault
 * spend paying its collateral to one are assembled in Ycash's
 * TransactionBuilder (proofs, binding signature) and returned unbuilt in
 * BuiltTx::builder. The caller runs FinishSapling() with NO lock held (one
 * spend proof per note, seconds each), then re-locks: a mint is complete, a
 * vault spend still needs SignVaultSpend(). ZIP-243 never covers a
 * scriptSig, so signing the transparent inputs after Build() keeps the
 * binding signature valid (the transaction_builder extension, mapping §13.5).
 *
 * DigiByte's builder (ref/digibyte/src/digidollar/txbuilder.cpp) assembles
 * Taproot vaults and signs BIP341 sighashes; here the vault is the P2SH
 * OP_IF script of §3.4 and the owner signature is ZIP-243 over the vault
 * script, the vault's nValue and the epoch branch id (mapping §13.1).
 */
namespace yellowback {

/** What a BuiltTx is (the RPC that made it; the wallet's own view). */
enum class BuiltKind { MINT, TRANSFER, REDEEM, RELEASE, CLAIM, SWEEP };

struct BuiltTx
{
    CMutableTransaction tx;
    BuiltKind kind;
    std::vector<COutPoint> ownYedOutputs;   //!< outputs to lock before CommitTransaction (stage i)
    std::set<COutPoint> yedInputs;          //!< YED outpoints consumed
    CPubKey freshKey;                       //!< the key drawn for this transaction (owner / token / collateral / change)
    std::string warning;                    //!< MINT: the keypool-low nag ("" if none)
    // Sapling shape: set by BuildMint/BuildRedeem/BuildClaim/BuildSweep, consumed by FinishSapling
    std::optional<TransactionBuilder> builder;
    std::string fundedFrom;                 //!< MINT: "transparent" | "sapling"
    std::string collateralTo;               //!< vault spends: the destination address as given or drawn

    // Common (§4.2a: refHeight, feeZat, payee, termClass, claimHeight, path)
    int refHeight;                          //!< R = indexTip - REF_LAG
    CAmount feeZat;                         //!< the enforcement fee paid (0 under FEE-0, for a release and a sweep)
    std::optional<CKeyID> payee;            //!< the fee output's key hash (nullopt = no fee output)
    int feeVout;                            //!< index of the fee output, -1 if none
    int termClass;                          //!< 0/1/2 = A/B/C
    uint32_t lockHeight;
    uint32_t claimHeight;
    std::string path;                       //!< "owner" | "claim" | "" (vault spends only)

    // MINT
    CAmount collateralZat;                  //!< vout[0].nValue of the mint

    // Vault spends (REDEEM / RELEASE / CLAIM / SWEEP)
    CAmount collateralOut;                  //!< zat paid to the destination
    int64_t burnCents;                      //!< YED burned (the debt for REDEEM/CLAIM, 0 otherwise)
    int64_t changeCents;                    //!< YED change (TRANSFER / REDEEM / CLAIM)
    int64_t extraBurnCents;                 //!< H4: sub-dollar remainder burned on top of the debt (REDEEM / CLAIM), else 0
    int changeVout;                         //!< index of the YED change output, -1 if none
    CScript vaultScript;                    //!< signing material for SignVaultSpend
    CAmount vaultValue;
    CPubKey ownerPubKey;
    std::vector<std::pair<CScript, CAmount>> yedPrevs;     //!< scriptPubKey/value of vin[1..] (YED inputs)

    BuiltTx() : kind(BuiltKind::TRANSFER), refHeight(0), feeZat(0), feeVout(-1), termClass(0), lockHeight(0), claimHeight(0),
                collateralZat(0), collateralOut(0), burnCents(0), changeCents(0), extraBurnCents(0), changeVout(-1), vaultValue(0) {}
    bool NeedsProving() const { return builder.has_value(); }
    bool IsVaultSpend() const { return kind == BuiltKind::REDEEM || kind == BuiltKind::RELEASE || kind == BuiltKind::CLAIM || kind == BuiltKind::SWEEP; }
};

// ---------------------------------------------------------------- pure shapes (§3.5; no wallet, no chain)

/** The MINT's fixed inputs. */
struct MintShape
{
    Cents cents;
    int termClass;
    uint32_t lockHeight;
    uint32_t claimHeight;                   //!< lockHeight + GRACE
    int refHeight;
    CPubKey owner;                          //!< vault ownerPubKey and the token output's key
    CAmount collateralZat;                  //!< vout[0].nValue (>= requiredZat, >= 4 * FEE_MIN, rounded)
    std::optional<CKeyID> payee;            //!< nullopt under FEE-0: no fee output, feeVout = 0xFF
    CAmount feeZat;                         //!< FeeZat(collateralZat)

    MintShape() : cents(0), termClass(0), lockHeight(0), claimHeight(0), refHeight(0), collateralZat(0), feeZat(0) {}
};

/**
 * vout[0] P2SH(vaultScript), vout[1] P2PKH(owner) TOKEN_VALUE, vout[2] OP_RETURN MINT
 * payload (feeVout = 3 or 0xFF), vout[3] P2PKH(payee) feeZat when there is a payee.
 * `feeVout` receives 3 or -1. Throws on an unencodable shape.
 */
std::vector<CTxOut> MintOutputs(const MintShape& shape, int& feeVout);

/** A vault spend's fixed inputs (REDEEM, CLAIM, the VOID release and the SWEEP). */
struct VaultSpendShape
{
    COutPoint vaultOut;
    CScript vaultScript;
    CAmount vaultValue;
    uint32_t lockHeight;
    uint32_t claimHeight;
    bool ownerPath;                         //!< owner scriptSig + nLockTime = lockHeight; else claim scriptSig + claimHeight
    bool withPayload;                       //!< REDEEM/CLAIM: burn + payload + fee; false = release/sweep (no burn, no fee, no payload)
    int refHeight;                          //!< REDEEM payload refHeight
    std::optional<CKeyID> payee;            //!< nullopt under FEE-0 (and always for a release/sweep)
    CAmount feeZat;
    std::vector<YedCoin> yedInputs;         //!< vin[1..]
    Cents changeCents;                      //!< 0 = no YED change output
    CScript changeScript;                   //!< the YED change P2PKH when changeCents > 0
    std::optional<CScript> collateralScript;//!< nullopt = a Sapling destination (the caller adds the note)
    CAmount networkFee;                     //!< YELLOWBACK_FEE

    VaultSpendShape() : vaultValue(0), lockHeight(0), claimHeight(0), ownerPath(true), withPayload(true), refHeight(0), feeZat(0),
                        changeCents(0), networkFee(0) {}
};

/** The transparent half of a vault spend, applied to a CMutableTransaction or a TransactionBuilder. */
struct VaultSpendPlan
{
    std::vector<CTxIn> vin;                 //!< vin[0] the vault (nSequence 0xFFFFFFFE), then the YED inputs
    std::vector<CTxOut> vout;               //!< transparent outputs in §3.5 order (without the Sapling collateral note)
    CAmount collateralOut;                  //!< to the destination (transparent vout[0] or the Sapling note)
    uint32_t nLockTime;
    int feeVout;                            //!< -1 if none
    int changeVout;                         //!< -1 if none
    int64_t burnCents;                      //!< yedIn - changeCents

    VaultSpendPlan() : collateralOut(0), nLockTime(0), feeVout(-1), changeVout(-1), burnCents(0) {}
};

/**
 * Transparent destination: vout[0] collateral, [fee], [YED change], [payload].
 * Sapling destination: [YED change] at vout[0], [payload], [fee] — the fee output is always
 * transparent and feeVout names it wherever it lands (M13). Release/sweep: one collateral
 * output (transparent) or none (Sapling). collateralOut = vaultValue + Σ yed nValue - networkFee -
 * feeZat - TOKEN_VALUE × change outputs; throws `vault-value-too-small` if that is not positive.
 */
VaultSpendPlan PlanVaultSpend(const VaultSpendShape& shape);

/**
 * Owner path: sign vin[0] with the vault owner's key over the vault script, the vault's nValue and
 * `branchId` (ZIP-243 binds both, mapping §13.1) and set `<sig> OP_1 <script>`; claim path: set
 * `OP_0 <script>`. Then SignSignature every YED input (out.yedPrevs) with the same branch id. The
 * keystore must hold the owner key (owner path) and the YED keys. Sapling shape: FinishSapling()
 * first. Fills out.ownYedOutputs with the change output. A CWallet is a CKeyStore.
 */
void SignVaultSpend(BuiltTx& out, const CKeyStore& keystore, uint32_t branchId, bool ownerPath);

// ---------------------------------------------------------------- wallet builders (cs_main, cs_wallet, cs_yellowback)

/**
 * The §3.5 MINT of `cents` YED locked for `lockBlocks` (the class follows, V19) at R = indexTip -
 * REF_LAG, after the MINTPOL-1 gate. `from`: "" = any confirmed transparent output; an s1…
 * address = that address's outputs only; a ys1… address = its Sapling notes (Sapling shape).
 */
BuiltTx BuildMint(YellowbackWallet& yw, Cents cents, int lockBlocks, CReserveKey& reservekey, const std::string& from = "");

/** recipients: P2PKH script -> cents. At most 14 recipients (one assignment slot is kept for change). */
BuiltTx BuildTransfer(YellowbackWallet& yw, const std::vector<std::pair<CScript, int64_t>>& recipients, CReserveKey& reservekey);

/**
 * ACTIVE vault: the owner-path REDEEM (burn = the debt, fee output, payload). VOID vault: the
 * §3.5 VOID RELEASE (owner path, no burn, no fee, no payload; L14). `to`: "" = a fresh
 * transparent key; an s1… address; a ys1… address (Sapling shape). Returned UNSIGNED: call
 * FinishSapling() first for the Sapling shape (no lock held), then SignVaultSpend(…, true).
 */
BuiltTx BuildRedeem(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to = "");

/** The claim-path spend of somebody's underwater ACTIVE vault from this wallet's YED. Unsigned; SignVaultSpend(…, false). */
BuiltTx BuildClaim(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to = "");

/** The §3.5 SWEEP of an own ACTIVE vault (L10): owner path, no burn, no fee, no payload. The caller checks IsAbandoned(). Unsigned. */
BuiltTx BuildSweep(YellowbackWallet& yw, const uint256& vaultTxid, const std::string& to = "");

/** Sapling shape: run TransactionBuilder::Build() (proofs, binding signature). No lock may be held. */
void FinishSapling(BuiltTx& out);

/**
 * SignerBranchId() and VerifyAllInputs() stay in policy.h (libbitcoin_server): the node-context
 * yed_validaterawtransaction uses them and cannot link the wallet library (mapping §13.5).
 */

/** The FEE-W selector of a vault spend: the 36-byte serialised vault outpoint (§3.7). */
std::vector<unsigned char> OutPointSelector(const COutPoint& out);

/**
 * The dry run of yed_send / yed_sendmany (H3, yed_estimatesend): the same floor-aware selection
 * over the same coins, with nothing signed, locked or committed. `recipients` only shapes the
 * argument checks (at most MAX_ASSIGNMENTS - 1); the selection depends on the total alone.
 * Requires cs_main, cs_wallet and cs_yellowback, like every Build*.
 */
struct SendEstimate
{
    int64_t amountCents;
    size_t recipients;
    bool workable;
    std::string stage;                  //!< "exact" | "single" | "greedy" | "search" | "none"
    std::vector<YedCoin> inputs;        //!< empty when !workable
    int64_t selectedCents;
    int64_t changeCents;
    int64_t spendableCents;             //!< the wallet's confirmed, unspent YED
    std::string error;                  //!< "" | "insufficient-yed" | "change-floor" | "too-many-inputs"
    std::optional<int64_t> below;       //!< H2 alternatives, only when !workable
    std::optional<int64_t> above;

    SendEstimate() : amountCents(0), recipients(0), workable(false), stage("none"), selectedCents(0),
                     changeCents(0), spendableCents(0) {}
};

SendEstimate EstimateTransfer(YellowbackWallet& yw, int64_t amountCents, size_t recipients);

/** The largest YED input count the builders accept (H11: 250 P2PKH inputs ≈ 37 kB). */
static const size_t MAX_YED_INPUTS = 250;

} // namespace yellowback

#endif // YCASH_YELLOWBACK_TXBUILDER_H
