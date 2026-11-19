/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput, loaded at run time from System32 (XInput1_4.dll,
 *     XInput1_3.dll, XInput9_1_0.dll); no XInput DLL means no pads
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 *
 * On macOS the keyboard and mouse drive port 0 as well, together with any
 * controller on it: the keys held, the mouse motion, buttons and wheel sit in
 * a ps3_host_input block (host_input.h) and PS3_INPUT_MAP rebinds them
 * (pad_input_map.h). The game's window fills the block from its events
 * (host_input_cocoa.h, called from the Metal backend's event pump); until a
 * window asks for the block, cellPad fills the keys itself by polling the
 * keys the map binds (no mouse then). Environment, all optional:
 *
 *   PS3_INPUT_MAP=<file>        bindings for keyboard, mouse buttons and
 *                               controller, and the [mouse] settings
 *   PS3_INPUT_MODE=both|controller|keyboard   what drives the game (both)
 *   PAD_NO_KEYBOARD=1           no keyboard and no mouse (as before the map)
 *   PS3_MOUSE=0                 no mouse camera (and no mouse buttons)
 *   PS3_MOUSE_SENS=<f>          mouse camera speed (1.0)
 *   PS3_MOUSE_INVERT_X=1, PS3_MOUSE_INVERT_Y=1
 *   PS3_MOUSE_DEADZONE=<0-120>  first stick step past centre (30)
 *   PS3_MOUSE_CAPTURE=0         no cursor capture: motion counts whenever
 *                               the window is key
 *   PS3_MOUSE_LOG=1             print what the mouse does to the stick
 *   PS3_RUMBLE=0, PS3_RUMBLE_STRENGTH=<0-200>, PS3_RUMBLE_LOG=1   vibration
 *   PS3_PAD_SDL=0               ignore physical controllers
 *   PS3_PAD_ADAPTIVE=<0-8>      DualSense trigger resistance (3; 0 = free)
 *   PS3_PAD_LIGHT=<RRGGBB>|off, PS3_PAD_LIGHT_HIT=0   DualSense light bar
 *   PS3_PAD_GYRO=<0-400>, PS3_PAD_GYRO_MODE=always|l1|l2, PS3_PAD_GYRO_INVERT_Y=1,
 *   PS3_PAD_GYRO_DZ=<0-0.5>     gyro added to the right stick (the camera)
 *
 * Windows keeps its fixed keyboard fallback (pad_poll_keyboard), and so does
 * a host whose keyboard is SDL's (Linux, or PS3RECOMP_PAD_KEYBOARD_SDL); the
 * [controller] bindings and the rumble settings apply to every SDL host.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */
#include "ps3emu/log_first.h"
#include "host_input.h"                   /* keyboard/mouse state from the window */
#include "pad_input_map.h"                /* PS3_INPUT_MAP: keyboard/mouse/controller bindings */

/* PAD_CLOCK=flips: PAD_SCRIPT and PAD_STICK times count guest flips instead of
 * wall-clock seconds. A loaded machine runs a title at a third of its usual
 * frame rate, and a wall-clock schedule then presses START on the logos; a
 * flip schedule replays the same screens however slow the run is. */
extern u32 cellGcm_flip_request_count(void);
static double pad_clock(unsigned long long t0_ms)
{
    static int flips = -1;
    if (flips < 0) { const char* e = getenv("PAD_CLOCK"); flips = e && !strcmp(e, "flips"); }
    return flips ? (double)cellGcm_flip_request_count()
                 : (double)(GetTickCount64() - t0_ms) / 1000.0;
}

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
  /* Types only. The exe must not import an XInput DLL: MinGW's libxinput is
   * xinput1_3.dll, which only the DirectX June 2010 redistributable installs,
   * so on a stock Windows 10/11 the exe did not start (0xC0000135). The DLL is
   * loaded at run time instead, see pad_xinput_load(). */
  #include <xinput.h>
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
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
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

static unsigned long long pad_now_ms(void);
static inline int pad_str_ieq(const char* a, const char* b);

/* The keyboard and mouse come from the ps3_host_input block through
 * PS3_INPUT_MAP (macOS); elsewhere the fixed keyboard fallback below. */
#if defined(__APPLE__) && !defined(_WIN32) && !defined(PS3RECOMP_PAD_KEYBOARD_SDL)
  #define PAD_KBM_HOST_INPUT 1
#else
  #define PAD_KBM_HOST_INPUT 0
#endif

/* PS3_INPUT_MAP=<file>: which keys, mouse buttons and controller buttons
 * drive which PS3 controls, and the mouse camera's settings (the format is
 * in pad_input_map.h). Read once, at the first cellPadInit (or the first
 * poll); with no file, or for whatever it leaves out, the built-in layout in
 * pim_defaults applies. */
static PadInputMap s_map;
static volatile int s_map_state;   /* 0 not loaded, 1 loaded */

/* Where the in-game menu saves the bindings: PS3_INPUT_MAP_SAVE when it is
 * set (a port whose PS3_INPUT_MAP is its own default layout, a file the
 * player's changes must not overwrite; the launcher then starts the next
 * run with the saved file); else PS3_INPUT_MAP; when that is not set either,
 * input.toml next to PS3_OVERLAY_INI (the menu's own settings file), which
 * the next start then reads. Empty = not saved (no PS3_OVERLAY_INI, or
 * PS3_INPUT_MAP set to ""). */
static void pad_map_save_path(char* out, size_t n)
{
    out[0] = 0;
    const char* sv = getenv("PS3_INPUT_MAP_SAVE");
    if (sv && *sv) { snprintf(out, n, "%s", sv); return; }
    const char* e = getenv("PS3_INPUT_MAP");
    if (e) { snprintf(out, n, "%s", e); return; }
    const char* ini = getenv("PS3_OVERLAY_INI");
    if (!ini || !*ini) return;
    const char* slash = strrchr(ini, '/');
    if (!slash) { snprintf(out, n, "input.toml"); return; }
    snprintf(out, n, "%.*s/input.toml", (int)(slash - ini), ini);
}
static void pad_map_load(void)
{
    if (s_map_state) return;
    const char* path = getenv("PS3_INPUT_MAP");
    char menu_path[1024];
    if (!path) {   /* what the in-game menu saved, if it did */
        pad_map_save_path(menu_path, sizeof menu_path);
        FILE* f = menu_path[0] ? fopen(menu_path, "r") : NULL;
        if (f) { fclose(f); path = menu_path; }
    }
    if (path && *path) {
        if (pim_load_file(&s_map, path, stderr))
            fprintf(stderr, "[cellPad] input map %s: %d change(s) from the defaults, %d name(s) ignored\n",
                    path, s_map.n_set, s_map.n_warnings);
        else
            fprintf(stderr, "[cellPad] input map %s: cannot open, using the default layout\n", path);
    } else {
        pim_defaults(&s_map);
    }
    __atomic_store_n(&s_map_state, 1, __ATOMIC_RELEASE);
}

static inline const PadInputMap* pad_map(void)
{
    if (!__atomic_load_n(&s_map_state, __ATOMIC_ACQUIRE)) pad_map_load();
    return &s_map;
}

static void pad_lock(void);
static void pad_unlock(void);
/* The in-game menu (libs/video/ps3_overlay.mm): the live map, and a new one.
 * Only the bindings change; the [mouse] settings stay as they were loaded. A
 * poll holds the poll lock while it reads the map, so the copy takes it too. */
void cellPad_host_input_map(PadInputMap* out) { *out = *pad_map(); }
void cellPad_host_set_input_map(const PadInputMap* m)
{
    pad_map();
    pad_lock();
    memcpy(s_map.key, m->key, sizeof s_map.key);
    memcpy(s_map.mouse, m->mouse, sizeof s_map.mouse);
    memcpy(s_map.pad, m->pad, sizeof s_map.pad);
    pad_unlock();
}
void cellPad_host_input_map_path(char* out, size_t n) { pad_map_save_path(out, n); }
/* 1 saved, 0 nowhere to save, -1 could not write. */
int cellPad_host_save_input_map(void)
{
    char path[1024];
    pad_map_save_path(path, sizeof path);
    if (!path[0]) return 0;
    const int ok = pim_save_file(pad_map(), path);
    fprintf(stderr, "[cellPad] input map %s: %s\n", path, ok ? "saved" : "could not write it");
    return ok ? 1 : -1;
}

/* Raw controller inputs of every pad (bit n = SDL button n, 21/22 the
 * triggers, as in pad_input_map.h), before the map: what the in-game menu
 * binds when the player presses a button to rebind. */
volatile uint32_t g_pad_overlay_raw;

#if PAD_BACKEND_SDL2 || PAD_KBM_HOST_INPUT   /* the XInput poll binds nothing through the map */

/* A stick axis from its two direction controls: `base` (centre, or the
 * physical stick) when neither is held, otherwise full deflection (both held
 * = 127, as two opposite keys always gave). */
static inline int pad_dir_axis(uint32_t ctl, int neg, int pos, int base)
{
    const int n = (ctl >> neg) & 1, p = (ctl >> pos) & 1;
    if (!n && !p) return base;
    return 128 + (p ? 127 : 0) - (n ? 128 : 0);
}
#endif

/* The window's keyboard/mouse block (host_input.h). A local one unless a
 * window host attaches its shared one; the Metal backend's own window fills
 * the local one. Whoever asks for the block (ps3_pad_host_input) or attaches
 * one is a window that feeds it from its events, and from then on cellPad no
 * longer polls the keys itself (s_input_fed, see pad_keys_poll_host). */
static ps3_host_input  s_input_local;
static ps3_host_input* s_input = &s_input_local;
static int             s_input_fed;
ps3_host_input* ps3_pad_host_input(void)
{
    __atomic_store_n(&s_input_fed, 1, __ATOMIC_RELEASE);
    return __atomic_load_n(&s_input, __ATOMIC_ACQUIRE);
}
void ps3_pad_host_input_attach(ps3_host_input* in)
{
    if (in) __atomic_store_n(&s_input_fed, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_input, in ? in : &s_input_local, __ATOMIC_RELEASE);
}

#if PAD_BACKEND_SDL2
/* PS3 port i's controller, by SDL instance id (-1 = none); the port keeps the
 * GUID of the last one it had so that pad gets the same port back. */
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static SDL_JoystickID      s_sdl_ids[PAD_MAX_HOST_PORTS] = { -1, -1, -1, -1 };
static SDL_JoystickGUID    s_sdl_last_guid[PAD_MAX_HOST_PORTS];
static int                 s_sdl_had[PAD_MAX_HOST_PORTS];
static unsigned long long  s_sdl_scan_ms;   /* last device-list scan (pad_now_ms) */
static int s_sdl_inited = 0;
#endif

/* A port with no device behind it: nothing pressed and both sticks CENTRED.
 * Zero is not neutral for a stick -- 0 is full deflection up / left. Port 0 is
 * served even with no device (see cellPadGetData), and with the SDL2 backend
 * and no controller plugged in its state was the zeroed array: every title
 * read both sticks pushed to the top-left corner. Twisted Metal's car drove
 * itself in circles and its chase camera pitched up at the sky, so the
 * player's own vehicle left the screen within seconds of a match starting.
 * A pad unplugged mid-game kept its last report the same way. */
static void pad_host_release(PadHostState* hs)
{
    memset(hs, 0, sizeof(*hs));
    hs->analog_lx = hs->analog_ly = 128;
    hs->analog_rx = hs->analog_ry = 128;
}

/* One poll at a time: the guest polls from several threads, and a poll that
 * opens or closes a controller must not race another one using it.
 * cellPadSetActDirect takes it too (it talks to the controller). */
#ifdef _WIN32
  #define pad_yield() Sleep(0)
#else
  #include <sched.h>
  #define pad_yield() sched_yield()
#endif
static volatile char s_poll_lock;
static int  pad_lock_try(void) { return !__atomic_test_and_set(&s_poll_lock, __ATOMIC_ACQUIRE); }
static void pad_lock(void)     { while (!pad_lock_try()) pad_yield(); }
static void pad_unlock(void)   { __atomic_clear(&s_poll_lock, __ATOMIC_RELEASE); }

/* CELL_PAD_STATUS_ASSIGN_CHANGES: a port whose reported connection changed
 * since the title last asked (cellPadGetInfo/GetInfo2/PeriphGetInfo), as
 * libpad reports a controller plugged in or taken away. */
static int s_port_reported[PAD_MAX_HOST_PORTS];
static int s_port_changed[PAD_MAX_HOST_PORTS];
static void pad_track_assign(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        const int on = s_host_state[i].connected || i == 0;   /* port 0: the virtual pad */
        if (on != s_port_reported[i]) { s_port_reported[i] = on; s_port_changed[i] = 1; }
    }
}
static u32 pad_status_take(u32 port)
{
    if (port >= PAD_MAX_HOST_PORTS) return 0;
    const int ch = __atomic_exchange_n(&s_port_changed[port], 0, __ATOMIC_RELAXED);
    return ch ? CELL_PAD_STATUS_ASSIGN_CHANGES : 0;
}

