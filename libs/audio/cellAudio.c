/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "cellAudio.h"
#include "audio_pacing.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include "../../runtime/memory/vm.h"         /* ppu_vm_size */
#include "../../runtime/memory/guest_world_rendezvous.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                             uint64_t source, uint64_t data1,
                                             uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint64_t ppu_timebase_now(void);

static u64 audio_system_time_us(void)
{
    const u64 frequency = 79800000ULL;
    u64 ticks = ppu_timebase_now();
    return (ticks / frequency) * 1000000ULL +
           (ticks % frequency) * 1000000ULL / frequency;
}

static int audio_tag_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("AUDIO_TAG_TRACE") ? 1 : 0;
    return enabled;
}

static int audio_notify_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("AUDIO_NOTIFY_TRACE") ? 1 : 0;
    return enabled;
}

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* cellAudio.h intentionally exposes only the subset currently consumed by
 * titles. Keep this ABI attribute local until the public header needs it. */
#define CELL_AUDIO_PORTATTR_INITLEVEL  0x0000000000001000ULL

/* This lock is initialized once and deliberately never destroyed. It gates
 * init/quit against public API entry; lock order is lifecycle then audio. */
#ifdef _WIN32
static INIT_ONCE s_audio_lifecycle_once = INIT_ONCE_STATIC_INIT;
static mutex_t s_audio_lifecycle_mutex;

static BOOL CALLBACK audio_lifecycle_mutex_init(PINIT_ONCE once, PVOID arg,
                                                PVOID* context)
{
    (void)once;
    (void)arg;
    (void)context;
    mutex_init(&s_audio_lifecycle_mutex);
    return TRUE;
}

static void audio_lifecycle_lock(void)
{
    InitOnceExecuteOnce(&s_audio_lifecycle_once, audio_lifecycle_mutex_init,
                        NULL, NULL);
    mutex_lock(&s_audio_lifecycle_mutex);
}
#else
static mutex_t s_audio_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

static void audio_lifecycle_lock(void)
{
    mutex_lock(&s_audio_lifecycle_mutex);
}
#endif

static void audio_lifecycle_unlock(void)
{
    mutex_unlock(&s_audio_lifecycle_mutex);
}

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  cur_pos;        /* guest-visible ring position */
    u64                  global_counter; /* monotonic block tag counter */
    u64                  write_index;    /* where the game is writing */
    u64                  timestamp_us;   /* system time at global_counter */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static int            s_audio_shutdown_pending = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];
/* One deterministic 2 MB guest window per audio port. The sample ring starts
 * at the first 1 MB boundary and the read index at the second. The largest
 * legal ring is only 256 KB, so slots never overlap. Reusing the slot on close
 * avoids the old bump allocator exhausting guest address space after repeated
 * PortOpen/PortClose or Init/Quit cycles. */
#define AUDIO_GUEST_DEFAULT_BASE  0x01000000u
#define AUDIO_GUEST_PORT_STRIDE   0x00200000u
#define AUDIO_GUEST_READIDX_OFF   0x00100000u
static u32            s_audio_guest_base = AUDIO_GUEST_DEFAULT_BASE;
static u64            s_audio_global_counter = 0;

void cellAudioSetGuestMemoryBase(u32 base)
{
    audio_lifecycle_lock();
    if (!s_audio_initialized && base && !(base & 0xFFFFFu))
        s_audio_guest_base = base;
    audio_lifecycle_unlock();
}

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Mixing thread */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
static mutex_t       s_audio_mutex;
static int           s_audio_mutex_initialized = 0;
static int           s_mix_thread_live = 0;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];
/* AUDIO_SILENT_OUTPUT swaps only the host submission buffer.  The mixer still
 * consumes guest rings and produces s_mix_buffer for diagnostics. */
static const float s_silent_output_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];
static int s_audio_silent_output;

static const float* audio_submission_buffer(void)
{
    return s_audio_silent_output ? s_silent_output_buffer : s_mix_buffer;
}

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

