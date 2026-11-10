/* Deterministic streaming 64-bit content hash for RSX upload sources.
 *
 * XXH64-style scalar algorithm: fixed seed zero, no CPU dispatch, and all
 * unaligned input loads pass through memcpy.  It is intentionally independent
 * of guest-memory ownership/invalidation; callers still read every byte. */
#ifndef PS3RECOMP_RSX_STREAM_HASH_H
#define PS3RECOMP_RSX_STREAM_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct rsx_stream_hash64_state {
    uint64_t total_len;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
    uint64_t v4;
    unsigned char tail[32];
    size_t tail_len;
} rsx_stream_hash64_state;

void rsx_stream_hash64_init(rsx_stream_hash64_state* state);
void rsx_stream_hash64_update(rsx_stream_hash64_state* state,
                              const void* data, size_t size);
uint64_t rsx_stream_hash64_digest(const rsx_stream_hash64_state* state);
uint64_t rsx_stream_hash64(const void* data, size_t size);

#endif