/* ---------------------------------------------------------------------------
 * Rumble (cellPadSetActDirect)
 *
 * A DualShock 3 has two motors: the small one (motor[0]) is on or off, the
 * large one (motor[1]) takes 0-255. A title sets them and they stay that way
 * until it sets them again; some titles only call when the value changes.
 * SDL's rumble runs for a duration instead, so the last request per port is
 * kept and renewed from the pad poll while it is non-zero. If the title stops
 * polling (a hang, a closed window), the motors stop within
 * PAD_RUMBLE_HOLD_MS. Large -> the host's low-frequency motor, small -> the
 * high-frequency one, which is how DualShock 4, DualSense and Xbox pads are
 * laid out too.
 *
 *   PS3_RUMBLE=0                 no vibration at all
 *   PS3_RUMBLE_STRENGTH=0..200   percent applied to both motors (100); above
 *                                100 boosts a weak large-motor value, capped
 *                                at the motor's maximum
 *   PS3_RUMBLE_LOG=1             print every request the title makes
 * -------------------------------------------------------------------------*/

#define PAD_RUMBLE_HOLD_MS     1000   /* one SDL request lasts this long ... */
#define PAD_RUMBLE_RENEW_MS     400   /* ... and is renewed this often */

typedef struct {
    u16 lo, hi;                 /* what the host motors were last asked for */
    unsigned long long sent_ms; /* when (pad_now_ms), 0 = nothing to renew */
    int dirty;                  /* changed while a poll held the lock: the poll sends it */
} PadRumble;
static PadRumble s_rumble[PAD_MAX_HOST_PORTS];

/* Strength in percent from PS3_RUMBLE / PS3_RUMBLE_STRENGTH: 0 = off. */
static int pad_rumble_strength_from(const char* on, const char* strength)
{
    if (on && on[0] == '0' && on[1] == 0) return 0;
    if (!strength || !*strength) return 100;
    char* end = NULL;
    long v = strtol(strength, &end, 10);
    if (end == strength) return 100;
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return (int)v;
}

static int s_rumble_pct = -1;
static int pad_rumble_strength(void)
{
    int s = s_rumble_pct;
    if (s < 0) {
        s = pad_rumble_strength_from(getenv("PS3_RUMBLE"), getenv("PS3_RUMBLE_STRENGTH"));
        if (s != 100) fprintf(stderr, "[cellPad] rumble %s%d%%\n", s ? "strength " : "off ", s);
        s_rumble_pct = s;
    }
    return s;
}

/* The in-game menu (ps3_overlay.mm): rumble strength in percent, 0 = off. */
int  cellPad_host_rumble_strength(void)       { return pad_rumble_strength(); }
void cellPad_host_set_rumble_strength(int pct) { s_rumble_pct = pct < 0 ? 0 : pct > 200 ? 200 : pct; }

/* The two host motor levels (0-65535) for a PS3 actuator request, at `pct`
 * percent strength. */
static void pad_rumble_levels(u8 small_on, u8 large, int pct, u16* lo, u16* hi)
{
    unsigned long l = (unsigned long)large * 257ul * (unsigned long)pct / 100ul;
    unsigned long h = small_on ? 65535ul * (unsigned long)pct / 100ul : 0;
    *lo = (u16)(l > 65535ul ? 65535ul : l);
    *hi = (u16)(h > 65535ul ? 65535ul : h);
}

/* Keyboard fallback for port 0 (Windows, and hosts whose keyboard is SDL's;
 * macOS reads the keyboard and mouse through PS3_INPUT_MAP instead, see
 * pad_poll_keys).
 *
 * On a machine with no controller plugged in a title gets a pad that is reported
 * present and never presses anything -- it can be watched but not played. That is
 * what The Simpsons Arcade Game hit: it reached its attract loop and no input
 * existed to start a game.
 *
 * Only fills in for port 0, only when the backend found no controller there, so a
 * real controller always wins and nothing changes for a port that has one. Keys
 * are read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too.
 *
 * Where the keys come from: GetAsyncKeyState with the XInput backend; on macOS
 * CGEventSourceKeyState, which needs no event tap and no window; elsewhere SDL's
 * keyboard state, which is kept current by whoever pumps SDL's events (in
 * practice the window of the video backend). PS3RECOMP_PAD_KEYBOARD_SDL selects
 * the SDL source on macOS too, which is how the keyboard test is built. */
#if PAD_BACKEND_XINPUT
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static int pad_key_down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
#define PADKEY_UP VK_UP
#define PADKEY_DOWN VK_DOWN
#define PADKEY_LEFT VK_LEFT
#define PADKEY_RIGHT VK_RIGHT
#define PADKEY_Z 'Z'
#define PADKEY_X 'X'
#define PADKEY_A 'A'
#define PADKEY_S 'S'
#define PADKEY_Q 'Q'
#define PADKEY_W 'W'
#define PADKEY_1 '1'
#define PADKEY_2 '2'
#define PADKEY_RETURN VK_RETURN
#define PADKEY_TAB VK_TAB
#elif defined(__APPLE__) && !defined(PS3RECOMP_PAD_KEYBOARD_SDL)
/* macOS: the keys and the layout are pad_poll_keys' (PS3_INPUT_MAP). These two serve its fallback for a game
 * whose window does not hand over its key events (pad_keys_poll_host): virtual key codes (Carbon
 * HIToolbox/Events.h kVK_*) polled with CGEventSourceKeyState, which needs no event tap and no window.
 * "Focused" is NSApplication.isActive of this process, queried through the Objective-C runtime so this file
 * stays plain C. */
/* The system headers are not included on purpose: ApplicationServices / objc.h clash with this
 * project's own `bool` and GetCurrentThread/GetCurrentProcess names, so the three entry points
 * are declared by hand (CGKeyCode is uint16_t, kCGEventSourceStateCombinedSessionState is 0). */
extern unsigned char CGEventSourceKeyState(int stateID, unsigned short key);
extern void* objc_getClass(const char* name);
extern void* sel_registerName(const char* name);
extern void objc_msgSend(void);
static int pad_host_window_focused(void)
{
    void* cls = objc_getClass("NSApplication");
    if (!cls) return 0;
    void* app = ((void* (*)(void*, void*))objc_msgSend)(cls, sel_registerName("sharedApplication"));
    return app && ((signed char (*)(void*, void*))objc_msgSend)(app, sel_registerName("isActive"));
}
static int pad_key_down(int key)
{
    return CGEventSourceKeyState(0, (unsigned short)key) != 0;
}
#elif PAD_BACKEND_SDL2
/* Everywhere else the keyboard is SDL's: scancodes are physical keys, like the macOS codes above. The state is kept
 * current by whoever pumps SDL's events (the window of the RSX backend); without a video subsystem nothing is ever
 * pressed. "Focused" is that some window of this process has the keyboard focus. */
