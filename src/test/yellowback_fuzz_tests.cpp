// Copyright (c) 2026 The Ycash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

// Replays the seed corpora of src/fuzzing/YellowbackTag, YellowbackPayload
// and YellowbackScript, and every found-and-fixed crash under each target's
// crashes/ directory, through the same checks the fuzz targets make, so
// `make check` covers them without a fuzzing build (plan §7 "Fuzzing", N34).
// The bytes are embedded by src/test/gen_yellowback_corpus.py (--write) and
// compared with the input/ files by its --check; the tables between the
// markers are generated, never edited by hand.

#include "yellowback/payload.h"
#include "yellowback/script.h"
#include "yellowback/tag.h"

#include "primitives/transaction.h"
#include "test/test_bitcoin.h"
#include "utilstrencodings.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>

using namespace yellowback;

namespace {

// BEGIN GENERATED CORPUS (src/test/gen_yellowback_corpus.py; do not edit by hand)
static const std::vector<std::pair<std::string, std::string>> TAG_CORPUS = {
    {"after_height", "87d612000387d6122459454421010150c30000000000000700abababababababababababababababababababab"},
    {"after_extranonce", "87d612000387d61200112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"after_pool_text", "87d612000387d6122f5963617368506f6f6c2f2459454421010150c30000000000000700abababababababababababababababababababab0011223344556677"},
    {"magic_without_push_opcode", "87d612000387d61259454421010150c30000000000000700abababababababababababababababababababab"},
    {"first_occurrence_short_then_full", "87d612000387d612245945442101002459454421010150c30000000000000700abababababababababababababababababababab"},
    {"two_tags", "87d612000387d6122459454421010150c30000000000000700abababababababababababababababababababab2459454421010160ea0000000000000700abababababababababababababababababababab"},
    {"bad_version", "87d612000387d6122459454421000150c30000000000000700abababababababababababababababababababab"},
    {"reserved_flag_bit", "87d612000387d6122459454421010350c30000000000000700abababababababababababababababababababab"},
    {"price_below_min", "87d612000387d6122459454421010163000000000000000700abababababababababababababababababababab"},
    {"price_above_max", "87d612000387d6122459454421010101e1f505000000000700abababababababababababababababababababab"},
    {"signal_only", "87d612000387d6122459454421010100000000000000000700abababababababababababababababababababab"},
    {"wrong_height", "88d612000387d6122459454421010150c30000000000000700abababababababababababababababababababab"},
    {"no_prefix", "87d612002459454421010150c30000000000000700abababababababababababababababababababab"},
    {"truncated_by_one", "87d612000387d6122459454421010150c30000000000000700ababababababababababababababababababab"},
    {"pushdata1_tag", "87d612000387d6124c2459454421010150c30000000000000700abababababababababababababababababababab"},
    {"empty", "87d61200"},
    {"height_only", "87d612000387d612"},
    {"all_0x24", "87d612000387d612242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424"},
    {"height_1", "010000005100112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_16", "100000006000112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_17", "11000000011100112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_65535", "ffff000003ffff0000112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_16777215", "ffffff0004ffffff0000112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_16777216", "00000001040000000100112233445566772459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_0_genesis_shape", "00000000002459454421010150c30000000000000700abababababababababababababababababababab"},
    {"height_negative_bits", "ffffffff0387d6122459454421010150c30000000000000700abababababababababababababababababababab"},
    {"pool_internal_miner", "40420f000340420f53"},
    {"pool_internal_miner_tagged", "40420f000340420f2459454421010150c30000000000000700abababababababababababababababababababab53"},
    {"pool_internal_miner_big_nonce", "41420f000341420f03563412"},
    {"pool_internal_miner_big_nonce_tagged", "41420f000341420f2459454421010150c30000000000000700abababababababababababababababababababab03563412"},
    {"pool_raw_en1_en2", "e0c8100003e0c810deadbeef0000000000000001"},
    {"pool_raw_en1_en2_tagged", "e0c8100003e0c8102459454421010150c30000000000000700ababababababababababababababababababababdeadbeef0000000000000001"},
    {"pool_text_push_then_raw", "804f120003804f12192f5669614254432f4d696e656420627920796563757365722f0d273744bd4d47ed"},
    {"pool_text_push_then_raw_tagged", "804f120003804f122459454421010150c30000000000000700abababababababababababababababababababab192f5669614254432f4d696e656420627920796563757365722f0d273744bd4d47ed"},
    {"pool_ntime_en_text", "20d613000320d613803bb16afb2fa0bba4536d42092f326d696e6572732f"},
    {"pool_ntime_en_text_tagged", "20d613000320d6132459454421010150c30000000000000700abababababababababababababababababababab803bb16afb2fa0bba4536d42092f326d696e6572732f"},
    {"pool_slush_shape", "c05c150003c05c15813bb16afb67a67da4a3b20c2f736c7573682f"},
    {"pool_slush_shape_tagged", "c05c150003c05c152459454421010150c30000000000000700abababababababababababababababababababab813bb16afb67a67da4a3b20c2f736c7573682f"},
    {"pool_mph_shape", "60e316000360e3162f79636173682e6d696e696e67706f6f6c6875622e636f6d2ff698fadcadb20399"},
    {"pool_mph_shape_tagged", "60e316000360e3162459454421010150c30000000000000700abababababababababababababababababababab2f79636173682e6d696e696e67706f6f6c6875622e636f6d2ff698fadcadb20399"},
    {"pool_padding_then_en", "006a180003006a1800004b0e1b83113d28507d36c228"},
    {"pool_padding_then_en_tagged", "006a180003006a182459454421010150c30000000000000700abababababababababababababababababababab00004b0e1b83113d28507d36c228"},
    {"pool_max_100_bytes", "a0f0190003a0f019332f412076657279206c6f6e6720706f6f6c207461676c696e652070616464696e67206f75742074686520636f696e626173652fd43ba8ce9471471e4529622efa6332179b19ab5ba09ca7669223545785097ea31d6297424eca0403b09cbd8a"},
    {"pool_max_100_bytes_tagged", "a0f0190003a0f0192459454421010150c30000000000000700abababababababababababababababababababab332f412076657279206c6f6e6720706f6f6c207461676c696e652070616464696e67206f75742074686520636f696e626173652fd43ba8ce947147"},
    {"pool_magic_in_extranonce", "40771b000340771b06825e9e308291e759454421b82080c45e4a7fffb93fb059b32477c069cb8e8d"},
    {"pool_magic_in_extranonce_tagged", "40771b000340771b2459454421010150c30000000000000700abababababababababababababababababababab06825e9e308291e759454421b82080c45e4a7fffb93fb059b32477c069cb8e8d"},
    {"random1", "dfd71271b6b1acc8049ae38a51eb7e773ac1adbdf256ed2bbe4ef17f0f3710da48a7d5e67cd5aabd"},
    {"random2", "f4e54ab423c3fb1a7986233a4a6e01c24ab18d28951e2ccfd3d513e4a336edf5280efe1778993f75ae3c67163bd4cedf0d612e01681d81504513f5c7e7a6f6da5adffc87e0015bff24c19ab576cb67069573608c108934792edde70302262e312499939b1468cfbc"},
};
static const std::vector<std::pair<std::string, std::string>> TAG_CRASHES = {
};
static const std::vector<std::pair<std::string, std::string>> PAYLOAD_CORPUS = {
    {"mint", "594202010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e7003"},
    {"mint_feevout_none", "594202010240420f00ffffffff0000000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ff"},
    {"mint_key_prefix_03", "594202010010270000e8030000b603000003cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e7003"},
    {"transfer_empty", "5942020200"},
    {"transfer_one", "59420202010164000000"},
    {"transfer_15", "594202020f006400000001650000000266000000036700000004680000000569000000066a000000076b000000086c000000096d0000000a6e0000000b6f0000000c700000000d710000000e72000000"},
    {"redeem_empty", "59420203b6030000ff00"},
    {"redeem_one", "59420203b603000003010139300000"},
    {"redeem_14", "59420203ffffffff000e006400000001650000000266000000036700000004680000000569000000066a000000076b000000086c000000096d0000000a6e0000000b6f0000000c700000000d71000000"},
    {"bad_magic", "594402010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e7003"},
    {"version1_mint", "594201010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70"},
    {"version3", "594203010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e7003"},
    {"unknown_type_04", "5942020401000000"},
    {"retired_type_10", "5942021050c3000000000000"},
    {"reserved_type_20", "5942022000"},
    {"short", "594202"},
    {"mint_short", "594202010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e"},
    {"mint_no_feevout", "594202010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70"},
    {"mint_trailing", "594202010010270000e8030000b603000002cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e700300"},
    {"mint_uncompressed_key", "594202010010270000e8030000b603000004000000000000000000000000000000000000000000000000000000000000000003"},
    {"transfer_dup", "594202020201010000000102000000"},
    {"transfer_zero", "59420202010100000000"},
    {"transfer_16", "594202021000010000000101000000020100000003010000000401000000050100000006010000000701000000080100000009010000000a010000000b010000000c010000000d010000000e010000000f01000000"},
    {"redeem_15", "5942020301000000ff0f00010000000101000000020100000003010000000401000000050100000006010000000701000000080100000009010000000a010000000b010000000c010000000d010000000e01000000"},
    {"redeem_short", "59420203b6030000ff0101393000"},
    {"long81", "594202020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
    {"random1", "80fabba98ea00abc5a99c8d1ab764db228098a808f1d41610caa237dc5861e419505ff8aa616ebd8"},
    {"random2", "391ef4c2fb24ed8a370d167422d4fc684098bdce175c451a0dffced764905ae148c9934310314d08b9b4b18b3a63cc424c3658dcc78e59fdfe136f6cab57c019eff6f892c44348eaffad559809d76e20"},
    {"empty", ""},
};
static const std::vector<std::pair<std::string, std::string>> PAYLOAD_CRASHES = {
};
static const std::vector<std::pair<std::string, std::string>> SCRIPT_CORPUS = {
    {"vault", "6303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"vault_4byte_heights", "630440548900b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac670440db8900b1755168"},
    {"vault_mixed_widths", "6303ffff7fb1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6704ff868000b1755168"},
    {"vault_opn_heights", "6351b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6752b1755168"},
    {"vault_2byte_heights", "630111b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac670112b1755168"},
    {"vault_threshold_minus_one", "6304fe64cd1db1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6704ff64cd1db1755168"},
    {"vault_nonminimal_lock", "6304c0d40100b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"vault_claim_le_lock", "6303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c0d401b1755168"},
    {"vault_time_locked", "63040065cd1db1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac67040165cd1db1755168"},
    {"vault_uncompressed_key", "6303c0d401b175410400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000ac6703c05b02b1755168"},
    {"vault_missing_endif", "6303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b17551"},
    {"vault_checksigverify", "6303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ad6703c05b02b1755168"},
    {"vault_trailing", "6303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b175516875"},
    {"scriptsig_owner", "48304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40410151336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_claim", "00336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_owner_op2", "48304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40410152336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_owner_nonminimal_1", "48304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f4041010101336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_claim_negzero", "0180336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_claim_with_sig", "48304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40410100336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_one_push", "336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_not_push_only", "51336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b175516875"},
    {"scriptsig_four_pushes", "48304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40410148304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40410151336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"scriptsig_pushdata1_script", "48304502210102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021022022232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404101514c336303c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ac6703c05b02b1755168"},
    {"pushdata2", "4d000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"},
    {"pushdata_truncated", "4c500102"},
    {"v1_vault", "03c0d401b1752102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70ad512102cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e7051ae"},
    {"p2pkh", "76a914000000000000000000000000000000000000000088ac"},
    {"random1", "976bebdd80f8c606bfd653c0a106a9f27f436a69c1090d38ea3f3aeffaa6ca0df860c3b6db4ee2bf5a6c2de0e61a735d8032f297fb87faec4ef06c7e"},
    {"random2", "b62040bc524813599b241bcc286ec58421927a92d73b1dc81410109a5aa287bf17dddd7fa390c0f559f24fdd094e8400e5a07c9edf8bdab70b603e15393afeb0006417e2a1a7f1f3ef296e7843705231533bd744afc71cda654afded1d80c9dba8cb18a6df47a8ca30ed481ae425cc06f7e94f415b036c4bc2434dcc97c487cf8c291c28eabc2e93b3bc6ac09c2348e624e60fc484a1a5dea9a9d43ac0d2cfe60c9b0fd535cad1f790d8340662f8d586a68325c941e54451de913c56c3215b8f0db8f03b806fba3715b3ebf512dde0d5fcee98e08fccb2e30509ff96b911ccf9ec3f501abb43ba6cb0f9a0b3b104939bcd9c68b46f5be02e28b7e4c9f82b16e2b528b216acb64a5e3b64c038149d9a7087d970218ba1c2c9272877e7e0548ffe79a0f88daa680b927aa20bc827eb447b867bd5c2c8a58587c35e971d030bc0e7c7845d9909651d74d53d7663aab1c737869692c3cdebcb4d0d32fe80dead723aec883a5d269e8cc6b03e682ae6605239fe1197a38b7b54b95a4e0fa15bd0087b2b9b6cf2e0894355e7261ce7e2675070"},
    {"empty", ""},
};
static const std::vector<std::pair<std::string, std::string>> SCRIPT_CRASHES = {
};
// END GENERATED CORPUS

typedef std::vector<std::pair<std::string, std::string>> Corpus;

/** The YellowbackTag target's body: LE32(nHeight) ‖ scriptSig. Returns whether a tag was found. */
bool ReplayTag(const std::string& name, const std::vector<unsigned char>& data)
{
    int nHeight = 1;
    size_t start = 0;
    if (data.size() >= 4) {
        nHeight = (int)(((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24)) & 0x7FFFFFFF);
        start = 4;
    }
    CScript scriptSig(data.begin() + start, data.end());
    std::optional<CoinbaseTag> tag = FindTag(scriptSig, nHeight);
    if (tag.has_value()) {
        CScript push = TagPush(tag.value());
        const CScript prefix = CScript() << nHeight;
        BOOST_CHECK_MESSAGE(std::search(scriptSig.begin() + prefix.size(), scriptSig.end(), push.begin(), push.end()) != scriptSig.end(), name + ": found tag is in the scriptSig");
        BOOST_CHECK_MESSAGE(IsValidTag(tag.value()), name + ": found tag is valid");
        BOOST_CHECK_EQUAL(EncodeTag(tag.value()).size(), TAG_SIZE);
    }
    for (int h : { 0, 1, 16, 17, 65535, 1234567 }) FindTag(CScript(data.begin(), data.end()), h);
    for (size_t n = 0; n < scriptSig.size(); n++) FindTag(CScript(scriptSig.begin(), scriptSig.begin() + n), nHeight);
    return tag.has_value();
}

/** The YellowbackPayload target's body. Returns whether the bytes decoded. */
bool ReplayPayload(const std::string& name, const std::vector<unsigned char>& data)
{
    Payload p;
    bool ok = DecodePayload(data, p);
    if (ok) BOOST_CHECK_MESSAGE(EncodePayload(p) == data, name + ": round trip");
    CScript script = PayloadScript(data);
    ExtractOpReturnData(script);
    CMutableTransaction mtx;
    mtx.vout.push_back(CTxOut(0, CScript(data.begin(), data.end())));
    mtx.vout.push_back(CTxOut(0, script));
    FindPayload(CTransaction(mtx));
    for (size_t n = 0; n < data.size(); n++) {
        Payload q;
        DecodePayload(std::vector<unsigned char>(data.begin(), data.begin() + n), q);
    }
    return ok;
}

/** The YellowbackScript target's body. Returns {vault parsed, spend path parsed}. */
std::pair<bool, bool> ReplayScript(const std::string& name, const std::vector<unsigned char>& data)
{
    CScript script(data.begin(), data.end());
    uint32_t lock, claim;
    CPubKey owner;
    bool isVault = ParseVaultScript(script, lock, owner, claim);
    if (isVault) {
        BOOST_CHECK_MESSAGE(VaultScript(lock, owner, claim) == script, name + ": vault round trip");
        BOOST_CHECK_MESSAGE(claim > lock, name + ": claim after lock");
    }
    std::optional<VaultSpendPath> path = ParseVaultSpendPath(script);
    CScript redeem;
    bool hasRedeem = ExtractRedeemScript(script, redeem);
    if (path.has_value()) {
        BOOST_CHECK_MESSAGE(hasRedeem && path->vaultScript == redeem, name + ": last push is the redeem script");
        BOOST_CHECK_MESSAGE(path->pushes >= 2, name + ": at least two pushes");
    }
    for (size_t n = 0; n < data.size(); n++) {
        CScript prefix(data.begin(), data.begin() + n);
        ParseVaultScript(prefix, lock, owner, claim);
        ParseVaultSpendPath(prefix);
        ExtractRedeemScript(prefix, redeem);
    }
    return { isVault, path.has_value() };
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(yellowback_fuzz_tests, BasicTestingSetup)

// Rule: TAG-1
// Rule: TAG-2
// Rule: TAG-5
BOOST_AUTO_TEST_CASE(tag_corpus_replay)
{
    int found = 0;
    for (const auto& e : TAG_CORPUS) {
        if (ReplayTag(e.first, ParseHex(e.second))) found++;
    }
    // after_height, after_extranonce, after_pool_text, two_tags, signal_only, pushdata1_tag (6);
    // the six height_* budget seeds (6); height_0_genesis_shape (1); the ten *_tagged pool seeds (10).
    // height_negative_bits masks to 2,147,483,647, which is not the seed's height prefix, so no tag.
    BOOST_CHECK_EQUAL(found, 6 + 6 + 1 + 10);
    for (const auto& e : TAG_CRASHES) ReplayTag("crash:" + e.first, ParseHex(e.second));
}

// Rule: MINT-1
// Rule: XFER-1
// Rule: RED-1
BOOST_AUTO_TEST_CASE(payload_corpus_replay)
{
    int decoded = 0;
    for (const auto& e : PAYLOAD_CORPUS) {
        if (ReplayPayload(e.first, ParseHex(e.second))) decoded++;
    }
    // mint, mint_feevout_none, mint_key_prefix_03, transfer_empty/one/15, redeem_empty/one/14,
    // and version1_mint (v1, retained: this count drops to 9 when Phase 2 deletes the version-1 branch).
    BOOST_CHECK_EQUAL(decoded, 10);
    for (const auto& e : PAYLOAD_CRASHES) ReplayPayload("crash:" + e.first, ParseHex(e.second));
}

// Rule: RED-1
// Rule: MINT-3
BOOST_AUTO_TEST_CASE(script_corpus_replay)
{
    int vaults = 0, paths = 0;
    for (const auto& e : SCRIPT_CORPUS) {
        auto r = ReplayScript(e.first, ParseHex(e.second));
        if (r.first) vaults++;
        if (r.second) paths++;
    }
    // vault, 4byte, mixed, opn, 2byte, threshold_minus_one.
    BOOST_CHECK_EQUAL(vaults, 6);
    // owner, claim, op2, nonminimal_1, negzero, claim_with_sig, four_pushes, pushdata1_script.
    BOOST_CHECK_EQUAL(paths, 8);
    for (const auto& e : SCRIPT_CRASHES) ReplayScript("crash:" + e.first, ParseHex(e.second));
}

BOOST_AUTO_TEST_SUITE_END()
