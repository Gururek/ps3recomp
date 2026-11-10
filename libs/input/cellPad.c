/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

#if defined(_WIN32) && !PAD_BACKEND_XINPUT
  #include <windows.h>
#endif

#if !defined(_WIN32)
  #include <pthread.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];

#if defined(_WIN32)
static SRWLOCK s_host_state_lock = SRWLOCK_INIT;
static void pad_state_lock(void) { AcquireSRWLockExclusive(&s_host_state_lock); }
static void pad_state_unlock(void) { ReleaseSRWLockExclusive(&s_host_state_lock); }
static SRWLOCK s_getdata_lock = SRWLOCK_INIT;
static void pad_getdata_lock(void) { AcquireSRWLockExclusive(&s_getdata_lock); }
static void pad_getdata_unlock(void) { ReleaseSRWLockExclusive(&s_getdata_lock); }
#else
static pthread_mutex_t s_host_state_lock = PTHREAD_MUTEX_INITIALIZER;
static void pad_state_lock(void) { pthread_mutex_lock(&s_host_state_lock); }
static void pad_state_unlock(void) { pthread_mutex_unlock(&s_host_state_lock); }
static pthread_mutex_t s_getdata_lock = PTHREAD_MUTEX_INITIALIZER;
static void pad_getdata_lock(void) { pthread_mutex_lock(&s_getdata_lock); }
static void pad_getdata_unlock(void) { pthread_mutex_unlock(&s_getdata_lock); }
#endif

#if defined(_WIN32)
static volatile LONG     s_debug_pulse_buttons = 0;
static volatile LONGLONG s_debug_pulse_until = 0;
static volatile LONG     s_debug_pulse_hold_ms = 0;
static volatile LONG     s_debug_pulse_neutral_pending = 0;
static volatile LONG     s_debug_pulse_press_pending = 0;
static volatile LONG     s_debug_analog_left = 0x8080;
static volatile LONGLONG s_debug_analog_until = 0;

void cellPadDebugPulse(u16 buttons, u32 hold_ms)
{
    pad_getdata_lock();
    if (!hold_ms) hold_ms = 120;
    InterlockedExchange(&s_debug_pulse_buttons, (LONG)buttons);
    InterlockedExchange(&s_debug_pulse_hold_ms, (LONG)hold_ms);
    InterlockedExchange64(&s_debug_pulse_until, 0);
    InterlockedExchange(&s_debug_pulse_press_pending, 0);
    /* Button handlers consume edges, but some Tokyo Jungle transitions do not
     * poll while a wall-clock pulse expires. Guarantee one guest-visible
     * neutral sample before arming each synthetic press. */
    InterlockedExchange(&s_debug_pulse_neutral_pending, 1);
    pad_getdata_unlock();
}

void cellPadDebugAnalogPulse(u8 left_x, u8 left_y, u32 hold_ms)
{
    pad_getdata_lock();
    if (!hold_ms) hold_ms = 120;
    InterlockedExchange(&s_debug_analog_left,
                        (LONG)left_x | ((LONG)left_y << 8));
    InterlockedExchange64(&s_debug_analog_until,
                          (LONGLONG)(GetTickCount64() + hold_ms));
    pad_getdata_unlock();
}

/* Private test channel used when the hidden D3D window is no longer pumping
 * WM_APP messages. The runner creates both objects before process start and
 * opts in by setting PS3_PAD_DEBUG_CHANNEL. The seven DWORD fields are fixed
 * little-endian values: magic, sequence, buttons, hold_ms, left_x, left_y,
 * kind. This is test-only and leaves normal physical/controller polling alone. */
#define PAD_DEBUG_CHANNEL_MAGIC       0x50414443u /* "PADC" */
#define PAD_DEBUG_CHANNEL_KIND_BUTTON 1u
#define PAD_DEBUG_CHANNEL_KIND_ANALOG 2u

typedef struct {
    DWORD magic;
    DWORD sequence;
    DWORD buttons;
    DWORD hold_ms;
    DWORD left_x;
    DWORD left_y;
    DWORD kind;
} PadDebugChannelCommand;

static INIT_ONCE s_debug_channel_once = INIT_ONCE_STATIC_INIT;
static HANDLE s_debug_channel_mapping = NULL;
static HANDLE s_debug_channel_event = NULL;
static const PadDebugChannelCommand* s_debug_channel_command = NULL;
static DWORD s_debug_channel_sequence = 0;

static BOOL CALLBACK pad_debug_channel_init(PINIT_ONCE once, PVOID parameter,
                                            PVOID* context)
{
    char channel_name[128];
    char event_name[134];
    DWORD channel_length;

    (void)once;
    (void)parameter;
    (void)context;

    channel_length = GetEnvironmentVariableA("PS3_PAD_DEBUG_CHANNEL",
                                               channel_name,
                                               (DWORD)sizeof(channel_name));
    if (!channel_length || channel_length >= sizeof(channel_name) ||
        channel_length + sizeof(".event") > sizeof(event_name))
        return TRUE;

    memcpy(event_name, channel_name, channel_length);
    memcpy(event_name + channel_length, ".event", sizeof(".event"));

    s_debug_channel_mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, channel_name);
    if (!s_debug_channel_mapping) {
        fprintf(stderr, "[cellPad] debug channel mapping open failed Win32=%lu\n",
                (unsigned long)GetLastError());
        return TRUE;
    }

    s_debug_channel_command = (const PadDebugChannelCommand*)MapViewOfFile(
        s_debug_channel_mapping, FILE_MAP_READ, 0, 0, sizeof(PadDebugChannelCommand));
    if (!s_debug_channel_command) {
        fprintf(stderr, "[cellPad] debug channel view map failed Win32=%lu\n",
                (unsigned long)GetLastError());
        CloseHandle(s_debug_channel_mapping);
        s_debug_channel_mapping = NULL;
        return TRUE;
    }

    s_debug_channel_event = OpenEventA(SYNCHRONIZE, FALSE, event_name);
    if (!s_debug_channel_event) {
        fprintf(stderr, "[cellPad] debug channel event open failed Win32=%lu\n",
                (unsigned long)GetLastError());
        UnmapViewOfFile(s_debug_channel_command);
        s_debug_channel_command = NULL;
        CloseHandle(s_debug_channel_mapping);
        s_debug_channel_mapping = NULL;
    } else {
        fprintf(stderr, "[cellPad] debug channel ready\n");
    }
    fflush(stderr);
    return TRUE;
}

static void pad_poll_debug_channel(void)
{
    PadDebugChannelCommand command;

    if (!InitOnceExecuteOnce(&s_debug_channel_once, pad_debug_channel_init, NULL, NULL) ||
        !s_debug_channel_command || !s_debug_channel_event ||
        WaitForSingleObject(s_debug_channel_event, 0) != WAIT_OBJECT_0)
        return;

    MemoryBarrier();
    command = *s_debug_channel_command;
    MemoryBarrier();
    if (command.magic != PAD_DEBUG_CHANNEL_MAGIC || !command.sequence ||
        command.sequence == s_debug_channel_sequence)
        return;

    s_debug_channel_sequence = command.sequence;
    fprintf(stderr,
            "[cellPad] debug channel command sequence=%lu kind=%lu buttons=0x%04lX hold_ms=%lu axes=%lu,%lu\n",
            (unsigned long)command.sequence, (unsigned long)command.kind,
            (unsigned long)command.buttons, (unsigned long)command.hold_ms,
            (unsigned long)command.left_x, (unsigned long)command.left_y);
    fflush(stderr);
    if (command.kind == PAD_DEBUG_CHANNEL_KIND_BUTTON &&
        command.buttons <= 0xFFFFu) {
        cellPadDebugPulse((u16)command.buttons, (u32)command.hold_ms);
    } else if (command.kind == PAD_DEBUG_CHANNEL_KIND_ANALOG &&
               command.buttons == 0 && command.left_x <= 0xFFu &&
               command.left_y <= 0xFFu) {
        cellPadDebugAnalogPulse((u8)command.left_x, (u8)command.left_y,
                                 (u32)command.hold_ms);
    }
}

static void pad_apply_debug_pulse(PadHostState* effective, u32 port_no)
{
    ULONGLONG until;

    if (port_no != 0)
        return;

    if (InterlockedExchange(&s_debug_pulse_neutral_pending, 0)) {
        u16 pulse = (u16)InterlockedCompareExchange(&s_debug_pulse_buttons, 0, 0);
        effective->connected = 1;
        effective->buttons &= (u16)~pulse;
        if (pulse & CELL_PAD_CTRL_TRIANGLE) effective->press_triangle = 0;
        if (pulse & CELL_PAD_CTRL_CIRCLE) effective->press_circle = 0;
        if (pulse & CELL_PAD_CTRL_CROSS) effective->press_cross = 0;
        if (pulse & CELL_PAD_CTRL_SQUARE) effective->press_square = 0;
        if (pulse & CELL_PAD_CTRL_L1) effective->press_l1 = 0;
        if (pulse & CELL_PAD_CTRL_R1) effective->press_r1 = 0;
        InterlockedExchange(&s_debug_pulse_press_pending, 1);
        return;
    }

    if (InterlockedExchange(&s_debug_pulse_press_pending, 0)) {
        LONG hold_ms = InterlockedCompareExchange(&s_debug_pulse_hold_ms, 0, 0);
        InterlockedExchange64(&s_debug_pulse_until,
                              (LONGLONG)(GetTickCount64() + (ULONGLONG)hold_ms));
    }

    until = (ULONGLONG)InterlockedCompareExchange64(&s_debug_pulse_until, 0, 0);
    if (until > GetTickCount64()) {
        u16 pulse = (u16)InterlockedCompareExchange(&s_debug_pulse_buttons, 0, 0);
        effective->connected = 1;
        effective->buttons |= pulse;
        effective->press_triangle = (pulse & CELL_PAD_CTRL_TRIANGLE) ? 255 : effective->press_triangle;
        effective->press_circle   = (pulse & CELL_PAD_CTRL_CIRCLE) ? 255 : effective->press_circle;
        effective->press_cross    = (pulse & CELL_PAD_CTRL_CROSS) ? 255 : effective->press_cross;
        effective->press_square   = (pulse & CELL_PAD_CTRL_SQUARE) ? 255 : effective->press_square;
        effective->press_l1       = (pulse & CELL_PAD_CTRL_L1) ? 255 : effective->press_l1;
        effective->press_r1       = (pulse & CELL_PAD_CTRL_R1) ? 255 : effective->press_r1;
    }

    until = (ULONGLONG)InterlockedCompareExchange64(&s_debug_analog_until, 0, 0);
    if (until > GetTickCount64()) {
        LONG left = InterlockedCompareExchange(&s_debug_analog_left, 0, 0);
        effective->connected = 1;
        effective->analog_lx = (u8)(left & 0xFF);
        effective->analog_ly = (u8)((left >> 8) & 0xFF);
    }
}
#else
void cellPadDebugPulse(u16 buttons, u32 hold_ms)
{
    (void)buttons;
    (void)hold_ms;
}