static int pad_host_window_focused(void)
{
    return SDL_GetKeyboardFocus() != NULL;
}
static int pad_key_down(int key)
{
    return SDL_GetKeyboardState(NULL)[key] != 0;
}
#define PADKEY_UP SDL_SCANCODE_UP
#define PADKEY_DOWN SDL_SCANCODE_DOWN
#define PADKEY_LEFT SDL_SCANCODE_LEFT
#define PADKEY_RIGHT SDL_SCANCODE_RIGHT
#define PADKEY_Z SDL_SCANCODE_Z
#define PADKEY_X SDL_SCANCODE_X
#define PADKEY_A SDL_SCANCODE_A
#define PADKEY_S SDL_SCANCODE_S
#define PADKEY_Q SDL_SCANCODE_Q
#define PADKEY_W SDL_SCANCODE_W
#define PADKEY_1 SDL_SCANCODE_1
#define PADKEY_2 SDL_SCANCODE_2
#define PADKEY_RETURN SDL_SCANCODE_RETURN
#define PADKEY_TAB SDL_SCANCODE_TAB
#endif

#if !PAD_KBM_HOST_INPUT
static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { PADKEY_UP,     CELL_PAD_CTRL_UP },      { PADKEY_DOWN,  CELL_PAD_CTRL_DOWN },
        { PADKEY_LEFT,   CELL_PAD_CTRL_LEFT },    { PADKEY_RIGHT, CELL_PAD_CTRL_RIGHT },
        { PADKEY_Z,      CELL_PAD_CTRL_CROSS },   { PADKEY_X,     CELL_PAD_CTRL_CIRCLE },
        { PADKEY_A,      CELL_PAD_CTRL_SQUARE },  { PADKEY_S,     CELL_PAD_CTRL_TRIANGLE },
        { PADKEY_Q,      CELL_PAD_CTRL_L1 },      { PADKEY_W,     CELL_PAD_CTRL_R1 },
        { PADKEY_1,      CELL_PAD_CTRL_L2 },      { PADKEY_2,     CELL_PAD_CTRL_R2 },
        { PADKEY_RETURN, CELL_PAD_CTRL_START },   { PADKEY_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (pad_key_down(map[i].vk)) btns |= map[i].btn;

    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no controller)\n");
          fflush(stdout); } }
}
#endif /* !PAD_KBM_HOST_INPUT */

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

/* XInput is resolved at run time. Only XInputGetState and XInputSetState are
 * called, and XInput9_1_0.dll exports both, so any of the three DLLs serves.
 * Loaded once, from System32 only (not the exe folder, the working directory
 * or PATH), and kept loaded until exit. No DLL, or one without the two entry
 * points, means every slot reads as unplugged. */
typedef DWORD (WINAPI *pad_xi_get_state_fn)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *pad_xi_set_state_fn)(DWORD, XINPUT_VIBRATION*);
static pad_xi_get_state_fn p_xi_get_state;
static pad_xi_set_state_fn p_xi_set_state;
static int s_xi_tried;

static int pad_xinput_load(void)
{
    static const wchar_t* const names[] = { L"XInput1_4.dll", L"XInput1_3.dll", L"XInput9_1_0.dll" };
    if (s_xi_tried) return p_xi_get_state != NULL;
    s_xi_tried = 1;
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        HMODULE m = LoadLibraryExW(names[k], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        pad_xi_get_state_fn g;
        pad_xi_set_state_fn st;
        if (!m) continue;
        g  = (pad_xi_get_state_fn)(void (*)(void))GetProcAddress(m, "XInputGetState");
        st = (pad_xi_set_state_fn)(void (*)(void))GetProcAddress(m, "XInputSetState");
        if (!g || !st) { FreeLibrary(m); continue; }
        p_xi_get_state = g;
        p_xi_set_state = st;
        printf("[cellPad] XInput from %ls\n", names[k]);
        return 1;
    }
    printf("[cellPad] no XInput DLL (XInput1_4, XInput1_3, XInput9_1_0): no XInput pads\n");
    return 0;
}

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

/* XInputGetState on a slot with nothing plugged in costs about 140 us (2 us on
 * a connected one), and the backend is polled at least once a frame: three
 * empty slots beside one pad were 0.6 ms of every frame on God of War HD's
 * main thread. An empty slot is looked at again once a second, which is how
 * soon a pad plugged in there is seen; the slots are staggered so one poll
 * never pays for all of them. */
#define PAD_EMPTY_RECHECK_MS 1000
static unsigned long long s_pad_empty_until[PAD_MAX_HOST_PORTS];

static void pad_poll_xinput(void)
{
    const unsigned long long now = pad_now_ms();
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_pad_empty_until[i] && now < s_pad_empty_until[i]) {
            s_host_state[i].connected = 0;
            continue;
        }
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = p_xi_get_state ? p_xi_get_state((DWORD)i, &state)
                                      : ERROR_DEVICE_NOT_CONNECTED;
        if (result != ERROR_SUCCESS) {
            pad_host_release(&s_host_state[i]);
            s_pad_empty_until[i] = now + PAD_EMPTY_RECHECK_MS
                                 + (unsigned long long)i * (PAD_EMPTY_RECHECK_MS / PAD_MAX_HOST_PORTS);
            continue;
        }
        s_pad_empty_until[i] = 0;

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

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
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init, only the run-time load of the DLL */
    pad_xinput_load();
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

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

/* The analog value of a PS3 trigger from the SDL inputs bound to it: the
 * trigger bound to it (the stronger, if both), full for a bound button that
 * is down. */
static u8 pad_sdl_analog(uint32_t bound, uint32_t in, u8 lt, u8 rt)
{
    u8 v = 0;
    if (bound & (1u << PIM_PAD_TRIGGER_LEFT)) v = lt;
    if ((bound & (1u << PIM_PAD_TRIGGER_RIGHT)) && rt > v) v = rt;
    if (bound & in & ((1u << PIM_PAD_TRIGGER_LEFT) - 1u)) v = 255;
    return v;
}

/* Send port i's rumble state to its controller, with a fresh duration. */
static void pad_sdl_rumble_send(int i)
{
    SDL_GameController* gc = s_sdl_controllers[i];
    PadRumble* r = &s_rumble[i];
    r->dirty = 0;
    if (!gc) return;
    const int on = r->lo || r->hi;
    SDL_GameControllerRumble(gc, r->lo, r->hi, on ? PAD_RUMBLE_HOLD_MS : 0);
    r->sent_ms = on ? pad_now_ms() : 0;
}

/* PS3_PAD_SDL=0: ignore physical controllers (a headless capture running
 * beside a windowed game would otherwise take the player's presses too). */
static int pad_sdl_off(void)
{
    static int off = -1;
    if (off < 0) { const char* e = getenv("PS3_PAD_SDL"); off = (e && e[0] == '0') ? 1 : 0; }
    return off;
}

/* ---- DualSense adaptive triggers -------------------------------------------
 * A PS3 pad's L2/R2 are plain springs. On a DualSense both triggers get a
 * light, even resistance, and a trigger that is held while the title runs
 * the motors (a hit landing, a clash) buzzes with them instead. The press
 * point stays where it was (PAD_TRIGGER_PRESS), so nothing about the
 * controls changes. The effects follow the physical triggers, whatever PS3
 * control the map binds them to. They go out through
 * SDL_GameControllerSendEffect as the trigger blocks of the DualSense output
 * report (the layout SDL's testgamecontroller sends), only when they change;
 * both triggers are set free again when the controller is closed and when
 * the process exits. A controller that refuses the effect (not a DualSense,
 * or no enhanced mode) keeps free triggers.
 *
 *   PS3_PAD_ADAPTIVE=<0-8>   the resistance's strength (3); 0 = free triggers
 * -------------------------------------------------------------------------*/

#define PAD_TRIGGER_PRESS       3000   /* SDL trigger value read as pressed (0-32767) */
#define PAD_ADAPT_MIN_GAP_MS      30   /* effect reports at most this often */

enum { PAD_TRIG_UNSET = -1, PAD_TRIG_OFF = 0, PAD_TRIG_FEEDBACK, PAD_TRIG_BUZZ /* + amplitude 1-8 */ };

typedef struct {
    int on;                       /* a DualSense taking effects */
    int right, left;              /* effect last sent (PAD_TRIG_*; BUZZ + amplitude) */
    unsigned long long sent_ms;
} PadAdaptive;
static PadAdaptive s_adapt[PAD_MAX_HOST_PORTS];

/* The resistance's strength, 1-8; 0 = off. */
static int pad_adaptive_strength_from(const char* e)
{
    if (!e || !*e) return 3;
    char* end = NULL;
    long v = strtol(e, &end, 10);
    if (end == e) return 3;
    return v < 0 ? 0 : v > 8 ? 8 : (int)v;
}
static int pad_adaptive_strength(void)
{
    static int s = -1;
    if (s < 0) s = pad_adaptive_strength_from(getenv("PS3_PAD_ADAPTIVE"));
    return s;
}

/* One trigger's 11-byte effect block (the DualSense's own effect modes:
 * 0x05 off, 0x21 feedback, 0x26 vibration). Positions are the ten zones of
 * the trigger's travel (0-9), strengths 1-8. */
