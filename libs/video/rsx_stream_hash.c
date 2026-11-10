#include "rsx_stream_hash.h"

#include <string.h>

#define RSX_H64_P1 UINT64_C(11400714785074694791)
#define RSX_H64_P2 UINT64_C(14029467366897019727)
#define RSX_H64_P3 UINT64_C(1609587929392839161)
#define RSX_H64_P4 UINT64_C(9650029242287828579)
#define RSX_H64_P5 UINT64_C(2870177450012600261)

static uint64_t rsx_h64_rotl(uint64_t value, unsigned count)
{
    return (value << count) | (value >> (64u - count));
}

/* Windows and clang-cl targets are little-endian.  The byte reconstruction
 * keeps the digest defined if this helper is built by a big-endian test host. */
static uint64_t rsx_h64_read64le(const void* data)
{
    uint64_t native_value;
    memcpy(&native_value, data, sizeof(native_value));
#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return native_value;
#else
    const unsigned char* bytes = (const unsigned char*)data;
    (void)native_value;
    return (uint64_t)bytes[0]        | ((uint64_t)bytes[1] << 8)  |
           ((uint64_t)bytes[2] << 16) | ((uint64_t)bytes[3] << 24) |
           ((uint64_t)bytes[4] << 32) | ((uint64_t)bytes[5] << 40) |
           ((uint64_t)bytes[6] << 48) | ((uint64_t)bytes[7] << 56);
#endif
}

static uint32_t rsx_h64_read32le(const void* data)
{
    uint32_t native_value;
    memcpy(&native_value, data, sizeof(native_value));
#if defined(_WIN32) || defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return native_value;
#else
    const unsigned char* bytes = (const unsigned char*)data;
    (void)native_value;
    return (uint32_t)bytes[0]        | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
#endif
}

static uint64_t rsx_h64_round(uint64_t acc, uint64_t input)
{
    acc += input * RSX_H64_P2;
    acc = rsx_h64_rotl(acc, 31);
    acc *= RSX_H64_P1;
    return acc;
}

static uint64_t rsx_h64_merge_round(uint64_t acc, uint64_t value)
{
    acc ^= rsx_h64_round(0, value);
    return acc * RSX_H64_P1 + RSX_H64_P4;
}

static void rsx_h64_consume32(rsx_stream_hash64_state* state,
                              const unsigned char* input)
{
    state->v1 = rsx_h64_round(state->v1, rsx_h64_read64le(input + 0));
    state->v2 = rsx_h64_round(state->v2, rsx_h64_read64le(input + 8));
    state->v3 = rsx_h64_round(state->v3, rsx_h64_read64le(input + 16));
    state->v4 = rsx_h64_round(state->v4, rsx_h64_read64le(input + 24));
}

void rsx_stream_hash64_init(rsx_stream_hash64_state* state)
{
    if (!state) return;
    state->total_len = 0;
    state->v1 = RSX_H64_P1 + RSX_H64_P2;
    state->v2 = RSX_H64_P2;
    state->v3 = 0;
    state->v4 = 0 - RSX_H64_P1;
    state->tail_len = 0;
}

void rsx_stream_hash64_update(rsx_stream_hash64_state* state,
                              const void* data, size_t size)
{
    if (!state || !size) return;
    if (!data) return;

    const unsigned char* input = (const unsigned char*)data;
    const unsigned char* end = input + size;
    state->total_len += (uint64_t)size;

    if (state->tail_len + size < sizeof(state->tail)) {
        memcpy(state->tail + state->tail_len, input, size);
        state->tail_len += size;
        return;
    }

    if (state->tail_len) {
        size_t needed = sizeof(state->tail) - state->tail_len;
        memcpy(state->tail + state->tail_len, input, needed);
        rsx_h64_consume32(state, state->tail);
        input += needed;
        state->tail_len = 0;
    }

    while ((size_t)(end - input) >= sizeof(state->tail)) {
        rsx_h64_consume32(state, input);
        input += sizeof(state->tail);
    }

    if (input != end) {
        state->tail_len = (size_t)(end - input);
        memcpy(state->tail, input, state->tail_len);
    }
}

uint64_t rsx_stream_hash64_digest(const rsx_stream_hash64_state* state)
{
    if (!state) return 0;

    uint64_t hash;
    if (state->total_len >= 32) {
        hash = rsx_h64_rotl(state->v1, 1) + rsx_h64_rotl(state->v2, 7) +
               rsx_h64_rotl(state->v3, 12) + rsx_h64_rotl(state->v4, 18);
        hash = rsx_h64_merge_round(hash, state->v1);
        hash = rsx_h64_merge_round(hash, state->v2);
        hash = rsx_h64_merge_round(hash, state->v3);
        hash = rsx_h64_merge_round(hash, state->v4);
    } else {
        hash = RSX_H64_P5;
    }

    hash += state->total_len;
    const unsigned char* input = state->tail;
    const unsigned char* end = input + state->tail_len;
    while ((size_t)(end - input) >= 8) {
        uint64_t lane = rsx_h64_round(0, rsx_h64_read64le(input));
        hash ^= lane;
        hash = rsx_h64_rotl(hash, 27) * RSX_H64_P1 + RSX_H64_P4;
        input += 8;
    }
    if ((size_t)(end - input) >= 4) {
        hash ^= (uint64_t)rsx_h64_read32le(input) * RSX_H64_P1;
        hash = rsx_h64_rotl(hash, 23) * RSX_H64_P2 + RSX_H64_P3;
        input += 4;
    }
    while (input != end) {
        hash ^= (uint64_t)(*input++) * RSX_H64_P5;
        hash = rsx_h64_rotl(hash, 11) * RSX_H64_P1;
    }

    hash ^= hash >> 33;
    hash *= RSX_H64_P2;
    hash ^= hash >> 29;
    hash *= RSX_H64_P3;
    hash ^= hash >> 32;
    return hash;
}

uint64_t rsx_stream_hash64(const void* data, size_t size)
{
    rsx_stream_hash64_state state;
    rsx_stream_hash64_init(&state);
    rsx_stream_hash64_update(&state, data, size);
    return rsx_stream_hash64_digest(&state);
}
