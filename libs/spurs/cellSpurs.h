/*
 * ps3recomp - cellSpurs HLE
 *
 * SPURS (SPU Runtime System): task management, workloads, event flags.
 * Maps SPU concepts onto host threads for HLE execution.
 */

#ifndef PS3RECOMP_CELL_SPURS_H
#define PS3RECOMP_CELL_SPURS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_SPURS_CORE_ERROR_AGAIN             (CELL_ERROR_BASE_SPURS | 0x01)
#define CELL_SPURS_CORE_ERROR_INVAL             (CELL_ERROR_BASE_SPURS | 0x02)
#define CELL_SPURS_CORE_ERROR_NOSYS             (CELL_ERROR_BASE_SPURS | 0x03)
#define CELL_SPURS_CORE_ERROR_NOMEM             (CELL_ERROR_BASE_SPURS | 0x04)
#define CELL_SPURS_CORE_ERROR_SRCH              (CELL_ERROR_BASE_SPURS | 0x05)
#define CELL_SPURS_CORE_ERROR_NOENT             (CELL_ERROR_BASE_SPURS | 0x06)
#define CELL_SPURS_CORE_ERROR_BUSY              (CELL_ERROR_BASE_SPURS | 0x0A)
#define CELL_SPURS_CORE_ERROR_STAT              (CELL_ERROR_BASE_SPURS | 0x0F)
#define CELL_SPURS_CORE_ERROR_ALIGN             (CELL_ERROR_BASE_SPURS | 0x10)
#define CELL_SPURS_CORE_ERROR_NULL_POINTER      (CELL_ERROR_BASE_SPURS | 0x11)
#define CELL_SPURS_CORE_ERROR_PERM              (CELL_ERROR_BASE_SPURS | 0x09)

#define CELL_SPURS_TASK_ERROR_AGAIN             (CELL_ERROR_BASE_SPURS | 0x21)
#define CELL_SPURS_TASK_ERROR_INVAL             (CELL_ERROR_BASE_SPURS | 0x22)
#define CELL_SPURS_TASK_ERROR_NOSYS             (CELL_ERROR_BASE_SPURS | 0x23)
#define CELL_SPURS_TASK_ERROR_NOMEM             (CELL_ERROR_BASE_SPURS | 0x24)
#define CELL_SPURS_TASK_ERROR_SRCH              (CELL_ERROR_BASE_SPURS | 0x25)
#define CELL_SPURS_TASK_ERROR_NOENT             (CELL_ERROR_BASE_SPURS | 0x26)
#define CELL_SPURS_TASK_ERROR_BUSY              (CELL_ERROR_BASE_SPURS | 0x2A)
#define CELL_SPURS_TASK_ERROR_STAT              (CELL_ERROR_BASE_SPURS | 0x2F)
#define CELL_SPURS_TASK_ERROR_ALIGN             (CELL_ERROR_BASE_SPURS | 0x30)
#define CELL_SPURS_TASK_ERROR_NULL_POINTER      (CELL_ERROR_BASE_SPURS | 0x31)
#define CELL_SPURS_TASK_ERROR_FAULT             (CELL_ERROR_BASE_SPURS | 0x32)
#define CELL_SPURS_TASK_ERROR_PERM              (CELL_ERROR_BASE_SPURS | 0x29)

#define CELL_SPURS_JOB_ERROR_INVAL              0x80410A02u
#define CELL_SPURS_JOB_ERROR_NOMEM              0x80410A04u
#define CELL_SPURS_JOB_ERROR_STAT               0x80410A0Fu
#define CELL_SPURS_JOB_ERROR_ALIGN              0x80410A10u
#define CELL_SPURS_JOB_ERROR_NULL_POINTER       0x80410A11u

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_SPURS_MAX_SPU                  8
#define CELL_SPURS_MAX_WORKLOAD             16
#define CELL_SPURS_MAX_TASK                 128
#define CELL_SPURS_MAX_TASKSET              16
#define CELL_SPURS_MAX_PRIORITY             16

#define CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS    16

/* Event flag modes */
#define CELL_SPURS_EVENT_FLAG_CLEAR_AUTO        0
#define CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL      1

/* Event flag wait modes */
#define CELL_SPURS_EVENT_FLAG_OR                0
#define CELL_SPURS_EVENT_FLAG_AND               1

/* Event flag directions */
#define CELL_SPURS_EVENT_FLAG_SPU2SPU           0
#define CELL_SPURS_EVENT_FLAG_SPU2PPU           1
#define CELL_SPURS_EVENT_FLAG_PPU2SPU           2
#define CELL_SPURS_EVENT_FLAG_ANY2ANY           3

/* Workload flags */
#define CELL_SPURS_WORKLOAD_ATTR_RESET_ON_DETACH   0x0001