static void pad_trig_block(uint8_t out[11], int effect, int strength)
{
    memset(out, 0, 11);
    if (effect == PAD_TRIG_FEEDBACK && strength > 0) {
        /* even resistance from zone 1 */
        unsigned active = 0;
        uint32_t force = 0;
        for (int z = 1; z < 10; z++) { active |= 1u << z; force |= (uint32_t)(strength - 1) << (3 * z); }
        out[0] = 0x21;
        out[1] = (uint8_t)(active & 0xFF); out[2] = (uint8_t)(active >> 8);
        out[3] = (uint8_t)force; out[4] = (uint8_t)(force >> 8);
        out[5] = (uint8_t)(force >> 16); out[6] = (uint8_t)(force >> 24);
    } else if (effect > PAD_TRIG_BUZZ) {
        /* vibration over the whole travel, amplitude 1-8, 30 Hz */
        const int amp = effect - PAD_TRIG_BUZZ;
        unsigned active = 0;
        uint32_t ampz = 0;
        for (int z = 0; z < 10; z++) { active |= 1u << z; ampz |= (uint32_t)(amp - 1) << (3 * z); }
        out[0] = 0x26;
        out[1] = (uint8_t)(active & 0xFF); out[2] = (uint8_t)(active >> 8);
        out[3] = (uint8_t)ampz; out[4] = (uint8_t)(ampz >> 8);
        out[5] = (uint8_t)(ampz >> 16); out[6] = (uint8_t)(ampz >> 24);
        out[9] = 30;
    } else {
        out[0] = 0x05;
    }
}

/* Send both triggers' effects; 0 when the controller took them. `reset`
 * also clears the power-save bits (valid_flag1 bit 1, byte 9): another
 * program that had the controller (Steam Input) can leave the trigger
 * motors and the light bar switched off in it, and nothing else turns them
 * back on until the controller is powered off. */
static int pad_trig_send_ex(SDL_GameController* gc, int right, int left, int reset)
{
    uint8_t st[47];   /* DS5EffectsState_t */
    memset(st, 0, sizeof st);
    st[0] = 0x04 | 0x08;   /* right and left trigger effects */
    if (reset) { st[1] = 0x02; st[9] = 0x00; }
    pad_trig_block(st + 10, right, pad_adaptive_strength());
    pad_trig_block(st + 21, left, pad_adaptive_strength());
    return SDL_GameControllerSendEffect(gc, st, (int)sizeof st);
}
static int pad_trig_send(SDL_GameController* gc, int right, int left) { return pad_trig_send_ex(gc, right, left, 0); }

/* Another program holding the controller (macOS): Steam with Steam Input on
 * rewrites the DualSense's output reports, so light bar and trigger effects
 * never show. Said once. */
#if defined(__APPLE__) && !defined(PAD_TEST_NO_LIBPROC)
#include <libproc.h>
static void pad_warn_steam(void)
{
    static int said;
    if (said) return;
    said = 1;
    int pids[4096];
    const int n = proc_listallpids(pids, (int)sizeof pids);
    for (int i = 0; i < n && i < 4096; i++) {
        char name[64];
        if (pids[i] > 0 && proc_name(pids[i], name, sizeof name) > 0 && !strcmp(name, "steam_osx")) {
            fprintf(stderr, "[cellPad] Steam is running: with Steam Input on it takes over the DualSense and"
                            " its light bar and adaptive triggers stay off. Quit Steam, or turn off"
                            " PlayStation controller support in Steam > Settings > Controller\n");
            return;
        }
    }
}
#else
static void pad_warn_steam(void) {}
#endif

/* A new controller on port p: a DualSense gets its triggers' feel. */
static void pad_adaptive_open(int p, SDL_GameController* gc)
{
    PadAdaptive* a = &s_adapt[p];
    memset(a, 0, sizeof *a);
    a->right = a->left = PAD_TRIG_UNSET;
    if (!pad_adaptive_strength() || SDL_GameControllerGetType(gc) != SDL_CONTROLLER_TYPE_PS5) return;
    pad_warn_steam();
    if (pad_trig_send_ex(gc, PAD_TRIG_FEEDBACK, PAD_TRIG_FEEDBACK, 1) != 0) {
        fprintf(stderr, "[cellPad] port %d: adaptive triggers not available (%s)\n", p, SDL_GetError());
        return;
    }
    a->on = 1;
    a->right = a->left = PAD_TRIG_FEEDBACK;
    a->sent_ms = pad_now_ms();
    fprintf(stderr, "[cellPad] port %d: adaptive triggers on (strength %d)\n", p, pad_adaptive_strength());
}

/* Free triggers again (closing, exiting). */
static void pad_adaptive_release(int p, SDL_GameController* gc, int gone)
{
    PadAdaptive* a = &s_adapt[p];
    if (a->on && !gone && gc) pad_trig_send(gc, PAD_TRIG_OFF, PAD_TRIG_OFF);
    memset(a, 0, sizeof *a);
}

/* The effects for this poll: a held trigger buzzes with the motors. */
static void pad_adaptive_update(int p, SDL_GameController* gc, int lt, int rt)
{
    PadAdaptive* a = &s_adapt[p];
    if (!a->on) return;
    const PadRumble* r = &s_rumble[p];
    const unsigned level = r->lo > r->hi ? r->lo : r->hi;
    int buzz = PAD_TRIG_FEEDBACK;
    if (level) {
        int amp = 1 + (int)(level * 7u / 65535u);
        buzz = PAD_TRIG_BUZZ + (amp > 8 ? 8 : amp);
    }
    const int right = rt > PAD_TRIGGER_PRESS ? buzz : PAD_TRIG_FEEDBACK;
    const int left  = lt > PAD_TRIGGER_PRESS ? buzz : PAD_TRIG_FEEDBACK;
    if (right != a->right || left != a->left) {
        const unsigned long long now = pad_now_ms();
        if (now - a->sent_ms >= PAD_ADAPT_MIN_GAP_MS && pad_trig_send(gc, right, left) == 0) {
            a->right = right;
            a->left = left;
            a->sent_ms = now;
        }
    }
}

/* At exit: no trigger stays stiff after the game. */
static void pad_adaptive_atexit(void)
{
    if (!pad_lock_try()) return;   /* a poll is mid-way: leave it */
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p]) pad_adaptive_release(p, s_sdl_controllers[p], 0);
    pad_unlock();
}

/* ---- DualSense extras: light bar, player light, gyro ----------------------
 * A PS3 pad has none of these; on a DualSense (and a DualShock 4 for the
 * light bar and the gyro):
 *   - the player lights show the port (1 lit for the first player);
 *   - the light bar takes PS3_PAD_LIGHT=<RRGGBB> (a port sets its game's
 *     colour) or goes dark with "off"; unset leaves SDL's own colour. While
 *     the title runs the motors (hits, clashes) it glows towards red with
 *     them (PS3_PAD_LIGHT_HIT=0: steady colour);
 *   - gyro, PS3_PAD_GYRO=<percent> (0 off, 100 = full right-stick deflection
 *     at about 115 degrees a second): turning and tilting the controller
 *     adds to the right stick, the camera. PS3_PAD_GYRO_MODE=always (the
 *     default) or l1 / l2: only while that PS3 control is held, on whatever
 *     the input map binds it to. Turning left looks left, tilting the top
 *     back looks up (PS3_PAD_GYRO_INVERT_Y=1 flips it). A small motion is
 *     lifted past the title's stick dead zone (PS3_PAD_GYRO_DZ, 0.10 of the
 *     travel).
 * -------------------------------------------------------------------------*/
typedef struct {
    int light_set;                /* PS3_PAD_LIGHT parsed: 1 colour, 0 unset, -1 off */
    uint8_t r, g, b;              /* the colour last sent */
    unsigned long long led_ms;
    int gyro;                     /* the gyro is on for this controller */
} PadExtras;
static PadExtras s_extra[PAD_MAX_HOST_PORTS];

/* PS3_PAD_LIGHT: 1 = a colour (in rgb), -1 = off, 0 = unset or not a colour. */
static int pad_light_parse(const char* e, uint8_t rgb[3])
{
    unsigned v = 0;
    int n = 0;
    rgb[0] = rgb[1] = rgb[2] = 0;
    if (!e || !*e) return 0;
    if (!strcmp(e, "off")) return -1;
    if (e[0] == '#') e++;
    if (strlen(e) != 6 || sscanf(e, "%6x%n", &v, &n) != 1 || n != 6) return 0;
    rgb[0] = (uint8_t)(v >> 16); rgb[1] = (uint8_t)(v >> 8); rgb[2] = (uint8_t)v;
    return 1;
}
static int pad_light_cfg(uint8_t rgb[3])
{
    static int mode = -2; static uint8_t c[3];
    if (mode == -2) mode = pad_light_parse(getenv("PS3_PAD_LIGHT"), c);
    memcpy(rgb, c, 3);
    return mode;
}
static int pad_light_hit(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3_PAD_LIGHT_HIT"); on = !(e && e[0] == '0'); }
    return on;
}
static float pad_gyro_sens(void)
{
    static float v = -1.0f;
    if (v < 0.0f) { const char* e = getenv("PS3_PAD_GYRO"); v = e && *e ? (float)atof(e) / 100.0f : 0.0f;
                    if (!(v >= 0.0f)) v = 0.0f; if (v > 4.0f) v = 4.0f; }
    return v;
}
/* The PS3 buttons that turn the gyro on while held; 0 = always. */
static u16 pad_gyro_gate(void)
{
    static int g = -1;
    if (g < 0) {
        const char* e = getenv("PS3_PAD_GYRO_MODE");
        g = 0;
        if (e && pad_str_ieq(e, "l1")) g = CELL_PAD_CTRL_L1;
        if (e && pad_str_ieq(e, "l2")) g = CELL_PAD_CTRL_L2;
    }
    return (u16)g;
}

static void pad_extras_open(int p, SDL_GameController* gc)
{
    PadExtras* x = &s_extra[p];
    memset(x, 0, sizeof *x);
    SDL_GameControllerSetPlayerIndex(gc, p);   /* the DualSense's player lights */
    uint8_t c[3];
    x->light_set = pad_light_cfg(c);
    if (x->light_set && SDL_GameControllerHasLED(gc)) {
        SDL_GameControllerSetLED(gc, c[0], c[1], c[2]);
        x->r = c[0]; x->g = c[1]; x->b = c[2];
        fprintf(stderr, "[cellPad] port %d: light bar #%02X%02X%02X\n", p, c[0], c[1], c[2]);
    }
    if (pad_gyro_sens() > 0.0f && SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO) &&
        SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE) == 0) {
        x->gyro = 1;
        fprintf(stderr, "[cellPad] port %d: gyro on (%.0f%%)\n", p, pad_gyro_sens() * 100.0f);
    }
}

