/* cellPad's DualSense extras: both triggers get a light resistance
 * (PS3_PAD_ADAPTIVE, strength 1-8, 0 = free) and a held trigger buzzes while
 * the title runs the motors; closing the controller frees both. The light
 * bar takes PS3_PAD_LIGHT and glows towards red with the motors; the gyro
 * (PS3_PAD_GYRO) adds to the right stick, always or only while L1/L2 is
 * held. A controller that is not a DualSense, or refuses effects, gets none
 * of it, and the press point of the triggers never moves.
 *
 *   clang -std=gnu17 -I include $(sdl2-config --cflags) -I "$(sdl2-config --prefix)/include" \
 *         libs/input/tests/test_pad_dualsense.c $(sdl2-config --libs) -o /tmp/test_pad_dualsense && \
 *   /tmp/test_pad_dualsense && /tmp/test_pad_dualsense plain && /tmp/test_pad_dualsense off
 *
 * No controller is opened: the test defines every SDL call the backend makes.
 * The settings are read once per process, so each mode is its own run.
 */
#define PAD_TEST_NO_LIBPROC 1
#include "../cellPad.c"
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* ---- the fake SDL: one controller ------------------------------------------ */
static struct {
    int ps5, refuse, closed;
    Sint16 lt, rt, rx, ry;
    uint32_t buttons;
    int effects;                 /* SendEffect calls */
    uint8_t last[47];            /* the last effect report */
    int leds; uint8_t led[3];
    int player;
    int sensor_on; float gyro[3];
} s_pad = { .ps5 = 1, .player = -1 };

int SDL_NumJoysticks(void) { return 1; }
SDL_bool SDL_IsGameController(int idx) { return idx == 0 ? SDL_TRUE : SDL_FALSE; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int idx) { return idx == 0 ? 1 : -1; }
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int idx) { SDL_JoystickGUID g; memset(&g, 0, sizeof g); g.data[0] = (Uint8)idx; return g; }
SDL_GameController* SDL_GameControllerOpen(int idx) { (void)idx; s_pad.closed = 0; return (SDL_GameController*)&s_pad; }
void SDL_GameControllerClose(SDL_GameController* gc) { (void)gc; s_pad.closed = 1; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc) { (void)gc; return SDL_TRUE; }
const char* SDL_GameControllerName(SDL_GameController* gc) { (void)gc; return "Fake DualSense"; }
SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController* gc)
{
    (void)gc; return s_pad.ps5 ? SDL_CONTROLLER_TYPE_PS5 : SDL_CONTROLLER_TYPE_XBOXONE;
}
SDL_bool SDL_GameControllerHasRumble(SDL_GameController* gc) { (void)gc; return SDL_TRUE; }
int SDL_GameControllerRumble(SDL_GameController* gc, Uint16 lo, Uint16 hi, Uint32 ms) { (void)gc; (void)lo; (void)hi; (void)ms; return 0; }
int SDL_GameControllerSendEffect(SDL_GameController* gc, const void* data, int size)
{
    (void)gc;
    assert(!s_pad.closed && size == 47);
    if (s_pad.refuse) return -1;
    memcpy(s_pad.last, data, 47);
    s_pad.effects++;
    return 0;
}
void SDL_GameControllerSetPlayerIndex(SDL_GameController* gc, int i) { (void)gc; s_pad.player = i; }
SDL_bool SDL_GameControllerHasLED(SDL_GameController* gc) { (void)gc; return s_pad.ps5 ? SDL_TRUE : SDL_FALSE; }
int SDL_GameControllerSetLED(SDL_GameController* gc, Uint8 r, Uint8 g, Uint8 b)
{
    (void)gc; s_pad.led[0] = r; s_pad.led[1] = g; s_pad.led[2] = b; s_pad.leds++; return 0;
}
SDL_bool SDL_GameControllerHasSensor(SDL_GameController* gc, SDL_SensorType t) { (void)gc; return s_pad.ps5 && t == SDL_SENSOR_GYRO ? SDL_TRUE : SDL_FALSE; }
int SDL_GameControllerSetSensorEnabled(SDL_GameController* gc, SDL_SensorType t, SDL_bool on) { (void)gc; (void)t; s_pad.sensor_on = on; return 0; }
int SDL_GameControllerGetSensorData(SDL_GameController* gc, SDL_SensorType t, float* d, int n)
{
    (void)gc; (void)t; assert(n == 3 && s_pad.sensor_on); memcpy(d, s_pad.gyro, sizeof s_pad.gyro); return 0;
}
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc, SDL_GameControllerButton b) { (void)gc; return (Uint8)((s_pad.buttons >> b) & 1u); }
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc, SDL_GameControllerAxis a)
{
    (void)gc;
    return a == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? s_pad.lt : a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? s_pad.rt
         : a == SDL_CONTROLLER_AXIS_RIGHTX ? s_pad.rx : a == SDL_CONTROLLER_AXIS_RIGHTY ? s_pad.ry : 0;
}
void SDL_GameControllerUpdate(void) {}
int SDL_PeepEvents(SDL_Event* ev, int n, SDL_eventaction a, Uint32 lo, Uint32 hi) { (void)ev; (void)n; (void)a; (void)lo; (void)hi; return 0; }
Uint32 SDL_WasInit(Uint32 flags) { return flags; }
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
const char* SDL_GetError(void) { return "fake"; }

