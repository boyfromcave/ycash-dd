/*
 * Copyright (c) 2026 The Ycash developers
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or https://www.opensource.org/licenses/mit-license.php .
 *
 * A ctypes shim over the tree's own vendored post-quantum code (src/crypto/pq/), for the DoS
 * measurement (pq_dos.py, doc/yellowback-pq-dos.md): SLH-DSA-SHA2-128s keygen/sign/verify with
 * exactly the calls src/crypto/pq/{scheme,sign}.cpp make, and a verify timing loop for both
 * schemes. Measurement tooling only; nothing here is linked into ycashd.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "slhdsa/slh_dsa.h"
#include "falcon/ycash_falcon.h"

#define SLH_N 16

int pqdos_slh_keygen(uint8_t *pk, uint8_t *sk, const uint8_t *seed48)
{
    return slh_keygen_internal(sk, pk, seed48, seed48 + SLH_N, seed48 + 2 * SLH_N, &slh_dsa_sha2_128s);
}

/* Deterministic (addrnd = NULL), pure mode, empty context: pq::Sign with no entropy. */
size_t pqdos_slh_sign(uint8_t *sig, const uint8_t *msg32, const uint8_t *sk)
{
    static const uint8_t no_ctx = 0;
    return slh_sign(sig, msg32, 32, &no_ctx, 0, sk, NULL, &slh_dsa_sha2_128s);
}

/* pq::Verify's two calls; 1 = valid. */
int pqdos_verify(int scheme, const uint8_t *pk, const uint8_t *sig, const uint8_t *msg32)
{
    static const uint8_t no_ctx = 0;
    if (scheme == 1)
        return slh_verify(msg32, 32, sig, 7856, &no_ctx, 0, pk, &slh_dsa_sha2_128s) == 1;
    if (scheme == 2)
        return ycash_falcon512_verify(sig, 666, msg32, 32, pk) == 0;
    return 0;
}

/* Microseconds per pqdos_verify over `iters` calls; *nvalid counts the valid results. */
double pqdos_bench_verify(int scheme, const uint8_t *pk, const uint8_t *sig, const uint8_t *msg32,
                          int iters, int *nvalid)
{
    struct timespec a, b;
    int ok = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < iters; i++)
        ok += pqdos_verify(scheme, pk, sig, msg32);
    clock_gettime(CLOCK_MONOTONIC, &b);
    if (nvalid) *nvalid = ok;
    return ((b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3) / iters;
}