/* The light bar for this poll: the colour, pulled towards red by the rumble. */
static void pad_extras_light(int p, SDL_GameController* gc)
{
    PadExtras* x = &s_extra[p];
    uint8_t c[3];
    const int mode = pad_light_cfg(c);
    if (!mode || !SDL_GameControllerHasLED(gc)) return;
    if (pad_light_hit()) {
        const PadRumble* r = &s_rumble[p];
        const unsigned level = r->lo > r->hi ? r->lo : r->hi;
        const float t = (float)level / 65535.0f;
        c[0] = (uint8_t)(c[0] + (255 - c[0]) * t);
        c[1] = (uint8_t)(c[1] * (1.0f - t));
        c[2] = (uint8_t)(c[2] * (1.0f - t));
    }
    if (c[0] == x->r && c[1] == x->g && c[2] == x->b) return;
    const unsigned long long now = pad_now_ms();
    if (now - x->led_ms < 40) return;
    if (SDL_GameControllerSetLED(gc, c[0], c[1], c[2]) == 0) { x->r = c[0]; x->g = c[1]; x->b = c[2]; x->led_ms = now; }
}

/* Gyro: what the controller's turn this poll adds to the right stick, in
 * SDL axis units. */
static void pad_extras_gyro(int p, SDL_GameController* gc, u16 btns, int* rx, int* ry)
{
    PadExtras* x = &s_extra[p];
    if (!x->gyro) return;
    static int inv_y = -1; static float dz = -1.0f;
    if (inv_y < 0) { const char* e = getenv("PS3_PAD_GYRO_INVERT_Y"); inv_y = e && e[0] == '1'; }
    if (dz < 0.0f) { const char* e = getenv("PS3_PAD_GYRO_DZ"); dz = e ? (float)atof(e) : 0.10f; if (!(dz >= 0.0f) || dz > 0.5f) dz = 0.10f; }
    const u16 gate = pad_gyro_gate();
    if (gate && !(btns & gate)) return;
    float g[3] = { 0, 0, 0 };   /* rad/s: pitch (x), yaw (y), roll (z) */
    if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_GYRO, g, 3) != 0) return;
    const float full = 2.0f / pad_gyro_sens();   /* rad/s for a full stick */
    float ax = -g[1] / full, ay = (inv_y ? g[0] : -g[0]) / full;
    const float noise = 0.015f / full;           /* the resting gyro's jitter */
    float* v[2] = { &ax, &ay };
    for (int k = 0; k < 2; k++) {
        float a = *v[k];
        if (fabsf(a) < noise) a = 0.0f;
        else a = (a > 0 ? 1.0f : -1.0f) * (dz + (1.0f - dz) * fminf(1.0f, fabsf(a)));
        *v[k] = a;
    }
    int nx = *rx + (int)(ax * 32767.0f), ny = *ry + (int)(ay * 32767.0f);
    *rx = nx < -32768 ? -32768 : nx > 32767 ? 32767 : nx;
    *ry = ny < -32768 ? -32768 : ny > 32767 ? 32767 : ny;
}

/* ---- hot-plug ------------------------------------------------------------
 * A DualSense over Bluetooth drops off when it is switched off or idles, and
 * comes back as a NEW SDL device (a new instance id, maybe another index).
 * This used to open controllers by device index == PS3 port and ask
 * SDL_IsGameController(port) again on every poll. With Homebrew's SDL2, which
 * is sdl2-compat over SDL3, a device index is resolved through a list that is
 * refreshed only by SDL_NumJoysticks() or by the SDL2 event filter (installed
 * with an SDL window, which the Metal build never makes): the list kept the
 * switched-off pad's instance id, SDL_IsGameController(0) stayed false, and
 * the pad that came back was never opened until the game was restarted.
 *
 * Now each poll drains SDL's joystick/controller events (nothing else here
 * reads them, and without a window nothing else does either, so they piled up
 * in the queue), closes a controller SDL says is gone, by its own handle, and
 * on a device event -- or every PAD_SDL_RESCAN_MS while a port is free, in
 * case a window's event loop ate the event -- walks the device list from
 * SDL_NumJoysticks() and opens every game controller that is not open yet:
 * into the port its GUID had last (the same pad reconnecting gets its port
 * back), else the lowest free one. A new controller starts with its motors
 * off and no rumble left over. The port reads connected again on the next
 * poll, and cellPadGetInfo/GetInfo2/PeriphGetInfo flag the change once with
 * CELL_PAD_STATUS_ASSIGN_CHANGES (port 0 is the virtual pad and never reads
 * disconnected). */
#define PAD_SDL_RESCAN_MS 1000

static int pad_sdl_port_of(SDL_JoystickID id)
{
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p] && s_sdl_ids[p] == id) return p;
    return -1;
}

/* Close port p's controller (gone, or shutting down). */
static void pad_sdl_close(int p, int gone)
{
    SDL_GameController* gc = s_sdl_controllers[p];
    if (!gc) return;
    if (gone) fprintf(stderr, "[cellPad] SDL controller on port %d disconnected (instance %d)\n", p, (int)s_sdl_ids[p]);
    else      SDL_GameControllerRumble(gc, 0, 0, 0);
    pad_adaptive_release(p, gc, gone);
    memset(&s_extra[p], 0, sizeof s_extra[p]);
    SDL_GameControllerClose(gc);
    s_sdl_controllers[p] = NULL;
    s_sdl_ids[p] = -1;
    memset(&s_rumble[p], 0, sizeof s_rumble[p]);
    pad_host_release(&s_host_state[p]);
}

/* Open every game controller SDL lists that no port has yet. */
static void pad_sdl_scan(void)
{
    const int n = SDL_NumJoysticks();   /* also refreshes sdl2-compat's index list */
    for (int idx = 0; idx < n; idx++) {
        if (!SDL_IsGameController(idx)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(idx);
        if (id < 0 || pad_sdl_port_of(id) >= 0) continue;
        const SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(idx);
        int port = -1, back = 0;
        for (int p = 0; p < PAD_MAX_HOST_PORTS && port < 0; p++)
            if (!s_sdl_controllers[p] && s_sdl_had[p] && !memcmp(&s_sdl_last_guid[p], &guid, sizeof guid)) { port = p; back = 1; }
        for (int p = 0; p < PAD_MAX_HOST_PORTS && port < 0; p++)
            if (!s_sdl_controllers[p]) port = p;
        if (port < 0) return;                 /* every port has a controller */
        SDL_GameController* gc = SDL_GameControllerOpen(idx);
        if (!gc) {
            fprintf(stderr, "[cellPad] SDL controller %d: open failed: %s\n", idx, SDL_GetError());
            continue;
        }
        s_sdl_controllers[port] = gc;
        s_sdl_ids[port] = id;
        s_sdl_last_guid[port] = guid;
        s_sdl_had[port] = 1;
        memset(&s_rumble[port], 0, sizeof s_rumble[port]);   /* nothing left over from before */
        SDL_GameControllerRumble(gc, 0, 0, 0);
        const char* name = SDL_GameControllerName(gc);
        fprintf(stderr, "[cellPad] SDL controller %s on port %d: %s (instance %d)%s\n",
                back ? "reconnected" : "connected", port, name ? name : "?", (int)id,
                SDL_GameControllerHasRumble(gc) ? "" : " (no rumble)");
        pad_adaptive_open(port, gc);
        pad_extras_open(port, gc);
    }
}

/* Device changes since the last poll; force = scan now (init). */
static void pad_sdl_hotplug(int force)
{
    int changed = force;
    SDL_Event ev[32];
    int got;
    /* SDL_JOYAXISMOTION (0x600) up to the touch events (0x700): every
     * joystick and game controller event, which only this file uses. */
    while ((got = SDL_PeepEvents(ev, 32, SDL_GETEVENT, SDL_JOYAXISMOTION, SDL_JOYAXISMOTION + 0xFF)) > 0)
        for (int i = 0; i < got; i++)
            if (ev[i].type == SDL_JOYDEVICEADDED || ev[i].type == SDL_JOYDEVICEREMOVED ||
                ev[i].type == SDL_CONTROLLERDEVICEADDED || ev[i].type == SDL_CONTROLLERDEVICEREMOVED)
                changed = 1;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p] && !SDL_GameControllerGetAttached(s_sdl_controllers[p])) {
            pad_sdl_close(p, 1);
            changed = 1;
        }
    int free_port = 0;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) if (!s_sdl_controllers[p]) free_port = 1;
    if (!free_port) return;
    const unsigned long long now = pad_now_ms();
    if (!changed && s_sdl_scan_ms && now - s_sdl_scan_ms < PAD_SDL_RESCAN_MS) return;
    s_sdl_scan_ms = now;
    pad_sdl_scan();
}