static int audio_backend_wait_for_block_slot(void)
{
    if (!s_sdl_audio_dev) {
        static unsigned fallback_phase = 0;
        SDL_Delay((fallback_phase++ % 3) == 2 ? 6 : 5);
        return s_mix_thread_running != 0;
    }

    while (s_mix_thread_running) {
        u32 queued = audio_backend_queued_samples();
        if (ps3_audio_full_block_fits(CELL_AUDIO_BLOCK_SAMPLES * 4, queued))
            return 1;
        SDL_Delay(1);
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode with a ~20ms buffer. TJ_AUDIO_BUFFER_MS
     * (10..200) enlarges it: the mixer keeps the shared-mode buffer full, so
     * a longer buffer rides out host stalls (alt-tab, CPU load) that would
     * otherwise drain 20 ms and crackle, at the cost of that much latency. */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
    {
        const char* ms_text = getenv("TJ_AUDIO_BUFFER_MS");
        long ms = ms_text && *ms_text ? strtol(ms_text, NULL, 10) : 0;
        if (ms >= 10 && ms <= 200) {
            buf_duration = (REFERENCE_TIME)ms * 10000;
            printf("[cellAudio] WASAPI buffer %ld ms\n", ms);
        }
    }
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            0, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available)
        num_samples = available;

    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples, 0);
    }
}

static u32 audio_backend_queued_samples(void)
{
    if (!s_wasapi_client) return UINT32_MAX;
    UINT32 padding = 0;
    if (FAILED(s_wasapi_client->lpVtbl->GetCurrentPadding(
            s_wasapi_client, &padding)))
        return UINT32_MAX;
    return padding;
}

static int audio_backend_wait_for_block_slot(void)
{
    if (!s_wasapi_client || !s_wasapi_render) {
        static unsigned fallback_phase = 0;
        Sleep((fallback_phase++ % 3) == 2 ? 6 : 5);
        return s_mix_thread_running != 0;
    }

    while (s_mix_thread_running) {
        u32 padding = audio_backend_queued_samples();
        if (ps3_audio_full_block_fits(s_wasapi_buf_frames, padding))
            return 1;

        if (s_wasapi_event)
            WaitForSingleObject(s_wasapi_event, 10);
        else
            Sleep(1);
    }
    return 0;
}

