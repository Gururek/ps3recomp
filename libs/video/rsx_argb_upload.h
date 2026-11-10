/* Convert immutable guest ARGB rows to a distinct RGBA upload allocation.
 * Caller validates pitches/spans. Every output pixel and padding byte is set. */
#ifndef PS3RECOMP_RSX_ARGB_UPLOAD_H
#define PS3RECOMP_RSX_ARGB_UPLOAD_H
#include <stdint.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

static inline void rsx_argb_upload_rows(uint8_t* dst, uint32_t dst_pitch,
    const uint8_t* src, uint32_t src_pitch, uint32_t pixels, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* s = src + (uint64_t)y * src_pitch;
        uint8_t* d = dst + (uint64_t)y * dst_pitch;
        uint32_t x = 0;
#if defined(__SSE2__) || defined(_M_X64)
        for (; pixels - x >= 4; x += 4) {
            __m128i argb = _mm_loadu_si128((const __m128i*)(s + (uint64_t)x * 4));
            __m128i rgba = _mm_or_si128(_mm_srli_epi32(argb, 8),
                                         _mm_slli_epi32(argb, 24));
            _mm_storeu_si128((__m128i*)(d + (uint64_t)x * 4), rgba);
        }
#endif
        for (; x < pixels; ++x) {
            d[x*4] = s[x*4+1]; d[x*4+1] = s[x*4+2];
            d[x*4+2] = s[x*4+3]; d[x*4+3] = s[x*4];
        }
        size_t row_bytes = (size_t)pixels * 4;
        if (dst_pitch > row_bytes) memset(d + row_bytes, 0, dst_pitch - row_bytes);
    }
}
#endif