static void pad_poll_sdl2(void)
{
    if (pad_sdl_off()) {
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) pad_host_release(&s_host_state[i]);
        return;
    }
    SDL_GameControllerUpdate();
    pad_sdl_hotplug(0);

    const PadInputMap* map = pad_map();
    uint32_t raw_all = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc) {
            pad_host_release(&s_host_state[i]);
            memset(&s_rumble[i], 0, sizeof s_rumble[i]);
            continue;
        }

        s_host_state[i].connected = 1;

        /* Keep a rumble the title set going (see PAD_RUMBLE_HOLD_MS), and send
         * one cellPadSetActDirect left to this poll. */
        if (s_rumble[i].dirty ||
            (s_rumble[i].sent_ms && pad_now_ms() - s_rumble[i].sent_ms >= PAD_RUMBLE_RENEW_MS))
            pad_sdl_rumble_send(i);

        /* Every SDL input, then the PS3 controls bound to them ([controller]
         * in PS3_INPUT_MAP; by default each button is its PS3 namesake and
         * the triggers are L2/R2, pressed past 3000). */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        uint32_t in = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < PIM_PAD_TRIGGER_LEFT; b++)
            if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) in |= 1u << b;
        if (lt > PAD_TRIGGER_PRESS) in |= 1u << PIM_PAD_TRIGGER_LEFT;
        if (rt > PAD_TRIGGER_PRESS) in |= 1u << PIM_PAD_TRIGGER_RIGHT;
        pad_adaptive_update(i, gc, lt, rt);
        raw_all |= in;
        uint32_t ctl = 0;
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (map->pad[c] & in) ctl |= 1u << c;
        u16 btns = (u16)(ctl & 0xFFFFu);

        s_host_state[i].buttons = btns;

        /* Analog sticks; a stick direction bound to a button pushes its axis
         * to the end while held. */
        s_host_state[i].analog_lx = (u8)pad_dir_axis(ctl, PIM_LS_LEFT, PIM_LS_RIGHT,
                                        pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)));
        s_host_state[i].analog_ly = (u8)pad_dir_axis(ctl, PIM_LS_UP, PIM_LS_DOWN,
                                        pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)));
        int rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        int ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        pad_extras_gyro(i, gc, btns, &rx, &ry);
        pad_extras_light(i, gc);
        s_host_state[i].analog_rx = (u8)pad_dir_axis(ctl, PIM_RS_LEFT, PIM_RS_RIGHT, pad_sdl_axis_to_u8(rx));
        s_host_state[i].analog_ry = (u8)pad_dir_axis(ctl, PIM_RS_UP, PIM_RS_DOWN, pad_sdl_axis_to_u8(ry));

        /* Analog L2/R2 from whatever is bound to them. */
        s_host_state[i].trigger_l2 = pad_sdl_analog(map->pad[PIM_L2], in, pad_sdl_trigger_to_u8(lt), pad_sdl_trigger_to_u8(rt));
        s_host_state[i].trigger_r2 = pad_sdl_analog(map->pad[PIM_R2], in, pad_sdl_trigger_to_u8(lt), pad_sdl_trigger_to_u8(rt));

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
    g_pad_overlay_raw = raw_all;
}

static void pad_init_backend(void)
{
    if (pad_sdl_off()) return;
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            /* The game is not always the focused app: on macOS its window
             * can belong to another process (the launcher in
             * tools/host_window), and then this process is never in the
             * foreground. Controller input must not depend on focus. The
             * hint stops SDL dropping joystick events while it has no
             * focused window and lets Apple's GameController framework
             * deliver them to a background process
             * (GCController.shouldMonitorBackgroundEvents). The HIDAPI
             * drivers (DualShock 4, DualSense) read IOKit directly and never
             * cared. SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0 in the
             * environment still wins over this. */
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
        atexit(pad_adaptive_atexit);
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) s_sdl_ids[p] = -1;

    /* Open any controllers already connected */
    pad_sdl_hotplug(1);
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) pad_sdl_close(i, 0);
    memset(s_rumble, 0, sizeof s_rumble);
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

#if PAD_KBM_HOST_INPUT
/* ---------------------------------------------------------------------------
 * Keyboard and mouse (macOS)
 *
 * Key state by macOS virtual key code, mouse motion, wheel and buttons sit in
 * a ps3_host_input block (host_input.h). The window that shows the game
 * fills it from its events (host_input_cocoa.h): the Metal backend's own
 * window in this process, or a window host through a shared page. Each poll
 * turns the block into port 0's state through the bindings in PS3_INPUT_MAP
 * (pad_input_map.h; the built-in layout is in pim_defaults), together with a
 * controller on port 0 if there is one.
 *
 * Until a window asks for the block (ps3_pad_host_input), nobody feeds it,
 * and the poll fills the keys itself: the keys the map binds are read with
 * CGEventSourceKeyState while this application is active, as the fixed
 * keyboard fallback did before the map. That covers the keyboard only: mouse
 * motion, buttons and the wheel exist only as the window's events.
 *
 * PAD_NO_KEYBOARD=1 switches the keyboard and the mouse off, as it switched
 * the keyboard fallback off: port 0 is then the controller's alone.
 *
 * Mouse camera: a stick is a rate and a mouse a displacement, so the right
 * stick deflects in proportion to the mouse's speed since the last poll,
 * with a light smoothing so a frame without motion events does not snap it
 * home, and starts past the title's own dead zone so slow moves still turn.
 * Settings, the variable first, then [mouse] in PS3_INPUT_MAP:
 *   PS3_MOUSE=0              camera = false: off (keys I/J/K/L only; the
 *                            mouse buttons do nothing either)
 *   PS3_MOUSE_SENS=<f>       sensitivity: speed multiplier (default 1.0)
 *   PS3_MOUSE_INVERT_Y=1     invert_y: moving the mouse up looks down
 *   PS3_MOUSE_INVERT_X=1     invert_x: moving it right looks left
 *   PS3_MOUSE_DEADZONE=<n>   deadzone: first stick step past centre, 0-120 (30)
 *   PS3_MOUSE_CAPTURE=0      capture = false: no cursor capture, motion
 *                            counts whenever the window is key (by default a
 *                            click in the game captures the mouse, and Esc,
 *                            Cmd or leaving the app releases it)
 *   PS3_MOUSE_LOG=1          print what the mouse does to the stick
 * -------------------------------------------------------------------------*/

/* Port-0 state from a button mask and stick positions (0-255, centre 128). */
static void pad_apply(PadHostState* hs, u16 btns, int lx, int ly, int rx, int ry)
{
    hs->buttons = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)lx; hs->analog_ly = (u8)ly;
    hs->analog_rx = (u8)rx; hs->analog_ry = (u8)ry;
    hs->trigger_l2 = (btns & CELL_PAD_CTRL_L2) ? 255 : 0;
    hs->trigger_r2 = (btns & CELL_PAD_CTRL_R2) ? 255 : 0;
    const u8 full = 255;
    hs->press_up       = (btns & CELL_PAD_CTRL_UP)       ? full : 0;
    hs->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? full : 0;
    hs->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? full : 0;
    hs->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? full : 0;
    hs->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? full : 0;
    hs->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? full : 0;
    hs->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? full : 0;
    hs->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? full : 0;
    hs->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? full : 0;
    hs->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? full : 0;
}

/* A stick value from a mouse speed (stick units, + = right/down) past a
 * dead zone of `dz`. */
static int pad_mouse_axis(float v, int dz)
{
    if (v > -0.5f && v < 0.5f) return 128;
    float m = (v < 0 ? -v : v) + (float)dz;
    if (m > 127.0f) m = 127.0f;
    return v < 0 ? 128 - (int)(m + 0.5f) - (m >= 127.0f ? 1 : 0) : 128 + (int)(m + 0.5f);
}

/* A wheel notch is a press: down for 70 ms, then up for 50 ms so the next
 * notch is a press of its own; up to 3 queued. */
typedef struct { int pending; unsigned long long down_until, up_until; } PadPulse;
static int pad_pulse(PadPulse* p, unsigned long long now)
{
    if (now < p->down_until) return 1;
    if (now < p->up_until) return 0;
    if (p->pending > 0) { p->pending--; p->down_until = now + 70; p->up_until = now + 120; return 1; }
    return 0;
}

/* An int setting: the variable, else the file's value (-1 = not there), else `def`. */
static int pad_env_int(const char* var, int file, int def)
{
    const char* e = getenv(var);
    return e && *e ? atoi(e) : file >= 0 ? file : def;
}

static int s_mouse_on = -1, s_mouse_invert_x, s_mouse_invert_y, s_mouse_dz, s_mouse_capture, s_mouse_log;
static float s_mouse_sens;
static void pad_mouse_settings_load(void)
{
    if (s_mouse_on >= 0) return;
    const PadInputMap* map = pad_map();
    const char* e = getenv("PS3_MOUSE_SENS");
    s_mouse_sens     = e && *e ? (float)atof(e) : map->mouse_sens > 0.0f ? map->mouse_sens : 1.0f;
    if (!(s_mouse_sens > 0.0f)) s_mouse_sens = 1.0f;
    s_mouse_invert_x = pad_env_int("PS3_MOUSE_INVERT_X", map->mouse_invert_x, 0) != 0;
    s_mouse_invert_y = pad_env_int("PS3_MOUSE_INVERT_Y", map->mouse_invert_y, 0) != 0;
    s_mouse_dz       = pad_env_int("PS3_MOUSE_DEADZONE", map->mouse_deadzone, 30);
    if (s_mouse_dz < 0) s_mouse_dz = 0;
    if (s_mouse_dz > 120) s_mouse_dz = 120;
    s_mouse_capture  = pad_env_int("PS3_MOUSE_CAPTURE", map->mouse_capture, 1) != 0;
    s_mouse_log      = getenv("PS3_MOUSE_LOG") ? 1 : 0;
    s_mouse_on       = pad_env_int("PS3_MOUSE", map->mouse_camera, 1) != 0;
}
/* The in-game menu (ps3_overlay.mm): the mouse camera and its settings. */
void cellPad_host_mouse_settings(int* on, float* sens, int* invert_x, int* invert_y)
{
    pad_mouse_settings_load();
    *on = s_mouse_on; *sens = s_mouse_sens; *invert_x = s_mouse_invert_x; *invert_y = s_mouse_invert_y;
}
void cellPad_host_set_mouse_settings(float sens, int invert_x, int invert_y)
{
    pad_mouse_settings_load();
    if (sens > 0.0f) s_mouse_sens = sens;
    s_mouse_invert_x = invert_x != 0;
    s_mouse_invert_y = invert_y != 0;
}
void cellPad_host_set_mouse_camera(int on)
{
    pad_mouse_settings_load();
    s_mouse_on = on != 0;
}