void cellPadDebugAnalogPulse(u8 left_x, u8 left_y, u32 hold_ms)
{
    (void)left_x;
    (void)left_y;
    (void)hold_ms;
}
#endif

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static int pad_key_down(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

/* ---------------------------------------------------------------------------
 * Configurable bindings (keyboard + XInput) from the INI file named by
 * TJ_CONTROLS_FILE: [keyboard] action = Key[, Key[, Key]] and
 * [controller] action = Button. Missing file or entries keep the built-in
 * defaults below; an empty value unbinds. The file is re-read when its
 * modification time changes (checked at most once per second), so edits
 * apply while the game runs.
 * -----------------------------------------------------------------------*/

enum {
    PAD_ACT_CROSS, PAD_ACT_CIRCLE, PAD_ACT_SQUARE, PAD_ACT_TRIANGLE,
    PAD_ACT_L1, PAD_ACT_R1, PAD_ACT_L2, PAD_ACT_R2, PAD_ACT_L3, PAD_ACT_R3,
    PAD_ACT_START, PAD_ACT_SELECT,
    PAD_ACT_UP, PAD_ACT_DOWN, PAD_ACT_LEFT, PAD_ACT_RIGHT,       /* last controller-bindable action */
    PAD_ACT_LS_UP, PAD_ACT_LS_DOWN, PAD_ACT_LS_LEFT, PAD_ACT_LS_RIGHT,
    PAD_ACT_RS_UP, PAD_ACT_RS_DOWN, PAD_ACT_RS_LEFT, PAD_ACT_RS_RIGHT,
    PAD_ACT_COUNT
};
#define PAD_KEYS_PER_ACT 3
#define PAD_XI_LT 0x10000u   /* pseudo-buttons for the analog triggers */
#define PAD_XI_RT 0x20000u

static const struct { const char* name; u16 ctrl; } s_pad_acts[PAD_ACT_COUNT] = {
    {"cross", CELL_PAD_CTRL_CROSS}, {"circle", CELL_PAD_CTRL_CIRCLE},
    {"square", CELL_PAD_CTRL_SQUARE}, {"triangle", CELL_PAD_CTRL_TRIANGLE},
    {"l1", CELL_PAD_CTRL_L1}, {"r1", CELL_PAD_CTRL_R1},
    {"l2", CELL_PAD_CTRL_L2}, {"r2", CELL_PAD_CTRL_R2},
    {"l3", CELL_PAD_CTRL_L3}, {"r3", CELL_PAD_CTRL_R3},
    {"start", CELL_PAD_CTRL_START}, {"select", CELL_PAD_CTRL_SELECT},
    {"up", CELL_PAD_CTRL_UP}, {"down", CELL_PAD_CTRL_DOWN},
    {"left", CELL_PAD_CTRL_LEFT}, {"right", CELL_PAD_CTRL_RIGHT},
    {"lstick_up", 0}, {"lstick_down", 0}, {"lstick_left", 0}, {"lstick_right", 0},
    {"rstick_up", 0}, {"rstick_down", 0}, {"rstick_left", 0}, {"rstick_right", 0},
};

static const struct { const char* name; int vk; } s_pad_key_names[] = {
    {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Backspace", VK_BACK},
    {"Tab", VK_TAB}, {"Escape", VK_ESCAPE}, {"CapsLock", VK_CAPITAL},
    {"Shift", VK_SHIFT}, {"LShift", VK_LSHIFT}, {"RShift", VK_RSHIFT},
    {"Ctrl", VK_CONTROL}, {"LCtrl", VK_LCONTROL}, {"RCtrl", VK_RCONTROL},
    {"Alt", VK_MENU}, {"LAlt", VK_LMENU}, {"RAlt", VK_RMENU},
    {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME},
    {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
    {"Numpad0", VK_NUMPAD0}, {"Numpad1", VK_NUMPAD1}, {"Numpad2", VK_NUMPAD2},
    {"Numpad3", VK_NUMPAD3}, {"Numpad4", VK_NUMPAD4}, {"Numpad5", VK_NUMPAD5},
    {"Numpad6", VK_NUMPAD6}, {"Numpad7", VK_NUMPAD7}, {"Numpad8", VK_NUMPAD8},
    {"Numpad9", VK_NUMPAD9}, {"NumpadMultiply", VK_MULTIPLY}, {"NumpadAdd", VK_ADD},
    {"NumpadSubtract", VK_SUBTRACT}, {"NumpadDecimal", VK_DECIMAL},
    {"NumpadDivide", VK_DIVIDE},
    {"Semicolon", VK_OEM_1}, {"Equals", VK_OEM_PLUS}, {"Comma", VK_OEM_COMMA},
    {"Minus", VK_OEM_MINUS}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2},
    {"Backtick", VK_OEM_3}, {"LeftBracket", VK_OEM_4}, {"Backslash", VK_OEM_5},
    {"RightBracket", VK_OEM_6}, {"Quote", VK_OEM_7},
    {"MouseLeft", VK_LBUTTON}, {"MouseRight", VK_RBUTTON},
    {"MouseMiddle", VK_MBUTTON}, {"Mouse4", VK_XBUTTON1}, {"Mouse5", VK_XBUTTON2},
};

static const struct { const char* name; unsigned mask; } s_pad_xi_names[] = {
    {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"LT", PAD_XI_LT}, {"RT", PAD_XI_RT},
    {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"Start", XINPUT_GAMEPAD_START}, {"Back", XINPUT_GAMEPAD_BACK},
    {"DpadUp", XINPUT_GAMEPAD_DPAD_UP}, {"DpadDown", XINPUT_GAMEPAD_DPAD_DOWN},
    {"DpadLeft", XINPUT_GAMEPAD_DPAD_LEFT}, {"DpadRight", XINPUT_GAMEPAD_DPAD_RIGHT},
};

static int      s_pad_keys[PAD_ACT_COUNT][PAD_KEYS_PER_ACT];
static unsigned s_pad_xi[PAD_ACT_RIGHT + 1];

static void pad_bindings_defaults(void)
{
    static const int keys[PAD_ACT_COUNT][PAD_KEYS_PER_ACT] = {
        {VK_SPACE, 'Z'}, {'F'}, {'E'}, {'V'},
        {VK_LCONTROL}, {VK_LSHIFT}, {'1'}, {'3'}, {0}, {VK_CAPITAL},
        {VK_RETURN}, {VK_BACK},
        {VK_NUMPAD8}, {VK_NUMPAD2}, {VK_NUMPAD4}, {VK_NUMPAD6},
        {'W'}, {'S'}, {'A'}, {'D'},
        {VK_UP}, {VK_DOWN}, {VK_LEFT}, {VK_RIGHT},
    };
    static const unsigned xi[PAD_ACT_RIGHT + 1] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, PAD_XI_LT, PAD_XI_RT,
        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
    };
    memcpy(s_pad_keys, keys, sizeof(s_pad_keys));
    memcpy(s_pad_xi, xi, sizeof(s_pad_xi));
}

/* ---------------------------------------------------------------------------
 * PC key prompts (cellPad_prompt_keys in cellPad.h). The renderer (atlas
 * key caps) and the game's text hook read the primary key of each action
 * without the pad lock: guest threads hold that lock while XInput polls all
 * ports. Writers run under the pad lock and publish through a sequence
 * counter (odd while the copy is being written).
 *
 * Active device: keyboard/mouse (default) until a controller button, a
 * trigger past half or a stick past half is newly pressed; a newly pressed
 * bound key switches back. Both count only while the game window is in
 * front, and the device changes at most once per PAD_PROMPT_SWITCH_MS (both
 * devices in use at once). While the F1/F2 screen is open, keys and device
 * stay as they were (art and text change together when it closes).
 * TJ_PROMPTS=0 turns the whole feature off (original PS3 prompts everywhere).
 * -----------------------------------------------------------------------*/
#define PAD_PROMPT_SWITCH_MS 500u
static volatile LONG   s_prompt_seq;            /* even: s_prompt_vks/sig stable */
static int             s_prompt_vks[PAD_ACT_COUNT];
static volatile LONG64 s_prompt_keys_sig;       /* 0 until the bindings are known */
static volatile LONG   s_prompt_device;         /* 0 keyboard/mouse, 1 controller */
static ULONGLONG       s_prompt_device_tick;    /* last device change, pad lock */
static unsigned        s_prompt_kb_prev;        /* bound-key mask, pad lock */
static unsigned        s_prompt_xi_prev[PAD_MAX_HOST_PORTS];

int cellPad_overlay_is_open(void);

static int pad_prompt_window_in_front(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

static int pad_prompt_enabled(void)
{
    static volatile LONG enabled = -1;
    LONG e = enabled;
    if (e < 0) {
        const char* v = getenv("TJ_PROMPTS");
        e = (v && strcmp(v, "0") == 0) ? 0 : 1;
        if (!e) fprintf(stderr, "[tj-prompts] disabled (TJ_PROMPTS=0): original PS3 prompts\n");
        /* Keyboard input switched off: a controller is the only device
         * (TJ_PROMPTS_TEST_KEYBOARD keeps keyboard prompts for hidden tests). */
        if (e && getenv("PS3_PAD_DISABLE_KEYBOARD") && !getenv("TJ_PROMPTS_TEST_KEYBOARD"))
            InterlockedExchange(&s_prompt_device, 1);
        InterlockedExchange(&enabled, e);
    }
    return (int)e;
}

/* Caller holds the pad lock. Publishes the primary (first nonzero) key of
 * every action when it changed. Held while F1/F2 is open: the close path
 * (cellPad_overlay_toggle) publishes the committed keys. */
static void pad_prompt_publish(void)
{
    if (!pad_prompt_enabled() || cellPad_overlay_is_open()) return;
    int vks[PAD_ACT_COUNT];
    unsigned long long h = 14695981039346656037ull;   /* FNV-1a over the primary keys */
    for (int a = 0; a < PAD_ACT_COUNT; a++) {
        vks[a] = 0;
        for (int k = 0; k < PAD_KEYS_PER_ACT && !vks[a]; k++) vks[a] = s_pad_keys[a][k];
        for (int b = 0; b < 4; b++) {
            h ^= (unsigned)(((unsigned)vks[a] >> (8 * b)) & 0xFFu);
            h *= 1099511628211ull;
        }
    }
    if (!h) h = 1;
    if ((unsigned long long)s_prompt_keys_sig == h) return;
    InterlockedIncrement(&s_prompt_seq);               /* odd: writing */
    memcpy(s_prompt_vks, vks, sizeof(vks));
    InterlockedExchange64(&s_prompt_keys_sig, (LONG64)h);
    InterlockedIncrement(&s_prompt_seq);               /* even: stable */
}

static void pad_prompt_set_device(LONG device)
{
    if (s_prompt_device == device || cellPad_overlay_is_open()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - s_prompt_device_tick < PAD_PROMPT_SWITCH_MS) return;
    s_prompt_device_tick = now;
    InterlockedExchange(&s_prompt_device, device);
}

int cellPad_prompt_keys(unsigned long long* sig, int vks[CELL_PAD_PROMPT_ACTIONS])
{
    if (!pad_prompt_enabled() || s_prompt_device) return 0;
    for (int attempt = 0; attempt < 1000; attempt++) {
        const LONG s1 = s_prompt_seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        const unsigned long long h = (unsigned long long)s_prompt_keys_sig;
        if (vks) memcpy(vks, s_prompt_vks, sizeof(s_prompt_vks));
        MemoryBarrier();
        if (s_prompt_seq != s1) continue;
        if (!h) return 0;
        if (sig) *sig = h;
        return 1;
    }
    return 0;
}
_Static_assert(PAD_ACT_COUNT == CELL_PAD_PROMPT_ACTIONS, "cellPad_prompt_keys action order");

/* Pad poll (pad lock held): a newly pressed bound key selects keyboard
 * prompts. Only called while the game window is in front. */
static void pad_prompt_note_keyboard(unsigned down_mask)
{
    const unsigned edge = down_mask & ~s_prompt_kb_prev;
    s_prompt_kb_prev = down_mask;
    if (edge) pad_prompt_set_device(0);
}

/* Pad poll (pad lock held): a newly pressed button, or a trigger/stick newly
 * past half, selects the original PS3 prompts. Resting sticks and triggers
 * (drift) never switch. gp == NULL: port disconnected. */
static void pad_prompt_note_xinput(int port, const XINPUT_GAMEPAD* gp)
{
    if (port < 0 || port >= PAD_MAX_HOST_PORTS) return;
    unsigned active = 0;
    if (gp) {
        active = gp->wButtons;
        if (gp->bLeftTrigger > 128)  active |= 1u << 16;
        if (gp->bRightTrigger > 128) active |= 1u << 17;
        if (gp->sThumbLX >  16384) active |= 1u << 18;
        if (gp->sThumbLX < -16384) active |= 1u << 19;
        if (gp->sThumbLY >  16384) active |= 1u << 20;
        if (gp->sThumbLY < -16384) active |= 1u << 21;
        if (gp->sThumbRX >  16384) active |= 1u << 22;
        if (gp->sThumbRX < -16384) active |= 1u << 23;
        if (gp->sThumbRY >  16384) active |= 1u << 24;
        if (gp->sThumbRY < -16384) active |= 1u << 25;
    }
    const unsigned edge = active & ~s_prompt_xi_prev[port];
    s_prompt_xi_prev[port] = active;
    /* Same rule as the keyboard: a pad touched while another window is in
     * front (or during a hidden run) does not change the prompts. */
    if (edge && pad_prompt_enabled() && pad_prompt_window_in_front())
        pad_prompt_set_device(1);
}

static char* pad_trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* Returns the virtual-key code for a binding name, or 0 if unknown. */
static int pad_key_from_name(const char* s)
{
    if (s[0] && !s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9' &&
        (s[2] == '\0' || (s[2] >= '0' && s[2] <= '9' && s[3] == '\0'))) {
        int n = atoi(s + 1);
        if (n >= 3 && n <= 24) return VK_F1 + n - 1;   /* F1/F2 open the Controls/Options screens */
    }
    for (size_t i = 0; i < sizeof(s_pad_key_names) / sizeof(s_pad_key_names[0]); i++)
        if (_stricmp(s_pad_key_names[i].name, s) == 0) return s_pad_key_names[i].vk;
    return 0;
}

/* Returns 1 when the file was applied (or rejected for good), 0 when it could
 * not be opened (the caller retries on the next check). Bindings change only
 * after a successful read. */
static int pad_bindings_load(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int saved_keys[PAD_ACT_COUNT][PAD_KEYS_PER_ACT];
    unsigned saved_xi[PAD_ACT_RIGHT + 1];
    memcpy(saved_keys, s_pad_keys, sizeof(saved_keys));
    memcpy(saved_xi, s_pad_xi, sizeof(saved_xi));
    pad_bindings_defaults();
    char line[512];
    int section = 0, line_no = 0;   /* 1 = [keyboard], 2 = [controller] */
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            fprintf(stderr, "[cellPad] controls line %d: too long, ignored\n", line_no);
            continue;
        }
        char* s = line;
        if (line_no == 1) {
            if ((unsigned char)s[0] == 0xFF || (unsigned char)s[0] == 0xFE) {
                fprintf(stderr, "[cellPad] controls file is UTF-16; save it as UTF-8 or ANSI\n");
                memcpy(s_pad_keys, saved_keys, sizeof(saved_keys));
                memcpy(s_pad_xi, saved_xi, sizeof(saved_xi));
                fclose(f);
                return 1;
            }
            if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
                s += 3;   /* UTF-8 BOM */
        }
        s = pad_trim(s);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            section = _strnicmp(s, "[keyboard]", 10) == 0 ? 1
                    : _strnicmp(s, "[controller]", 12) == 0 ? 2 : 0;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq || !section) continue;
        *eq = '\0';
        char* name = pad_trim(s);
        char* value = pad_trim(eq + 1);
        int act = -1;
        for (int a = 0; a < PAD_ACT_COUNT; a++)
            if (_stricmp(s_pad_acts[a].name, name) == 0) { act = a; break; }
        if (act < 0) {
            fprintf(stderr, "[cellPad] controls line %d: unknown action '%s'\n", line_no, name);
            continue;
        }
        if (section == 1) {
            int keys[PAD_KEYS_PER_ACT] = {0};
            int count = 0, bad = 0;
            for (char* tok = strtok(value, ","); tok; tok = strtok(NULL, ",")) {
                char* t = pad_trim(tok);
                if (!*t) continue;
                int vk = pad_key_from_name(t);
                if (!vk) {
                    fprintf(stderr, "[cellPad] controls line %d: unknown key '%s'\n", line_no, t);
                    bad = 1;
                } else if (count < PAD_KEYS_PER_ACT) {
                    keys[count++] = vk;
                }
            }
            if (count || !bad) memcpy(s_pad_keys[act], keys, sizeof(keys));
        } else if (act <= PAD_ACT_RIGHT) {
            if (!*value || _stricmp(value, "None") == 0) { s_pad_xi[act] = 0; continue; }
            int found = 0;
            for (size_t i = 0; i < sizeof(s_pad_xi_names) / sizeof(s_pad_xi_names[0]); i++)
                if (_stricmp(s_pad_xi_names[i].name, value) == 0) {
                    s_pad_xi[act] = s_pad_xi_names[i].mask;
                    found = 1;
                    break;
                }
            if (!found)
                fprintf(stderr, "[cellPad] controls line %d: unknown controller button '%s'\n",
                        line_no, value);
        }
    }
    fclose(f);
    fprintf(stderr, "[cellPad] controls loaded from %s\n", path);
    return 1;
}