/* ---------------------------------------------------------------------------
 * Structures
 *
 * Note: On real PS3 hardware these must be 128-byte aligned for DMA.
 * We relax alignment for host HLE but keep the size fields.
 * -----------------------------------------------------------------------*/

/* Forward declarations for opaque internal data */
typedef struct CellSpurs CellSpurs;
typedef struct CellSpursAttribute CellSpursAttribute;
typedef struct CellSpursTaskset CellSpursTaskset;
typedef struct CellSpursTasksetAttribute CellSpursTasksetAttribute;
typedef struct CellSpursTask CellSpursTask;
typedef struct CellSpursTaskAttribute CellSpursTaskAttribute;
typedef struct CellSpursBarrier CellSpursBarrier;
typedef struct CellSpursEventFlag CellSpursEventFlag;
typedef struct CellSpursWorkloadAttribute CellSpursWorkloadAttribute;

typedef u32 CellSpursWorkloadId;
typedef u32 CellSpursTaskId;

/* SPURS attribute -- used to configure a SPURS instance before creation */
struct CellSpursAttribute {
    u32 revision;                 /* 0x00 */
    u32 sdkVersion;               /* 0x04 */
    u32 nSpus;                    /* 0x08 */
    s32 spuPriority;              /* 0x0C */
    s32 ppuPriority;              /* 0x10 */
    u8  exitIfNoWork;             /* 0x14 */
    u8  prefix[15];               /* 0x15, not NUL terminated */
    u32 prefixSize;               /* 0x24 */
    u32 flags;                    /* 0x28 */
    u32 container;                /* 0x2C */
    u32 unk0;                     /* 0x30 */
    u32 unk1;                     /* 0x34 */
    u8  swlPriority[8];           /* 0x38 */
    u32 swlMaxSpu;                /* 0x40 */
    u32 swlIsPreemptible;         /* 0x44 */
    u8  padding[440];             /* 0x48 */
};

/* Main SPURS instance */
struct CellSpurs {
    u32  initialized;
    u32  nSpus;
    u32  flags;
    u8   prefix[16];
    /* Internal worker state (host threads) */
    void* _internal;
    u8   _padding[256];
};

/* Taskset -- container for SPU tasks */
struct CellSpursTaskset {
    u32  initialized;
    u32  taskCount;
    u32  shutdownRequested;
    CellSpurs* spurs;
    u8   _padding[128];
};

struct CellSpursTasksetAttribute {
    /* Exact 0x200-byte guest ABI layout.  Multi-byte fields live big-endian in
     * guest memory and must be accessed through vm_read/vm_write, never by
     * dereferencing this host struct. */
    u32  revision;                         /* 0x00 */
    u32  sdkVersion;                       /* 0x04 */
    u64  args;                             /* 0x08 */
    u8   priority[CELL_SPURS_MAX_SPU];     /* 0x10 */
    u32  maxContention;                    /* 0x18 */
    u32  name_ea;                          /* 0x1C */
    u32  tasksetSize;                      /* 0x20 */
    s32  enableClearLs;                    /* 0x24 */
    u8   reserved[472];                    /* 0x28 */
};

/* Task -- represents a unit of SPU work */
struct CellSpursTask {
    u32  id;
    u32  active;
    u32  completed;
    void* entryPoint;
    u64  argA;
    u64  argB;
    u8   _padding[64];
};

struct CellSpursTaskAttribute {
    /* Public SDK object is opaque, 0x100 bytes, 16-byte aligned.  cellSpurs.c
     * keeps its HLE metadata in guest-endian words inside this storage. */
    u8 reserved[256];
};

/* Event flag -- SPURS-level event synchronization */
struct CellSpursEventFlag {
    u32  initialized;
    u16  bits;
    u16  clearMode;
    u16  direction;
    u8   _padding[64];
};

/* Workload attribute */
struct CellSpursWorkloadAttribute {
    u32  revision;
    u32  sdkVersion;
    u64  pm;
    u32  sizePm;
    u64  data;
    u8   priority[CELL_SPURS_MAX_SPU];
    u32  minContention;
    u32  maxContention;
    u8   _padding[128];
};

/* ---------------------------------------------------------------------------
 * SPURS core functions
 * -----------------------------------------------------------------------*/

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork);
s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr);
s32 cellSpursFinalize(CellSpurs* spurs);

s32 cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                 s32 spuPriority, s32 ppuPriority,
                                 u8 exitIfNoWork);
s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size);
s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* attr,
                                            s32 type);
s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* attr);

s32 cellSpursGetNumSpuThread(const CellSpurs* spurs, u32* nThreads);
s32 cellSpursSetMaxContention(CellSpurs* spurs, CellSpursWorkloadId wid,
                              u32 maxContention);