static void poll_once(void)
{
    usleep((PAD_POLL_INTERVAL_MS + PAD_ADAPT_MIN_GAP_MS + 12) * 1000);   /* past the poll, effect and LED gaps */
    cellPad_poll();
}
static void rumble(int large)
{
    CellPadActParam* act = (CellPadActParam*)(vm_base + 0x3000);
    act->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = 0;
    act->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = (u8)large;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x3000) == CELL_OK);
}
#define RIGHT (s_pad.last + 10)
#define LEFT  (s_pad.last + 21)

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "on";
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);

    /* The settings' parsers. */
    assert(pad_adaptive_strength_from(NULL) == 3 && pad_adaptive_strength_from("") == 3 && pad_adaptive_strength_from("x") == 3);
    assert(pad_adaptive_strength_from("0") == 0 && pad_adaptive_strength_from("8") == 8 && pad_adaptive_strength_from("99") == 8 && pad_adaptive_strength_from("-2") == 0);
    uint8_t c[3];
    assert(pad_light_parse(NULL, c) == 0 && pad_light_parse("", c) == 0 && pad_light_parse("red", c) == 0 && pad_light_parse("12345", c) == 0);
    assert(pad_light_parse("off", c) == -1 && c[0] == 0 && c[1] == 0 && c[2] == 0);
    assert(pad_light_parse("#D6AA46", c) == 1 && c[0] == 0xD6 && c[1] == 0xAA && c[2] == 0x46);
    assert(pad_light_parse("c8102e", c) == 1 && c[0] == 0xC8 && c[1] == 0x10 && c[2] == 0x2E);

    if (!strcmp(mode, "plain")) {
        /* Not a DualSense: no effect, no light, no gyro; the triggers press as before. */
        s_pad.ps5 = 0;
        setenv("PS3_PAD_LIGHT", "D6AA46", 1);
        setenv("PS3_PAD_GYRO", "100", 1);
        assert(cellPadInit(4) == CELL_OK);
        s_pad.rt = 3001; s_pad.gyro[1] = 1.0f;
        poll_once();
        assert(s_pad.effects == 0 && s_pad.leds == 0 && !s_pad.sensor_on);
        assert(s_host_state[0].buttons & CELL_PAD_CTRL_R2);
        assert(s_host_state[0].analog_rx == 127 || s_host_state[0].analog_rx == 128);
        puts("test_pad_dualsense plain: OK");
        return 0;
    }
    if (!strcmp(mode, "off")) {
        /* PS3_PAD_ADAPTIVE=0: free triggers; PS3_PAD_SDL=0 in a second init is
         * a process setting, so only the adaptive switch is checked here. */
        setenv("PS3_PAD_ADAPTIVE", "0", 1);
        setenv("PS3_PAD_LIGHT", "off", 1);
        assert(cellPadInit(4) == CELL_OK);
        poll_once();
        assert(s_pad.effects == 0);
        assert(s_pad.leds == 1 && s_pad.led[0] == 0 && s_pad.led[1] == 0 && s_pad.led[2] == 0);   /* dark */
        rumble(255);
        poll_once();
        assert(s_pad.leds == 2 && s_pad.led[0] == 255);   /* a hit still glows from dark */
        puts("test_pad_dualsense off: OK");
        return 0;
    }

    setenv("PS3_PAD_ADAPTIVE", "4", 1);
    setenv("PS3_PAD_LIGHT", "#D6AA46", 1);
    setenv("PS3_PAD_GYRO", "100", 1);
    setenv("PS3_PAD_GYRO_MODE", "l1", 1);
    assert(cellPadInit(4) == CELL_OK);

    /* Opened: both triggers a feedback effect of strength 4 from zone 1, the
     * power-save bits cleared; player light 1; the port's colour; gyro on. */
    assert(s_pad.effects == 1 && s_pad.last[0] == 0x0C && s_pad.last[1] == 0x02);
    assert(RIGHT[0] == 0x21 && LEFT[0] == 0x21);
    assert(RIGHT[1] == 0xFE && RIGHT[2] == 0x03);                 /* zones 1-9 */
    assert((RIGHT[3] & 7) == 0 && ((RIGHT[3] >> 3) & 7) == 3);    /* zone 0 free, zone 1 strength 4 */
    assert(s_pad.player == 0 && s_pad.sensor_on);
    assert(s_pad.leds == 1 && s_pad.led[0] == 0xD6 && s_pad.led[1] == 0xAA && s_pad.led[2] == 0x46);

    /* Nothing changes: nothing is sent again. The press point is 3000. */
    s_pad.rt = 3000; poll_once();
    assert(s_pad.effects == 1 && s_pad.leds == 1 && !(s_host_state[0].buttons & CELL_PAD_CTRL_R2));
    s_pad.rt = 3001; poll_once();
    assert(s_pad.effects == 1 && (s_host_state[0].buttons & CELL_PAD_CTRL_R2));

    /* The motors run with the right trigger held: it buzzes, the left stays;
     * the light goes red. */
    rumble(255);
    poll_once();
    assert(s_pad.effects == 2 && RIGHT[0] == 0x26 && RIGHT[9] == 30 && LEFT[0] == 0x21);
    assert((RIGHT[3] & 7) == 7);                                  /* full motor: amplitude 8 */
    assert(s_pad.leds == 2 && s_pad.led[0] == 255 && s_pad.led[1] == 0 && s_pad.led[2] == 0);
    /* Released while they still run: resistance again. */
    s_pad.rt = 0; poll_once();
    assert(s_pad.effects == 3 && RIGHT[0] == 0x21);
    /* The left one held, a weaker motor: a weaker buzz there. */
    s_pad.lt = 20000; rumble(64); poll_once();
    assert(s_pad.effects == 4 && LEFT[0] == 0x26 && (LEFT[3] & 7) > 0 && (LEFT[3] & 7) < 7 && RIGHT[0] == 0x21);
    /* The motors stop: resistance and the port's colour again. */
    rumble(0); poll_once();
    assert(s_pad.effects == 5 && LEFT[0] == 0x21);
    assert(s_pad.led[0] == 0xD6 && s_pad.led[1] == 0xAA && s_pad.led[2] == 0x46);
    s_pad.lt = 0; poll_once();

    /* Gyro, gated by L1: nothing until it is held; then a turn to the left
     * (yaw > 0) pushes the stick left, a tilt back (pitch > 0) pushes it up. */
    s_pad.gyro[1] = 1.0f; poll_once();
    assert(s_host_state[0].analog_rx >= 127 && s_host_state[0].analog_rx <= 128);
    s_pad.buttons = 1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER; poll_once();
    assert(s_host_state[0].analog_rx < 80);
    s_pad.gyro[1] = -4.0f; poll_once();
    assert(s_host_state[0].analog_rx == 255);                     /* clamped */
    s_pad.gyro[1] = 0.0f; s_pad.gyro[0] = 1.0f; poll_once();
    assert(s_host_state[0].analog_ry < 80 && s_host_state[0].analog_rx >= 127 && s_host_state[0].analog_rx <= 128);
    s_pad.gyro[0] = 0.005f; poll_once();                          /* the resting jitter: nothing */
    assert(s_host_state[0].analog_ry >= 127 && s_host_state[0].analog_ry <= 128);
    s_pad.buttons = 0;

    /* The controller refuses a later effect: the old one stays recorded, and
     * it is tried again. */
    s_pad.refuse = 1; s_pad.rt = 30000; rumble(255); poll_once();
    assert(s_pad.effects == 5 && s_adapt[0].right == PAD_TRIG_FEEDBACK);
    s_pad.refuse = 0; poll_once();
    assert(s_pad.effects == 6 && RIGHT[0] == 0x26);

    /* Closing frees both triggers, before the handle is closed. */
    assert(cellPadEnd() == CELL_OK);
    assert(s_pad.closed && s_pad.effects == 7 && RIGHT[0] == 0x05 && LEFT[0] == 0x05);
    puts("test_pad_dualsense: OK");
    return 0;
}