/* Called from the pad poll (serialized by s_host_state_lock); reloads when
 * the file changes. */
static char s_pad_controls_path[MAX_PATH];

static FILETIME s_pad_loaded_time;      /* write time of the file the tables came from */
static void pad_ovl_changed(void);

/* Defaults + file path, once. Caller holds the pad lock. */
static void pad_bindings_init(void)
{
    static int initialized;
    if (initialized) return;
    initialized = 1;
    pad_bindings_defaults();
    const char* p = getenv("TJ_CONTROLS_FILE");
    if (p && *p) snprintf(s_pad_controls_path, sizeof(s_pad_controls_path), "%s", p);
    pad_prompt_publish();
}

static void pad_bindings_refresh(void)
{
    static ULONGLONG next_check;
    pad_bindings_init();
    const char* path = s_pad_controls_path;
    if (!path[0]) return;
    ULONGLONG now = GetTickCount64();
    if (now < next_check) return;
    next_check = now + 1000;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &s_pad_loaded_time) == 0) return;
    if (pad_bindings_load(path)) {          /* open failed: retry next second */
        s_pad_loaded_time = fa.ftLastWriteTime;
        pad_prompt_publish();
        pad_ovl_changed();                  /* the Controls screen shows the new file */
    }
}

static u8 pad_xi_trigger_value(unsigned bind, const XINPUT_GAMEPAD* gp, int pressed)
{
    if (bind == PAD_XI_LT) return gp->bLeftTrigger;
    if (bind == PAD_XI_RT) return gp->bRightTrigger;
    return pressed ? 255 : 0;
}