#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;

        /* cur_pos is the ring slot currently being consumed. It is distinct
         * from the monotonically increasing block-tag counter. */
        u32 block_idx = (u32)port->cur_pos;
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d tag=%llu pos=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->global_counter,
                        (unsigned long long)port->cur_pos, block_idx, map);
            } } }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Firmware exposes the next ring position (0..nBlock-1), not the
         * monotonic audio tag. Tokyo Jungle wraps its local cursor to 0..15;
         * publishing the tag here makes that cursor permanently unable to
         * catch the guest-visible value after the first wrap, so PCM callbacks
         * never run. The index is a 64-bit big-endian guest value. */
        port->global_counter = s_audio_global_counter;
        port->timestamp_us = audio_system_time_us();
        port->cur_pos = ps3_audio_next_ring_position(port->cur_pos, nblock);
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->cur_pos);
    }

    /* One global tag timeline is shared by all ports, as on cellAudio. */
    s_audio_global_counter++;

    mutex_unlock(&s_audio_mutex);

    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue).
     * Snapshot under the same mutex used by set/remove, but never hold it
     * while looking up or pushing to an external event queue. */
    AudioNotifySlot notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];
    mutex_lock(&s_audio_mutex);
    memcpy(notify_queues, s_notify_queues, sizeof(notify_queues));
    mutex_unlock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(notify_queues[i].key);
        int rc = qid ? sys_event_queue_push_by_id(qid, notify_queues[i].key,
                                                  0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0)
                     : -1;
        if (audio_notify_trace_enabled()) {
            static unsigned count;
            if (count++ < 128)
                fprintf(stderr,
                        "[audio-notify] slot=%d key=0x%llX qid=%u push=%d\n",
                        i, (unsigned long long)notify_queues[i].key, qid, rc);
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    if (!guest_world_register_current("audio-mix")) return 0;
    printf("[cellAudio] Mixing thread started\n");
    /* TJ_AUDIO_MMCSS=1: schedule the mixer like other game audio (MMCSS
     * "Games" task + time-critical priority) so CPU load elsewhere cannot
     * starve the output buffer. avrt.dll is loaded on demand. */
    {
        const char* mm = getenv("TJ_AUDIO_MMCSS");
        if (mm && *mm && strcmp(mm, "0") != 0) {
            typedef HANDLE (WINAPI *AvSetFn)(LPCWSTR, LPDWORD);
            HMODULE avrt = LoadLibraryW(L"avrt.dll");
            AvSetFn av_set = avrt ? (AvSetFn)(void*)GetProcAddress(
                avrt, "AvSetMmThreadCharacteristicsW") : NULL;
            DWORD task_index = 0;
            HANDLE task = av_set ? av_set(L"Games", &task_index) : NULL;
            BOOL prio = SetThreadPriority(GetCurrentThread(),
                                          THREAD_PRIORITY_TIME_CRITICAL);
            printf("[cellAudio] mixer MMCSS %s, time-critical %s\n",
                   task ? "on" : "unavailable", prio ? "on" : "failed");
        }
    }

    while (s_mix_thread_running) {
        /* Consume one guest ring block only when the host can accept the full
         * 256-frame block. The previous 2 ms loop outran 48 kHz by ~2.6x and
         * still advanced the guest read index when WASAPI accepted a partial
         * write, causing underruns and over-frequent notify events. */
        if (!audio_backend_wait_for_block_slot())
            break;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        audio_backend_submit(audio_submission_buffer(), CELL_AUDIO_BLOCK_SAMPLES);

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    guest_world_unregister_current();
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    if (!guest_world_register_current("audio-mix")) return NULL;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        if (!audio_backend_wait_for_block_slot())
            break;
        audio_mix_one_block();
        audio_backend_submit(audio_submission_buffer(), CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    guest_world_unregister_current();
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
#endif
    s_mix_thread_live = 1;
    return 0;
}

static int audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
    if (!s_mix_thread_live)
        return 0;
#ifdef _WIN32
    if (!s_mix_thread) {
        fprintf(stderr, "[cellAudio] mixing thread handle missing\n");
        return -1;
    }
    DWORD wait = WaitForSingleObject(s_mix_thread, 2000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "[cellAudio] mixing thread did not exit (wait=%lu)\n",
                (unsigned long)wait);
        return -1;
    }
    CloseHandle(s_mix_thread);
    s_mix_thread = NULL;
#else
    int rc = pthread_join(s_mix_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "[cellAudio] mixing thread join failed (%d)\n", rc);
        return -1;
    }
#endif
    s_mix_thread_live = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

static int audio_guest_range_valid(u32 ea, size_t size)
{
    if (!ea || size == 0 || !VM_READY)
        return 0;
    return !ppu_vm_size || ((u64)ea + size <= ppu_vm_size);
}

