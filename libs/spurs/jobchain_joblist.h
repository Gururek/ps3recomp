#pragma once

/* CellSpursJobList is guest big-endian:
 *   be u32 numJobs;
 *   be u32 sizeOfJob;
 *   be u64 eaJobList;
 *
 * Keep parsing independent of the guest VM so the exact bounds arithmetic has
 * a small host regression test. */
#include <stdint.h>

#define JOBCHAIN_JOBLIST_HEADER_SIZE       16u
#define JOBCHAIN_JOBLIST_MAX_JOBS          4096u
#define JOBCHAIN_JOBLIST_MAX_SIZE           0x400u
#define JOBCHAIN_JOBCHAIN_JOB_BUDGET        4096u

typedef struct jobchain_joblist {
    uint32_t num_jobs;
    uint32_t size_desc;
    uint32_t jobs_ea;
} jobchain_joblist;

typedef int (*jobchain_joblist_read_fn)(void* opaque, uint32_t ea,
                                        uint8_t out[JOBCHAIN_JOBLIST_HEADER_SIZE]);
typedef int (*jobchain_job_dispatch_fn)(void* opaque, uint32_t job_ea,
                                        uint32_t size_desc, uint32_t job_index);
typedef int (*jobchain_job_should_stop_fn)(void* opaque);

enum jobchain_job_dispatch_result {
    JOBCHAIN_JOB_UNHANDLED = 0,
    JOBCHAIN_JOB_DISPATCHED = 1,
    JOBCHAIN_JOB_NOOP = 2,
    JOBCHAIN_JOB_INVALID = -1,
    JOBCHAIN_JOB_BUDGET_EXHAUSTED = -2,
    JOBCHAIN_JOB_STOPPED = -3,
    JOBCHAIN_JOB_DISPATCH_FAILED = -4,
};

static inline uint32_t jobchain_joblist_read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int jobchain_joblist_size_valid(uint32_t size_desc)
{
    return size_desc == 0x40u ||
           (size_desc <= JOBCHAIN_JOBLIST_MAX_SIZE && (size_desc & 0x7Fu) == 0);
}

/* guest_limit is exclusive.  Pass 2^32 for an unchecked full PS3 EA space. */
static inline int jobchain_joblist_decode(const uint8_t header[JOBCHAIN_JOBLIST_HEADER_SIZE],
                                          uint64_t guest_limit,
                                          jobchain_joblist* out)
{
    uint32_t num_jobs;
    uint32_t size_desc;
    uint64_t jobs_ea64;
    uint64_t list_end;

    if (!header || !out || guest_limit == 0) return 0;

    num_jobs = jobchain_joblist_read_be32(header + 0);
    size_desc = jobchain_joblist_read_be32(header + 4);
    jobs_ea64 = ((uint64_t)jobchain_joblist_read_be32(header + 8) << 32) |
                jobchain_joblist_read_be32(header + 12);

    /* Sony accepts an empty list as a no-op. Its unused size/pointer fields
     * need not describe a descriptor array. */
    if (num_jobs == 0) {
        out->num_jobs = 0;
        out->size_desc = size_desc;
        out->jobs_ea = (uint32_t)jobs_ea64;
        return 1;
    }

    if (num_jobs > JOBCHAIN_JOBLIST_MAX_JOBS ||
        !jobchain_joblist_size_valid(size_desc) || jobs_ea64 == 0 ||
        jobs_ea64 > UINT32_MAX || (jobs_ea64 & 15u) != 0)
        return 0;

    /* Use 64-bit arithmetic before converting the guest EA back to u32. */
    list_end = jobs_ea64 + (uint64_t)num_jobs * size_desc;
    if (list_end < jobs_ea64 || list_end > guest_limit || list_end > (UINT64_C(1) << 32))
        return 0;

    out->num_jobs = num_jobs;
    out->size_desc = size_desc;
    out->jobs_ea = (uint32_t)jobs_ea64;
    return 1;
}

static inline uint32_t jobchain_joblist_descriptor_ea(const jobchain_joblist* list,
                                                       uint32_t index)
{
    return list->jobs_ea + index * list->size_desc;
}

static inline int jobchain_dispatch_one_job(uint32_t job_ea, uint32_t size_desc,
                                            uint32_t* jobs,
                                            uint32_t job_budget,
                                            jobchain_job_should_stop_fn should_stop,
                                            void* stop_opaque,
                                            jobchain_job_dispatch_fn dispatch,
                                            void* dispatch_opaque)
{
    if (!jobs || !dispatch || *jobs >= job_budget)
        return JOBCHAIN_JOB_BUDGET_EXHAUSTED;
    if (should_stop && should_stop(stop_opaque))
        return JOBCHAIN_JOB_STOPPED;
    if (dispatch(dispatch_opaque, job_ea, size_desc, *jobs) != 0)
        return JOBCHAIN_JOB_DISPATCH_FAILED;
    ++*jobs;
    return JOBCHAIN_JOB_DISPATCHED;
}

/* Execute one JOB/JOBLIST command's job-dispatch portion. The caller owns PC
 * flow control; this helper owns the shared ordered/indexed/budgeted semantics
 * so direct JOB and each JOBLIST descriptor follow one path. */
static inline int jobchain_dispatch_command(uint64_t cmd, uint32_t direct_size_desc,
                                            uint64_t guest_limit, uint32_t* jobs,
                                            uint32_t job_budget,
                                            jobchain_joblist_read_fn read_header,
                                            void* read_opaque,
                                            jobchain_job_should_stop_fn should_stop,
                                            void* stop_opaque,
                                            jobchain_job_dispatch_fn dispatch,
                                            void* dispatch_opaque)
{
    uint32_t op = (uint32_t)(cmd & 7u);

    if (cmd != 0 && op == 0)
        return jobchain_dispatch_one_job((uint32_t)(cmd & ~7ull), direct_size_desc,
                                         jobs, job_budget, should_stop, stop_opaque,
                                         dispatch, dispatch_opaque);

    if (op == 6) {
        uint64_t header_ea64 = cmd & ~7ull;
        uint8_t header[JOBCHAIN_JOBLIST_HEADER_SIZE];
        jobchain_joblist list;

        if (!read_header || header_ea64 > UINT32_MAX ||
            (header_ea64 & 15u) != 0 ||
            header_ea64 + JOBCHAIN_JOBLIST_HEADER_SIZE > guest_limit ||
            !read_header(read_opaque, (uint32_t)header_ea64, header) ||
            !jobchain_joblist_decode(header, guest_limit, &list))
            return JOBCHAIN_JOB_INVALID;

        if (list.num_jobs == 0)
            return JOBCHAIN_JOB_NOOP;

        for (uint32_t i = 0; i < list.num_jobs; ++i) {
            int result = jobchain_dispatch_one_job(
                jobchain_joblist_descriptor_ea(&list, i), list.size_desc,
                jobs, job_budget, should_stop, stop_opaque, dispatch,
                dispatch_opaque);
            if (result != JOBCHAIN_JOB_DISPATCHED)
                return result;
        }
        return JOBCHAIN_JOB_DISPATCHED;
    }

    return JOBCHAIN_JOB_UNHANDLED;
}
