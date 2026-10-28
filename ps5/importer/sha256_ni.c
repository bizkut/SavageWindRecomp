// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-256 for IVFC blocks. The fallback remains nstool's SHA-256 implementation.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <mbedtls/sha256.h>
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#include <immintrin.h>
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32-n)); }
__attribute__((target("sha,ssse3,sse4.1")))
static void compress(uint32_t state[8], const unsigned char *data, size_t blocks) {
    __m128i a = _mm_loadu_si128((const __m128i *)state);
    __m128i b = _mm_loadu_si128((const __m128i *)(state+4));
    a = _mm_shuffle_epi32(a, 0xb1); b = _mm_shuffle_epi32(b, 0x1b);
    __m128i x = _mm_alignr_epi8(a,b,8), y = _mm_blend_epi16(b,a,0xf0);
    while (blocks--) {
        uint32_t w[64];
        for (int i=0;i<16;i++) { uint32_t v; memcpy(&v,data+4*i,4); w[i]=__builtin_bswap32(v); }
        for (int i=16;i<64;i++) {
            uint32_t p=w[i-15], q=w[i-2];
            w[i]=(rotr(p,7)^rotr(p,18)^(p>>3))+w[i-16]+(rotr(q,17)^rotr(q,19)^(q>>10))+w[i-7];
        }
        __m128i sx=x, sy=y;
        for (int i=0;i<64;i+=4) {
            __m128i v=_mm_set_epi32(w[i+3]+K[i+3],w[i+2]+K[i+2],w[i+1]+K[i+1],w[i]+K[i]);
            y=_mm_sha256rnds2_epu32(y,x,v);
            x=_mm_sha256rnds2_epu32(x,y,_mm_shuffle_epi32(v,0x0e));
        }
        x=_mm_add_epi32(x,sx); y=_mm_add_epi32(y,sy); data+=64;
    }
    a=_mm_shuffle_epi32(x,0x1b); b=_mm_shuffle_epi32(y,0xb1);
    _mm_storeu_si128((__m128i *)state,_mm_blend_epi16(a,b,0xf0));
    _mm_storeu_si128((__m128i *)(state+4),_mm_alignr_epi8(b,a,8));
}
static void sha256_fast(unsigned char out[32], const unsigned char *data, size_t size) {
    uint32_t state[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t full=size/64, rem=size%64;
    compress(state,data,full);
    unsigned char last[128]={0};
    memcpy(last,data+full*64,rem); last[rem]=0x80;
    size_t n=rem<56?64:128; uint64_t bits=(uint64_t)size*8;
    for (int i=0;i<8;i++) last[n-1-i]=(unsigned char)(bits>>(8*i));
    compress(state,last,n/64);
    for (int i=0;i<8;i++) { uint32_t v=__builtin_bswap32(state[i]); memcpy(out+4*i,&v,4); }
}

static pthread_once_t sha_once = PTHREAD_ONCE_INIT;
static int sha_available;
static void sha_init(void) {
    unsigned a, b, c, d;
    const char *reason = "CPU lacks SHA-NI, SSSE3 or SSE4.1";
    if (!__get_cpuid(1,&a,&b,&c,&d) || !(c & bit_SSSE3) || !(c & bit_SSE4_1) ||
        !__get_cpuid_count(7,0,&a,&b,&c,&d) || !(b & (1u<<29))) goto done;
    unsigned char *input = malloc(1048576);
    if (!input) { reason = "self-test allocation failed"; goto done; }
    for (size_t i = 0; i < 1048576; i++) input[i] = (unsigned char)((i * 73 + (i >> 8) + 11) & 255);
    memcpy(input, "abc", 3);
    const size_t lengths[] = {0, 3, 64, 1048576};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        unsigned char fast[32], reference[32];
        sha256_fast(fast, input, lengths[i]);
        if (mbedtls_sha256_ret(input, lengths[i], reference, 0) != 0 || memcmp(fast, reference, 32)) {
            free(input);
            reason = "self-test mismatch";
            goto done;
        }
    }
    free(input);
    sha_available = 1;
done:
#ifdef __PROSPERO__
    if (sha_available) fprintf(stderr, "savagewind-import: sha256: sha_ni selected\n");
    else fprintf(stderr, "savagewind-import: sha256: software (%s)\n", reason);
#else
    (void)reason;
#endif
}

int savage_sha256_ni(unsigned char out[32], const unsigned char *data, size_t size) {
    pthread_once(&sha_once, sha_init);
    if (!sha_available) return 0;
    sha256_fast(out, data, size);
    return 1;
}
#else
int savage_sha256_ni(unsigned char out[32], const unsigned char *data, size_t size) {
    (void)out; (void)data; (void)size; return 0;
}
#endif