s32 cellAudioInit(void)
{
    printf("[cellAudio] Init()\n");

    audio_lifecycle_lock();
    if (s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_ALREADY_INIT;
    }
    if (s_audio_shutdown_pending) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    s_audio_global_counter = 0;
    { const char* silent = getenv("AUDIO_SILENT_OUTPUT");
      s_audio_silent_output = silent && silent[0] == '1' && silent[1] == '\0'; }
    if (!s_audio_mutex_initialized) {
        mutex_init(&s_audio_mutex);
        s_audio_mutex_initialized = 1;
    }

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }

    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    audio_lifecycle_lock();
    if (!s_audio_initialized && !s_audio_shutdown_pending) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    /* Reject new public calls before waiting for the mix thread. The persistent
     * audio mutex remains valid even if this wait times out. */
    s_audio_initialized = 0;
    s_audio_shutdown_pending = 1;
    if (audio_stop_mix_thread() < 0) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    mutex_lock(&s_audio_mutex);
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }
    mutex_unlock(&s_audio_mutex);

    s_audio_shutdown_pending = 0;
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    /* param / portNum are GUEST addresses. Decode the 32-bit PPU ABI fields
     * individually; raw struct access is both host-endian and host-layout
     * dependent. */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!audio_guest_range_valid(param_ea, sizeof(CellAudioPortParam)) ||
        !audio_guest_range_valid(portNum_ea, sizeof(u32))) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    u64 attr = vm_read64(param_ea + 16);  /* attr     */
    float level = 1.0f;
    if (attr & CELL_AUDIO_PORTATTR_INITLEVEL) {
        u32 level_bits = vm_read32(param_ea + 24); /* float level */
        memcpy(&level, &level_bits, sizeof(level));
        if (!isfinite(level)) {
            audio_lifecycle_unlock();
            return CELL_AUDIO_ERROR_PARAM;
        }
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;
    }
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store decoded host-endian guest fields, not raw BE bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->param.attr     = attr;
    port->param.level    = level;
    port->cur_pos = 0;
    port->global_counter = 0;
    port->write_index = 0;
    port->timestamp_us = audio_system_time_us();

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    u32 guest_buf = s_audio_guest_base +
                    (u32)found * AUDIO_GUEST_PORT_STRIDE;
    u32 guest_ridx = guest_buf + AUDIO_GUEST_READIDX_OFF;
    uint64_t guest_end = (uint64_t)guest_ridx + sizeof(u64);
    if (!audio_guest_range_valid(guest_buf, port->buf_size) ||
        !audio_guest_range_valid(guest_ridx, sizeof(u64)) ||
        (ppu_vm_size && guest_end > ppu_vm_size)) {
        memset(port, 0, sizeof(*port));
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* current ring position (BE u64) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    if (portNum >= CELL_AUDIO_PORT_MAX) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (bump-allocated in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    if (s_ports[portNum].running) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;
    }

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    if (!s_ports[portNum].running) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;
    }

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            audio_lifecycle_unlock();
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            audio_lifecycle_unlock();
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!audio_guest_range_valid(cfg, 32)) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }
    u32 tag_ea = (u32)(uintptr_t)tag;
    if (portNum >= CELL_AUDIO_PORT_MAX ||
        !audio_guest_range_valid(tag_ea, sizeof(u64))) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    if (!port->in_use) {
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }
    if (blockNo >= port->param.nBlock) {
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    u64 value = ps3_audio_port_block_tag(port->global_counter,
                                         port->cur_pos, blockNo);
    if (audio_tag_trace_enabled()) {
        static unsigned count;
        if (count++ < 128)
            fprintf(stderr, "[audio-tag] block port=%u block=%llu global=%llu pos=%llu -> tag=%llu\n",
                    portNum, (unsigned long long)blockNo,
                    (unsigned long long)port->global_counter,
                    (unsigned long long)port->cur_pos,
                    (unsigned long long)value);
    }
    vm_write64(tag_ea, value);

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }
    u32 stamp_ea = (u32)(uintptr_t)stamp;
    if (portNum >= CELL_AUDIO_PORT_MAX ||
        !audio_guest_range_valid(stamp_ea, sizeof(u64))) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    if (!port->in_use) {
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    u64 value = 0;
    if (!ps3_audio_tag_timestamp(port->global_counter, tag,
                                 port->timestamp_us, &value)) {
        if (audio_tag_trace_enabled()) {
            static unsigned pending_count;
            if (pending_count++ < 128)
                fprintf(stderr, "[audio-tag] timestamp port=%u tag=%llu global=%llu -> PENDING\n",
                        portNum, (unsigned long long)tag,
                        (unsigned long long)port->global_counter);
        }
        mutex_unlock(&s_audio_mutex);
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;
    }

    if (audio_tag_trace_enabled()) {
        static unsigned ready_count;
        if (ready_count++ < 128)
            fprintf(stderr, "[audio-tag] timestamp port=%u tag=%llu global=%llu -> %llu\n",
                    portNum, (unsigned long long)tag,
                    (unsigned long long)port->global_counter,
                    (unsigned long long)value);
    }

    vm_write64(stamp_ea, value);

    mutex_unlock(&s_audio_mutex);
    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    audio_lifecycle_lock();
    if (!s_audio_initialized) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_NOT_INIT;
    }

    if (portNum >= CELL_AUDIO_PORT_MAX || !status) {
        audio_lifecycle_unlock();
        return CELL_AUDIO_ERROR_PARAM;
    }

    mutex_lock(&s_audio_mutex);
    if (!s_ports[portNum].in_use) {
        *status = CELL_AUDIO_STATUS_CLOSE;
    } else if (s_ports[portNum].running) {
        *status = CELL_AUDIO_STATUS_RUN;
    } else {
        *status = CELL_AUDIO_STATUS_READY;
    }
    mutex_unlock(&s_audio_mutex);

    audio_lifecycle_unlock();
    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