/* ---------------------------------------------------------------------------
 * In-game Controls screen (F1, or Back+Start on a controller). The renderer
 * draws the image built here over the game and the window procedure forwards
 * keys; both run on the presenter thread. Binding changes take the pad lock
 * because guest threads poll the pad. Game input is held neutral while open.
 * Closing saves to TJ_CONTROLS_FILE in the same format the parser reads.
 *
 * Second page: Options (F2, or LB/RB from the Controls page). The game host
 * registers its options at startup with cellPad_overlay_register_option; this
 * file only lists them and calls their get/set callbacks (the host owns the
 * values and their file). With no registered option the page does not exist:
 * F2 and LB/RB do nothing.
 * -----------------------------------------------------------------------*/
#pragma comment(lib, "gdi32.lib")

#define PAD_OVL_COL_PAD 3              /* columns 0-2 keys, 3 controller */
#define PAD_OVL_PAGE_CONTROLS 0
#define PAD_OVL_PAGE_OPTIONS  1
#define PAD_OVL_MAX_OPTIONS   8

typedef struct PadOvlOption {
    const char* label;
    const char* help;
    const char* const* values;
    int count;
    int (*get)(void);
    int (*set)(int);                   /* returns 1 saved, 0 not saved, -1 change refused */
} PadOvlOption;

static PadOvlOption s_ovl_opts[PAD_OVL_MAX_OPTIONS];
static volatile LONG s_ovl_opt_count;
static int s_ovl_page, s_ovl_opt_row;

void cellPad_overlay_toggle(void);
void cellPad_overlay_key(int vk, long lp);
void cellPad_overlay_page_key(int page);
int  cellPad_overlay_register_option(const char* label, const char* help,
                                     const char* const* values, int count,
                                     int (*get)(void), int (*set)(int));

static volatile LONG s_ovl_open;
static volatile LONG s_ovl_hold;       /* per-port mask: neutral until that port's buttons are released */
static ULONGLONG s_ovl_hold_until;     /* the hold never outlasts this tick count */
static int  s_ovl_row, s_ovl_col, s_ovl_capture, s_ovl_dirty, s_ovl_reset_armed;
static LONG s_ovl_version = 1, s_ovl_drawn;
static WORD s_ovl_prev_buttons[4];
static BYTE s_ovl_prev_lt[4], s_ovl_prev_rt[4];
static int  s_ovl_combo_down;
static char s_ovl_status[160];         /* Controls page messages */
static char s_ovl_opt_status[160];     /* Options page messages */

/* What each PS3 input does in Tokyo Jungle (from the game's own tutorial text
 * and player guides); inputs with no known action show just the button. */
static const char* const s_ovl_labels[PAD_ACT_COUNT] = {
    "Jump  (Cross)", "Eat / mark / carry  (Circle)", "Claw attack  (Square)",
    "Pack: fight / decoy  (Triangle)", "Creep  (L1)", "Bite  (R1)",
    "Pack commands  (L2)", "R2", "L3 (left stick click)", "Roar  (R3)",
    "Pause / map  (Start)", "Info  (Select)",
    "Items  (D-pad up)", "D-pad down", "D-pad left", "D-pad right",
    "Move up", "Move down", "Move left", "Move right",
    "Dodge up  (right stick)", "Dodge down", "Dodge left", "Dodge right",
};

static void pad_ovl_hold_all(void)
{
    s_ovl_hold_until = GetTickCount64() + 2000;
    InterlockedExchange(&s_ovl_hold, 0xF);
}

static const char* pad_vk_name(int vk)
{
    static char buf[8];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1); return buf; }
    for (size_t i = 0; i < sizeof(s_pad_key_names) / sizeof(s_pad_key_names[0]); i++)
        if (s_pad_key_names[i].vk == vk) return s_pad_key_names[i].name;
    return NULL;
}

static const char* pad_xi_name(unsigned mask)
{
    for (size_t i = 0; i < sizeof(s_pad_xi_names) / sizeof(s_pad_xi_names[0]); i++)
        if (s_pad_xi_names[i].mask == mask) return s_pad_xi_names[i].name;
    return "None";
}

static int pad_ovl_cols_for_row(int row) { return row <= PAD_ACT_RIGHT ? 4 : 3; }

static void pad_ovl_changed(void) { InterlockedIncrement(&s_ovl_version); }

