#ifndef PS3RECOMP_AUDIO_PACING_H
#define PS3RECOMP_AUDIO_PACING_H

#include "cellAudio.h"

static inline int ps3_audio_full_block_fits(u32 capacity, u32 padding)
{
    return padding <= capacity &&
           capacity - padding >= CELL_AUDIO_BLOCK_SAMPLES;
}

static inline u64 ps3_audio_port_block_tag(u64 global_counter,
                                            u64 current_position,
                                            u64 block_no)
{
    return global_counter + block_no - current_position;
}

static inline u64 ps3_audio_next_ring_position(u64 current_position,
                                                u64 num_blocks)
{
    if (!num_blocks)
        return 0;
    return (current_position + 1) % num_blocks;
}

static inline int ps3_audio_tag_timestamp(u64 current_tag,
                                          u64 requested_tag,
                                          u64 current_stamp,
                                          u64* stamp)
{
    if (requested_tag > current_tag)
        return 0;

    u64 blocks = current_tag - requested_tag;
    if (blocks > UINT64_MAX / CELL_AUDIO_PERIOD_US) {
        *stamp = 0;
        return 1;
    }

    u64 delta = blocks * CELL_AUDIO_PERIOD_US;
    *stamp = delta > current_stamp ? 0 : current_stamp - delta;
    return 1;
}

#endif /* PS3RECOMP_AUDIO_PACING_H */