/* ASCII case-insensitive equality (no strcasecmp on Windows). */
static inline int pad_str_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* PS3_INPUT_MODE: what drives the game.
 *   both (default)   controllers, keyboard and mouse together
 *   controller       controllers only: keys and the mouse do nothing
 *   keyboard         keyboard and mouse only: controllers are ignored */
enum { PAD_MODE_BOTH, PAD_MODE_CONTROLLER, PAD_MODE_KEYBOARD };
static int s_input_mode = -1;
static int pad_input_mode_from(const char* e)
{
    if (!e || !*e) return PAD_MODE_BOTH;
    if (pad_str_ieq(e, "controller") || pad_str_ieq(e, "pad") || pad_str_ieq(e, "gamepad")) return PAD_MODE_CONTROLLER;
    if (pad_str_ieq(e, "keyboard") || pad_str_ieq(e, "kbm") || pad_str_ieq(e, "keyboard_mouse") ||
        pad_str_ieq(e, "mouse")) return PAD_MODE_KEYBOARD;
    if (!pad_str_ieq(e, "both") && !pad_str_ieq(e, "all"))
        fprintf(stderr, "[cellPad] PS3_INPUT_MODE=%s: expected both, controller or keyboard; both\n", e);
    return PAD_MODE_BOTH;
}
static int pad_input_mode(void)
{
    int m = s_input_mode;
    if (m < 0) { m = pad_input_mode_from(getenv("PS3_INPUT_MODE")); s_input_mode = m; }
    return m;
}
/* The in-game menu (ps3_overlay.mm) switches it live. */
int  cellPad_host_input_mode(void) { return pad_input_mode(); }
void cellPad_host_set_input_mode(int mode)
{
    s_input_mode = mode == PAD_MODE_CONTROLLER || mode == PAD_MODE_KEYBOARD ? mode : PAD_MODE_BOTH;
}

/* Keys and mouse buttons that were down when the in-game menu closed (the
 * Esc or the click that closed it, still held at the first poll after): kept
 * from the game until let go. */
static uint32_t s_menu_keys[4], s_menu_mouse;
static int s_menu_was_open;
volatile int g_pad_overlay_open;

/* PAD_NO_KEYBOARD (any value): no keyboard and no mouse. */
static int s_kbm_off = -1;
static int pad_kbm_off(void)
{
    int off = s_kbm_off;
    if (off < 0) {
        off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
        if (off && pad_input_mode() == PAD_MODE_KEYBOARD)
            fprintf(stderr, "[cellPad] PAD_NO_KEYBOARD with PS3_INPUT_MODE=keyboard: nothing drives the game\n");
        s_kbm_off = off;
    }
    return off;
}

/* What the window should do with the mouse (ps3_host_input.game_flags). */
static uint32_t pad_mouse_flags(void)
{
    pad_mouse_settings_load();
    uint32_t f = PS3_HOST_INPUT_ACTIVE;
    if (s_mouse_on && pad_input_mode() != PAD_MODE_CONTROLLER && !pad_kbm_off()) {
        f |= PS3_HOST_INPUT_MOUSE_CAMERA;
        if (s_mouse_capture) f |= PS3_HOST_INPUT_CAPTURE;
    }
    return f;
}

/* No window feeds the block: fill its keys by polling. Only the keys the map
 * binds are asked for, and only while this application is active, so typing
 * in another one does not drive the game and a key held while switching away
 * is released. Says once which of the two sources the keyboard has. */
static void pad_keys_poll_host(ps3_host_input* in, const PadInputMap* map)
{
    static int said = -1;
    const int fed = __atomic_load_n(&s_input_fed, __ATOMIC_ACQUIRE);
    if (fed != said) {
        said = fed;
        fprintf(stderr, fed ? "[cellPad] keyboard and mouse: the window's events\n"
                            : "[cellPad] keyboard: polled (no window hands over its events; no mouse)\n");
    }
    if (fed) return;
    uint32_t want[4] = {0, 0, 0, 0}, down[4] = {0, 0, 0, 0};
    for (int c = 0; c < PIM_N_CONTROLS; c++)
        for (int i = 0; i < 4; i++) want[i] |= map->key[c][i];
    if ((want[0] | want[1] | want[2] | want[3]) && pad_host_window_focused())
        for (unsigned vk = 0; vk < 128; vk++)
            if ((want[vk >> 5] >> (vk & 31) & 1u) && pad_key_down((int)vk))
                down[vk >> 5] |= 1u << (vk & 31);
    for (int i = 0; i < 4; i++)
        if (atomic_load(&in->keys[i]) != down[i]) atomic_store(&in->keys[i], down[i]);
}

/* Mouse buttons and wheel -> controls in *ctl; motion -> *mrx and *mry (-1
 * when the mouse is not moving, so the keys keep the right stick). */
static void pad_poll_mouse(ps3_host_input* in, uint32_t* ctl, int* mrx, int* mry)
{
    static float sx, sy; static unsigned long long last;
    static int32_t wheel_acc; static PadPulse wheel_up, wheel_down;
    const PadInputMap* map = pad_map();
    *mrx = *mry = -1;
    pad_mouse_settings_load();
    /* Drained whatever the setting, so nothing is left to jump the camera
     * when the mouse comes back. */
    const int32_t dx = atomic_exchange(&in->mouse_dx, 0);
    const int32_t dy = atomic_exchange(&in->mouse_dy, 0);
    const int32_t w  = atomic_exchange(&in->wheel, 0);
    if (!s_mouse_on) return;
    const unsigned long long now = pad_now_ms();
    uint32_t b = atomic_load(&in->mouse_buttons);
    s_menu_mouse &= b;                      /* held when the in-game menu closed: out until let go */
    b &= ~s_menu_mouse;
    if ((w > 0 && wheel_acc < 0) || (w < 0 && wheel_acc > 0)) wheel_acc = 0;   /* turned back */
    wheel_acc += w;
    while (wheel_acc >= 16)  { wheel_acc -= 16; if (wheel_up.pending < 3)   wheel_up.pending++; }
    while (wheel_acc <= -16) { wheel_acc += 16; if (wheel_down.pending < 3) wheel_down.pending++; }
    if (pad_pulse(&wheel_up, now))   b |= PIM_MOUSE_WHEEL_UP;
    if (pad_pulse(&wheel_down, now)) b |= PIM_MOUSE_WHEEL_DOWN;
    if (b)
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (map->mouse[c] & b) *ctl |= 1u << c;
    unsigned long long dt = last ? now - last : 16;
    last = now;
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;                 /* a long gap is not one slow move */
    /* px per ms; 1.5 px/ms (a brisk flick) reaches full deflection at SENS 1. */
    const float k = 127.0f / 1.5f * s_mouse_sens;
    const float vx = (float)dx / 16.0f / (float)dt * k * (s_mouse_invert_x ? -1.0f : 1.0f);
    const float vy = (float)dy / 16.0f / (float)dt * k * (s_mouse_invert_y ? -1.0f : 1.0f);
    sx = sx * 0.35f + vx * 0.65f;
    sy = sy * 0.35f + vy * 0.65f;
    if (sx > -0.5f && sx < 0.5f) sx = 0.0f;
    if (sy > -0.5f && sy < 0.5f) sy = 0.0f;
    if (sx != 0.0f) *mrx = pad_mouse_axis(sx, s_mouse_dz);
    if (sy != 0.0f) *mry = pad_mouse_axis(sy, s_mouse_dz);   /* deltaY > 0 = down, as the stick */
    if (s_mouse_log) {
        static int logged; static int lrx = -1, lry = -1; static uint32_t lb;
        if (logged < 400 && (*mrx != lrx || *mry != lry || b != lb)) {
            logged++; lrx = *mrx; lry = *mry; lb = b;
            printf("[cellPad] mouse: d=(%d,%d)/16 px in %llu ms -> rx=%d ry=%d buttons=0x%X\n",
                   dx, dy, dt, *mrx, *mry, b); fflush(stdout);
        }
    }
}

/* Port 0 from the keyboard and mouse. With a real pad on port 0 (`merge`:
 * the SDL poll filled it), they add to it instead: buttons are OR-ed, and an
 * axis the keys or mouse move wins over the pad's, so an idle paired
 * controller does not switch the keyboard off. */
