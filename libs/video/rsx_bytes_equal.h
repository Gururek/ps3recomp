/* Exact equality for immutable snapshot reuse. No hash, sampling or overread. */
#ifndef PS3RECOMP_RSX_BYTES_EQUAL_H
#define PS3RECOMP_RSX_BYTES_EQUAL_H

#include <stddef.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
#if defined(__clang__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#define RSX_BYTES_EQUAL_HAS_AVX2 1
#endif

static inline int rsx_bytes_equal(const void* left, const void* right, size_t size)
{
    const unsigned char* a = (const unsigned char*)left;
    const unsigned char* b = (const unsigned char*)right;
#if defined(__SSE2__) || defined(_M_X64)
    while (size >= 64) {
        __m128i diff = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(a)),
                                    _mm_loadu_si128((const __m128i*)(b)));
        diff = _mm_or_si128(diff, _mm_xor_si128(
            _mm_loadu_si128((const __m128i*)(a + 16)),
            _mm_loadu_si128((const __m128i*)(b + 16))));
        diff = _mm_or_si128(diff, _mm_xor_si128(
            _mm_loadu_si128((const __m128i*)(a + 32)),
            _mm_loadu_si128((const __m128i*)(b + 32))));
        diff = _mm_or_si128(diff, _mm_xor_si128(
            _mm_loadu_si128((const __m128i*)(a + 48)),
            _mm_loadu_si128((const __m128i*)(b + 48))));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) != 0xFFFF)
            return 0;
        a += 64; b += 64; size -= 64;
    }
    while (size >= 16) {
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(
                _mm_loadu_si128((const __m128i*)a),
                _mm_loadu_si128((const __m128i*)b))) != 0xFFFF)
            return 0;
        a += 16; b += 16; size -= 16;
    }
#endif
    return size == 0 || memcmp(a, b, size) == 0;
}

#if defined(RSX_BYTES_EQUAL_HAS_AVX2)
/* The caller must check both CPU and OS AVX2 support before dispatching here.
 * Target only this routine; older x64 hosts retain the SSE2 implementation. */
__attribute__((target("avx2")))
static int rsx_bytes_equal_avx2(const void* left, const void* right, size_t size)
{
    const unsigned char* a = (const unsigned char*)left;
    const unsigned char* b = (const unsigned char*)right;
    while (size >= 128) {
        __m256i diff = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)a),
                                       _mm256_loadu_si256((const __m256i*)b));
        diff = _mm256_or_si256(diff, _mm256_xor_si256(
            _mm256_loadu_si256((const __m256i*)(a + 32)),
            _mm256_loadu_si256((const __m256i*)(b + 32))));
        diff = _mm256_or_si256(diff, _mm256_xor_si256(
            _mm256_loadu_si256((const __m256i*)(a + 64)),
            _mm256_loadu_si256((const __m256i*)(b + 64))));
        diff = _mm256_or_si256(diff, _mm256_xor_si256(
            _mm256_loadu_si256((const __m256i*)(a + 96)),
            _mm256_loadu_si256((const __m256i*)(b + 96))));
        if (!_mm256_testz_si256(diff, diff)) return 0;
        a += 128; b += 128; size -= 128;
    }
    return rsx_bytes_equal(a, b, size);
}
#endif

#endif
