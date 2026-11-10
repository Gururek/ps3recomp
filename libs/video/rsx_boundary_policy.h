#ifndef RSX_BOUNDARY_POLICY_H
#define RSX_BOUNDARY_POLICY_H

#include <stdint.h>

static inline int rsx_boundary_should_arm_survival_profile(
    uint32_t tile_index, uint32_t location, uint32_t offset,
    uint32_t size, uint32_t pitch)
{
    return tile_index == 10u && location == 0u &&
           offset == UINT32_C(0x0F510000) &&
           size == UINT32_C(0x003A0000) && pitch == 5120u;
}

typedef enum rsx_boundary_decision {
    RSX_BOUNDARY_ACCUMULATE,
    RSX_BOUNDARY_COALESCE,
    RSX_BOUNDARY_PRESENT,
} rsx_boundary_decision;

/* consumed_generation identifies the flip already consumed by the ticker.
 * submitted_boundary_generation identifies content this backend has submitted,
 * not completed on the GPU. */
static inline rsx_boundary_decision rsx_boundary_decide(
    uint64_t consumed_generation, uint64_t submitted_boundary_generation,
    int have_display_work)
{
    if (!have_display_work)
        return RSX_BOUNDARY_ACCUMULATE;
    if (consumed_generation == 0u)
        return RSX_BOUNDARY_PRESENT;
    return consumed_generation == submitted_boundary_generation
        ? RSX_BOUNDARY_COALESCE : RSX_BOUNDARY_PRESENT;
}

static inline uint64_t rsx_boundary_submitted_generation_after_submit(
    uint64_t consumed_generation, uint64_t submitted_boundary_generation)
{
    return consumed_generation != 0u
        ? consumed_generation : submitted_boundary_generation;
}

#endif