static void pad_poll_keys(int merge)
{
    ps3_host_input* in = __atomic_load_n(&s_input, __ATOMIC_ACQUIRE);
    const PadInputMap* map = pad_map();
    if (g_pad_overlay_open) {
        /* The in-game menu has the keyboard and the mouse: the window gives
         * it the pointer, the wheel is left to it, motion is dropped, and
         * whatever is held now stays out of the game until it is let go. */
        const uint32_t menu = PS3_HOST_INPUT_ACTIVE | PS3_HOST_INPUT_MENU;
        if (atomic_load(&in->game_flags) != menu) atomic_store(&in->game_flags, menu);
        atomic_store(&in->mouse_dx, 0);
        atomic_store(&in->mouse_dy, 0);
        s_menu_was_open = 1;
        return;
    }
    if (s_menu_was_open) {               /* the first poll since the menu closed */
        s_menu_was_open = 0;
        for (int i = 0; i < 4; i++) s_menu_keys[i] = atomic_load(&in->keys[i]);
        s_menu_mouse = atomic_load(&in->mouse_buttons);
    }
    const uint32_t flags = pad_mouse_flags();
    if (atomic_load(&in->game_flags) != flags) atomic_store(&in->game_flags, flags);
    if (pad_kbm_off()) {
        /* PAD_NO_KEYBOARD: port 0 stays as the controller poll left it.
         * Motion is still drained, so nothing piles up in the block. */
        atomic_store(&in->mouse_dx, 0);
        atomic_store(&in->mouse_dy, 0);
        atomic_store(&in->wheel, 0);
        return;
    }
    pad_keys_poll_host(in, map);
    uint32_t keys[4];
    for (int i = 0; i < 4; i++) {
        keys[i] = atomic_load(&in->keys[i]);
        s_menu_keys[i] &= keys[i];
        keys[i] &= ~s_menu_keys[i];
    }
    uint32_t ctl = 0;
    if (keys[0] | keys[1] | keys[2] | keys[3])
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (pim_keys_hit(map, c, keys)) ctl |= 1u << c;
    int mrx, mry;
    pad_poll_mouse(in, &ctl, &mrx, &mry);
    if (pad_input_mode() == PAD_MODE_CONTROLLER) { ctl = 0; mrx = mry = -1; }
    u16 btns = (u16)(ctl & 0xFFFFu);
    /* Stick axes: 0 = left/up, 255 = right/down. */
    int lx = pad_dir_axis(ctl, PIM_LS_LEFT, PIM_LS_RIGHT, 128);
    int ly = pad_dir_axis(ctl, PIM_LS_UP, PIM_LS_DOWN, 128);
    int rx = pad_dir_axis(ctl, PIM_RS_LEFT, PIM_RS_RIGHT, 128);
    int ry = pad_dir_axis(ctl, PIM_RS_UP, PIM_RS_DOWN, 128);
    if (mrx >= 0) rx = mrx;   /* mouse motion wins over I/J/K/L while it moves */
    if (mry >= 0) ry = mry;
    PadHostState* hs = &s_host_state[0];
    if (merge) {
        btns |= hs->buttons;
        if (lx == 128) lx = hs->analog_lx;
        if (ly == 128) ly = hs->analog_ly;
        if (rx == 128) rx = hs->analog_rx;
        if (ry == 128) ry = hs->analog_ry;
        const u8 l2 = hs->trigger_l2, r2 = hs->trigger_r2;
        pad_apply(hs, btns, lx, ly, rx, ry);
        if (l2 > hs->trigger_l2) hs->trigger_l2 = l2;   /* keep an analog trigger's own value */
        if (r2 > hs->trigger_r2) hs->trigger_r2 = r2;
        return;
    }
    { static u16 said = 0;
      if (btns != said) {
          static int n = 0;
          if (PS3_LOG_FIRST(n, 4)) { printf("[cellPad] keyboard/mouse on port 0 (buttons 0x%04X)\n", btns); fflush(stdout); }
          said = btns;
      } }
    pad_apply(hs, btns, lx, ly, rx, ry);
}
#endif /* PAD_KBM_HOST_INPUT */

/* The in-game menu (libs/video/ps3_overlay.mm). Plain globals rather than
 * calls, so this file still builds on its own (the tests include it):
 *   g_pad_overlay_open     set by the menu while it is open: every port is
 *                          held neutral (connected, nothing pressed)
 *   g_pad_overlay_toggle   counted up here when L3+R3 go down together on a
 *                          controller (PS3_OVERLAY_PAD=0: never); the menu
 *                          opens or closes on each change. While both are
 *                          held neither reaches the title.
 *   g_pad_overlay_nav      port 0's controller before it is held neutral
 *                          (buttons | lx << 16 | ly << 24; rx, ry in _nav2),
 *                          which the menu navigates with
 * A controller button held when the menu closes (the Circle that closed it)
 * stays out of the game until it is let go, as the keys do. */
volatile uint32_t g_pad_overlay_toggle;
volatile uint32_t g_pad_overlay_nav = 0x80800000u, g_pad_overlay_nav2 = 0x8080u;
static void pad_overlay_gate(void)
{
    static int combo_on = -1, held, was_open;
    static u16 menu_btns[PAD_MAX_HOST_PORTS];
    if (combo_on < 0) { const char* e = getenv("PS3_OVERLAY_PAD"); combo_on = !(e && e[0] == '0'); }
    const u16 both = CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3;
    int down = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected && (s_host_state[i].buttons & both) == both) down = 1;
    if (combo_on && down && !held) __atomic_add_fetch(&g_pad_overlay_toggle, 1, __ATOMIC_RELEASE);
    if (combo_on) held = down ? 1 : (held && (s_host_state[0].buttons & both)) ? 1 : 0;
    const PadHostState* p = &s_host_state[0];
    if (p->connected) {
        g_pad_overlay_nav  = p->buttons | ((uint32_t)p->analog_lx << 16) | ((uint32_t)p->analog_ly << 24);
        g_pad_overlay_nav2 = p->analog_rx | ((uint32_t)p->analog_ry << 8);
    } else {
        g_pad_overlay_nav = 0x80800000u; g_pad_overlay_nav2 = 0x8080u;
    }
    if (held)
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) s_host_state[i].buttons &= (u16)~both;
    const int open = g_pad_overlay_open;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (open) continue;
        if (was_open) menu_btns[i] = s_host_state[i].buttons;   /* held at the first poll after closing */
        menu_btns[i] &= s_host_state[i].buttons;
        s_host_state[i].buttons &= (u16)~menu_btns[i];
    }
    was_open = open;
}
static void pad_overlay_neutral(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        PadHostState* hs = &s_host_state[i];
        const int c = hs->connected;
        memset(hs, 0, sizeof *hs);
        hs->connected = c;
        hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
    }
}

static void pad_poll_backend(void)
{
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    if (!pad_lock_try()) return;   /* another thread is polling: its result serves this read */
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
    pad_overlay_gate();   /* the controllers alone: before the keys join in */
#if PAD_KBM_HOST_INPUT
    /* PS3_INPUT_MODE=keyboard: the game does not see the controllers (they
     * have done their part for the in-game menu: L3+R3, navigation). */
    if (pad_input_mode() == PAD_MODE_KEYBOARD)
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) pad_host_release(&s_host_state[i]);
    /* The keyboard and mouse always count; with a real pad on port 0 they
     * are merged into it. */
    pad_poll_keys(s_host_state[0].connected);
#else
    if (!s_host_state[0].connected) pad_poll_keyboard();
#endif
    if (g_pad_overlay_open) pad_overlay_neutral();
    pad_track_assign();
    pad_unlock();
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_map_load();   /* PS3_INPUT_MAP, before any poll can read it */
    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        pad_host_release(&s_host_state[i]);

    pad_lock();
    pad_init_backend();
    pad_unlock();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();
    /* What is there at start is the baseline; only later changes are
     * flagged CELL_PAD_STATUS_ASSIGN_CHANGES. */
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_port_reported[i] = s_host_state[i].connected || i == 0;
        s_port_changed[i] = 0;
    }

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_lock();
    pad_shutdown_backend();
    pad_unlock();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

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
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* Finalize this call's packet freshness before any injected hold budget
     * advances. len=0 drain reads still build the ordinary payload, but do
     * not consume a PAD_FILE report. See the emission contract below. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second (or flip, under PAD_CLOCK=flips), each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[256]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 256) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)%s\n", n_ev,
                     *p ? " -- TRUNCATED at 256, the rest are ignored" : "");
          }
      }
      if (s_sc && port_no == 0) {
          double now = pad_clock(t0);
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[256], held[256];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[256];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of positive guest reports and empty the file on admission. A driver
     * outside the process can look at the log or a frame dump, decide which
     * screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 reports (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, 0, 0);
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) {
              if (data->len > 0) held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && pad_clock(s_t0) >= s_from && pad_clock(s_t0) < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
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
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
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
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * Freshness was finalized before input injection above. Hand out one
     * packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, (u8)((on ? CELL_PAD_STATUS_CONNECTED : 0) | pad_status_take(i)));
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED | pad_status_take(i);
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED | pad_status_take(i);
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
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED | pad_status_take(i);
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
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED | pad_status_take(i);
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 2))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 16))
        printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
               port_no, port_setting); }

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

/* cellPadInfoSensorMode (NID 0x78200559) / cellPadSetSensorMode (NID 0xBE5BE3BA)
 *
 * The same pair for the SIXAXIS sensors, which the virtual pad also advertises
 * (CELL_PAD_CAPABILITY_SENSOR_MODE). Unresolved, Info answered 0 -- "no
 * sensors" from a pad whose capability word says otherwise -- and Set never
 * turned the sensor block of CellPadData on. With SENSOR_ON the packet grows to
 * CELL_PAD_LEN_CHANGE_SENSOR_ON and carries the at-rest readings (see
 * cellPadGetData), which is what a DS3 lying still reports. */
s32 cellPadInfoSensorMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetSensorMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    const u32 was = s_port_setting[port_no] & CELL_PAD_SETTING_SENSOR_ON;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_SENSOR_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_SENSOR_ON;
    if (was != (s_port_setting[port_no] & CELL_PAD_SETTING_SENSOR_ON))
        printf("[cellPad] SetSensorMode(port %u) -> %s\n", port_no, mode ? "on" : "off");
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

    const u8 small_on = param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL];
    const u8 large    = param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE];
    { static int log = -1; if (log < 0) log = getenv("PS3_RUMBLE_LOG") ? 1 : 0;
      if (log) fprintf(stderr, "[cellPad] SetActDirect port %u: small %u, large %u\n",
                       port_no, small_on, large); }
    if (port_no >= PAD_MAX_HOST_PORTS)
        return CELL_OK;

    /* A title may call this every frame with the same values: only a change
     * goes out, the poll renews what is already running. */
    u16 lo, hi;
    pad_rumble_levels(small_on, large, pad_rumble_strength(), &lo, &hi);
    PadRumble* r = &s_rumble[port_no];
    const int changed = r->lo != lo || r->hi != hi;
    r->lo = lo;
    r->hi = hi;

#if PAD_BACKEND_XINPUT
    /* XInput keeps the motors at what they were last set to: no renewal. */
    if (changed && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)lo;
        vib.wRightMotorSpeed = (WORD)hi;
        if (p_xi_set_state) p_xi_set_state((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (changed) {
        r->dirty = 1;                  /* a poll holding the lock sends it ... */
        if (pad_lock_try()) {          /* ... else it goes now */
            if (r->dirty) pad_sdl_rumble_send((int)port_no);
            pad_unlock();
        }
    }
#endif
    (void)changed;
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
