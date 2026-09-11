#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .
"""Seed corpora for the Yellowback fuzz targets (plan §7 "Fuzzing", N34).

One deterministic generator owns three corpora:

    src/fuzzing/YellowbackTag/input/*.bin       LE32(nHeight) ‖ coinbase scriptSig
    src/fuzzing/YellowbackPayload/input/*.bin   OP_RETURN payload bytes (version 2)
    src/fuzzing/YellowbackScript/input/*.bin    vault scripts and vault scriptSigs

and the C++ tables embedded in src/test/yellowback_fuzz_tests.cpp between the
BEGIN/END GENERATED CORPUS markers, so `make check` replays every seed (and
every file under src/fuzzing/<Target>/crashes/, a found-and-fixed crash)
without a fuzzing build or a path.

    gen_yellowback_corpus.py            print the C++ tables
    gen_yellowback_corpus.py --write    write input/*.bin and update the C++ file
    gen_yellowback_corpus.py --check    exit 1 unless input/ and the C++ file match (CI)

Run from anywhere; paths are relative to this file. No third-party modules.
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
FUZZ_DIR = os.path.join(SRC, "fuzzing")
CPP = os.path.join(HERE, "yellowback_fuzz_tests.cpp")
TARGETS = ("YellowbackTag", "YellowbackPayload", "YellowbackScript")
BEGIN = "// BEGIN GENERATED CORPUS (src/test/gen_yellowback_corpus.py; do not edit by hand)"
END = "// END GENERATED CORPUS"

# ---------------------------------------------------------------- script helpers (ref/ycash/src/script/script.h)
OP_0, OP_PUSHDATA1, OP_PUSHDATA2, OP_1NEGATE, OP_1, OP_16 = 0x00, 0x4C, 0x4D, 0x4F, 0x51, 0x60
OP_IF, OP_ELSE, OP_ENDIF, OP_DROP, OP_DUP, OP_RETURN = 0x63, 0x67, 0x68, 0x75, 0x76, 0x6A
OP_CHECKSIG, OP_CHECKSIGVERIFY, OP_CHECKMULTISIG, OP_CLTV = 0xAC, 0xAD, 0xAE, 0xB1
OP_TRUE = OP_1

# A fixed syntactically valid compressed key (also used by the unit tests).
KEY = bytes.fromhex("02cb81cc0269783ebd9e6484b5495343036874a407856f230a8ee0384986759e70")
UNCOMPRESSED_KEY = bytes([0x04]) + bytes(64)
FAKE_SIG = bytes([0x30, 0x45, 0x02, 0x21]) + bytes(range(1, 34)) + bytes([0x02, 0x20]) + bytes(range(34, 66)) + bytes([0x01])
assert len(FAKE_SIG) == 72
TAG_MAGIC = b"YED!"
LOCKTIME_THRESHOLD = 500000000


def scriptnum(n):
    """Minimal CScriptNum serialisation (CScriptNum::serialize)."""
    if n == 0:
        return b""
    neg = n < 0
    a = abs(n)
    out = bytearray()
    while a:
        out.append(a & 0xFF)
        a >>= 8
    if out[-1] & 0x80:
        out.append(0x80 if neg else 0x00)
    elif neg:
        out[-1] |= 0x80
    return bytes(out)


def push(data):
    """CScript::operator<<(vector): the minimal push for data (never OP_N)."""
    n = len(data)
    if n < OP_PUSHDATA1:
        return bytes([n]) + data
    if n <= 0xFF:
        return bytes([OP_PUSHDATA1, n]) + data
    return bytes([OP_PUSHDATA2]) + struct.pack("<H", n) + data


def push_num(n):
    """CScript::operator<<(int64_t): OP_0 / OP_1..OP_16 / OP_1NEGATE / a minimal CScriptNum push."""
    if n == 0:
        return bytes([OP_0])
    if n == -1:
        return bytes([OP_1NEGATE])
    if 1 <= n <= 16:
        return bytes([OP_1 + n - 1])
    return push(scriptnum(n))


def op(*ops):
    return bytes(ops)


def rnd(seed, n):
    """Deterministic pseudo-random bytes (no `random` module: the corpus must not drift)."""
    out = b""
    counter = 0
    while len(out) < n:
        out += hashlib.sha256(("yellowback-corpus/%s/%d" % (seed, counter)).encode()).digest()
        counter += 1
    return out[:n]


# ---------------------------------------------------------------- tag (§3.2)
def tag(price=50000, flags=0x01, version=1, source=0x0007, key=bytes([0xAB]) * 20):
    return TAG_MAGIC + bytes([version, flags]) + struct.pack("<Q", price) + struct.pack("<H", source) + key


def tag_push(*a, **kw):
    return push(tag(*a, **kw))


def tag_seed(height, script_sig):
    return struct.pack("<I", height) + script_sig


def tag_corpus():
    h = 1234567
    pre = push_num(h)
    ex8 = bytes.fromhex("0011223344556677")
    seeds = [
        ("after_height", tag_seed(h, pre + tag_push())),
        ("after_extranonce", tag_seed(h, pre + ex8 + tag_push())),
        ("after_pool_text", tag_seed(h, pre + b"/YcashPool/" + tag_push() + ex8)),
        ("magic_without_push_opcode", tag_seed(h, pre + tag())),
        ("first_occurrence_short_then_full", tag_seed(h, pre + bytes([0x24]) + TAG_MAGIC + b"\x01\x00" + tag_push())),
        ("two_tags", tag_seed(h, pre + tag_push() + tag_push(60000))),
        ("bad_version", tag_seed(h, pre + tag_push(version=0))),
        ("reserved_flag_bit", tag_seed(h, pre + tag_push(flags=0x03))),
        ("price_below_min", tag_seed(h, pre + tag_push(price=99))),
        ("price_above_max", tag_seed(h, pre + tag_push(price=100000001))),
        ("signal_only", tag_seed(h, pre + tag_push(price=0))),
        ("wrong_height", tag_seed(h + 1, pre + tag_push())),
        ("no_prefix", tag_seed(h, tag_push())),
        ("truncated_by_one", tag_seed(h, pre + tag_push()[:-1])),
        ("pushdata1_tag", tag_seed(h, pre + bytes([OP_PUSHDATA1, 0x24]) + tag())),
        ("empty", tag_seed(h, b"")),
        ("height_only", tag_seed(h, pre)),
        ("all_0x24", tag_seed(h, pre + bytes([0x24]) * 60)),
        ("height_1", tag_seed(1, push_num(1) + ex8 + tag_push())),
        ("height_16", tag_seed(16, push_num(16) + ex8 + tag_push())),
        ("height_17", tag_seed(17, push_num(17) + ex8 + tag_push())),
        ("height_65535", tag_seed(65535, push_num(65535) + ex8 + tag_push())),
        ("height_16777215", tag_seed(16777215, push_num(16777215) + ex8 + tag_push())),
        ("height_16777216", tag_seed(16777216, push_num(16777216) + ex8 + tag_push())),
        ("height_0_genesis_shape", tag_seed(0, push_num(0) + tag_push())),
        ("height_negative_bits", tag_seed(0xFFFFFFFF, pre + tag_push())),
    ]
    # Ten pool-shaped coinbase scriptSigs (synthetic: no mainnet node was available
    # when this corpus was made; the shapes are those real pools use — height push,
    # nTime/extranonce1/extranonce2 as raw bytes, pool text as a push or raw, and
    # Ycash's own IncrementExtraNonce layout `<height> <CScriptNum extranonce>`).
    pools = [
        ("pool_internal_miner", 1000000, push_num(1000000) + push_num(3)),                                    # miner.cpp:720 shape
        ("pool_internal_miner_big_nonce", 1000001, push_num(1000001) + push_num(0x123456)),
        ("pool_raw_en1_en2", 1100000, push_num(1100000) + bytes.fromhex("deadbeef") + bytes.fromhex("0000000000000001")),
        ("pool_text_push_then_raw", 1200000, push_num(1200000) + push(b"/ViaBTC/Mined by yecuser/") + rnd("en3", 8)),
        ("pool_ntime_en_text", 1300000, push_num(1300000) + struct.pack("<I", 1790000000) + rnd("en5", 8) + push(b"/2miners/")),
        ("pool_slush_shape", 1400000, push_num(1400000) + struct.pack("<I", 1790000001) + rnd("en6", 4) + rnd("en6b", 4) + b"/slush/"),
        ("pool_mph_shape", 1500000, push_num(1500000) + b"/ycash.miningpoolhub.com/" + rnd("en7", 8)),
        ("pool_padding_then_en", 1600000, push_num(1600000) + b"\x00\x00" + rnd("en8", 12)),
        ("pool_max_100_bytes", 1700000, None),                                                              # filled below
        ("pool_magic_in_extranonce", 1800000, push_num(1800000) + rnd("en10", 8) + TAG_MAGIC + rnd("en10b", 20)),
    ]
    out = []
    for name, height, sig in pools:
        if sig is None:
            sig = push_num(height) + push(b"/A very long pool tagline padding out the coinbase/")
            sig += rnd("en9", 100 - len(sig))
            assert len(sig) == 100
        out.append((name, tag_seed(height, sig)))
        # The same coinbase with a tag inserted after the height push.
        pre_len = len(push_num(height))
        tagged = sig[:pre_len] + tag_push() + sig[pre_len:]
        if len(tagged) > 100:
            tagged = tagged[:100]
        out.append((name + "_tagged", tag_seed(height, tagged)))
    seeds += out
    seeds.append(("random1", rnd("tag-random1", 40)))
    seeds.append(("random2", rnd("tag-random2", 104)))
    return seeds


# ---------------------------------------------------------------- payload (§3.3)
def hdr(t, version=2):
    return bytes([0x59, 0x42, version, t])


def mint(term=0, cents=10000, lock=1000, ref=950, key=KEY, fee=3, version=2):
    return hdr(1, version) + bytes([term]) + struct.pack("<III", cents, lock, ref) + key + bytes([fee])


def transfer(assignments, version=2):
    body = bytes([len(assignments)])
    for v, c in assignments:
        body += bytes([v]) + struct.pack("<I", c)
    return hdr(2, version) + body


def redeem(ref, fee, assignments):
    body = struct.pack("<I", ref) + bytes([fee, len(assignments)])
    for v, c in assignments:
        body += bytes([v]) + struct.pack("<I", c)
    return hdr(3) + body


def payload_corpus():
    fifteen = [(i, 100 + i) for i in range(15)]
    fourteen = fifteen[:14]
    return [
        ("mint", mint()),
        ("mint_feevout_none", mint(term=2, cents=1000000, lock=0xFFFFFFFF, ref=0, fee=0xFF)),
        ("mint_key_prefix_03", mint(key=bytes([0x03]) + KEY[1:])),
        ("transfer_empty", transfer([])),
        ("transfer_one", transfer([(1, 100)])),
        ("transfer_15", transfer(fifteen)),
        ("redeem_empty", redeem(950, 0xFF, [])),
        ("redeem_one", redeem(950, 3, [(1, 12345)])),
        ("redeem_14", redeem(0xFFFFFFFF, 0, fourteen)),
        ("bad_magic", b"\x59\x44" + mint()[2:]),
        ("version1_mint", hdr(1, 1) + bytes([0]) + struct.pack("<III", 10000, 1000, 950) + KEY),   # v1, retained until Phase 2
        ("version3", mint(version=3)),
        ("unknown_type_04", hdr(4) + b"\x01\x00\x00\x00"),
        ("retired_type_10", hdr(0x10) + struct.pack("<Q", 50000)),
        ("reserved_type_20", hdr(0x20) + b"\x00"),
        ("short", b"\x59\x42\x02"),
        ("mint_short", mint()[:-2]),
        ("mint_no_feevout", mint()[:-1]),
        ("mint_trailing", mint() + b"\x00"),
        ("mint_uncompressed_key", hdr(1) + bytes([0]) + struct.pack("<III", 10000, 1000, 950) + UNCOMPRESSED_KEY[:33] + b"\x03"),
        ("transfer_dup", transfer([(1, 1), (1, 2)])),
        ("transfer_zero", transfer([(1, 0)])),
        ("transfer_16", transfer([(i, 1) for i in range(16)])),
        ("redeem_15", redeem(1, 0xFF, [(i, 1) for i in range(15)])),
        ("redeem_short", redeem(950, 0xFF, [(1, 12345)])[:-1]),
        ("long81", hdr(2) + bytes(77)),
        ("random1", rnd("payload-random1", 40)),
        ("random2", rnd("payload-random2", 80)),
        ("empty", b""),
    ]


# ---------------------------------------------------------------- script (§3.4)
def vault(lock=120000, claim=120000 + 34560, key=KEY, checksig=OP_CHECKSIG, endif=True, lock_bytes=None):
    s = op(OP_IF)
    s += push(lock_bytes) if lock_bytes is not None else push_num(lock)
    s += op(OP_CLTV, OP_DROP) + push(key) + op(checksig)
    s += op(OP_ELSE) + push_num(claim) + op(OP_CLTV, OP_DROP, OP_TRUE)
    if endif:
        s += op(OP_ENDIF)
    return s


def owner_sig(script, sig=FAKE_SIG, selector=op(OP_1)):
    return push(sig) + selector + push(script)


def claim_sig(script, selector=op(OP_0)):
    return selector + push(script)


def script_corpus():
    v = vault()
    return [
        ("vault", v),
        ("vault_4byte_heights", vault(9000000, 9000000 + 34560)),
        ("vault_mixed_widths", vault(8388607, 8388607 + 34560)),
        ("vault_opn_heights", vault(1, 2)),
        ("vault_2byte_heights", vault(17, 18)),
        ("vault_threshold_minus_one", vault(LOCKTIME_THRESHOLD - 2, LOCKTIME_THRESHOLD - 1)),
        ("vault_nonminimal_lock", vault(lock_bytes=bytes.fromhex("c0d40100"))),
        ("vault_claim_le_lock", vault(120000, 120000)),
        ("vault_time_locked", vault(LOCKTIME_THRESHOLD, LOCKTIME_THRESHOLD + 1)),
        ("vault_uncompressed_key", vault(key=UNCOMPRESSED_KEY)),
        ("vault_missing_endif", vault(endif=False)),
        ("vault_checksigverify", vault(checksig=OP_CHECKSIGVERIFY)),
        ("vault_trailing", v + op(OP_DROP)),
        ("scriptsig_owner", owner_sig(v)),
        ("scriptsig_claim", claim_sig(v)),
        ("scriptsig_owner_op2", owner_sig(v, selector=op(OP_1 + 1))),
        ("scriptsig_owner_nonminimal_1", owner_sig(v, selector=bytes([0x01, 0x01]))),
        ("scriptsig_claim_negzero", claim_sig(v, selector=bytes([0x01, 0x80]))),
        ("scriptsig_claim_with_sig", owner_sig(v, selector=op(OP_0))),
        ("scriptsig_one_push", push(v)),
        ("scriptsig_not_push_only", op(OP_1) + push(v) + op(OP_DROP)),
        ("scriptsig_four_pushes", push(FAKE_SIG) + push(FAKE_SIG) + op(OP_1) + push(v)),
        ("scriptsig_pushdata1_script", push(FAKE_SIG) + op(OP_1) + bytes([OP_PUSHDATA1, len(v)]) + v),
        ("pushdata2", bytes([OP_PUSHDATA2]) + struct.pack("<H", 256) + bytes(256)),
        ("pushdata_truncated", bytes([OP_PUSHDATA1, 0x50, 0x01, 0x02])),
        ("v1_vault", push_num(120000) + op(OP_CLTV, OP_DROP) + push(KEY) + op(OP_CHECKSIGVERIFY, OP_1) + push(KEY) + op(OP_1, OP_CHECKMULTISIG)),
        ("p2pkh", op(OP_DUP, 0xA9) + push(bytes(20)) + op(0x88, OP_CHECKSIG)),
        ("random1", rnd("script-random1", 60)),
        ("random2", rnd("script-random2", 400)),
        ("empty", b""),
    ]


CORPORA = {
    "YellowbackTag": tag_corpus,
    "YellowbackPayload": payload_corpus,
    "YellowbackScript": script_corpus,
}


# ---------------------------------------------------------------- files and the C++ table
def crashes(target):
    d = os.path.join(FUZZ_DIR, target, "crashes")
    if not os.path.isdir(d):
        return []
    out = []
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if name.startswith(".") or not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            out.append((name, f.read()))
    return out


def cpp_table(var, entries):
    lines = ["static const std::vector<std::pair<std::string, std::string>> %s = {" % var]
    for name, data in entries:
        lines.append('    {"%s", "%s"},' % (name, data.hex()))
    lines.append("};")
    return "\n".join(lines)


def cpp_block():
    parts = [BEGIN]
    for target in TARGETS:
        seeds = CORPORA[target]()
        names = [n for n, _ in seeds]
        assert len(names) == len(set(names)), "duplicate seed name in %s" % target
        assert len(seeds) >= 20, "%s needs >= 20 seeds" % target
        upper = target.replace("Yellowback", "").upper()
        parts.append(cpp_table("%s_CORPUS" % upper, seeds))
        parts.append(cpp_table("%s_CRASHES" % upper, crashes(target)))
    parts.append(END)
    return "\n".join(parts) + "\n"


def splice(text, block):
    a = text.find(BEGIN)
    b = text.find(END)
    if a < 0 or b < 0:
        raise SystemExit("markers not found in %s" % CPP)
    b += len(END) + 1
    return text[:a] + block + text[b:]


def expected_files(target):
    return {name + ".bin": data for name, data in CORPORA[target]()}


def write_all():
    for target in TARGETS:
        d = os.path.join(FUZZ_DIR, target, "input")
        os.makedirs(d, exist_ok=True)
        want = expected_files(target)
        for name in os.listdir(d):
            if name.endswith(".bin") and name not in want:
                os.remove(os.path.join(d, name))
        for name, data in want.items():
            with open(os.path.join(d, name), "wb") as f:
                f.write(data)
    with open(CPP) as f:
        text = f.read()
    with open(CPP, "w") as f:
        f.write(splice(text, cpp_block()))


def check_all():
    ok = True
    for target in TARGETS:
        d = os.path.join(FUZZ_DIR, target, "input")
        want = expected_files(target)
        have = {n for n in os.listdir(d) if n.endswith(".bin")} if os.path.isdir(d) else set()
        for name in sorted(set(want) | have):
            p = os.path.join(d, name)
            if name not in want:
                print("%s: unexpected file %s" % (target, name))
                ok = False
            elif not os.path.exists(p):
                print("%s: missing %s" % (target, name))
                ok = False
            else:
                with open(p, "rb") as f:
                    if f.read() != want[name]:
                        print("%s: %s differs" % (target, name))
                        ok = False
        if len(have) < 20:
            print("%s: only %d corpus files (need >= 20)" % (target, len(have)))
            ok = False
    with open(CPP) as f:
        text = f.read()
    if splice(text, cpp_block()) != text:
        print("%s: embedded corpus table is stale (run --write)" % CPP)
        ok = False
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write", action="store_true", help="write input/*.bin and update the C++ tables")
    g.add_argument("--check", action="store_true", help="verify input/*.bin and the C++ tables; exit 1 on drift")
    args = ap.parse_args()
    if args.write:
        write_all()
        for target in TARGETS:
            print("%s: %d seeds" % (target, len(CORPORA[target]())))
        return 0
    if args.check:
        if check_all():
            print("corpus ok")
            return 0
        return 1
    sys.stdout.write(cpp_block())
    return 0


if __name__ == "__main__":
    sys.exit(main())