/* Writes the current bindings; caller must not hold the pad lock. */
static int pad_bindings_save(void)
{
    if (!s_pad_controls_path[0]) return 0;
    int keys[PAD_ACT_COUNT][PAD_KEYS_PER_ACT];
    unsigned xi[PAD_ACT_RIGHT + 1];
    pad_state_lock();
    memcpy(keys, s_pad_keys, sizeof(keys));
    memcpy(xi, s_pad_xi, sizeof(xi));
    pad_state_unlock();
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_pad_controls_path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "[cellPad] controls NOT saved: cannot write %s\n", tmp);
        snprintf(s_ovl_status, sizeof(s_ovl_status), "Could not save the controls file (it will retry on close).");
        return 0;
    }
    fprintf(f, "; Tokyo Jungle PC controls. Change them in game with F1, with \"Tokyo Jungle Controls\", or by hand.\n");
    fprintf(f, "; [keyboard] action = Key, Key, Key  (empty = unbound)   [controller] action = Button (None = unbound)\n\n[keyboard]\n");
    for (int a = 0; a < PAD_ACT_COUNT; a++) {
        fprintf(f, "%s =", s_pad_acts[a].name);
        int first = 1;
        for (int k = 0; k < PAD_KEYS_PER_ACT; k++) {
            const char* n = keys[a][k] ? pad_vk_name(keys[a][k]) : NULL;
            if (!n) continue;
            fprintf(f, "%s%s", first ? " " : ", ", n);
            first = 0;
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n[controller]\n");
    for (int a = 0; a <= PAD_ACT_RIGHT; a++)
        fprintf(f, "%s = %s\n", s_pad_acts[a].name, xi[a] ? pad_xi_name(xi[a]) : "None");
    int ok = fclose(f) == 0;
    DWORD err = 0;
    /* The game's own reload or another program may hold the file for an instant. */
    for (int attempt = 0; ok && attempt < 6; attempt++) {
        if (MoveFileExA(tmp, s_pad_controls_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { err = 0; break; }
        err = GetLastError();
        Sleep(50);
    }
    if (!ok || err) {
        DeleteFileA(tmp);
        fprintf(stderr, "[cellPad] controls NOT saved to %s (Win32 %lu)\n", s_pad_controls_path, (unsigned long)err);
        snprintf(s_ovl_status, sizeof(s_ovl_status), "Could not save the controls file (Win32 %lu); it will retry on close.",
                 (unsigned long)err);
        return 0;
    }
    /* Remember our own write so the poll does not reload what we just saved. */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(s_pad_controls_path, GetFileExInfoStandard, &fa)) {
        pad_state_lock();
        s_pad_loaded_time = fa.ftLastWriteTime;
        pad_state_unlock();
    }
    fprintf(stderr, "[cellPad] controls saved to %s\n", s_pad_controls_path);
    return 1;
}

/* Pad poll (under the pad lock): keep the game's view neutral while the
 * screen is open and until the keys/buttons used to close it are released. */
static void pad_ovl_block_input(void)
{
    if (!s_ovl_open && !s_ovl_hold) return;
    if (!s_ovl_open && GetTickCount64() > s_ovl_hold_until) { InterlockedExchange(&s_ovl_hold, 0); return; }
    LONG hold = s_ovl_hold;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        PadHostState* p = &s_host_state[i];
        if (!s_ovl_open) {
            if (i >= 4 || !(hold & (1L << i))) continue;
            if (!p->buttons && p->trigger_l2 <= PAD_TRIGGER_THRESHOLD && p->trigger_r2 <= PAD_TRIGGER_THRESHOLD) {
                hold &= ~(1L << i);    /* this port is released: give it back to the game */
                continue;
            }
        }
        const int connected = p->connected;
        memset(p, 0, sizeof(*p));
        p->connected = connected;
        p->analog_lx = p->analog_ly = p->analog_rx = p->analog_ry = 128;
    }
    if (!s_ovl_open) InterlockedExchange(&s_ovl_hold, hold);
}

int cellPad_overlay_is_open(void) { return s_ovl_open != 0; }
int cellPad_overlay_capturing(void) { return s_ovl_open && s_ovl_capture; }

/* Game options for the Options page. Call before the window opens (startup);
 * label/help/values must stay valid for the whole run. Returns the option's
 * index, or -1 when it was rejected. */
int cellPad_overlay_register_option(const char* label, const char* help,
                                    const char* const* values, int count,
                                    int (*get)(void), int (*set)(int))
{
    const LONG n = s_ovl_opt_count;
    if (!label || !values || count < 2 || !get || !set || n >= PAD_OVL_MAX_OPTIONS) {
        fprintf(stderr, "[cellPad] option '%s' not registered\n", label ? label : "(null)");
        return -1;
    }
    for (int i = 0; i < count; i++)
        if (!values[i]) return -1;
    PadOvlOption o = {label, help, values, count, get, set};
    s_ovl_opts[n] = o;
    InterlockedExchange(&s_ovl_opt_count, n + 1);
    pad_ovl_changed();
    return (int)n;
}

static void pad_ovl_open_at(int col)
{
    pad_state_lock();
    pad_bindings_init();
    pad_state_unlock();
    s_ovl_page = PAD_OVL_PAGE_CONTROLS;
    s_ovl_row = 0; s_ovl_col = col; s_ovl_capture = 0; s_ovl_reset_armed = 0;
    if (!s_pad_controls_path[0])
        snprintf(s_ovl_status, sizeof(s_ovl_status), "No controls file set: changes last until the game closes.");
    else if (!s_ovl_dirty)
        s_ovl_status[0] = 0;   /* keep a pending "could not save" message */
    InterlockedExchange(&s_ovl_open, 1);
    pad_ovl_changed();
}

/* Shows a page; opens the screen when it is closed. A pending binding
 * capture is cancelled first. */
static void pad_ovl_show_page(int page)
{
    if (page == PAD_OVL_PAGE_CONTROLS && !s_ovl_open) { pad_ovl_open_at(0); return; }
    s_ovl_capture = 0;
    s_ovl_reset_armed = 0;
    s_ovl_page = page;
    if (page == PAD_OVL_PAGE_OPTIONS) {
        s_ovl_opt_status[0] = 0;
        if (s_ovl_opt_row >= (int)s_ovl_opt_count) s_ovl_opt_row = 0;
    }
    if (page == PAD_OVL_PAGE_CONTROLS) {
        pad_state_lock();
        pad_bindings_init();
        pad_state_unlock();
        if (s_ovl_row >= PAD_ACT_COUNT) s_ovl_row = 0;
        if (s_ovl_col >= pad_ovl_cols_for_row(s_ovl_row)) s_ovl_col = 0;
        /* Same messages as opening with F1. */
        if (!s_pad_controls_path[0])
            snprintf(s_ovl_status, sizeof(s_ovl_status), "No controls file set: changes last until the game closes.");
        else if (!s_ovl_dirty)
            s_ovl_status[0] = 0;   /* keep a pending "could not save" message */
    }
    InterlockedExchange(&s_ovl_open, 1);
    pad_ovl_changed();
}

/* F1 (page 0) / F2 (page 1) from the window procedure: opens that page,
 * switches to it from the other page, or closes it when it is showing.
 * F1 while waiting for a key still only cancels the wait. */
void cellPad_overlay_page_key(int page)
{
    if (page && !s_ovl_opt_count) return;   /* no Options page without options */
    page = page ? PAD_OVL_PAGE_OPTIONS : PAD_OVL_PAGE_CONTROLS;
    if (!s_ovl_open) { pad_ovl_show_page(page); return; }
    if (s_ovl_capture) {
        cellPad_overlay_key(VK_ESCAPE, 0);
        if (page == s_ovl_page) return;
    }
    if (page == s_ovl_page) cellPad_overlay_toggle();
    else pad_ovl_show_page(page);
}

static void pad_ovl_option_step(int index, int dir)
{
    if (index < 0 || index >= (int)s_ovl_opt_count) return;
    const PadOvlOption* o = &s_ovl_opts[index];
    int cur = o->get();
    if (cur < 0 || cur >= o->count) cur = 0;
    const int next = (cur + dir + o->count) % o->count;
    const int saved = o->set(next);
    int now = o->get();
    if (now < 0 || now >= o->count) now = 0;
    snprintf(s_ovl_opt_status, sizeof(s_ovl_opt_status), "%s: %s%s", o->label, o->values[now],
             saved < 0 ? "  (not available)"
                       : saved ? "  (saved)" : "  (could not save: lasts until the game closes)");
}

static void pad_ovl_options_key(int vk, long lp)
{
    const int repeat = (lp & (1L << 30)) != 0;
    const int n = (int)s_ovl_opt_count;
    switch (vk) {
    case VK_UP:    if (n) s_ovl_opt_row = (s_ovl_opt_row + n - 1) % n; break;
    case VK_DOWN:  if (n) s_ovl_opt_row = (s_ovl_opt_row + 1) % n; break;
    case VK_LEFT: case VK_RIGHT: case VK_RETURN: case VK_SPACE:
        if (repeat) return;
        pad_ovl_option_step(s_ovl_opt_row, vk == VK_LEFT ? -1 : 1);
        break;
    case VK_ESCAPE:
        if (repeat) return;
        cellPad_overlay_toggle();
        return;
    default:
        return;
    }
    pad_ovl_changed();
}

void cellPad_overlay_toggle(void)
{
    if (!s_ovl_open) { pad_ovl_open_at(0); return; }
    s_ovl_capture = 0;
    s_ovl_reset_armed = 0;
    if (s_ovl_dirty && pad_bindings_save()) s_ovl_dirty = 0;   /* failed: stays dirty, next close retries */
    pad_ovl_hold_all();
    InterlockedExchange(&s_ovl_open, 0);
    pad_state_lock();
    pad_prompt_publish();       /* key prompts: the keys set while the screen was open */
    pad_state_unlock();
    pad_ovl_changed();
}

static void pad_ovl_set_key(int row, int col, int vk)
{
    pad_state_lock();
    if (col < PAD_KEYS_PER_ACT) {
        /* A key appears once per action; binding it again just moves it here. */
        for (int k = 0; k < PAD_KEYS_PER_ACT; k++)
            if (k != col && vk && s_pad_keys[row][k] == vk) s_pad_keys[row][k] = 0;
        s_pad_keys[row][col] = vk;
    } else if (row <= PAD_ACT_RIGHT) {
        s_pad_xi[row] = (unsigned)vk;
    }
    pad_prompt_publish();
    pad_state_unlock();
    s_ovl_dirty = 1;
}

/* Keyboard and mouse input from the window procedure (key-down messages). */
void cellPad_overlay_key(int vk, long lp)
{
    if (!s_ovl_open) return;
    if (s_ovl_page == PAD_OVL_PAGE_OPTIONS) { pad_ovl_options_key(vk, lp); return; }
    const int repeat = (lp & (1L << 30)) != 0;
    if (vk == VK_SHIFT)   vk = (int)MapVirtualKeyA((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) vk = (lp & (1L << 24)) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU)    vk = (lp & (1L << 24)) ? VK_RMENU : VK_LMENU;
    if (s_ovl_capture) {
        if (repeat) return;
        if (vk == VK_ESCAPE) {
            s_ovl_capture = 0;
            snprintf(s_ovl_status, sizeof(s_ovl_status), "Cancelled.");
        } else if (s_ovl_col < PAD_OVL_COL_PAD && vk != VK_F1 && vk != VK_F2 && pad_vk_name(vk)) {
            pad_ovl_set_key(s_ovl_row, s_ovl_col, vk);
            s_ovl_capture = 0;
            snprintf(s_ovl_status, sizeof(s_ovl_status), "%s set to %s.", s_ovl_labels[s_ovl_row], pad_vk_name(vk));
        }
        pad_ovl_changed();
        return;
    }
    const int cols = pad_ovl_cols_for_row(s_ovl_row);
    if (vk != VK_F5) s_ovl_reset_armed = 0;
    switch (vk) {
    case VK_UP:    s_ovl_row = (s_ovl_row + PAD_ACT_COUNT - 1) % PAD_ACT_COUNT; break;
    case VK_DOWN:  s_ovl_row = (s_ovl_row + 1) % PAD_ACT_COUNT; break;
    case VK_LEFT:  s_ovl_col = (s_ovl_col + cols - 1) % cols; break;
    case VK_RIGHT: s_ovl_col = (s_ovl_col + 1) % cols; break;
    case VK_RETURN: case VK_SPACE:
        if (repeat) return;
        s_ovl_capture = 1;
        s_ovl_status[0] = 0;
        break;
    case VK_DELETE: case VK_BACK:
        if (repeat) return;
        pad_ovl_set_key(s_ovl_row, s_ovl_col, 0);
        snprintf(s_ovl_status, sizeof(s_ovl_status), "Cleared.");
        break;
    case VK_F5:
        if (repeat) return;
        if (!s_ovl_reset_armed) {   /* F5 is a common quick-save key: ask for a second press */
            s_ovl_reset_armed = 1;
            snprintf(s_ovl_status, sizeof(s_ovl_status), "Press F5 again to reset ALL controls to the defaults.");
            break;
        }
        s_ovl_reset_armed = 0;
        pad_state_lock();
        pad_bindings_defaults();
        pad_prompt_publish();
        pad_state_unlock();
        s_ovl_dirty = 1;
        snprintf(s_ovl_status, sizeof(s_ovl_status), "All controls reset to defaults.");
        break;
    case VK_ESCAPE:
        if (repeat) return;
        cellPad_overlay_toggle();
        return;
    default:
        return;
    }
    if (s_ovl_col >= pad_ovl_cols_for_row(s_ovl_row)) s_ovl_col = pad_ovl_cols_for_row(s_ovl_row) - 1;
    pad_ovl_changed();
}

/* Once per presented frame: controller toggle (Back+Start), navigation and
 * controller-button capture. */
void cellPad_overlay_tick(void)
{
    /* Same rules as the game's own controller path: honour the automation
     * switch, and only react while the game window has focus. */
    static int disable_xinput = -1;
    if (disable_xinput < 0) disable_xinput = getenv("PS3_PAD_DISABLE_XINPUT") ? 1 : 0;
    if (disable_xinput) return;
    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &fg_pid) || fg_pid != GetCurrentProcessId()) return;
    WORD pressed_any = 0, held_any = 0;
    unsigned new_pad = 0;
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_STATE st;
        memset(&st, 0, sizeof(st));
        /* Probing an empty slot is slow; re-check disconnected ones once a second. */
        static ULONGLONG next_probe[4];
        const ULONGLONG now = GetTickCount64();
        if (now < next_probe[i]) continue;
        if (XInputGetState(i, &st) != ERROR_SUCCESS) {
            s_ovl_prev_buttons[i] = 0; s_ovl_prev_lt[i] = s_ovl_prev_rt[i] = 0;
            next_probe[i] = now + 1000;
            continue;
        }
        WORD b = st.Gamepad.wButtons;
        WORD edge = (WORD)(b & ~s_ovl_prev_buttons[i]);
        int lt = st.Gamepad.bLeftTrigger > PAD_TRIGGER_THRESHOLD, rt = st.Gamepad.bRightTrigger > PAD_TRIGGER_THRESHOLD;
        if (!new_pad) {
            if (lt && !s_ovl_prev_lt[i]) new_pad = PAD_XI_LT;
            else if (rt && !s_ovl_prev_rt[i]) new_pad = PAD_XI_RT;
            else if (edge) {
                for (size_t n = 0; n < sizeof(s_pad_xi_names) / sizeof(s_pad_xi_names[0]); n++)
                    if (s_pad_xi_names[n].mask <= 0xFFFF && (edge & s_pad_xi_names[n].mask)) { new_pad = s_pad_xi_names[n].mask; break; }
            }
        }
        pressed_any |= edge;
        held_any |= b;
        s_ovl_prev_buttons[i] = b;
        s_ovl_prev_lt[i] = (BYTE)lt; s_ovl_prev_rt[i] = (BYTE)rt;
    }
    const int combo = (held_any & XINPUT_GAMEPAD_BACK) && (held_any & XINPUT_GAMEPAD_START);
    if (combo && !s_ovl_combo_down) {
        s_ovl_combo_down = 1;
        if (s_ovl_open) cellPad_overlay_toggle();
        else pad_ovl_open_at(PAD_OVL_COL_PAD);   /* opened by controller: start on its column */
        return;
    }
    if (!combo) s_ovl_combo_down = 0;
    if (!s_ovl_open || s_ovl_combo_down) return;
    if (s_ovl_page == PAD_OVL_PAGE_OPTIONS) {
        if (pressed_any & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
            pad_ovl_show_page(PAD_OVL_PAGE_CONTROLS);
            return;
        }
        if (pressed_any & XINPUT_GAMEPAD_DPAD_UP)    cellPad_overlay_key(VK_UP, 0);
        if (pressed_any & XINPUT_GAMEPAD_DPAD_DOWN)  cellPad_overlay_key(VK_DOWN, 0);
        if (pressed_any & XINPUT_GAMEPAD_DPAD_LEFT)  cellPad_overlay_key(VK_LEFT, 0);
        if (pressed_any & XINPUT_GAMEPAD_DPAD_RIGHT) cellPad_overlay_key(VK_RIGHT, 0);
        if (pressed_any & XINPUT_GAMEPAD_A)          cellPad_overlay_key(VK_RETURN, 0);
        if (pressed_any & XINPUT_GAMEPAD_B)          cellPad_overlay_toggle();
        return;
    }
    if (!s_ovl_capture && s_ovl_opt_count &&
        (pressed_any & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
        pad_ovl_show_page(PAD_OVL_PAGE_OPTIONS);
        return;
    }
    if (s_ovl_capture) {
        if (s_ovl_col == PAD_OVL_COL_PAD && new_pad) {
            pad_ovl_set_key(s_ovl_row, s_ovl_col, (int)new_pad);
            s_ovl_capture = 0;
            snprintf(s_ovl_status, sizeof(s_ovl_status), "%s set to %s.", s_ovl_labels[s_ovl_row], pad_xi_name(new_pad));
            pad_ovl_changed();
        } else if (s_ovl_col < PAD_OVL_COL_PAD && (pressed_any & XINPUT_GAMEPAD_B)) {
            cellPad_overlay_key(VK_ESCAPE, 0);   /* B cancels waiting for a key */
        }
        return;
    }
    if (pressed_any & XINPUT_GAMEPAD_DPAD_UP)    cellPad_overlay_key(VK_UP, 0);
    if (pressed_any & XINPUT_GAMEPAD_DPAD_DOWN)  cellPad_overlay_key(VK_DOWN, 0);
    if (pressed_any & XINPUT_GAMEPAD_DPAD_LEFT)  cellPad_overlay_key(VK_LEFT, 0);
    if (pressed_any & XINPUT_GAMEPAD_DPAD_RIGHT) cellPad_overlay_key(VK_RIGHT, 0);
    if (pressed_any & XINPUT_GAMEPAD_A)          cellPad_overlay_key(VK_RETURN, 0);
    if (pressed_any & XINPUT_GAMEPAD_X)          cellPad_overlay_key(VK_DELETE, 0);
    if (pressed_any & XINPUT_GAMEPAD_B)          cellPad_overlay_toggle();
}

