#!/usr/bin/env python3
# Copyright (c) 2026 The Ycash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
yellowback_model.py -- a second, independent implementation of the Yellowback
v2 protocol (docs/plans/yellowback-v2-development-plan.md section 3) in pure
Python, used as the cross-implementation check of the node (plan N23, P8).

It re-implements, from the plan text alone and with Python integers only:

  section 3.2  the coinbase tag decoder            (TAG-1..5)          find_tag()
  section 3.3  the payload codec                   (MINT/TRANSFER/REDEEM) decode_payload(), tx_payload()
  section 3.4  the vault script and path detection                      vault_script(), spend_path()
  section 3.6  the state tables and the state hash (N18)                YellowbackModel.state_hash()
  section 3.7  the snapshot arithmetic             (PRICE, SIGMA, ACT, HALT, REG-4, FEE-1/2)
  section 3.8  the money rules                     (IN-1..3, TX-0, MINT-1..8, XFER-1..3, RED-1..4)
  section 3.9  BLK-1 (blockInvalid + enforcementOn per block; the model never rejects)

The model is fed block by block (``feed_block``) with ``getblock <hash> 2``-style
transaction dicts and maintains Tags, Judgements, Activation, Vaults, Tokens,
TxLog, Totals and Snapshots.  ``assert_model_matches(node)`` rebuilds it from a
live node and compares it with the node's RPC answers.

Arithmetic conventions (section 3.7, M1): every quantity is a non-negative
integer; every division is floor division unless written ceil; ``isqrt`` is the
floor square root; ``lower_median`` is the element at index (n-1)//2 of the
sorted values; the products that section 3.7 computes in ``arith_uint256`` are
reduced modulo 2**256 (``u256``) so an overflow, should one ever occur, is
reproduced rather than hidden.  Totality (section 3.8): a rule whose input is
undefined (``None``) is false.

Every serialisation choice that section 3.6 leaves open is written down in
SERIALISATION.md next to this file; the C++ implementation must make the same
choices for the state hash to agree.

Only the standard library is used.  Nothing here imports mininode (it needs
asyncore, gone in Python 3.12) or talks to a node except assert_model_matches.
"""

import hashlib
import json
import math
import struct
from collections import OrderedDict
from decimal import Decimal

# ---------------------------------------------------------------------------
# Units and protocol constants (section 3.1)

COIN = 10 ** 8
MAX_MONEY = 21_000_000 * COIN          # ref/ycash/src/amount.h
BPS = 10_000
LOCKTIME_THRESHOLD = 500_000_000       # ref/ycash/src/script/script.h
BLOCKS_PER_HOUR = 48
BLOCKS_PER_DAY = 1_152
BLOCKS_PER_YEAR = 420_480

TAG_MAGIC = b'YED!'
TAG_VERSION = 1
TAG_SIZE = 36
TAG_PATTERN = b'\x24' + TAG_MAGIC      # direct-push opcode 0x24 (36) followed by the magic (P10)

PAYLOAD_MAGIC = b'YB'
PAYLOAD_VERSION = 2
PAYLOAD_MINT = 0x01
PAYLOAD_TRANSFER = 0x02
PAYLOAD_REDEEM = 0x03
MAX_PAYLOAD = 80
FEE_VOUT_NONE = 0xFF

# Activation.status, declaration order (section 3.6)
SIGNALING, LOCKED_IN, ACTIVE = 0, 1, 2
ACTIVATION_NAMES = {SIGNALING: 'SIGNALING', LOCKED_IN: 'LOCKED_IN', ACTIVE: 'ACTIVE'}

# Vaults.status, declaration order (section 3.6)
V_ACTIVE, V_VOID, V_CLOSED, V_CLAIMED = 0, 1, 2, 3
VAULT_STATUS_NAMES = {V_ACTIVE: 'ACTIVE', V_VOID: 'VOID', V_CLOSED: 'CLOSED', V_CLAIMED: 'CLAIMED'}

# haltMask bits, declaration order (section 3.6)
HALT_NOT_ACTIVE = 1 << 0
HALT_NO_PRICE = 1 << 1
HALT_PARTICIPATION = 1 << 2
HALT_GLOBAL_RATIO = 1 << 3
HALT_DIVERGENCE = 1 << 4
HALT_ENFORCEMENT = 1 << 5
HALT_NAMES = OrderedDict([
    (HALT_NOT_ACTIVE, 'NOT_ACTIVE'),
    (HALT_NO_PRICE, 'NO_PRICE'),
    (HALT_PARTICIPATION, 'PARTICIPATION'),
    (HALT_GLOBAL_RATIO, 'GLOBAL_RATIO'),
    (HALT_DIVERGENCE, 'DIVERGENCE'),
    (HALT_ENFORCEMENT, 'ENFORCEMENT'),
])

# Script opcodes
OP_0 = 0x00
OP_PUSHDATA1 = 0x4c
OP_PUSHDATA2 = 0x4d
OP_PUSHDATA4 = 0x4e
OP_1NEGATE = 0x4f
OP_RESERVED = 0x50
OP_1 = 0x51
OP_16 = 0x60
OP_RETURN = 0x6a
OP_IF = 0x63
OP_ELSE = 0x67
OP_ENDIF = 0x68
OP_DROP = 0x75
OP_DUP = 0x76
OP_EQUAL = 0x87
OP_EQUALVERIFY = 0x88
OP_HASH160 = 0xa9
OP_CHECKSIG = 0xac
OP_CHECKLOCKTIMEVERIFY = 0xb1

SAPLING_TX_VERSION = 4
SAPLING_VERSION_GROUP_ID = 0x892F2085

# Verdict strings (section 4.2a)
VERDICT_OK = 'ok'
VERDICT_BURNED = 'burned'


class Params(object):
    """Every value a rule reads (section 3.1).  Build with Params.regtest(...) or Params.mainnet(...)."""

    # The "hashed" record (section 3.6 Params): startHeight, sigmaRefBps, supplyCapBps, enforceUntil.

    def __init__(self, network, start_height, sigma_ref_bps, supply_cap_bps, enforce_until,
                 p_fast_window, p_mid_window, p_slow_window,
                 signal_window, activation_threshold, participation_floor, activation_delay,
                 enforcement_floor, enforcement_resume, valve_blocks, abandon_blocks,
                 n_reg, n_penalty, peer_lag, peer_min, deviation_bps, accuracy_band_bps,
                 accuracy_window, payee_tilt_bps, payee_window, fee_min, fee_bps, grace,
                 claim_threshold_bps, global_ratio_halt_bps, divergence_bps,
                 class_min, class_max, base_ratio_bps, vol_window, vol_step,
                 vol_periods_per_year, sigma_mult_max_bps, min_mint, max_mint, min_output,
                 max_output, token_value, yellowback_fee, ref_window, ref_lag,
                 price_min=100, price_max=100_000_000):
        self.network = network
        self.start_height = start_height
        self.sigma_ref_bps = sigma_ref_bps
        self.supply_cap_bps = supply_cap_bps
        self.enforce_until = enforce_until
        self.p_fast_window = p_fast_window
        self.p_mid_window = p_mid_window
        self.p_slow_window = p_slow_window
        self.signal_window = signal_window
        self.activation_threshold = activation_threshold
        self.participation_floor = participation_floor
        self.activation_delay = activation_delay
        self.enforcement_floor = enforcement_floor
        self.enforcement_resume = enforcement_resume
        self.valve_blocks = valve_blocks
        self.abandon_blocks = abandon_blocks
        self.n_reg = n_reg
        self.n_penalty = n_penalty
        self.peer_lag = peer_lag
        self.peer_min = peer_min
        self.deviation_bps = deviation_bps
        self.accuracy_band_bps = accuracy_band_bps
        self.accuracy_window = accuracy_window
        self.payee_tilt_bps = payee_tilt_bps
        self.payee_window = payee_window
        self.fee_min = fee_min
        self.fee_bps = fee_bps
        self.grace = grace
        self.claim_threshold_bps = claim_threshold_bps
        self.global_ratio_halt_bps = global_ratio_halt_bps
        self.divergence_bps = divergence_bps
        self.class_min = list(class_min)
        self.class_max = list(class_max)
        self.base_ratio_bps = list(base_ratio_bps)
        self.vol_window = vol_window
        self.vol_step = vol_step
        self.vol_periods_per_year = vol_periods_per_year
        self.sigma_mult_max_bps = sigma_mult_max_bps
        self.min_mint = min_mint
        self.max_mint = max_mint
        self.min_output = min_output
        self.max_output = max_output
        self.token_value = token_value
        self.yellowback_fee = yellowback_fee
        self.ref_window = ref_window
        self.ref_lag = ref_lag
        self.price_min = price_min
        self.price_max = price_max

    # WINDOW_MIN_FILL (section 3.1, L9): fast ceil(W/2); mid and slow ceil(2W/3)
    @property
    def min_fill_fast(self):
        return ceil_div(self.p_fast_window, 2)

    @property
    def min_fill_mid(self):
        return ceil_div(2 * self.p_mid_window, 3)

    @property
    def min_fill_slow(self):
        return ceil_div(2 * self.p_slow_window, 3)

    def class_range(self, term_class):
        """classRange(termClass) as (lo, hi) with lo <= d <= hi, or None for an invalid class."""
        if term_class not in (0, 1, 2):
            return None
        return (self.class_min[term_class], self.class_max[term_class])

    @classmethod
    def regtest(cls, start_height, sigma_ref_bps=0, supply_cap_bps=0, enforce_until=0):
        return cls(
            network='regtest', start_height=start_height, sigma_ref_bps=sigma_ref_bps,
            supply_cap_bps=supply_cap_bps, enforce_until=enforce_until,
            p_fast_window=8, p_mid_window=24, p_slow_window=64,
            signal_window=64, activation_threshold=48, participation_floor=39, activation_delay=64,
            enforcement_floor=32, enforcement_resume=39, valve_blocks=6, abandon_blocks=128,
            n_reg=24, n_penalty=12, peer_lag=4, peer_min=3, deviation_bps=1000, accuracy_band_bps=300,
            accuracy_window=24, payee_tilt_bps=10_000, payee_window=10,
            fee_min=50_000_000, fee_bps=25, grace=24, claim_threshold_bps=11_000,
            global_ratio_halt_bps=25_000, divergence_bps=2_000,
            # class A [48, 96], B (96, 144], C (144, 240]  (section 3.1 regtest column)
            class_min=[48, 97, 145], class_max=[96, 144, 240], base_ratio_bps=[50_000, 40_000, 30_000],
            vol_window=64, vol_step=8, vol_periods_per_year=8_760, sigma_mult_max_bps=30_000,
            min_mint=10_000, max_mint=1_000_000, min_output=100, max_output=10_000_000,
            token_value=10_000, yellowback_fee=1_000, ref_window=40, ref_lag=2)

    @classmethod
    def mainnet(cls, start_height, enforce_until, network='main'):
        return cls(
            network=network, start_height=start_height, sigma_ref_bps=10_000,
            supply_cap_bps=1_500, enforce_until=enforce_until,
            p_fast_window=96, p_mid_window=576, p_slow_window=2_016,
            signal_window=2_016, activation_threshold=1_512, participation_floor=1_210, activation_delay=2_016,
            enforcement_floor=1_008, enforcement_resume=1_210, valve_blocks=6, abandon_blocks=4_032,
            n_reg=576, n_penalty=288, peer_lag=10, peer_min=5, deviation_bps=1000, accuracy_band_bps=300,
            accuracy_window=576, payee_tilt_bps=10_000, payee_window=100,
            fee_min=50_000_000, fee_bps=25, grace=34_560, claim_threshold_bps=11_000,
            global_ratio_halt_bps=25_000, divergence_bps=2_000,
            # class A [34,560, 103,680], B (103,680, 420,480], C (420,480, 2,102,400]
            class_min=[34_560, 103_681, 420_481], class_max=[103_680, 420_480, 2_102_400],
            base_ratio_bps=[50_000, 40_000, 30_000],
            vol_window=2_016, vol_step=48, vol_periods_per_year=8_760, sigma_mult_max_bps=30_000,
            min_mint=10_000, max_mint=1_000_000, min_output=100, max_output=10_000_000,
            token_value=10_000, yellowback_fee=1_000, ref_window=40, ref_lag=2)


def params_from_getinfo(info):
    """Params from a ``yed_getinfo`` result (section 4.5): the four hashed values from ``params``,
    everything else from the network's compiled-in table (the regtest table for regtest)."""
    p = info['params']
    network = info.get('network', 'regtest')
    if network == 'regtest':
        params = Params.regtest(int(p['startHeight']),
                                sigma_ref_bps=int(p.get('sigmaRefBps', 0)),
                                supply_cap_bps=int(p.get('supplyCapBps', 0)),
                                enforce_until=int(p.get('enforceUntilHeight', 0) or 0))
    else:
        params = Params.mainnet(int(p['startHeight']), int(p.get('enforceUntilHeight', 0) or 0),
                                network=network)
        params.sigma_ref_bps = int(p.get('sigmaRefBps', params.sigma_ref_bps))
        params.supply_cap_bps = int(p.get('supplyCapBps', params.supply_cap_bps))
    return params


