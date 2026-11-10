/*
 * ps3recomp - cellGcmSys HLE module
 *
 * RSX (Reality Synthesizer) graphics system interface.
 * Manages initialization, display buffers, flip control, command buffer
 * control, tile/zcull configuration, IO memory mapping, report/label
 * areas, and address-to-offset translation for the GPU.
 *
 * Note: Full RSX command buffer processing (the NV47xx methods, fragment/
 * vertex programs, texture setup, etc.) is a separate massive undertaking
 * handled by the RSX command processor module, not this file.
 */

#ifndef PS3RECOMP_CELL_GCM_SYS_H
#define PS3RECOMP_CELL_GCM_SYS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* Flip modes */
#define CELL_GCM_DISPLAY_HSYNC          1
#define CELL_GCM_DISPLAY_VSYNC          2

/* Flip status */
#define CELL_GCM_FLIP_STATUS_DONE       0
#define CELL_GCM_FLIP_STATUS_WAITING    1

/* Surface color formats */
#define CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5   1
#define CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5   2
#define CELL_GCM_SURFACE_R5G6B5               3
#define CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8   4
#define CELL_GCM_SURFACE_X8R8G8B8             5
#define CELL_GCM_SURFACE_A8R8G8B8             8
#define CELL_GCM_SURFACE_B8                   9
#define CELL_GCM_SURFACE_G8B8                 10
#define CELL_GCM_SURFACE_F_W16Z16Y16X16       11
#define CELL_GCM_SURFACE_F_W32Z32Y32X32       12
#define CELL_GCM_SURFACE_F_X32                13
#define CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8   14
#define CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8   15
#define CELL_GCM_SURFACE_A8B8G8R8             16

/* Depth buffer formats */
#define CELL_GCM_SURFACE_Z16                  1
#define CELL_GCM_SURFACE_Z24S8                2

/* Max display buffers */
#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM 8

/* Tile / Zcull limits */
#define CELL_GCM_MAX_TILE_COUNT         15
#define CELL_GCM_MAX_ZCULL_COUNT        8

/* Report / label limits */
#define CELL_GCM_MAX_REPORT_COUNT       256
#define CELL_GCM_MAX_LABEL_COUNT        256

/* Report data size (timestamp u64 + value u32 + pad u32 = 16 bytes) */
#define CELL_GCM_REPORT_DATA_SIZE       16

/* Location */
#define CELL_GCM_LOCATION_LOCAL         0  /* video memory */
#define CELL_GCM_LOCATION_MAIN          1  /* main memory */

/* IO mapping limits */
/* The RSX offset tables are page based; titles may create many separate main
 * memory mappings over their lifetime. Tokyo Jungle consumes 16 during boot
 * and needs additional notify/render mappings when Survival starts, so the old
 * software-only cap rejected valid mappings and stopped FIFO translation. */
#define CELL_GCM_MAX_IO_MAPPINGS        256

/* Debug output levels */
#define CELL_GCM_DEBUG_LEVEL0           0
#define CELL_GCM_DEBUG_LEVEL1           1
#define CELL_GCM_DEBUG_LEVEL2           2

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Command buffer control registers (mapped to RSX FIFO control area) */
typedef struct CellGcmControl {
    volatile u32 put;       /* write pointer (offset into cmd buffer) */
    volatile u32 get;       /* read pointer (offset into cmd buffer) */
    volatile u32 ref;       /* reference value (written by RSX on completion) */
} CellGcmControl;

/* Optional host-side frame-profiler snapshot.  This is diagnostic state only:
 * callers must opt in with TJ_FRAME_PROF before the HLE records a producer. */
typedef struct CellGcmFrameProfSnapshot {
    u32 generation;
    u32 flip_sequence;
    u32 source_kind;       /* 1=direct HLE producer, 2=FIFO dispatcher */
    u32 source_host_tid;   /* producer for direct HLE, dispatcher for FIFO */
    u32 put;
    u32 get;
    u32 current;
} CellGcmFrameProfSnapshot;