static void pad_ovl_text(HDC dc, HFONT font, COLORREF color, int x, int y, int w, int h, const char* s, UINT fmt)
{
    RECT r = {x, y, x + w, y + h};
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextA(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void pad_ovl_box(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF edge)
{
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(dc, &r, b);
    DeleteObject(b);
    if (edge != fill) {
        HBRUSH e = CreateSolidBrush(edge);
        FrameRect(dc, &r, e);
        DeleteObject(e);
    }
}

static void pad_ovl_draw_controls(HDC dc, int w, int h, HFONT f_title, HFONT f_head, HFONT f_row, HFONT f_small);
static void pad_ovl_draw_options(HDC dc, int w, int h, HFONT f_title, HFONT f_head, HFONT f_row, HFONT f_small);

/* Draws the screen into a BGRA image (w x h, pitch bytes per row) when its
 * content changed. Returns 1 if the image was rewritten. */
int cellPad_overlay_render(void* bgra, unsigned pitch, unsigned w, unsigned h)
{
    static HDC dc;
    static HBITMAP bmp;
    static void* bits;
    static HFONT f_title, f_head, f_row, f_small;
    static unsigned bw, bh;
    LONG version = s_ovl_version;
    if (version == s_ovl_drawn && bw == w && bh == h) return 0;
    if (!dc || bw != w || bh != h) {
        if (bmp) DeleteObject(bmp);
        if (!dc) dc = CreateCompatibleDC(NULL);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;      /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!dc || !bmp) return 0;
        SelectObject(dc, bmp);
        bw = w; bh = h;
        f_title = CreateFontA(-34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
        f_head  = CreateFontA(-17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
        /* 15 px text fits the 20 px rows without clipping descenders. */
        f_row   = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
        f_small = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    }
    pad_ovl_box(dc, 0, 0, (int)w, (int)h, RGB(14, 16, 20), RGB(90, 90, 90));
    SetBkMode(dc, TRANSPARENT);
    if (s_ovl_page == PAD_OVL_PAGE_OPTIONS)
        pad_ovl_draw_options(dc, (int)w, (int)h, f_title, f_head, f_row, f_small);
    else
        pad_ovl_draw_controls(dc, (int)w, (int)h, f_title, f_head, f_row, f_small);
    GdiFlush();

    /* GDI leaves alpha at 0: make the panel uniformly 92% opaque. */
    for (unsigned y = 0; y < h; y++) {
        const unsigned char* src = (const unsigned char*)bits + (size_t)y * w * 4;
        unsigned char* dst = (unsigned char*)bgra + (size_t)y * pitch;
        for (unsigned x = 0; x < w; x++) {
            dst[x * 4 + 0] = src[x * 4 + 0];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 2];
            dst[x * 4 + 3] = 235;
        }
    }
    s_ovl_drawn = version;
    return 1;
}

static const COLORREF s_ovl_fg = RGB(232, 232, 232), s_ovl_dim = RGB(150, 150, 150);
static const COLORREF s_ovl_accent = RGB(240, 196, 64);

static void pad_ovl_draw_controls(HDC dc, int w, int h, HFONT f_title, HFONT f_head, HFONT f_row, HFONT f_small)
{
    (void)h;
    const COLORREF fg = s_ovl_fg, dim = s_ovl_dim, accent = s_ovl_accent;
    int keys[PAD_ACT_COUNT][PAD_KEYS_PER_ACT];
    unsigned xi[PAD_ACT_RIGHT + 1];
    pad_state_lock();
    memcpy(keys, s_pad_keys, sizeof(keys));
    memcpy(xi, s_pad_xi, sizeof(xi));
    pad_state_unlock();

    pad_ovl_text(dc, f_title, accent, 32, 14, 400, 44, "CONTROLS", DT_LEFT);
    pad_ovl_text(dc, f_small, dim, (int)w - 532, 8, 500, 26,
                 s_ovl_opt_count ? "F1 / Esc / B: save and close    F2 / LB / RB: Options"
                                 : "F1 / Esc / B: save and close", DT_RIGHT);
    pad_ovl_text(dc, f_small, dim, (int)w - 532, 32, 500, 22, "The game keeps running: pause it first (Start).", DT_RIGHT);

    const int col_x[5] = {32, 326, 470, 614, 790};
    const int col_w[5] = {288, 140, 140, 140, 190};
    const char* heads[5] = {"Action", "Key 1", "Key 2", "Key 3", "Controller"};
    for (int c = 0; c < 5; c++) pad_ovl_text(dc, f_head, dim, col_x[c], 64, col_w[c], 24, heads[c], DT_LEFT);
    const int row_y0 = 92, row_h = 20;   /* 24 rows end at 572; footer + status fit in 640 */
    for (int a = 0; a < PAD_ACT_COUNT; a++) {
        const int y = row_y0 + a * row_h;
        if (a == s_ovl_row) pad_ovl_box(dc, 24, y, (int)w - 48, row_h, RGB(30, 33, 40), RGB(30, 33, 40));
        pad_ovl_text(dc, f_row, fg, col_x[0], y, col_w[0], row_h, s_ovl_labels[a], DT_LEFT);
        for (int c = 0; c < 4; c++) {
            char cell[48];
            COLORREF color = fg;
            if (c < PAD_KEYS_PER_ACT) {
                const char* n = keys[a][c] ? pad_vk_name(keys[a][c]) : NULL;
                snprintf(cell, sizeof(cell), "%s", n ? n : "-");
                if (!n) color = dim;
            } else if (a <= PAD_ACT_RIGHT) {
                snprintf(cell, sizeof(cell), "%s", xi[a] ? pad_xi_name(xi[a]) : "None");
                if (!xi[a]) color = dim;
            } else {
                snprintf(cell, sizeof(cell), "%s", a < PAD_ACT_RS_UP ? "left stick" : "right stick");
                color = dim;
            }
            const int selected = a == s_ovl_row && c == s_ovl_col;
            if (selected) {
                pad_ovl_box(dc, col_x[c + 1] - 6, y + 1, col_w[c + 1], row_h - 2,
                            s_ovl_capture ? RGB(110, 84, 16) : RGB(52, 46, 24), accent);
                if (s_ovl_capture)
                    snprintf(cell, sizeof(cell), "%s", c < PAD_KEYS_PER_ACT ? "press a key..." : "press a button...");
                color = s_ovl_capture ? RGB(255, 255, 255) : accent;
            }
            pad_ovl_text(dc, f_row, color, col_x[c + 1], y, col_w[c + 1] - 12, row_h, cell, DT_LEFT);
        }
    }
    const int foot_y = row_y0 + PAD_ACT_COUNT * row_h + 8;
    pad_ovl_text(dc, f_small, dim, 32, foot_y, (int)w - 64, 22,
                 "Arrows / D-pad: move    Enter / A: change    Delete / X: clear    F5 twice: all defaults    Esc / B while changing: cancel",
                 DT_LEFT);
    if (s_ovl_status[0])
        pad_ovl_text(dc, f_small, accent, 32, foot_y + 22, (int)w - 64, 22, s_ovl_status, DT_LEFT);
}

/* Options page: one row per registered option (label, its one-line help,
 * and the current value), then the key help and the last change. */
static void pad_ovl_draw_options(HDC dc, int w, int h, HFONT f_title, HFONT f_head, HFONT f_row, HFONT f_small)
{
    const COLORREF fg = s_ovl_fg, dim = s_ovl_dim, accent = s_ovl_accent;
    pad_ovl_text(dc, f_title, accent, 32, 14, 400, 44, "OPTIONS", DT_LEFT);
    pad_ovl_text(dc, f_small, dim, w - 532, 8, 500, 26, "F2 / Esc / B: close    F1 / LB / RB: Controls", DT_RIGHT);
    pad_ovl_text(dc, f_small, dim, w - 532, 32, 500, 22, "The game keeps running: pause it first (Start).", DT_RIGHT);
    pad_ovl_text(dc, f_head, dim, 32, 64, 500, 24, "Option", DT_LEFT);
    pad_ovl_text(dc, f_head, dim, 760, 64, 200, 24, "Setting", DT_LEFT);
    const int n = (int)s_ovl_opt_count;
    const int row_y0 = 96, row_h = 48;
    if (!n)
        pad_ovl_text(dc, f_row, dim, 32, row_y0, w - 64, 24, "No options for this game.", DT_LEFT);
    for (int i = 0; i < n; i++) {
        const PadOvlOption* o = &s_ovl_opts[i];
        const int y = row_y0 + i * row_h;
        const int selected = i == s_ovl_opt_row;
        if (selected) pad_ovl_box(dc, 24, y, w - 48, row_h - 4, RGB(30, 33, 40), RGB(30, 33, 40));
        pad_ovl_text(dc, f_head, fg, 32, y + 2, 700, 24, o->label, DT_LEFT);
        if (o->help) pad_ovl_text(dc, f_small, dim, 32, y + 24, 700, 20, o->help, DT_LEFT);
        int cur = o->get();
        if (cur < 0 || cur >= o->count) cur = 0;
        char cell[64];
        snprintf(cell, sizeof(cell), selected ? "<   %s   >" : "%s", o->values[cur]);
        if (selected) pad_ovl_box(dc, 754, y + 8, 200, row_h - 20, RGB(52, 46, 24), accent);
        pad_ovl_text(dc, f_row, selected ? accent : fg, 760, y + 8, 188, row_h - 20, cell,
                     selected ? DT_CENTER : DT_LEFT);
    }
    const int foot_y = h - 74;
    pad_ovl_text(dc, f_small, dim, 32, foot_y - 24, w - 64, 22, "Changes apply at once and are remembered.", DT_LEFT);
    pad_ovl_text(dc, f_small, dim, 32, foot_y, w - 64, 22,
                 "Up / Down / D-pad: select    Left / Right / Enter / A: change    F2 / Esc / B: close",
                 DT_LEFT);
    if (s_ovl_opt_status[0])
        pad_ovl_text(dc, f_small, accent, 32, foot_y + 22, w - 64, 22, s_ovl_opt_status, DT_LEFT);
}

static void pad_poll_keyboard(void)
{
    PadHostState* pad = &s_host_state[0];
    u16 btns = pad->buttons;

    static int disable_keyboard = -1;
    if (disable_keyboard < 0)
        disable_keyboard = getenv("PS3_PAD_DISABLE_KEYBOARD") ? 1 : 0;
    if (disable_keyboard) {
        pad->buttons = btns;
        return;
    }

    /* The keyboard mapper is a virtual controller, not an optional source on
     * top of XInput.  Keep port 0 connected even when no physical XInput pad
     * exists so games that consult cellPadGetInfo2 before cellPadGetData do not
     * discard valid keyboard input as coming from a disconnected port. */
    pad->connected = 1;

    /* GetAsyncKeyState is process-global, not window-scoped. Without this
     * guard, typing in Codex or any other foreground application drives the
     * game in the background (for example V/E/Enter become Triangle/R1/Start).
     * Physical XInput state has already been collected above and is preserved. */
    HWND foreground = GetForegroundWindow();
    DWORD foreground_pid = 0;
    if (!foreground || !GetWindowThreadProcessId(foreground, &foreground_pid)
            || foreground_pid != GetCurrentProcessId()) {
        pad->buttons = btns;
        return;
    }

    /* Bindings from TJ_CONTROLS_FILE (defaults: Space/Z Cross, F Circle,
     * E Square, V Triangle, LCtrl/LShift L1/R1, 1/3 L2/R2, CapsLock R3,
     * Enter Start, Backspace Select, numpad 8/2/4/6 D-pad, WASD left stick,
     * arrows right stick). */
    unsigned prompt_down = 0;
    for (int a = 0; a < PAD_ACT_COUNT; a++) {
        int down = 0;
        for (int k = 0; k < PAD_KEYS_PER_ACT && !down; k++)
            down = s_pad_keys[a][k] && pad_key_down(s_pad_keys[a][k]);
        if (!down) continue;
        prompt_down |= 1u << a;
        switch (a) {
        case PAD_ACT_LS_UP:    pad->analog_ly = 0;   break;
        case PAD_ACT_LS_DOWN:  pad->analog_ly = 255; break;
        case PAD_ACT_LS_LEFT:  pad->analog_lx = 0;   break;
        case PAD_ACT_LS_RIGHT: pad->analog_lx = 255; break;
        case PAD_ACT_RS_UP:    pad->analog_ry = 0;   break;
        case PAD_ACT_RS_DOWN:  pad->analog_ry = 255; break;
        case PAD_ACT_RS_LEFT:  pad->analog_rx = 0;   break;
        case PAD_ACT_RS_RIGHT: pad->analog_rx = 255; break;
        default:               btns |= s_pad_acts[a].ctrl; break;
        }
    }
    pad_prompt_note_keyboard(prompt_down);

    pad->buttons = btns;
    pad->trigger_l2 = (btns & CELL_PAD_CTRL_L2) ? 255 : pad->trigger_l2;
    pad->trigger_r2 = (btns & CELL_PAD_CTRL_R2) ? 255 : pad->trigger_r2;
    pad->press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
    pad->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    pad->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    pad->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    pad->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    pad->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    pad->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    pad->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    pad->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
    pad->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
}

static void pad_poll_xinput(void)
{
    /* Deterministic bring-up mode: a physical controller remains global even
     * while the game is unfocused, which can advance menus during automated
     * captures. Keep keyboard input available (it has its own foreground
     * guard) while explicitly suppressing XInput when requested. */
    pad_bindings_refresh();
    static int disable_xinput = -1;
    if (disable_xinput < 0)
        disable_xinput = getenv("PS3_PAD_DISABLE_XINPUT") ? 1 : 0;
    if (disable_xinput) {
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
            memset(&s_host_state[i], 0, sizeof(s_host_state[i]));
            s_host_state[i].analog_lx = 128;
            s_host_state[i].analog_ly = 128;
            s_host_state[i].analog_rx = 128;
            s_host_state[i].analog_ry = 128;
        }
        pad_poll_keyboard();
        pad_ovl_block_input();
        return;
    }

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        pad_prompt_note_xinput(i, result == ERROR_SUCCESS ? &state.Gamepad : NULL);
        if (result != ERROR_SUCCESS) {
            memset(&s_host_state[i], 0, sizeof(s_host_state[i]));
            s_host_state[i].analog_lx = 128;
            s_host_state[i].analog_ly = 128;
            s_host_state[i].analog_rx = 128;
            s_host_state[i].analog_ry = 128;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;
        /* Back+Start opens the Controls screen on the next frame; keep the game
         * from seeing Select+Start in the meantime. */
        if ((gp->wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) ==
                (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START))
            pad_ovl_hold_all();

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* through the [controller]
         * bindings (defaults: A Cross, B Circle, X Square, Y Triangle, LB/RB
         * L1/R1, LT/RT L2/R2, stick clicks L3/R3, Start, Back Select, D-pad). */
        unsigned held = gp->wButtons;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)  held |= PAD_XI_LT;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD) held |= PAD_XI_RT;
        u16 btns = 0;
        for (int a = 0; a <= PAD_ACT_RIGHT; a++)
            if (s_pad_xi[a] && (held & s_pad_xi[a])) btns |= s_pad_acts[a].ctrl;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers: analog when bound to LT/RT, otherwise full/none */
        s_host_state[i].trigger_l2 = pad_xi_trigger_value(s_pad_xi[PAD_ACT_L2], gp,
                                                          (btns & CELL_PAD_CTRL_L2) != 0);
        s_host_state[i].trigger_r2 = pad_xi_trigger_value(s_pad_xi[PAD_ACT_R2], gp,
                                                          (btns & CELL_PAD_CTRL_R2) != 0);

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }

    pad_poll_keyboard();
    pad_ovl_block_input();
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