s32 cellSpursSetPriorities(CellSpurs* spurs, CellSpursWorkloadId wid,
                           const u8* priorities);

s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic);
s32 cellSpursDetachLv2EventQueue(CellSpurs* spurs, u8 port);

/* ---------------------------------------------------------------------------
 * Taskset functions
 * -----------------------------------------------------------------------*/

s32 cellSpursCreateTaskset(CellSpurs* spurs, CellSpursTaskset* taskset,
                           u64 args, const u8* priority, u32 maxContention);
s32 cellSpursCreateTasksetWithAttribute(CellSpurs* spurs,
                                        CellSpursTaskset* taskset,
                                        const CellSpursTasksetAttribute* attr);
s32 cellSpursDestroyTaskset(CellSpursTaskset* taskset);
s32 cellSpursShutdownTaskset(CellSpursTaskset* taskset);
s32 cellSpursJoinTaskset(CellSpursTaskset* taskset);

s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr);
s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute* attr,
                                      const char* name);

/* ---------------------------------------------------------------------------
 * Task functions
 * -----------------------------------------------------------------------*/

/* 7-arg SDK ABI: ... , CellSpursTaskLsPattern* lsPattern, CellSpursTaskArgument* argument.
 * Both are guest EAs (passed as u32 by the generic HLE adapter). */
s32 cellSpursCreateTask(CellSpursTaskset* taskset, CellSpursTaskId* taskId,
                        void* elf, void* context, u32 sizeContext,
                        u32 lsPattern_ea, u32 argument_ea);
s32 cellSpursJoinTask(CellSpursTaskset* taskset, CellSpursTaskId taskId,
                      s32* exitCode);
s32 cellSpursSendSignal(CellSpursTaskset* taskset, CellSpursTaskId taskId);

s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr);

/* Full 14-argument SDK form. Arguments 9-14 live in the PPU parameter-save
 * area and are supplied by a context-aware HLE wrapper. */
s32 cellSpursJobChainAttributeInitializeFull(
    u32 jmRevision, u32 sdkRevision, u64 attr_ea, u64 entry_ea,
    u32 sizeJobDescriptor, u32 maxGrabbedJob, u64 priority_ea,
    u32 maxContention, u32 autoRequestSpuCount, u32 tag1, u32 tag2,
    u32 isFixedMemAlloc, u32 maxSizeJobDescriptor,
    u32 initialRequestSpuCount);

/* ---------------------------------------------------------------------------
 * Barrier functions
 * -----------------------------------------------------------------------*/

s32 cellSpursBarrierInitialize(CellSpursTaskset* taskset,
                               CellSpursBarrier* barrier, u32 total);

/* ---------------------------------------------------------------------------
 * Workload functions
 * -----------------------------------------------------------------------*/

s32 cellSpursAddWorkload(CellSpurs* spurs, CellSpursWorkloadId* wid,
                         const void* pm, u32 sizePm, u64 data,
                         const u8* priority, u32 minContention,
                         u32 maxContention);
s32 cellSpursAddWorkloadWithAttribute(CellSpurs* spurs,
                                       CellSpursWorkloadId* wid,
                                       const CellSpursWorkloadAttribute* attr);
s32 cellSpursRemoveWorkload(CellSpurs* spurs, CellSpursWorkloadId wid);

s32 cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         const void* pm, u32 sizePm,
                                         u64 data, const u8* priority,
                                         u32 minContention,
                                         u32 maxContention);

s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value);
s32 cellSpursReadyCountSwap(CellSpurs* spurs, CellSpursWorkloadId wid,
                            u32* old, u32 value);
s32 cellSpursReadyCountCompareAndSwap(CellSpurs* spurs,
                                       CellSpursWorkloadId wid,
                                       u32* old, u32 compare, u32 value);
s32 cellSpursWakeUp(CellSpurs* spurs);

/* ---------------------------------------------------------------------------
 * Event flag functions
 * -----------------------------------------------------------------------*/

s32 cellSpursEventFlagInitialize(CellSpursTaskset* taskset,
                                 CellSpursEventFlag* eventFlag,
                                 u32 clearMode, u32 direction);
s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* eventFlag);

s32 cellSpursEventFlagSet(CellSpursEventFlag* eventFlag, u16 bits);
s32 cellSpursEventFlagWait(CellSpursEventFlag* eventFlag, u16* bits,
                           u32 mode);
s32 cellSpursEventFlagTryWait(CellSpursEventFlag* eventFlag, u16* bits,
                              u32 mode);
s32 cellSpursEventFlagClear(CellSpursEventFlag* eventFlag, u16 bits);
s32 cellSpursEventFlagGetDirection(CellSpursEventFlag* eventFlag,
                                   u32* direction);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SPURS_H */