/* GCM context data — the game reads/writes this to submit RSX commands.
 * On PS3, gCellGcmCurrentContext points to one of these.
 * All pointer fields are guest addresses (u32) in the recomp.
 * Layout verified against compiled SDK inline code (Yakuza: Dead Souls
 * EBOOT, command-write helper at 0xEBC0C8) and RPCS3's CellGcmContextData:
 * begin/end/current/callback, with callback LAST (offset 0xC). The fields
 * are big-endian in guest memory — byte-swap on host access. */
typedef struct CellGcmContextData {
    u32 begin;      /* start of command buffer (guest addr) */
    u32 end;        /* end of command buffer (guest addr) */
    u32 current;    /* current write position (guest addr) */
    u32 callback;   /* overflow callback function OPD (guest addr) */
} CellGcmContextData;

/* Display buffer configuration */
typedef struct CellGcmDisplayInfo {
    u32 offset;         /* offset in local memory */
    u32 pitch;          /* pitch in bytes */
    u32 width;
    u32 height;
} CellGcmDisplayInfo;

enum { CELL_GCM_FLIP_SOURCE_DIRECT_HLE = 1, CELL_GCM_FLIP_SOURCE_FIFO = 2 };

/* Host-side flip handoff. Generation, buffer_id and source travel together so
 * a present/drain boundary cannot pair one flip's sequence with another
 * flip's display surface. */
typedef struct CellGcmFlipTicket {
    u64 generation;
    u32 buffer_id;
    u32 source;
} CellGcmFlipTicket;

enum {
    CELL_GCM_FLIP_PUBLICATION_QPC_OFF = 0,
    CELL_GCM_FLIP_PUBLICATION_QPC_VALID = 1,
    CELL_GCM_FLIP_PUBLICATION_QPC_FAILED = 2
};

/* Optional host diagnostic returned with a ticket. This is deliberately not
 * part of CellGcmFlipTicket: producer/consumer ticket ABI remains unchanged. */
typedef struct CellGcmFlipPublicationTiming {
    u64 publication_qpc;
    u32 publication_qpc_state;
} CellGcmFlipPublicationTiming;

/* Offset table for address translation */
typedef struct CellGcmOffsetTable {
    u16* ioAddress;     /* main memory -> IO offset mapping */
    u16* eaAddress;     /* IO offset -> main memory mapping */
} CellGcmOffsetTable;

/* GCM configuration returned by cellGcmInit */
typedef struct CellGcmConfig {
    u32 localAddress;       /* start of local (video) memory in guest space */
    u32 ioAddress;          /* start of IO-mapped main memory */
    u32 localSize;          /* size of local memory */
    u32 ioSize;             /* size of IO-mapped region */
    u32 memoryFrequency;    /* RSX memory clock */
    u32 coreFrequency;      /* RSX core clock */
} CellGcmConfig;

/* Tile configuration */
typedef struct CellGcmTileInfo {
    u32 tile;           /* tile register value */
    u32 limit;          /* tile limit address */
    u32 pitch;          /* pitch in bytes */
    u32 format;         /* compression format */
    u32 offset;         /* base offset in local memory */
    u32 size;           /* tile region size in bytes */
    u32 base;           /* base address for compression */
    u32 bank;           /* memory bank */
    u8  bound;          /* 1 if bound to RSX, 0 if unbound */
} CellGcmTileInfo;

/* Zcull configuration */
typedef struct CellGcmZcullInfo {
    u32 offset;         /* offset in local memory */
    u32 width;          /* width in pixels */
    u32 height;         /* height in pixels */
    u32 cullStart;      /* zcull start address */
    u32 zFormat;        /* depth format */
    u32 aaFormat;       /* anti-alias format */
    u32 zcullDir;       /* direction */
    u32 zcullFormat;    /* zcull format register */
    u32 sFunc;          /* stencil function */
    u32 sRef;           /* stencil reference */
    u32 sMask;          /* stencil mask */
    u8  bound;          /* 1 if bound to RSX, 0 if unbound */
} CellGcmZcullInfo;

/* Report data (16 bytes, matching RSX report format) */
typedef struct CellGcmReportData {
    u64 timestamp;      /* RSX timestamp */
    u32 value;          /* report value */
    u32 pad;            /* padding to 16 bytes */
} CellGcmReportData;

/* Notify data (same layout as report) */
typedef struct CellGcmNotifyData {
    u64 timestamp;
    u64 zero;           /* always 0 on notification */
} CellGcmNotifyData;