#if !PAD_BACKEND_XINPUT
/* The in-game Controls screen needs the Windows keyboard/XInput backend. */
int  cellPad_overlay_is_open(void) { return 0; }
int  cellPad_overlay_capturing(void) { return 0; }
void cellPad_overlay_toggle(void) {}
void cellPad_overlay_key(int vk, long lp) { (void)vk; (void)lp; }
void cellPad_overlay_page_key(int page) { (void)page; }
int  cellPad_overlay_register_option(const char* label, const char* help,
                                     const char* const* values, int count,
                                     int (*get)(void), int (*set)(int))
{
    (void)label; (void)help; (void)values; (void)count; (void)get; (void)set;
    return -1;
}
void cellPad_overlay_tick(void) {}
int  cellPad_overlay_render(void* bgra, unsigned pitch, unsigned w, unsigned h)
{
    (void)bgra; (void)pitch; (void)w; (void)h;
    return 0;
}
/* No keyboard bindings without the Windows backend: original PS3 prompts. */
int cellPad_prompt_keys(unsigned long long* sig, int vks[CELL_PAD_PROMPT_ACTIONS])
{
    (void)sig; (void)vks;
    return 0;
}
#endif

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

static void pad_poll_backend(void)
{
#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
}

static void pad_poll_and_snapshot(PadHostState snapshot[PAD_MAX_HOST_PORTS])
{
    pad_state_lock();
    pad_poll_backend();
    memcpy(snapshot, s_host_state, sizeof(s_host_state));
    pad_state_unlock();
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    if (max_connect == 0 || max_connect > CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_host_state[i].analog_lx = 128;
        s_host_state[i].analog_ly = 128;
        s_host_state[i].analog_rx = 128;
        s_host_state[i].analog_ry = 128;
    }

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_state_lock();
    pad_poll_backend();
    pad_state_unlock();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_state_lock();
    pad_shutdown_backend();
    pad_state_unlock();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_state_lock();
        pad_poll_backend();
        pad_state_unlock();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. These
 * runtime helpers translate + byte-swap. (cellPad was written for host pointers
 * and was never actually invoked until its NIDs were fixed.) */
extern void vm_write8 (unsigned long long a, unsigned char  v);
extern void vm_write16(unsigned long long a, unsigned short v);
extern void vm_write32(unsigned long long a, unsigned int   v);
extern unsigned char vm_read8(unsigned long long a);

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Event-backed private runner commands bypass a stalled D3D WndProc.
     * This polls before the sample lock because cellPadDebugPulse owns that
     * lock while arming its neutral-before-press state. */
#if defined(_WIN32)
    pad_poll_debug_channel();
#endif

    /* Serialize the complete guest-visible sample. Debug pulse phases must be
     * emitted neutral-before-press even when multiple guest threads poll. */
    pad_getdata_lock();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    PadHostState effective;
    u32 setting;

    /* Backend state is physical-only. Snapshot it before adding test input. */
    memset(&effective, 0, sizeof(effective));
    pad_state_lock();
    pad_poll_backend();
    if (port_no < PAD_MAX_HOST_PORTS)
        effective = s_host_state[port_no];
    setting = s_port_setting[port_no];
    pad_state_unlock();

#if defined(_WIN32)
    /* Private window-message pulse used by unattended launch/menu tests. */
    pad_apply_debug_pulse(&effective, port_no);
#endif

    /* Deterministic one-shot input for unattended title/menu verification.
     * PS3_PAD_ONESHOT_MS=<delay> injects exactly one pulse, measured from the
     * first pad poll with wall-clock time. Optional PS3_PAD_ONESHOT_BTN and
     * PS3_PAD_ONESHOT_HOLD_MS select the mask and hold duration. This is test
     * scaffolding only; unset in normal interactive runs. */
    { static int one_init = 0, one_enabled = 0;
      static ULONGLONG one_start = 0, one_delay = 0, one_hold = 700;
      static u16 one_btn = CELL_PAD_CTRL_START;
      if (!one_init) {
          const char* de = getenv("PS3_PAD_ONESHOT_MS");
          const char* be = getenv("PS3_PAD_ONESHOT_BTN");
          const char* he = getenv("PS3_PAD_ONESHOT_HOLD_MS");
          one_enabled = de && *de;
          if (one_enabled) one_delay = strtoull(de, NULL, 0);
          if (be && *be) one_btn = (u16)strtoul(be, NULL, 0);
          if (he && *he) one_hold = strtoull(he, NULL, 0);
          one_start = GetTickCount64();
          one_init = 1;
      }
      if (one_enabled && port_no == 0) {
          ULONGLONG elapsed = GetTickCount64() - one_start;
          /* Deterministic test input is also a virtual pad.  Advertising only
           * button data is insufficient: Tokyo Jungle checks pad connection
           * status and ignores input from ports reported as disconnected. */
          effective.connected = 1;
          if (elapsed >= one_delay && elapsed < one_delay + one_hold)
              effective.buttons |= one_btn;
          else
              effective.buttons &= (u16)~one_btn;
      } }

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) effective.buttons |= btn;
            else         effective.buttons &= ~btn;
            effective.connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!effective.connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; its effective state is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &effective;

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* PS3 CellPadData packet header: button[0] is reserved and button[1]
     * identifies report format 0x7 plus half the report length. */
    data->button[0] = 0;
    data->button[1] = (u16)((0x7u << 4) | ((len / 2) < 15 ? (len / 2) : 15));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* Opt-in edge trace for title/menu input bring-up. Log only changes so a
     * normal 30-60 Hz poll loop cannot flood redirected output. */
    static int trace_buttons = -1;
    if (trace_buttons < 0) trace_buttons = getenv("PS3_PAD_TRACE") ? 1 : 0;
    if (port_no == 0 && trace_buttons) {
        static u16 last_buttons = 0xFFFFu;
        if (hs->buttons != last_buttons) {
            fprintf(stderr, "[cellPad] buttons=0x%04X digital1=0x%02X digital2=0x%02X\n",
                    hs->buttons,
                    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1],
                    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2]);
            last_buttons = hs->buttons;
        }
    }

    /* LBP_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("LBP_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* Analog sticks */
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    pad_getdata_unlock();
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[7]       +0x1A u16 product_id[7] +0x28 u8 status[7]
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    PadHostState host_state[PAD_MAX_HOST_PORTS];
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_and_snapshot(host_state);

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (host_state[i].connected) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  host_state[i].connected);
        vm_write16(gaddr + 0x0C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x1A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x28 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    PadHostState host_state[PAD_MAX_HOST_PORTS];
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_and_snapshot(host_state);

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    PadHostState host_state[PAD_MAX_HOST_PORTS];
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_and_snapshot(host_state);

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (_n++ < 2)
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    memset(info, 0, sizeof(CellPadCapabilityInfo));

    /* Report standard DualShock 3 capabilities */
    info->info[0] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                   | CELL_PAD_CAPABILITY_PRESS_MODE
                   | CELL_PAD_CAPABILITY_SENSOR_MODE
                   | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                   | CELL_PAD_CAPABILITY_ACTUATOR;

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* HLE pointer arguments are raw guest effective addresses.  Reading the
     * typed pointer directly dereferenced low guest memory as a host address
     * whenever Tokyo Jungle updated rumble. */
    const unsigned long long param_ea = (unsigned int)(uintptr_t)param;
    const u8 small_motor = vm_read8(param_ea + CELL_PAD_ACTUATOR_PARAM_SMALL);
    const u8 large_motor = vm_read8(param_ea + CELL_PAD_ACTUATOR_PARAM_LARGE);

    pad_state_lock();
#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(large_motor * 257);
        vib.wRightMotorSpeed = (WORD)(small_motor * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(large_motor * 257),
            (Uint16)(small_motor * 257),
            100 /* duration ms */
        );
    }
#endif

    pad_state_unlock();

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