# ---------------------------------------------------------------------------
# Arithmetic helpers (section 3.7 conventions)

U256_MASK = (1 << 256) - 1


def u256(x):
    """arith_uint256 wrap-around: every product/sum section 3.7 computes in arith_uint256."""
    return x & U256_MASK


def ceil_div(a, b):
    return -((-a) // b)


def isqrt(x):
    return math.isqrt(x)


def lower_median(values):
    """lowerMedian(S): the element at 0-based index floor((n-1)/2) of the sorted values; None if empty."""
    if not values:
        return None
    s = sorted(values)
    return s[(len(s) - 1) // 2]


def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def sigma_mult_bps(samples, sigma_ref_bps, periods_per_year, max_bps):
    """SIGMA-1.  samples = [s_0, s_1, ..., s_n] (n returns); any None => max_bps (K12);
    sigma_ref_bps == 0 => 10,000 (regtest: multiplier fixed at 1)."""
    if sigma_ref_bps == 0:
        return BPS
    if any(s is None for s in samples):
        return max_bps
    n = len(samples) - 1
    if n <= 0:
        return max_bps
    acc = 0
    for k in range(n):
        a, b = samples[k], samples[k + 1]
        r = u256(abs(a - b) * BPS) // b          # b >= PRICE_MIN > 0
        acc = u256(acc + u256(r * r))
    var = acc // n
    sigma_annual = isqrt(u256(var * periods_per_year))
    return clamp(u256(sigma_annual * BPS) // sigma_ref_bps, BPS, max_bps)


def min_ratio_bps(base_ratio_bps, sigma_mult):
    return (base_ratio_bps * sigma_mult) // BPS


def required_zat(cents, min_ratio, p_mint):
    """requiredZat = ceil(cents * minRatioBps * COIN / pMint); None when unsatisfiable (> MAX_MONEY, K14)
    or when pMint is undefined."""
    if p_mint is None or p_mint <= 0:
        return None
    q = ceil_div(u256(cents * min_ratio * COIN), p_mint)
    if q > MAX_MONEY:
        return None
    return q


def cap_cents(issued_zat, p_mint):
    if p_mint is None:
        return None
    return u256(issued_zat * p_mint) // (COIN * BPS)


def supply_cap_cents(issued_zat, p_mint, cap_bps):
    if cap_bps == 0:
        return None
    c = cap_cents(issued_zat, p_mint)
    if c is None:
        return None
    return u256(c * cap_bps) // BPS


def global_ratio_bps(collateral_zat, p_mint, supply_cents):
    if p_mint is None or supply_cents <= 0:
        return None
    return u256(collateral_zat * p_mint) // (COIN * supply_cents)


def is_underwater(collateral_zat, p_claim, minted_cents, threshold_bps):
    if p_claim is None:
        return False
    return u256(collateral_zat * p_claim) < u256(minted_cents * threshold_bps * COIN)


def fee_zat(collateral_zat, fee_min, fee_bps):
    return max(fee_min, (collateral_zat * fee_bps) // BPS)


# ---------------------------------------------------------------------------
# Hashing, base58, secp256k1 key validity

def sha256(b):
    return hashlib.sha256(b).digest()


def hash256(b):
    return sha256(sha256(b))


def _ripemd160_py(msg):
    """Pure-Python RIPEMD-160, used only when hashlib lacks it."""
    def rol(x, n):
        return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF
    f = [lambda x, y, z: x ^ y ^ z,
         lambda x, y, z: (x & y) | (~x & z),
         lambda x, y, z: (x | ~y) ^ z,
         lambda x, y, z: (x & z) | (y & ~z),
         lambda x, y, z: x ^ (y | ~z)]
    K = [0x00000000, 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xA953FD4E]
    KK = [0x50A28BE6, 0x5C4DD124, 0x6D703EF3, 0x7A6D76E9, 0x00000000]
    R = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
         7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
         3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
         1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
         4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13]
    RR = [5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
          6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
          15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
          8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
          12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11]
    S = [11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
         7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
         11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
         11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
         9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6]
    SS = [8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
          9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
          9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
          15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
          8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11]
    h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]
    ml = len(msg)
    msg = msg + b'\x80' + b'\x00' * ((55 - ml) % 64) + struct.pack('<Q', ml * 8)
    for off in range(0, len(msg), 64):
        X = list(struct.unpack('<16I', msg[off:off + 64]))
        al, bl, cl, dl, el = h
        ar, br, cr, dr, er = h
        for j in range(80):
            rnd = j // 16
            t = (rol((al + f[rnd](bl, cl, dl) + X[R[j]] + K[rnd]) & 0xFFFFFFFF, S[j]) + el) & 0xFFFFFFFF
            al, el, dl, cl, bl = el, dl, rol(cl, 10), bl, t
            t = (rol((ar + f[4 - rnd](br, cr, dr) + X[RR[j]] + KK[rnd]) & 0xFFFFFFFF, SS[j]) + er) & 0xFFFFFFFF
            ar, er, dr, cr, br = er, dr, rol(cr, 10), br, t
        t = (h[1] + cl + dr) & 0xFFFFFFFF
        h[1] = (h[2] + dl + er) & 0xFFFFFFFF
        h[2] = (h[3] + el + ar) & 0xFFFFFFFF
        h[3] = (h[4] + al + br) & 0xFFFFFFFF
        h[4] = (h[0] + bl + cr) & 0xFFFFFFFF
        h[0] = t
    return struct.pack('<5I', *h)


def ripemd160(b):
    try:
        return hashlib.new('ripemd160', b).digest()
    except ValueError:
        return _ripemd160_py(b)


def hash160(b):
    return ripemd160(sha256(b))


_B58 = b'123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'


def base58check_encode(payload):
    data = payload + hash256(payload)[:4]
    n = int.from_bytes(data, 'big')
    out = bytearray()
    while n > 0:
        n, r = divmod(n, 58)
        out.append(_B58[r])
    for c in data:
        if c == 0:
            out.append(_B58[0])
        else:
            break
    return bytes(reversed(out)).decode('ascii')


def base58check_decode(s):
    """Returns the payload (version bytes + body) or None on a bad checksum / character."""
    n = 0
    for ch in s.encode('ascii'):
        idx = _B58.find(bytes([ch]))
        if idx < 0:
            return None
        n = n * 58 + idx
    body = n.to_bytes((n.bit_length() + 7) // 8, 'big') if n else b''
    pad = 0
    for ch in s:
        if ch == '1':
            pad += 1
        else:
            break
    data = b'\x00' * pad + body
    if len(data) < 4 or hash256(data[:-4])[:4] != data[-4:]:
        return None
    return data[:-4]


def address_key_hash(addr):
    """The trailing 20 bytes of a Base58Check address (any version), or None."""
    payload = base58check_decode(addr)
    if payload is None or len(payload) < 20:
        return None
    return payload[-20:]


_SECP_P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F


def is_valid_compressed_pubkey(b):
    """CPubKey::IsFullyValid for a 33-byte compressed key: prefix 02/03, x < p, x^3 + 7 a square."""
    if len(b) != 33 or b[0] not in (2, 3):
        return False
    x = int.from_bytes(b[1:], 'big')
    if x >= _SECP_P:
        return False
    rhs = (pow(x, 3, _SECP_P) + 7) % _SECP_P
    y = pow(rhs, (_SECP_P + 1) // 4, _SECP_P)
    return (y * y) % _SECP_P == rhs


# ---------------------------------------------------------------------------
# Script helpers (section 3.4)

def script_num(n):
    """CScriptNum::serialize (minimal, little-endian, sign bit)."""
    if n == 0:
        return b''
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
    """CScript << std::vector<unsigned char>: the canonical push for the length."""
    n = len(data)
    if n < OP_PUSHDATA1:
        return bytes([n]) + data
    if n <= 0xFF:
        return bytes([OP_PUSHDATA1, n]) + data
    if n <= 0xFFFF:
        return bytes([OP_PUSHDATA2]) + struct.pack('<H', n) + data
    return bytes([OP_PUSHDATA4]) + struct.pack('<I', n) + data


def push_int(n):
    """CScript << int64_t: OP_0, OP_1NEGATE, OP_1..OP_16 or a CScriptNum push."""
    if n == 0:
        return bytes([OP_0])
    if n == -1:
        return bytes([OP_1NEGATE])
    if 1 <= n <= 16:
        return bytes([OP_1 + n - 1])
    return push(script_num(n))


def height_prefix(height):
    """The BIP34 height push ``CScript() << nHeight`` (ref/ycash/src/main.cpp:4477)."""
    return push_int(height)


def vault_script(lock_height, owner_pubkey, claim_height):
    return (bytes([OP_IF]) + push_int(lock_height) + bytes([OP_CHECKLOCKTIMEVERIFY, OP_DROP])
            + push(owner_pubkey) + bytes([OP_CHECKSIG, OP_ELSE]) + push_int(claim_height)
            + bytes([OP_CHECKLOCKTIMEVERIFY, OP_DROP, OP_1, OP_ENDIF]))


def p2sh_script(redeem_script):
    return bytes([OP_HASH160, 20]) + hash160(redeem_script) + bytes([OP_EQUAL])


def p2pkh_script(key_hash):
    return bytes([OP_DUP, OP_HASH160, 20]) + key_hash + bytes([OP_EQUALVERIFY, OP_CHECKSIG])


def is_p2sh(spk):
    return len(spk) == 23 and spk[0] == OP_HASH160 and spk[1] == 20 and spk[22] == OP_EQUAL


def p2sh_hash(spk):
    return spk[2:22] if is_p2sh(spk) else None


def p2pkh_key(spk):
    """The 20-byte key hash of a P2PKH scriptPubKey, or None."""
    if (len(spk) == 25 and spk[0] == OP_DUP and spk[1] == OP_HASH160 and spk[2] == 20
            and spk[23] == OP_EQUALVERIFY and spk[24] == OP_CHECKSIG):
        return spk[3:23]
    return None


def parse_pushes(script):
    """The pushed values of a push-only script (CScript::IsPushOnly: every opcode <= OP_16),
    or None if the script is not push-only or is truncated.  OP_RESERVED (0x50) is counted as
    push-only by IsPushOnly but pushes nothing and fails evaluation; this reader treats it as
    not push-only (the conservative reading; see SERIALISATION.md, ambiguity list)."""
    pushes = []
    i, n = 0, len(script)
    while i < n:
        op = script[i]
        i += 1
        if op <= 0x4b:
            size = op
        elif op == OP_PUSHDATA1:
            if i + 1 > n:
                return None
            size = script[i]
            i += 1
        elif op == OP_PUSHDATA2:
            if i + 2 > n:
                return None
            size = struct.unpack('<H', script[i:i + 2])[0]
            i += 2
        elif op == OP_PUSHDATA4:
            if i + 4 > n:
                return None
            size = struct.unpack('<I', script[i:i + 4])[0]
            i += 4
        elif op == OP_1NEGATE:
            pushes.append(b'\x81')
            continue
        elif OP_1 <= op <= OP_16:
            pushes.append(bytes([op - OP_1 + 1]))
            continue
        else:
            return None
        if i + size > n:
            return None
        pushes.append(bytes(script[i:i + size]))
        i += size
    return pushes


def cast_to_bool(v):
    """CastToBool: false for empty and for negative zero (0x80 with all other bytes zero)."""
    for i, c in enumerate(v):
        if c != 0:
            if i == len(v) - 1 and c == 0x80:
                return False
            return True
    return False


def spend_path(script_sig):
    """Path detection (section 3.4): the selector is the push before the redeem-script push.
    Returns 'owner', 'claim', or None when the scriptSig is not push-only / has < 2 pushes (RED-1 fails)."""
    pushes = parse_pushes(script_sig)
    if pushes is None or len(pushes) < 2:
        return None
    return 'owner' if cast_to_bool(pushes[-2]) else 'claim'


def script_single_push(script):
    """If ``script`` is exactly ``OP_RETURN <one push>`` return the pushed data, else None.
    Any GetOp-valid push encoding is accepted (the canonical one is what the wallet writes)."""
    if len(script) < 2 or script[0] != OP_RETURN:
        return None
    pushes = parse_pushes(script[1:])
    if pushes is None or len(pushes) != 1:
        return None
    # parse_pushes accepts OP_1..OP_16 / OP_1NEGATE as pushes; those are not "a push of 4..80 bytes"
    op = script[1]
    if not (1 <= op <= OP_PUSHDATA4):
        return None
    return pushes[0]


# ---------------------------------------------------------------------------
# section 3.2  Coinbase tag codec

class Tag(object):
    __slots__ = ('version', 'flags', 'price_micro_usd', 'source_mask', 'payout_key')

    def __init__(self, version, flags, price_micro_usd, source_mask, payout_key):
        self.version = version
        self.flags = flags
        self.price_micro_usd = price_micro_usd
        self.source_mask = source_mask
        self.payout_key = payout_key

    @property
    def signal(self):
        return bool(self.flags & 1)

    @property
    def is_quote(self):
        return self.price_micro_usd > 0

    @property
    def kind(self):
        return 'quote' if self.is_quote else 'signal'

    def __eq__(self, other):
        return isinstance(other, Tag) and self.as_tuple() == other.as_tuple()

    def as_tuple(self):
        return (self.version, self.flags, self.price_micro_usd, self.source_mask, self.payout_key)

    def __repr__(self):
        return 'Tag(v=%d flags=%d price=%d mask=%d key=%s)' % (
            self.version, self.flags, self.price_micro_usd, self.source_mask, self.payout_key.hex())


def encode_tag(flags, price_micro_usd, source_mask, payout_key, version=TAG_VERSION):
    """The 36-byte tag body."""
    assert len(payout_key) == 20
    return (TAG_MAGIC + bytes([version & 0xFF, flags & 0xFF]) + struct.pack('<Q', price_micro_usd)
            + struct.pack('<H', source_mask) + payout_key)


def tag_push(flags, price_micro_usd, source_mask, payout_key, version=TAG_VERSION):
    """``0x24 || 36 bytes`` (what a miner appends to the coinbase scriptSig after the height push)."""
    return b'\x24' + encode_tag(flags, price_micro_usd, source_mask, payout_key, version)


def tag_is_valid(tag, params):
    """TAG-2."""
    return (tag.version == TAG_VERSION and (tag.flags & 0xFE) == 0
            and (tag.price_micro_usd == 0
                 or params.price_min <= tag.price_micro_usd <= params.price_max))


def find_tag(script_sig, height, params):
    """TAG-1..5: skip ``CScript() << height``, find the first ``24 59 45 44 21``, read the 32 bytes
    after it, apply TAG-2.  Returns a valid Tag or None (invalid tag == no tag; never scans past
    the first occurrence, TAG-5)."""
    prefix = height_prefix(height)
    if len(script_sig) < len(prefix):
        return None
    rest = bytes(script_sig[len(prefix):])
    pos = rest.find(TAG_PATTERN)
    if pos < 0:
        return None
    body = rest[pos + len(TAG_PATTERN):pos + len(TAG_PATTERN) + 32]
    if len(body) < 32:
        return None
    version = body[0]
    flags = body[1]
    price = struct.unpack('<Q', body[2:10])[0]
    mask = struct.unpack('<H', body[10:12])[0]
    key = body[12:32]
    tag = Tag(version, flags, price, mask, key)
    if not tag_is_valid(tag, params):
        return None
    return tag


# ---------------------------------------------------------------------------
# section 3.3  Payload codec

class Payload(object):
    """A well-formed payload.  type in {PAYLOAD_MINT, PAYLOAD_TRANSFER, PAYLOAD_REDEEM}."""
    __slots__ = ('type', 'term_class', 'cents', 'lock_height', 'ref_height', 'owner_pubkey',
                 'fee_vout', 'assignments')

    def __init__(self, type_):
        self.type = type_
        self.term_class = None
        self.cents = None
        self.lock_height = None
        self.ref_height = None
        self.owner_pubkey = None
        self.fee_vout = None
        self.assignments = []      # list of (vout, cents)

    @property
    def type_name(self):
        return {PAYLOAD_MINT: 'MINT', PAYLOAD_TRANSFER: 'TRANSFER', PAYLOAD_REDEEM: 'REDEEM'}[self.type]


def encode_mint(term_class, cents, lock_height, ref_height, owner_pubkey, fee_vout):
    assert len(owner_pubkey) == 33
    return (PAYLOAD_MAGIC + bytes([PAYLOAD_VERSION, PAYLOAD_MINT, term_class & 0xFF])
            + struct.pack('<III', cents, lock_height, ref_height) + owner_pubkey + bytes([fee_vout & 0xFF]))


def encode_transfer(assignments):
    out = PAYLOAD_MAGIC + bytes([PAYLOAD_VERSION, PAYLOAD_TRANSFER, len(assignments)])
    for vout, cents in assignments:
        out += bytes([vout & 0xFF]) + struct.pack('<I', cents)
    return out


def encode_redeem(ref_height, fee_vout, assignments):
    out = (PAYLOAD_MAGIC + bytes([PAYLOAD_VERSION, PAYLOAD_REDEEM]) + struct.pack('<I', ref_height)
           + bytes([fee_vout & 0xFF, len(assignments)]))
    for vout, cents in assignments:
        out += bytes([vout & 0xFF]) + struct.pack('<I', cents)
    return out


def decode_payload(data, n_vout=None, opret_index=None):
    """Decode a payload body (the data of the OP_RETURN push).  Returns a Payload, or None when the
    body is malformed or of an unknown type/version (non-Yellowback).  With ``n_vout`` and
    ``opret_index`` the assignment checks of section 3.3 (vout in range, no duplicate, not the
    OP_RETURN, cents != 0) are applied too; without them only the byte-level shape is checked."""
    if len(data) < 4 or len(data) > MAX_PAYLOAD:
        return None
    if data[0:2] != PAYLOAD_MAGIC or data[2] != PAYLOAD_VERSION:
        return None
    t = data[3]
    body = data[4:]
    if t == PAYLOAD_MINT:
        if len(body) != 47:
            return None
        p = Payload(PAYLOAD_MINT)
        p.term_class = body[0]
        p.cents, p.lock_height, p.ref_height = struct.unpack('<III', body[1:13])
        p.owner_pubkey = bytes(body[13:46])
        p.fee_vout = body[46]
        return p
    if t == PAYLOAD_TRANSFER:
        if len(body) < 1:
            return None
        count = body[0]
        if len(body) != 1 + 5 * count:
            return None
        p = Payload(PAYLOAD_TRANSFER)
        rest = body[1:]
    elif t == PAYLOAD_REDEEM:
        if len(body) < 6:
            return None
        p = Payload(PAYLOAD_REDEEM)
        p.ref_height = struct.unpack('<I', body[0:4])[0]
        p.fee_vout = body[4]
        count = body[5]
        if len(body) != 6 + 5 * count:
            return None
        rest = body[6:]
    else:
        return None
    seen = set()
    for i in range(count):
        vout = rest[5 * i]
        cents = struct.unpack('<I', rest[5 * i + 1:5 * i + 5])[0]
        if cents == 0 or vout in seen:
            return None
        if n_vout is not None and vout >= n_vout:
            return None
        if opret_index is not None and vout == opret_index:
            return None
        seen.add(vout)
        p.assignments.append((vout, cents))
    return p


def tx_payload(vout_scripts):
    """The payload of a transaction given its scriptPubKeys: (Payload, opReturnIndex) when the
    transaction has exactly one OP_RETURN output of the section 3.3 shape carrying a well-formed
    payload, else (None, opReturnIndex-or-None) -- a non-Yellowback transaction."""
    opret = [i for i, s in enumerate(vout_scripts) if len(s) >= 1 and s[0] == OP_RETURN]
    if len(opret) != 1:
        return None, None
    idx = opret[0]
    data = script_single_push(vout_scripts[idx])
    if data is None:
        return None, idx
    return decode_payload(data, n_vout=len(vout_scripts), opret_index=idx), idx


# ---------------------------------------------------------------------------
# Transactions: the model's normalised shape, getblock-2 JSON and raw hex

class TxIn(object):
    __slots__ = ('prev_txid', 'prev_n', 'script_sig', 'sequence')

    def __init__(self, prev_txid, prev_n, script_sig, sequence=0xFFFFFFFF):
        self.prev_txid = prev_txid      # display-order hex, or None for a coinbase input
        self.prev_n = prev_n
        self.script_sig = script_sig    # bytes
        self.sequence = sequence


class TxOut(object):
    __slots__ = ('value', 'script')

    def __init__(self, value, script):
        self.value = value              # zat
        self.script = script            # bytes


class Tx(object):
    """A transaction as the model reads it: txid (display-order hex), transparent inputs, outputs."""
    __slots__ = ('txid', 'vin', 'vout', 'lock_time', 'expiry_height')

    def __init__(self, txid, vin, vout, lock_time=0, expiry_height=0):
        self.txid = txid
        self.vin = vin
        self.vout = vout
        self.lock_time = lock_time
        self.expiry_height = expiry_height

    @property
    def is_coinbase(self):
        return len(self.vin) == 1 and self.vin[0].prev_txid is None


def _zat(out):
    if 'valueZat' in out:
        return int(out['valueZat'])
    if 'valueSat' in out:
        return int(out['valueSat'])
    return int((Decimal(str(out['value'])) * COIN).to_integral_value())


def tx_from_json(d):
    """From a ``getblock <hash> 2`` / ``getrawtransaction <txid> 1`` transaction object."""
    vin = []
    for i in d['vin']:
        if 'coinbase' in i:
            vin.append(TxIn(None, 0xFFFFFFFF, bytes.fromhex(i['coinbase']), int(i.get('sequence', 0xFFFFFFFF))))
        else:
            vin.append(TxIn(i['txid'], int(i['vout']), bytes.fromhex(i['scriptSig']['hex']),
                            int(i.get('sequence', 0xFFFFFFFF))))
    vout = [TxOut(_zat(o), bytes.fromhex(o['scriptPubKey']['hex'])) for o in d['vout']]
    return Tx(d['txid'], vin, vout, int(d.get('locktime', 0)), int(d.get('expiryheight', 0)))


def _read_compact(b, i):
    n = b[i]
    i += 1
    if n < 253:
        return n, i
    if n == 253:
        return struct.unpack('<H', b[i:i + 2])[0], i + 2
    if n == 254:
        return struct.unpack('<I', b[i:i + 4])[0], i + 4
    return struct.unpack('<Q', b[i:i + 8])[0], i + 8


def _compact(n):
    if n < 253:
        return bytes([n])
    if n < 0x10000:
        return b'\xfd' + struct.pack('<H', n)
    if n < 0x100000000:
        return b'\xfe' + struct.pack('<I', n)
    return b'\xff' + struct.pack('<Q', n)


def tx_from_hex(h):
    """Parse a raw v4 (Sapling) or pre-Overwinter transaction; shielded components are skipped
    (TX-0).  Only what the model reads is kept."""
    b = bytes.fromhex(h)
    txid = hash256(b)[::-1].hex()
    header = struct.unpack('<I', b[0:4])[0]
    overwintered = bool(header >> 31)
    version = header & 0x7FFFFFFF
    i = 4
    if overwintered:
        i += 4  # nVersionGroupId
    n, i = _read_compact(b, i)
    vin = []
    for _ in range(n):
        prev = b[i:i + 32][::-1].hex()
        pn = struct.unpack('<I', b[i + 32:i + 36])[0]
        i += 36
        sl, i = _read_compact(b, i)
        ss = b[i:i + sl]
        i += sl
        seq = struct.unpack('<I', b[i:i + 4])[0]
        i += 4
        is_cb = prev == '00' * 32 and pn == 0xFFFFFFFF
        vin.append(TxIn(None if is_cb else prev, pn, ss, seq))
    n, i = _read_compact(b, i)
    vout = []
    for _ in range(n):
        val = struct.unpack('<q', b[i:i + 8])[0]
        i += 8
        sl, i = _read_compact(b, i)
        vout.append(TxOut(val, b[i:i + sl]))
        i += sl
    lock_time = struct.unpack('<I', b[i:i + 4])[0]
    i += 4
    expiry = 0
    if overwintered:
        expiry = struct.unpack('<I', b[i:i + 4])[0]
        i += 4
    # the rest (valueBalance, shielded spends/outputs, joinsplits, signatures) is ignored
    _ = version
    return Tx(txid, vin, vout, lock_time, expiry)


def serialize_tx_v4(vin, vout, lock_time=0, expiry_height=0):
    """A transparent-only Sapling (v4) transaction: what the wallet builds on every network today.
    vin: [(prev_txid_hex or None, n, script_sig_bytes, sequence)], vout: [(value, script)]."""
    r = struct.pack('<I', (1 << 31) | SAPLING_TX_VERSION) + struct.pack('<I', SAPLING_VERSION_GROUP_ID)
    r += _compact(len(vin))
    for prev, n, ss, seq in vin:
        r += (bytes(32) if prev is None else bytes.fromhex(prev)[::-1]) + struct.pack('<I', n)
        r += _compact(len(ss)) + ss + struct.pack('<I', seq)
    r += _compact(len(vout))
    for val, s in vout:
        r += struct.pack('<q', val) + _compact(len(s)) + s
    r += struct.pack('<I', lock_time) + struct.pack('<I', expiry_height)
    r += struct.pack('<q', 0)       # valueBalance
    r += _compact(0) + _compact(0)  # vShieldedSpend, vShieldedOutput
    r += _compact(0)                # vJoinSplit
    return r


# ---------------------------------------------------------------------------
# Regtest subsidy schedule (ref/ycash/src/main.cpp GetBlockSubsidy, consensus/params.cpp Halving)

def regtest_subsidy(height, blossom_height=1):
    """GetBlockSubsidy on regtest: slow start 0, pre-Blossom halving 144, post 288, Blossom at
    ``blossom_height`` (the functional tests activate every upgrade at 1)."""
    n = 125 * COIN // 10
    if blossom_height is not None and height >= blossom_height:
        scaled = (blossom_height * 2) + (height - blossom_height)
        halvings = scaled // 288
        if halvings >= 64:
            return 0
        return (n // 2) >> halvings
    halvings = height // 144
    if halvings >= 64:
        return 0
    return n >> halvings


# ---------------------------------------------------------------------------
# The state (section 3.6)

class TagRecord(object):
    __slots__ = ('payout_key', 'price_micro_usd', 'signal', 'source_mask')

    def __init__(self, payout_key, price_micro_usd, signal, source_mask):
        self.payout_key = payout_key
        self.price_micro_usd = price_micro_usd
        self.signal = signal
        self.source_mask = source_mask

    @property
    def is_quote(self):
        return self.price_micro_usd > 0


class Judgement(object):
    __slots__ = ('evaluated', 'in_band', 'penalized')

    def __init__(self, evaluated=False, in_band=False, penalized=False):
        self.evaluated = evaluated
        self.in_band = in_band
        self.penalized = penalized


class Activation(object):
    __slots__ = ('status', 'lock_in_height', 'activate_height')

    def __init__(self, status=SIGNALING, lock_in_height=0, activate_height=0):
        self.status = status
        self.lock_in_height = lock_in_height
        self.activate_height = activate_height

    def copy(self):
        return Activation(self.status, self.lock_in_height, self.activate_height)

    def as_dict(self):
        return {'status': ACTIVATION_NAMES[self.status], 'lockInHeight': self.lock_in_height,
                'activateHeight': self.activate_height}


class Vault(object):
    __slots__ = ('owner_pubkey', 'term_class', 'lock_height', 'claim_height', 'collateral_zat',
                 'minted_cents', 'mint_height', 'ref_height', 'status', 'void_reason',
                 'close_height', 'closing_txid', 'burned_cents', 'fee_paid_zat', 'unbacked')

    def __init__(self):
        self.owner_pubkey = b''
        self.term_class = 0
        self.lock_height = 0
        self.claim_height = 0
        self.collateral_zat = 0
        self.minted_cents = 0
        self.mint_height = 0
        self.ref_height = 0
        self.status = V_VOID
        self.void_reason = ''
        self.close_height = 0
        self.closing_txid = None      # display-order hex or None
        self.burned_cents = 0
        self.fee_paid_zat = 0
        self.unbacked = False

    def as_dict(self, outpoint):
        return {'txid': outpoint[0], 'vout': outpoint[1], 'status': VAULT_STATUS_NAMES[self.status],
                'ownerPubKey': self.owner_pubkey.hex(), 'termClass': 'ABC'[self.term_class] if self.term_class < 3 else self.term_class,
                'lockHeight': self.lock_height, 'claimHeight': self.claim_height,
                'collateralZat': self.collateral_zat, 'mintedCents': self.minted_cents,
                'mintHeight': self.mint_height, 'refHeight': self.ref_height,
                'feePaidZat': self.fee_paid_zat, 'closeHeight': self.close_height,
                'closingTxid': self.closing_txid, 'burnedCents': self.burned_cents,
                'unbacked': self.unbacked, 'voidReason': self.void_reason}


class Token(object):
    __slots__ = ('cents', 'n_value', 'script_pub_key', 'height')

    def __init__(self, cents, n_value, script_pub_key, height):
        self.cents = cents
        self.n_value = n_value
        self.script_pub_key = script_pub_key
        self.height = height


class TxLogRecord(object):
    __slots__ = ('height', 'type', 'path', 'verdict', 'yed_in', 'yed_out', 'burned', 'fee_zat',
                 'payee', 'assigned', 'spent_tokens', 'closed_vaults')

    def __init__(self, height):
        self.height = height
        self.type = 'NONE'
        self.path = ''
        self.verdict = VERDICT_OK
        self.yed_in = 0
        self.yed_out = 0
        self.burned = 0
        self.fee_zat = 0
        self.payee = None           # 20-byte key hash or None
        self.assigned = []          # [(vout, cents)]
        self.spent_tokens = []      # [(txid, n)]
        self.closed_vaults = []     # [(txid, n)]

    def as_dict(self, txid):
        return {'txid': txid, 'height': self.height, 'type': self.type, 'path': self.path,
                'verdict': self.verdict, 'yedIn': self.yed_in, 'yedOut': self.yed_out,
                'burned': self.burned, 'feeZat': self.fee_zat,
                'payee': self.payee.hex() if self.payee else None,
                'assigned': [{'vout': v, 'cents': c} for v, c in self.assigned],
                'spentTokens': ['%s:%d' % op for op in self.spent_tokens],
                'closedVaults': ['%s:%d' % op for op in self.closed_vaults]}


class Totals(object):
    __slots__ = ('supply_cents', 'collateral_zat', 'active_vaults', 'void_vaults', 'closed_vaults',
                 'claimed_vaults', 'unbacked_cents')

    def __init__(self):
        self.supply_cents = 0
        self.collateral_zat = 0
        self.active_vaults = 0
        self.void_vaults = 0
        self.closed_vaults = 0
        self.claimed_vaults = 0
        self.unbacked_cents = 0

    def as_dict(self):
        return {'supplyCents': self.supply_cents, 'collateralZat': self.collateral_zat,
                'activeVaults': self.active_vaults, 'voidVaults': self.void_vaults,
                'closedVaults': self.closed_vaults, 'claimedVaults': self.claimed_vaults,
                'unbackedCents': self.unbacked_cents}


class Snapshot(object):
    __slots__ = ('block_hash', 'tagged', 'quote', 'signal_count', 'activation', 'p_fast', 'p_mid',
                 'p_slow', 'p_mint', 'p_claim', 'sigma_mult_bps', 'issued_zat', 'supply_cents',
                 'collateral_zat', 'global_ratio_bps', 'halt_mask', 'virtual')

    def __init__(self):
        self.block_hash = '00' * 32
        self.tagged = False
        self.quote = False
        self.signal_count = 0
        self.activation = Activation()
        self.p_fast = None
        self.p_mid = None
        self.p_slow = None
        self.p_mint = None
        self.p_claim = None
        self.sigma_mult_bps = BPS
        self.issued_zat = 0
        self.supply_cents = 0
        self.collateral_zat = 0
        self.global_ratio_bps = None
        self.halt_mask = 0
        self.virtual = False

    @classmethod
    def virtual_snapshot(cls):
        """The virtual snapshot below START_HEIGHT (section 3.6)."""
        s = cls()
        s.halt_mask = HALT_NOT_ACTIVE | HALT_NO_PRICE
        s.virtual = True
        return s

    def halt_names(self):
        return [name for bit, name in HALT_NAMES.items() if self.halt_mask & bit]

    def as_dict(self, height):
        return {'height': height, 'blockHash': self.block_hash, 'tagged': self.tagged, 'quote': self.quote,
                'signalCount': self.signal_count, 'activation': self.activation.as_dict(),
                'pFast': self.p_fast, 'pMid': self.p_mid, 'pSlow': self.p_slow,
                'pMint': self.p_mint, 'pClaim': self.p_claim, 'sigmaMultBps': self.sigma_mult_bps,
                'issuedZat': self.issued_zat, 'supplyCents': self.supply_cents,
                'collateralZat': self.collateral_zat, 'globalRatioBps': self.global_ratio_bps,
                'haltMask': self.halt_names()}


class BlockVerdict(object):
    __slots__ = ('height', 'block_hash', 'block_invalid', 'enforcement_on', 'reason')

    def __init__(self, height, block_hash, block_invalid, enforcement_on, reason):
        self.height = height
        self.block_hash = block_hash
        self.block_invalid = block_invalid
        self.enforcement_on = enforcement_on
        self.reason = reason

    @property
    def rejected(self):
        """What BLK-2 clause 1 would do on an enforcing node outside IBD (ACT-7/L11 aside)."""
        return self.block_invalid and self.enforcement_on


# ---------------------------------------------------------------------------
# Serialisation for the state hash (section 3.6, SERIALISATION.md)

def _u8(v):
    return struct.pack('<B', v & 0xFF)


def _u16(v):
    return struct.pack('<H', v)


def _u32(v):
    return struct.pack('<I', v)


def _i32(v):
    return struct.pack('<i', v)


def _i64(v):
    return struct.pack('<q', v)


def _u64(v):
    return struct.pack('<Q', v)


def _u32be(v):
    return struct.pack('>I', v)


def _bool(v):
    return _u8(1 if v else 0)


def _str(s):
    b = s.encode('utf-8')
    return _compact(len(b)) + b


def _bytes(b):
    return _compact(len(b)) + bytes(b)


def _hash(hex_display):
    """uint256 raw bytes: the internal byte order (the display hex reversed)."""
    if not hex_display:
        return bytes(32)
    return bytes.fromhex(hex_display)[::-1]


def _outpoint_key(prefix, outpoint):
    return prefix + _hash(outpoint[0]) + _u32be(outpoint[1])


def _price(v):
    return _i64(0 if v is None else v)


def _ser_activation(a):
    return _u8(a.status) + _i32(a.lock_in_height) + _i32(a.activate_height)


def _ser_snapshot(s):
    return (_hash(s.block_hash) + _bool(s.tagged) + _bool(s.quote) + _u32(s.signal_count)
            + _ser_activation(s.activation) + _price(s.p_fast) + _price(s.p_mid) + _price(s.p_slow)
            + _price(s.p_mint) + _price(s.p_claim) + _i32(s.sigma_mult_bps) + _i64(s.issued_zat)
            + _i64(s.supply_cents) + _i64(s.collateral_zat)
            + _i64(0 if s.global_ratio_bps is None else s.global_ratio_bps) + _u32(s.halt_mask))


def _ser_vault(v):
    return (_bytes(v.owner_pubkey) + _u8(v.term_class) + _i32(v.lock_height) + _i32(v.claim_height)
            + _i64(v.collateral_zat) + _i64(v.minted_cents) + _i32(v.mint_height) + _i32(v.ref_height)
            + _u8(v.status) + _str(v.void_reason) + _i32(v.close_height) + _hash(v.closing_txid)
            + _i64(v.burned_cents) + _i64(v.fee_paid_zat) + _bool(v.unbacked))


def _ser_token(t):
    return _i64(t.cents) + _i64(t.n_value) + _bytes(t.script_pub_key) + _i32(t.height)


def _ser_totals(t):
    return (_i64(t.supply_cents) + _i64(t.collateral_zat) + _u32(t.active_vaults) + _u32(t.void_vaults)
            + _u32(t.closed_vaults) + _u32(t.claimed_vaults) + _i64(t.unbacked_cents))


def _outpoint_sort_key(op):
    return (_hash(op[0]), op[1])


# ---------------------------------------------------------------------------
# The model

class YellowbackModel(object):
    """Section 3 as a state machine fed block by block.  See the module docstring."""

    SCHEMA_VERSION = 2

    def __init__(self, params, issued_before_start=0):
        self.params = params
        self.tip_height = -1
        self.tip_hash = None
        self.tags = {}            # height -> TagRecord
        self.judgements = {}      # height -> Judgement
        self.activation = Activation()
        self.vaults = {}          # (txid, n) -> Vault
        self.tokens = {}          # (txid, n) -> Token
        self.txlog = OrderedDict()  # txid -> TxLogRecord
        self.totals = Totals()
        self.snapshots = {}       # height -> Snapshot
        self.blocks = {}          # height -> BlockVerdict
        # issuedZat is carried from the previous snapshot; the virtual snapshot below START_HEIGHT
        # carries ``issued_before_start`` (default 0 = the sum over [START_HEIGHT, H]; see SERIALISATION.md)
        self._issued_below_start = issued_before_start

    # -- lookups ------------------------------------------------------------

    def snapshot(self, height):
        """Snapshots[height]: the virtual snapshot below START_HEIGHT, None when above the tip."""
        if height < self.params.start_height:
            s = Snapshot.virtual_snapshot()
            s.issued_zat = self._issued_below_start
            return s
        return self.snapshots.get(height)

    def txinfo(self, txid):
        rec = self.txlog.get(txid)
        return None if rec is None else rec.as_dict(txid)

    def vault(self, outpoint):
        v = self.vaults.get(outpoint)
        return None if v is None else v.as_dict(outpoint)

    def vault_list(self, status=None):
        rows = []
        for op in sorted(self.vaults, key=_outpoint_sort_key):
            v = self.vaults[op]
            if status is None or VAULT_STATUS_NAMES[v.status] == status.upper():
                rows.append(v.as_dict(op))
        return rows

    vaults_list = vault_list

    def stats(self):
        """The yed_getstats shape (section 4.5) at the tip."""
        s = self.snapshot(self.tip_height) if self.tip_height >= self.params.start_height else None
        d = {'height': self.tip_height}
        d.update(self.totals.as_dict())
        if s is None:
            s = Snapshot.virtual_snapshot()
        d.update({'issuedZat': s.issued_zat, 'pFast': s.p_fast, 'pMid': s.p_mid, 'pSlow': s.p_slow,
                  'pMint': s.p_mint, 'pClaim': s.p_claim, 'sigmaMultBps': s.sigma_mult_bps,
                  'globalRatioBps': s.global_ratio_bps,
                  'supplyCapCents': supply_cap_cents(s.issued_zat, s.p_mint, self.params.supply_cap_bps),
                  'haltMask': s.halt_names(), 'mintingAllowed': s.activation.status == ACTIVE and s.halt_mask == 0})
        return d

    def block_verdict(self, height):
        return self.blocks.get(height)

    def enforcement_on(self, height):
        """ACT-5 at height H, read from Snapshots[H - 1]."""
        prev = self.snapshot(height - 1)
        if prev is None or prev.virtual:
            return False
        if prev.activation.status != ACTIVE or (prev.halt_mask & HALT_ENFORCEMENT):
            return False
        if self.params.enforce_until and height > self.params.enforce_until:
            return False
        return True

    def is_abandoned(self):
        """The section 4.6 predicate (L10, L12): ENFORCEMENT set continuously for ABANDON_BLOCKS at the tip."""
        n = self.params.abandon_blocks
        if self.tip_height - n + 1 < self.params.start_height:
            return False
        for h in range(self.tip_height - n + 1, self.tip_height + 1):
            s = self.snapshots.get(h)
            if s is None or not (s.halt_mask & HALT_ENFORCEMENT):
                return False
        return True

    # -- section 3.7 derived quantities ------------------------------------

    def _quote_prices(self, lo, hi):
        """priceMicroUsd of the quote tags with lo < h <= hi."""
        out = []
        for h in range(max(lo + 1, self.params.start_height), hi + 1):
            t = self.tags.get(h)
            if t is not None and t.is_quote:
                out.append(t.price_micro_usd)
        return out

    def median(self, window, min_fill, height):
        vals = self._quote_prices(height - window, height)
        if len(vals) < min_fill:
            return None
        return lower_median(vals)

    def signal_count(self, height):
        n = 0
        for h in range(max(height - self.params.signal_window + 1, self.params.start_height), height + 1):
            t = self.tags.get(h)
            if t is not None and t.signal:
                n += 1
        return n

    def eligible_payees(self, ref_height):
        """E(R): the payoutKeys of the quote tags at h in (R - PAYEE_WINDOW, R], height order, deduplicated."""
        keys = []
        for h in range(max(ref_height - self.params.payee_window + 1, self.params.start_height), ref_height + 1):
            t = self.tags.get(h)
            if t is not None and t.is_quote and t.payout_key not in keys:
                keys.append(t.payout_key)
        return keys

    def registered(self, key, ref_height):
        for h in range(max(ref_height - self.params.n_reg + 1, self.params.start_height), ref_height + 1):
            t = self.tags.get(h)
            if t is not None and t.is_quote and t.payout_key == key:
                return True
        return False

    def penalized(self, key, ref_height, n_penalty=None):
        n_penalty = self.params.n_penalty if n_penalty is None else n_penalty
        lag = self.params.peer_lag
        for t, tag in self.tags.items():
            if tag.is_quote and tag.payout_key == key and t + lag < ref_height <= t + lag + n_penalty:
                j = self.judgements.get(t)
                if j is not None and j.penalized:
                    return True
        return False

    def accuracy_bps(self, key, ref_height, accuracy_window=None):
        w = self.params.accuracy_window if accuracy_window is None else accuracy_window
        lag = self.params.peer_lag
        quoted = in_band = 0
        for t in range(ref_height - lag - w + 1, ref_height - lag + 1):
            tag = self.tags.get(t)
            j = self.judgements.get(t)
            if tag is not None and tag.is_quote and tag.payout_key == key and j is not None and j.evaluated:
                quoted += 1
                if j.in_band:
                    in_band += 1
        return (BPS * in_band) // quoted if quoted else 0

    def _judge(self, height):
        """REG-4 for the quote tag at t = H - PEER_LAG."""
        lag = self.params.peer_lag
        t = height - lag
        tag = self.tags.get(t)
        if t < self.params.start_height or tag is None or not tag.is_quote:
            return
        peers = []
        for h in range(t - lag, t + lag):
            if h == t:
                continue
            p = self.tags.get(h)
            if p is not None and p.is_quote:
                peers.append(p.price_micro_usd)
        j = Judgement()
        if len(peers) >= self.params.peer_min:
            m = lower_median(peers)
            dev = (abs(tag.price_micro_usd - m) * BPS) // m
            j.evaluated = True
            j.in_band = dev <= self.params.accuracy_band_bps
            j.penalized = dev > self.params.deviation_bps
        self.judgements[t] = j

    def _sigma(self, height, p_fast_now):
        p = self.params
        samples = [p_fast_now]
        for k in range(1, p.vol_window // p.vol_step + 1):
            s = self.snapshot(height - k * p.vol_step)
            samples.append(None if (s is None or s.virtual) else s.p_fast)
        return sigma_mult_bps(samples, p.sigma_ref_bps, p.vol_periods_per_year, p.sigma_mult_max_bps)

    # -- feeding ------------------------------------------------------------

    def feed_block_json(self, block, subsidy_zat):
        """Feed a ``getblock <hash> 2`` result."""
        txs = [tx_from_json(t) if isinstance(t, dict) else t for t in block['tx']]
        cb = txs[0].vin[0].script_sig.hex()
        return self.feed_block(int(block['height']), block['hash'], cb, subsidy_zat, txs[1:])

    def feed_block(self, height, block_hash, coinbase_scriptsig_hex, subsidy_zat, txs):
        """Apply one block.  ``txs`` are the non-coinbase transactions as getblock-2 dicts or Tx
        objects (a leading coinbase Tx is skipped).  Returns the BlockVerdict (None below START_HEIGHT)."""
        p = self.params
        if self.tip_height >= 0 and height != self.tip_height + 1:
            raise ValueError('block %d fed after tip %d: blocks must arrive in order (no reorg support)' % (height, self.tip_height))
        if height < p.start_height:
            # Blocks below START_HEIGHT are ignored completely (section 3.8); only the tip moves on.
            self.tip_height = height
            return None
        txs = [tx_from_json(t) if isinstance(t, dict) else t for t in txs]
        txs = [t for t in txs if not t.is_coinbase]

        # TAG-1..5
        tag = find_tag(bytes.fromhex(coinbase_scriptsig_hex), height, p)
        if tag is not None:
            self.tags[height] = TagRecord(tag.payout_key, tag.price_micro_usd, tag.signal, tag.source_mask)

        enforcing = self.enforcement_on(height)
        block_invalid = False
        reason = ''
        for tx in txs:
            failed = self._apply_tx(tx, height)
            if failed and not block_invalid:
                block_invalid = True
                reason = '%s:%s' % (failed, tx.txid)

        # SNAP
        self._snap(height, block_hash, subsidy_zat, tag)
        self.tip_height = height
        self.tip_hash = block_hash
        verdict = BlockVerdict(height, block_hash, block_invalid, enforcing, reason)
        self.blocks[height] = verdict
        return verdict

    def _snap(self, height, block_hash, subsidy_zat, tag):
        p = self.params
        self._judge(height)
        # ACT-1..3
        count = self.signal_count(height)
        a = self.activation
        if a.status == SIGNALING and height >= p.start_height + p.signal_window - 1 and count >= p.activation_threshold:
            a.status = LOCKED_IN
            a.lock_in_height = height
            a.activate_height = height + p.activation_delay
        if a.status == LOCKED_IN and height >= a.activate_height:
            a.status = ACTIVE
        s = Snapshot()
        s.block_hash = block_hash
        s.tagged = tag is not None
        s.quote = tag is not None and tag.is_quote
        s.signal_count = count
        s.activation = a.copy()
        s.p_fast = self.median(p.p_fast_window, p.min_fill_fast, height)
        s.p_mid = self.median(p.p_mid_window, p.min_fill_mid, height)
        s.p_slow = self.median(p.p_slow_window, p.min_fill_slow, height)
        if None not in (s.p_fast, s.p_mid, s.p_slow):
            s.p_mint = min(s.p_fast, s.p_mid, s.p_slow)
        if None not in (s.p_mid, s.p_slow):
            s.p_claim = max(s.p_mid, s.p_slow)
        s.sigma_mult_bps = self._sigma(height, s.p_fast)
        prev = self.snapshot(height - 1)
        s.issued_zat = prev.issued_zat + subsidy_zat
        s.supply_cents = self.totals.supply_cents
        s.collateral_zat = self.totals.collateral_zat
        s.global_ratio_bps = global_ratio_bps(s.collateral_zat, s.p_mint, s.supply_cents)
        # HALT-1..4, ACT-4, ACT-6
        mask = 0
        if a.status != ACTIVE:
            mask |= HALT_NOT_ACTIVE
        if s.p_mint is None:
            mask |= HALT_NO_PRICE
        if s.p_mint is not None and s.supply_cents > 0 and s.global_ratio_bps < p.global_ratio_halt_bps:
            mask |= HALT_GLOBAL_RATIO
        if None not in (s.p_fast, s.p_mid, s.p_slow):
            if (s.p_fast * BPS < (BPS - p.divergence_bps) * s.p_mid
                    or s.p_mid * BPS < (BPS - p.divergence_bps) * s.p_slow):
                mask |= HALT_DIVERGENCE
        part = bool(prev.halt_mask & HALT_PARTICIPATION)
        if part:
            part = count < p.activation_threshold
        if a.status == ACTIVE and count < p.participation_floor:
            part = True
        if part:
            mask |= HALT_PARTICIPATION
        enf = bool(prev.halt_mask & HALT_ENFORCEMENT)
        if enf:
            enf = count < p.enforcement_resume
        if a.status == ACTIVE and count < p.enforcement_floor:
            enf = True
        if enf:
            mask |= HALT_ENFORCEMENT
        s.halt_mask = mask
        self.snapshots[height] = s

    # -- section 3.8 ---------------------------------------------------------

    def _apply_tx(self, tx, height):
        """IN-1..3, TX-0, MINT/XFER/RED.  Returns the failing RED verdict (BLK-1 condition) or None."""
        rec = TxLogRecord(height)
        outpoints = [(i.prev_txid, i.prev_n) for i in tx.vin]
        # IN-1
        yed_in = 0
        for op in outpoints:
            t = self.tokens.pop(op, None)
            if t is not None:
                yed_in += t.cents
                rec.spent_tokens.append(op)
        rec.yed_in = yed_in
        active_spent = [op for op in outpoints if op in self.vaults and self.vaults[op].status == V_ACTIVE]
        void_spent = [op for op in outpoints if op in self.vaults and self.vaults[op].status == V_VOID]
        scripts = [o.script for o in tx.vout]
        payload, opret = tx_payload(scripts)

        failing = None
        touched = bool(rec.spent_tokens) or bool(active_spent) or bool(void_spent)
        if active_spent:
            # M3: RED-1..4 only, whatever the payload
            rec.type = 'REDEEM'
            failing = self._apply_vault_spend(tx, height, rec, payload, opret, active_spent, outpoints, yed_in)
            touched = True
        elif payload is not None and payload.type == PAYLOAD_MINT:
            rec.type = 'MINT'
            created = self._apply_mint(tx, height, rec, payload, opret)
            touched = touched or created
        elif payload is not None:
            rec.type = payload.type_name
            self._apply_transfer(tx, height, rec, payload, yed_in)
        else:
            rec.type = 'NONE'
            if yed_in > 0:
                rec.verdict = VERDICT_BURNED

        # IN-2 for VOID vaults: an ordinary spend that closes them (K3)
        for op in void_spent:
            v = self.vaults[op]
            v.status = V_CLOSED
            v.close_height = height
            v.closing_txid = tx.txid
            v.unbacked = False
            self.totals.void_vaults -= 1
            self.totals.closed_vaults += 1
            rec.closed_vaults.append(op)

        # IN-3 (for a MINT the burn formula's yedOut is 0, N19; _apply_mint set rec.yed_out = cents separately)
        burn_out = 0 if rec.type == 'MINT' else rec.yed_out
        burned = yed_in - burn_out
        rec.burned = burned
        self.totals.supply_cents -= burned
        for op in rec.closed_vaults:
            self.vaults[op].burned_cents = burned
        if rec.type == 'REDEEM' and active_spent:
            for op in active_spent:
                v = self.vaults[op]
                if v.status == V_CLOSED and failing is not None:
                    v.unbacked = burned < v.minted_cents
                    self.totals.unbacked_cents += max(0, v.minted_cents - burned)
        if touched:
            self.txlog[tx.txid] = rec
        return failing

    def _apply_mint(self, tx, height, rec, pl, opret):
        """MINT-1..8.  Returns True when a Vaults entry was created (ACTIVE or VOID)."""
        p = self.params
        verdict = self._mint_verdict(tx, height, pl, opret)
        vout0 = tx.vout[0] if tx.vout else None
        if verdict == VERDICT_OK:
            v = Vault()
            v.owner_pubkey = pl.owner_pubkey
            v.term_class = pl.term_class
            v.lock_height = pl.lock_height
            v.claim_height = pl.lock_height + p.grace
            v.collateral_zat = vout0.value
            v.minted_cents = pl.cents
            v.mint_height = height
            v.ref_height = pl.ref_height
            v.status = V_ACTIVE
            fee_applies = bool(self.eligible_payees(pl.ref_height))
            if fee_applies:
                v.fee_paid_zat = tx.vout[pl.fee_vout].value
                rec.fee_zat = v.fee_paid_zat
                rec.payee = p2pkh_key(tx.vout[pl.fee_vout].script)
            self.vaults[(tx.txid, 0)] = v
            self.tokens[(tx.txid, 1)] = Token(pl.cents, tx.vout[1].value, tx.vout[1].script, height)
            self.totals.supply_cents += pl.cents
            self.totals.collateral_zat += vout0.value
            self.totals.active_vaults += 1
            rec.yed_out = pl.cents
            rec.assigned = [(1, pl.cents)]
            rec.verdict = VERDICT_OK
            return True
        rec.verdict = verdict
        rec.yed_out = 0
        if vout0 is not None and is_p2sh(vout0.script):
            v = Vault()
            v.owner_pubkey = pl.owner_pubkey
            v.term_class = pl.term_class
            v.lock_height = pl.lock_height
            v.claim_height = pl.lock_height + p.grace
            v.collateral_zat = vout0.value
            v.minted_cents = pl.cents
            v.mint_height = height
            v.ref_height = pl.ref_height
            v.status = V_VOID
            v.void_reason = verdict
            self.vaults[(tx.txid, 0)] = v
            self.totals.void_vaults += 1
            return True
        return False

    def _mint_verdict(self, tx, height, pl, opret):
        p = self.params
        # MINT-2 (signed arithmetic)
        rng = p.class_range(pl.term_class)
        if rng is None:
            return 'bad-mint-class'
        if not (p.min_mint <= pl.cents <= p.max_mint):
            return 'bad-mint-amount'
        if not (pl.lock_height + p.grace < LOCKTIME_THRESHOLD):
            return 'bad-mint-lock-height'
        if not (height - p.ref_window <= pl.ref_height <= height - 1) or pl.ref_height < p.start_height:
            return 'bad-mint-ref-height'
        if not (pl.lock_height > pl.ref_height) or not (rng[0] <= pl.lock_height - pl.ref_height <= rng[1]):
            return 'bad-mint-lock-height'
        # MINT-3
        if len(tx.vout) < 3:
            return 'bad-mint-outputs'
        if not is_valid_compressed_pubkey(pl.owner_pubkey):
            return 'bad-mint-owner-key'
        expected = p2sh_script(vault_script(pl.lock_height, pl.owner_pubkey, pl.lock_height + p.grace))
        if tx.vout[0].script != expected:
            return 'bad-mint-vault-script'
        # MINT-4
        s = self.snapshot(pl.ref_height)
        if s is None or s.virtual or s.activation.status != ACTIVE:
            return 'mint-not-active'
        if s.halt_mask & HALT_NOT_ACTIVE:
            return 'mint-not-active'
        if s.halt_mask & HALT_NO_PRICE:
            return 'mint-halted-no-price'
        if s.halt_mask & (HALT_PARTICIPATION | HALT_ENFORCEMENT):
            return 'mint-halted-participation'
        if s.halt_mask & HALT_GLOBAL_RATIO:
            return 'mint-halted-global-ratio'
        if s.halt_mask & HALT_DIVERGENCE:
            return 'mint-halted-divergence'
        # MINT-5
        req = required_zat(pl.cents, min_ratio_bps(p.base_ratio_bps[pl.term_class], s.sigma_mult_bps), s.p_mint)
        if req is None:
            return 'mint-unsatisfiable'
        if tx.vout[0].value < req or tx.vout[0].value < 4 * p.fee_min:
            return 'bad-mint-collateral'
        # MINT-6
        cap = supply_cap_cents(s.issued_zat, s.p_mint, p.supply_cap_bps)
        if cap is not None and self.totals.supply_cents + pl.cents > cap:
            return 'mint-supply-cap'
        # MINT-7
        if opret == 1:
            return 'bad-mint-token-output'
        # MINT-8
        eligible = self.eligible_payees(pl.ref_height)
        if eligible:
            fv = pl.fee_vout
            if fv == FEE_VOUT_NONE or fv >= len(tx.vout) or fv in (0, 1, opret):
                return 'bad-mint-fee'
            key = p2pkh_key(tx.vout[fv].script)
            if key is None or key not in eligible:
                return 'bad-mint-fee'
            if tx.vout[fv].value < fee_zat(tx.vout[0].value, p.fee_min, p.fee_bps):
                return 'bad-mint-fee'
        return VERDICT_OK

    def _apply_transfer(self, tx, height, rec, pl, yed_in):
        """XFER-1..3 for a TRANSFER or a REDEEM payload that spends no ACTIVE vault."""
        p = self.params
        total = sum(c for _, c in pl.assignments)
        verdict = VERDICT_OK
        for _, cents in pl.assignments:
            if not (p.min_output <= cents <= p.max_output):
                verdict = 'bad-transfer-assignment'
                break
        if verdict == VERDICT_OK and total > yed_in:
            verdict = 'transfer-over-assigned'
        if verdict == VERDICT_OK and yed_in <= 0:
            verdict = 'transfer-no-yed-input'
        if verdict != VERDICT_OK:
            rec.verdict = verdict
            rec.yed_out = 0
            return
        for vout, cents in pl.assignments:
            self.tokens[(tx.txid, vout)] = Token(cents, tx.vout[vout].value, tx.vout[vout].script, height)
        rec.assigned = list(pl.assignments)
        rec.yed_out = total
        rec.verdict = VERDICT_BURNED if total < yed_in else VERDICT_OK

    def _apply_vault_spend(self, tx, height, rec, pl, opret, active_spent, outpoints, yed_in):
        """RED-1..4 over a transaction that spends at least one ACTIVE vault.  Returns the failing
        verdict or None; applies IN-2 either way."""
        path = spend_path(tx.vin[0].script_sig)
        rec.path = path or ''
        verdict = self._red_verdict(tx, height, pl, opret, active_spent, outpoints, path, yed_in)
        vault_op = outpoints[0]
        if verdict == VERDICT_OK:
            v = self.vaults[vault_op]
            total = sum(c for _, c in pl.assignments)
            for vout, cents in pl.assignments:
                self.tokens[(tx.txid, vout)] = Token(cents, tx.vout[vout].value, tx.vout[vout].script, height)
            rec.assigned = list(pl.assignments)
            rec.yed_out = total
            rec.verdict = VERDICT_OK
            v.status = V_CLOSED if path == 'owner' else V_CLAIMED
            v.close_height = height
            v.closing_txid = tx.txid
            v.unbacked = False
            # feePaidZat is rewritten by the close: the fee this spend paid (0 under FEE-0); see SERIALISATION.md
            v.fee_paid_zat = 0
            if self.eligible_payees(pl.ref_height):
                v.fee_paid_zat = tx.vout[pl.fee_vout].value
                rec.fee_zat = v.fee_paid_zat
                rec.payee = p2pkh_key(tx.vout[pl.fee_vout].script)
            self.totals.collateral_zat -= v.collateral_zat
            self.totals.active_vaults -= 1
            if path == 'owner':
                self.totals.closed_vaults += 1
            else:
                self.totals.claimed_vaults += 1
            rec.closed_vaults.append(vault_op)
            return None
        rec.verdict = verdict
        rec.yed_out = 0
        for op in active_spent:
            v = self.vaults[op]
            v.status = V_CLOSED
            v.close_height = height
            v.closing_txid = tx.txid
            self.totals.collateral_zat -= v.collateral_zat
            self.totals.active_vaults -= 1
            self.totals.closed_vaults += 1
            rec.closed_vaults.append(op)
        return verdict

    def _red_verdict(self, tx, height, pl, opret, active_spent, outpoints, path, yed_in):
        p = self.params
        # RED-1
        if outpoints[0] not in active_spent or len(active_spent) != 1:
            return 'vault-spend-malformed'
        if path is None:
            return 'vault-spend-malformed'
        if pl is None or pl.type != PAYLOAD_REDEEM:
            return 'vault-spend-malformed'
        if not (height - p.ref_window <= pl.ref_height <= height - 1) or pl.ref_height < p.start_height:
            return 'vault-spend-malformed'
        for _, cents in pl.assignments:
            if not (p.min_output <= cents <= p.max_output):
                return 'vault-spend-malformed'
        vault = self.vaults[outpoints[0]]
        # RED-2
        total = sum(c for _, c in pl.assignments)
        burn = yed_in - total
        if burn < vault.minted_cents:
            return 'vault-spend-missing-burn' if burn <= 0 else 'vault-spend-short-burn'
        # RED-3
        eligible = self.eligible_payees(pl.ref_height)
        if eligible:
            fv = pl.fee_vout
            assigned_vouts = [v for v, _ in pl.assignments]
            if fv == FEE_VOUT_NONE or fv >= len(tx.vout) or fv == opret or fv in assigned_vouts:
                return 'vault-spend-bad-fee'
            key = p2pkh_key(tx.vout[fv].script)
            if key is None or key not in eligible:
                return 'vault-spend-bad-payee'
            if tx.vout[fv].value < fee_zat(vault.collateral_zat, p.fee_min, p.fee_bps):
                return 'vault-spend-bad-fee'
        # RED-4
        if path == 'claim':
            s = self.snapshot(pl.ref_height)
            p_claim = None if (s is None or s.virtual) else s.p_claim
            if not is_underwater(vault.collateral_zat, p_claim, vault.minted_cents, p.claim_threshold_bps):
                return 'vault-claim-not-underwater'
        return VERDICT_OK

    # -- state hash (section 3.6) --------------------------------------------

    def state_hash_preimage(self):
        p = self.params
        out = bytearray()
        out += b'T' + _i32(max(self.tip_height, 0)) + _hash(self.tip_hash) + _u32(self.SCHEMA_VERSION) + _str(p.network)
        for h in sorted(self.tags):
            t = self.tags[h]
            out += b'Q' + _u32be(h) + t.payout_key + _u64(t.price_micro_usd) + _bool(t.signal) + _u16(t.source_mask)
        for h in sorted(self.judgements):
            j = self.judgements[h]
            out += b'J' + _u32be(h) + _bool(j.evaluated) + _bool(j.in_band) + _bool(j.penalized)
        out += b'C' + _ser_activation(self.activation)
        for op in sorted(self.vaults, key=_outpoint_sort_key):
            out += _outpoint_key(b'V', op) + _ser_vault(self.vaults[op])
        for op in sorted(self.tokens, key=_outpoint_sort_key):
            out += _outpoint_key(b'K', op) + _ser_token(self.tokens[op])
        out += b'G' + _ser_totals(self.totals)
        for h in sorted(self.snapshots):
            out += b'S' + _u32be(h) + _ser_snapshot(self.snapshots[h])
        out += b'P' + _i32(p.start_height) + _i32(p.sigma_ref_bps) + _i32(p.supply_cap_bps) + _i32(p.enforce_until)
        return bytes(out)

    def state_hash(self):
        return sha256(self.state_hash_preimage()).hex()


# ---------------------------------------------------------------------------
# Live-node comparison (section 6.0 item 4, section 7)

class ModelMismatch(AssertionError):
    pass


def _norm_status(s):
    return str(s).replace('_', '').replace('-', '').lower()


def _norm_outpoint(x):
    if isinstance(x, dict):
        return '%s:%d' % (x['txid'], int(x['vout']))
    return str(x)


def _price_eq(model_v, rpc_v):
    if model_v is None:
        return rpc_v is None or int(rpc_v) == 0
    return rpc_v is not None and int(rpc_v) == model_v


def _key_of(rpc_payee):
    """A payee as the RPC renders it: an address (decoded to its key hash), a hex key, or None."""
    if rpc_payee in (None, ''):
        return None
    if isinstance(rpc_payee, str) and len(rpc_payee) == 40:
        try:
            return bytes.fromhex(rpc_payee)
        except ValueError:
            pass
    return address_key_hash(str(rpc_payee))


def _check(cond, what, height=None, extra=''):
    if not cond:
        where = '' if height is None else ' at height %d' % height
        raise ModelMismatch('model mismatch%s: %s %s' % (where, what, extra))


def subsidy_from_rpc(node, height):
    """GetBlockSubsidy(height) = miner + founders + every funding stream of ``getblocksubsidy``."""
    r = node.getblocksubsidy(height)
    total = Decimal(str(r.get('miner', 0))) + Decimal(str(r.get('founders', 0)))
    for fs in r.get('fundingstreams', []) or []:
        total += Decimal(str(fs.get('value', 0)))
    return int((total * COIN).to_integral_value())


def model_transaction(model, tx, height):
    """The accounting half as one call (P8): apply one non-coinbase transaction (a ``getblock 2``
    dict or a Tx) to ``model`` at ``height`` per section 3.8 (IN-1..3, TX-0, MINT-1..8, XFER-1..3,
    RED-1..4) and return ``(txlog_record_or_None, failing_red_verdict_or_None)``.  ``feed_block``
    does exactly this for every transaction of a block before the SNAP; this entry point lets a
    test model one transaction against an in-block state without a block."""
    t = tx_from_json(tx) if isinstance(tx, dict) else tx
    if t.is_coinbase:
        return None, None
    failing = model._apply_tx(t, height)
    return model.txlog.get(t.txid), failing


def build_model_from_node(node, params=None, check_tags=True):
    """Rebuild the model from a live node over [0, tip] via getblockcount/getblockhash/getblock 2."""
    if params is None:
        params = params_from_getinfo(node.yed_getinfo())
    model = YellowbackModel(params)
    tip = node.getblockcount()
    for h in range(0, tip + 1):
        bh = node.getblockhash(h)
        blk = node.getblock(bh, 2)
        subsidy = subsidy_from_rpc(node, h) if h >= params.start_height else 0
        if check_tags and h >= params.start_height:
            cb = blk['tx'][0]['vin'][0]['coinbase']
            tag = find_tag(bytes.fromhex(cb), h, params)
            rpc = node.yed_gettag(str(h))
            _check(bool(rpc.get('found')) == (tag is not None), 'yed_gettag.found', h, repr(rpc))
            if tag is not None:
                _check(_norm_status(rpc.get('kind')) in (tag.kind, tag.kind + 'only', tag.kind + 'tag'),
                       'yed_gettag.kind', h, repr(rpc))
                _check(bool(rpc.get('signal')) == tag.signal, 'yed_gettag.signal', h)
                _check(int(rpc.get('priceMicroUsd', 0)) == tag.price_micro_usd, 'yed_gettag.priceMicroUsd', h)
                _check(int(rpc.get('sourceMask', 0)) == tag.source_mask, 'yed_gettag.sourceMask', h)
                _check(_key_of(rpc.get('payoutAddress')) == tag.payout_key, 'yed_gettag.payoutAddress', h)
        model.feed_block_json(blk, subsidy)
    return model


def compare_history(model, node):
    """Every yed_gethistory field for every height in [START_HEIGHT, tip]."""
    start = model.params.start_height
    tip = model.tip_height
    rows = []
    h = start
    while h <= tip:
        to = min(h + 2015, tip)
        rows.extend(node.yed_gethistory(h, to))
        h = to + 1
    _check(len(rows) == max(0, tip - start + 1), 'yed_gethistory row count', extra='%d rows for [%d, %d]' % (len(rows), start, tip))
    for row in rows:
        h = int(row['height'])
        s = model.snapshots.get(h)
        _check(s is not None, 'snapshot missing in model', h)
        _check(row['blockHash'] == s.block_hash, 'blockHash', h)
        _check(bool(row['tagged']) == s.tagged, 'tagged', h)
        _check(bool(row['quote']) == s.quote, 'quote', h)
        _check(int(row['signalCount']) == s.signal_count, 'signalCount', h)
        act = row['activation']
        if isinstance(act, dict):
            _check(_norm_status(act['status']) == _norm_status(ACTIVATION_NAMES[s.activation.status]), 'activation.status', h, repr(act))
            _check(int(act.get('lockInHeight', 0)) == s.activation.lock_in_height, 'activation.lockInHeight', h)
            _check(int(act.get('activateHeight', 0)) == s.activation.activate_height, 'activation.activateHeight', h)
        else:
            _check(_norm_status(act) == _norm_status(ACTIVATION_NAMES[s.activation.status]), 'activation', h, repr(act))
        for name, val in (('pFast', s.p_fast), ('pMid', s.p_mid), ('pSlow', s.p_slow), ('pMint', s.p_mint), ('pClaim', s.p_claim)):
            _check(_price_eq(val, row.get(name)), name, h, 'model=%r rpc=%r' % (val, row.get(name)))
        _check(int(row['sigmaMultBps']) == s.sigma_mult_bps, 'sigmaMultBps', h, 'model=%r rpc=%r' % (s.sigma_mult_bps, row['sigmaMultBps']))
        _check(int(row['issuedZat']) == s.issued_zat, 'issuedZat', h, 'model=%r rpc=%r' % (s.issued_zat, row['issuedZat']))
        _check(int(row['supplyCents']) == s.supply_cents, 'supplyCents', h)
        _check(int(row['collateralZat']) == s.collateral_zat, 'collateralZat', h)
        _check(_price_eq(s.global_ratio_bps, row.get('globalRatioBps')), 'globalRatioBps', h)
        rpc_mask = row.get('haltMask', [])
        if isinstance(rpc_mask, int):
            _check(rpc_mask == s.halt_mask, 'haltMask', h)
        else:
            _check(set(str(x).upper() for x in rpc_mask) == set(s.halt_names()), 'haltMask', h, 'model=%r rpc=%r' % (s.halt_names(), rpc_mask))


def compare_txinfo(model, node):
    for txid, rec in model.txlog.items():
        info = node.yed_gettxinfo(txid)
        _check(int(info['height']) == rec.height, 'yed_gettxinfo.height', extra=txid)
        _check(_norm_status(info['type']) == _norm_status(rec.type), 'yed_gettxinfo.type', rec.height, '%s model=%s rpc=%s' % (txid, rec.type, info['type']))
        _check(_norm_status(info.get('path') or '') == _norm_status(rec.path), 'yed_gettxinfo.path', rec.height, txid)
        _check(info['verdict'] == rec.verdict, 'yed_gettxinfo.verdict', rec.height, '%s model=%s rpc=%s' % (txid, rec.verdict, info['verdict']))
        _check(int(info['yedIn']) == rec.yed_in, 'yed_gettxinfo.yedIn', rec.height, txid)
        _check(int(info['yedOut']) == rec.yed_out, 'yed_gettxinfo.yedOut', rec.height, txid)
        _check(int(info['burned']) == rec.burned, 'yed_gettxinfo.burned', rec.height, txid)
        _check(int(info.get('feeZat', 0)) == rec.fee_zat, 'yed_gettxinfo.feeZat', rec.height, txid)
        _check(_key_of(info.get('payee')) == rec.payee, 'yed_gettxinfo.payee', rec.height, txid)
        got = sorted((int(a['vout']), int(a['cents'])) for a in info.get('assigned', []))
        _check(got == sorted(rec.assigned), 'yed_gettxinfo.assigned', rec.height, txid)
        got = sorted(_norm_outpoint(x) for x in info.get('spentTokens', []))
        _check(got == sorted('%s:%d' % op for op in rec.spent_tokens), 'yed_gettxinfo.spentTokens', rec.height, txid)
        got = sorted(_norm_outpoint(x) for x in info.get('closedVaults', []))
        _check(got == sorted('%s:%d' % op for op in rec.closed_vaults), 'yed_gettxinfo.closedVaults', rec.height, txid)


def compare_vaults(model, node):
    rows = []
    skip = 0
    while True:
        page = node.yed_listvaults('', 1000, skip)
        if not page:
            break
        rows.extend(page)
        skip += len(page)
        if len(page) < 1000:
            break
    by_op = {(r['txid'], int(r.get('vout', 0))): r for r in rows}
    _check(set(by_op) == set(model.vaults), 'yed_listvaults set', extra='model=%r rpc=%r' % (sorted(model.vaults), sorted(by_op)))
    for op, v in model.vaults.items():
        r = by_op[op]
        _check(_norm_status(r['status']) == _norm_status(VAULT_STATUS_NAMES[v.status]), 'vault.status', v.mint_height, '%s:%d' % op)
        _check(str(r['ownerPubKey']).lower() == v.owner_pubkey.hex(), 'vault.ownerPubKey', v.mint_height, '%s:%d' % op)
        tc = r.get('termClass')
        _check(tc in (v.term_class, 'ABC'[v.term_class] if v.term_class < 3 else None), 'vault.termClass', v.mint_height, '%s:%d' % op)
        for name, val in (('lockHeight', v.lock_height), ('claimHeight', v.claim_height), ('collateralZat', v.collateral_zat),
                          ('mintedCents', v.minted_cents), ('mintHeight', v.mint_height), ('refHeight', v.ref_height),
                          ('feePaidZat', v.fee_paid_zat), ('closeHeight', v.close_height), ('burnedCents', v.burned_cents)):
            if name in r and r[name] is not None:
                _check(int(r[name]) == val, 'vault.%s' % name, v.mint_height, '%s:%d model=%r rpc=%r' % (op[0], op[1], val, r[name]))
        _check(bool(r.get('unbacked', False)) == v.unbacked, 'vault.unbacked', v.mint_height, '%s:%d' % op)
        _check((r.get('voidReason') or '') == v.void_reason, 'vault.voidReason', v.mint_height, '%s:%d' % op)
        if v.closing_txid is not None:
            _check(r.get('closingTxid') == v.closing_txid, 'vault.closingTxid', v.mint_height, '%s:%d' % op)


def compare_stats(model, node):
    st = node.yed_getstats()
    m = model.stats()
    for name in ('height', 'supplyCents', 'collateralZat', 'activeVaults', 'voidVaults', 'closedVaults',
                 'claimedVaults', 'unbackedCents', 'issuedZat', 'sigmaMultBps'):
        _check(int(st[name]) == m[name], 'yed_getstats.%s' % name, extra='model=%r rpc=%r' % (m[name], st[name]))
    for name in ('pFast', 'pMid', 'pSlow', 'pMint', 'pClaim', 'globalRatioBps', 'supplyCapCents'):
        _check(_price_eq(m[name], st.get(name)), 'yed_getstats.%s' % name, extra='model=%r rpc=%r' % (m[name], st.get(name)))
    _check(set(str(x).upper() for x in st.get('haltMask', [])) == set(m['haltMask']), 'yed_getstats.haltMask')
    _check(bool(st.get('mintingAllowed')) == m['mintingAllowed'], 'yed_getstats.mintingAllowed')


def assert_model_matches(node, full=False, params=None):
    """Rebuild the model from ``node`` and compare it with the node's answers: every
    ``yed_gethistory`` row (always) and, with ``full=True``, ``yed_gettxinfo`` for every
    Yellowback-relevant transaction, ``yed_listvaults``, ``yed_getstats`` and the state hash.
    Raises ModelMismatch (an AssertionError) naming the first differing field.  Returns the model."""
    model = build_model_from_node(node, params)
    compare_history(model, node)
    if full:
        compare_txinfo(model, node)
        compare_vaults(model, node)
        compare_stats(model, node)
        rpc_hash = node.yed_getstatehash()
        if isinstance(rpc_hash, dict):
            rpc_hash = rpc_hash.get('hash') or rpc_hash.get('statehash') or rpc_hash.get('stateHash')
        _check(str(rpc_hash).lower() == model.state_hash(), 'yed_getstatehash', extra='model=%s rpc=%s' % (model.state_hash(), rpc_hash))
    return model


# ---------------------------------------------------------------------------
# Golden vector replay (qa/rpc-tests/test_framework/yellowback_golden.json)

def replay_golden(doc):
    """Feed the blocks of a golden document (see test_yellowback_model.py for the generator)
    and return the model.  Each block is {height, hash, subsidyZat, txs: [raw hex...]} with
    txs[0] the coinbase."""
    p = doc['params']
    params = Params.regtest(int(p['startHeight']), int(p['sigmaRefBps']), int(p['supplyCapBps']), int(p['enforceUntil']))
    model = YellowbackModel(params)
    for b in doc['blocks']:
        txs = [tx_from_hex(h) for h in b['txs']]
        model.feed_block(int(b['height']), b['hash'], txs[0].vin[0].script_sig.hex(), int(b['subsidyZat']), txs[1:])
    return model


def load_golden(path):
    with open(path) as f:
        return json.load(f)