/* Callback types */
typedef void (*CellGcmFlipHandler)(u32 head);
typedef void (*CellGcmVBlankHandler)(u32 head);
typedef void (*CellGcmUserHandler)(u32 cause);
typedef void (*CellGcmSecondVHandler)(u32 head);
typedef s32  (*CellGcmContextCallback)(void* context, u32 count);
typedef void (*CellGcmGraphicsHandler)(u32 val);
typedef void (*CellGcmQueueHandler)(u32 head);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Initialization */

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress);

/* Reusable core of _cellGcmInitBody (NID 0x15BAE46B) — the function the SDK's
 * cellGcmInit() macro actually calls, imported by virtually every PS3 game.
 *
 * It can't be a self-contained HLE function in this library because it must
 * (a) allocate a guest CellGcmContextData and (b) write the game's
 * context-out pointer in guest memory — both of which require the owning game
 * project's vm. So the project's bridge supplies those as callbacks and this
 * helper performs the proven, layout-correct setup:
 *   - calls cellGcmInit(cmdSize, ioSize, ioAddress)
 *   - allocates a 16-byte CellGcmContextData via `galloc`
 *   - fills begin@+0, end@+4, current@+8, callback@+0xC (the layout verified in
 *     the shipping Simpsons port; a wrong layout makes the game read `current`
 *     as the callback OPD and stall) via `gwrite32` (which must byte-swap to BE)
 *   - writes the context's guest address to *ctx_out_addr
 * Returns the guest address of the new CellGcmContextData.
 *
 * Game bridge usage (gpr3=ctx_out, gpr4=cmdSize, gpr5=ioSize, gpr6=ioAddress):
 *   ctx->gpr[3] = cellGcmSetupContext(gpr3, gpr4, gpr5, gpr6,
 *                                     my_guest_alloc, my_vm_write32) ? 0 : err;
 */
typedef u32  (*CellGcmGuestAlloc)(u32 size, u32 align);
typedef void (*CellGcmGuestWrite32)(u32 guest_addr, u32 value);
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32);

s32 cellGcmGetConfiguration(CellGcmConfig* config);

/* Command buffer control */

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void);

/* Read a best-effort, guest-memory-safe FIFO/flip snapshot for TJ_FRAME_PROF. */
void cellGcm_frame_prof_snapshot(CellGcmFrameProfSnapshot* out);

/* Display / flip */

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void);

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode);

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void);

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void);

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void);

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height);

/* NID: 0xEAA52F23 */
s32 cellGcmSetFlipCommand(u32 bufferId);

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue);

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId);

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index);

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void);

/* Host-side coherent flip publication/consumption. `take` consumes exactly
 * one pending SetFlip ticket; `consumed_snapshot` is the ticket currently
 * owning the FIFO batch being drained. */
int cellGcm_take_flip_ticket(CellGcmFlipTicket* out_ticket);
int cellGcm_take_flip_ticket_timed(CellGcmFlipTicket* out_ticket,
                                   CellGcmFlipPublicationTiming* out_timing);
int cellGcm_flip_consumed_ticket_snapshot(CellGcmFlipTicket* out_ticket);
/* After presentation, complete exactly the ticket taken by this host driver.
 * Returns zero for an invalid, stale or already completed ticket. */
int cellGcm_complete_flip_ticket(const CellGcmFlipTicket* ticket);
int cellGcm_flip_uses_vsync(void);

/* Optional host-owned interrupt thread. Register before guest execution and
 * unregister after its final drain. A zero id preserves cooperative pumping.
 * The wake hook only signals the dispatcher; it must not execute guest code. */
typedef void (*CellGcmInterruptWake)(void);
void cellGcmSetHostInterruptDispatcher(u32 host_thread_id, CellGcmInterruptWake wake);

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler);

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler);

/* Host-side ticks. Call from the game's host driver to fire any
 * registered guest VBlank/Flip handlers via the g_ps3_guest_caller
 * mechanism (see ps3emu/guest_call.h). Many games drive their
 * title-screen state machine from the VBlank handler. */
void cellGcmTickVBlank(void);
void cellGcmTickFlip(void);

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler);

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler);

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void);

/* Address translation / IO mapping */

/* NID: 0x0E6B0DFF */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table);

/* NID: 0xDB769B32 */
s32 cellGcmAddressToOffset(u32 address, u32* offset);

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset);

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size);
s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags);

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea);

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io);

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea);

/* Internal location-aware translation.  Unlike the legacy value-only helper,
 * this reports an unmapped MAIN IO page without conflating it with EA zero. */
int cellGcmResolveLocatedChecked(int local, u32 offset, u32* out);

/* Label / report / timestamp */

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index);

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index);

/* NID: 0x97FC4B73 */
u64 cellGcmGetTimeStamp(u32 index);

/* Tile / Zcull */

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank);

/* NID: 0x06EDEA25 */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask);

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index);

/* NID: 0x1F61F9D3 */
s32 cellGcmBindZcull(u8 index);

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index);

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index);

/* Misc */

/* NID: 0x107BF789 */
u32 cellGcmGetTiledPitchSize(u32 size);

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level);

/* --- Additional functions needed by Tokyo Jungle --- */

/* Tile info (alternative to cellGcmSetTile) */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank);

/* Notify data area (similar to report data) */
u32 cellGcmGetNotifyDataAddress(u32 index);

/* Timestamp read from the selected report-data location */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location);

/* Default FIFO size configuration */
s32 cellGcmSetDefaultFifoSize(u32 size);

/* Internal flip commands (called by game code directly) */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId);
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId, u32 labelIndex, u32 labelValue);

/* --- Additional functions (RPCS3 parity) --- */

/* FIFO command buffer callback */
s32 cellGcmCallback(void* context, u32 count);

/* Map RSX local memory */
s32 cellGcmMapLocalMemory(u32* address, u32* size);

/* Shutdown RSX */
void cellGcmTerminate(void);

/* IO map size queries */
u32 cellGcmGetMaxIoMapSize(void);
s32 cellGcmReserveIoMapSize(u32 size);
s32 cellGcmUnreserveIoMapSize(u32 size);

/* VBlank counter */
u32 cellGcmGetVBlankCount(void);

/* Tile / Zcull info getters */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index);
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index);

/* Display info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index);

/* Backend display identity lookup. Matches an exact registered surface, or a
 * strictly proven next-equal-stride history surface, and returns its geometry. */
int cellGcmGetDisplaySurfaceInfo(u32 offset, u32 pitch, u32 width, u32 height,
                                 CellGcmDisplayInfo* out_info);
/* Strict unregistered next-stride history alias -> its two registered parent
 * identities. Used only by deferred backend ownership resolution. */
int cellGcmGetDisplayHistoryAliasParents(u32 offset, u32 pitch, u32 width,
                                         u32 height,
                                         CellGcmDisplayInfo* out_first,
                                         CellGcmDisplayInfo* out_second);
int cellGcmGetDisplaySurfaceContaining(u32 offset, u32 pitch, u32 width, u32 height,
                                       CellGcmDisplayInfo* out_info,
                                       u32* out_x, u32* out_y);

/* Default FIFO mode / command buffer */
void cellGcmInitDefaultFifoMode(s32 mode);
void cellGcmSetDefaultCommandBuffer(void);

/* Debug dump (no-op) */
void cellGcmDumpGraphicsError(void);

/* Default word sizes */
u32 cellGcmGetDefaultCommandWordSize(void);
u32 cellGcmGetDefaultSegmentWordSize(void);

/* Immediate flip */
s32 cellGcmSetFlipImmediate(u32 bufferId);

/* Flip status setter */
void cellGcmSetFlipStatus(u32 status);

/* Graphics / queue handler setters */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler);
void cellGcmSetQueueHandler(CellGcmQueueHandler handler);

/* Frequency / VBlank configuration */
void cellGcmSetSecondVFrequency(u32 freq);
void cellGcmSetVBlankFrequency(u32 freq);

/* User command */
void cellGcmSetUserCommand(u32 cmd);

/* Invalidate tile */
s32 cellGcmSetInvalidateTile(u8 index);

/* EA IO address remap (stub) */
void cellGcmSortRemapEaIoAddress(void);

/* Report data with location */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location);
u32 cellGcmGetReportDataLocation(u32 index, u32 location);


#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GCM_SYS_H */
