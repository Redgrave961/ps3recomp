#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/platform/ps3_sync.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "ps3emu/log_first.h"
#include "../video/rsx_core.h"

static double pad_clock(unsigned long long t0_ms)
{
    static int flips = -1;
    if (flips < 0) { const char* e = getenv("PAD_CLOCK"); flips = e && !strcmp(e, "flips"); }
    return flips ? (double)rsx_core_frames_presented()
                 : (double)(GetTickCount64() - t0_ms) / 1000.0;
}

#if defined(PS3RECOMP_PAD_USE_SDL)
  #define PAD_BACKEND_SDL  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT || defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
#endif

#if PAD_BACKEND_SDL
  #include <SDL3/SDL.h>
#endif

#define PAD_MAX_HOST_PORTS  4

typedef struct {
    int  connected;
    u16  buttons;
    u8   analog_lx;
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;
    u8   trigger_r2;
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
    u16  sensor[4];
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
static int s_data_fresh[PAD_MAX_HOST_PORTS];
static int s_xbox_layout;

int cellPad_xbox_layout(void)
{
    return __atomic_load_n(&s_xbox_layout, __ATOMIC_RELAXED);
}

#if PAD_BACKEND_SDL
static SDL_Gamepad*        s_sdl_controllers[PAD_MAX_HOST_PORTS];
static SDL_JoystickID      s_sdl_ids[PAD_MAX_HOST_PORTS];
static SDL_GUID            s_sdl_last_guid[PAD_MAX_HOST_PORTS];
static int                 s_sdl_had[PAD_MAX_HOST_PORTS];
static unsigned long long  s_sdl_scan_ms;
static int s_sdl_inited = 0;
#endif

static void pad_host_release(PadHostState* hs)
{
    static const u16 sensor_rest[4] = { 512, 399, 512, 512 };
    memset(hs, 0, sizeof(*hs));
    hs->analog_lx = hs->analog_ly = 128;
    hs->analog_rx = hs->analog_ry = 128;
    memcpy(hs->sensor, sensor_rest, sizeof hs->sensor);
}

static ps3_lock s_poll_lock;
static int  pad_lock_try(void)
{
    uint32_t z = 0;
    return __atomic_compare_exchange_n(&s_poll_lock.v, &z, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void pad_lock(void)     { ps3_lock_acquire(&s_poll_lock); }
static void pad_unlock(void)   { ps3_lock_release(&s_poll_lock); }

static int s_port_reported[PAD_MAX_HOST_PORTS];
static int s_port_changed[PAD_MAX_HOST_PORTS];
static void pad_track_assign(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        const int on = s_host_state[i].connected || i == 0;
        if (on != s_port_reported[i]) { s_port_reported[i] = on; s_port_changed[i] = 1; }
    }
}
static u32 pad_status_take(u32 port)
{
    if (port >= PAD_MAX_HOST_PORTS) return 0;
    const int ch = __atomic_exchange_n(&s_port_changed[port], 0, __ATOMIC_RELAXED);
    return ch ? CELL_PAD_STATUS_ASSIGN_CHANGES : 0;
}

#define PAD_RUMBLE_HOLD_MS     1000
#define PAD_RUMBLE_RENEW_MS     400

typedef struct {
    u16 lo, hi;
    unsigned long long sent_ms;
} PadRumble;
static PadRumble s_rumble[PAD_MAX_HOST_PORTS];

static unsigned long long pad_now_ms(void);

static void pad_rumble_levels(u8 small_on, u8 large, u16* lo, u16* hi)
{
    *lo = (u16)(large * 257u);
    *hi = small_on ? 0xFFFFu : 0u;
}

#if defined(_WIN32)
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
#elif defined(__APPLE__)
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
#define PADKEY_UP 126
#define PADKEY_DOWN 125
#define PADKEY_LEFT 123
#define PADKEY_RIGHT 124
#define PADKEY_Z 6
#define PADKEY_X 7
#define PADKEY_A 0
#define PADKEY_S 1
#define PADKEY_Q 12
#define PADKEY_W 13
#define PADKEY_1 18
#define PADKEY_2 19
#define PADKEY_RETURN 36
#define PADKEY_TAB 48
#elif PAD_BACKEND_SDL
static int pad_host_window_focused(void)
{
    return SDL_GetKeyboardFocus() != NULL;
}
static int pad_key_down(int key)
{
    return SDL_GetKeyboardState(NULL)[SDL_GetScancodeFromKey((SDL_Keycode)key, NULL)];
}
#define PADKEY_UP SDLK_UP
#define PADKEY_DOWN SDLK_DOWN
#define PADKEY_LEFT SDLK_LEFT
#define PADKEY_RIGHT SDLK_RIGHT
#define PADKEY_Z SDLK_Z
#define PADKEY_X SDLK_X
#define PADKEY_A SDLK_A
#define PADKEY_S SDLK_S
#define PADKEY_Q SDLK_Q
#define PADKEY_W SDLK_W
#define PADKEY_1 SDLK_1
#define PADKEY_2 SDLK_2
#define PADKEY_RETURN SDLK_RETURN
#define PADKEY_TAB SDLK_TAB
#endif

static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
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

#if PAD_BACKEND_XINPUT

#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

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

    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

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

        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

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
    pad_xinput_load();
}

#endif

#if PAD_BACKEND_SDL

static u8 pad_sdl_axis_to_u8(int raw)
{
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u16 pad_sensor_word(float v)
{
    return (u16)fminf(fmaxf(v, 0.0f), 1023.0f);
}

static void pad_sdl_sensors(SDL_Gamepad* gp, u16* s)
{
    float a[3], g[3];
    if (SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, a, 3))
        for (int k = 0; k < 3; k++) s[k] = pad_sensor_word(a[k] / SDL_STANDARD_GRAVITY * -113.0f + 512.0f);
    if (SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, g, 3))
        s[3] = pad_sensor_word(g[1] * (180.0f / SDL_PI_F) * (123.0f / 90.0f) + 512.0f);
}

static void pad_sdl_rumble_send(int i)
{
    SDL_Gamepad* gp = s_sdl_controllers[i];
    if (!gp) return;
    PadRumble* r = &s_rumble[i];
    const int on = r->lo || r->hi;
    SDL_RumbleGamepad(gp, r->lo, r->hi, on ? PAD_RUMBLE_HOLD_MS : 0);
    r->sent_ms = on ? pad_now_ms() : 0;
}

#define PAD_SDL_RESCAN_MS 1000

static int pad_sdl_port_of(SDL_JoystickID id)
{
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p] && s_sdl_ids[p] == id) return p;
    return -1;
}

static void pad_sdl_close(int p, int gone)
{
    SDL_Gamepad* gp = s_sdl_controllers[p];
    if (!gp) return;
    if (gone) fprintf(stderr, "[cellPad] SDL gamepad on port %d disconnected (instance %u)\n", p, (unsigned)s_sdl_ids[p]);
    else      SDL_RumbleGamepad(gp, 0, 0, 0);
    SDL_CloseGamepad(gp);
    s_sdl_controllers[p] = NULL;
    s_sdl_ids[p] = 0;
    memset(&s_rumble[p], 0, sizeof s_rumble[p]);
    pad_host_release(&s_host_state[p]);
}

static void pad_sdl_use_layout(SDL_Gamepad* gp)
{
    const SDL_GamepadType t = SDL_GetGamepadType(gp);
    const int xbox = t != SDL_GAMEPAD_TYPE_PS3 && t != SDL_GAMEPAD_TYPE_PS4 && t != SDL_GAMEPAD_TYPE_PS5;
    __atomic_store_n(&s_xbox_layout, xbox, __ATOMIC_RELAXED);
}

static void pad_sdl_scan(void)
{
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int k = 0; k < n; k++) {
        const SDL_JoystickID id = ids[k];
        if (!id || pad_sdl_port_of(id) >= 0) continue;
        const SDL_GUID guid = SDL_GetGamepadGUIDForID(id);
        int port = -1, back = 0;
        for (int p = 0; p < PAD_MAX_HOST_PORTS && port < 0; p++)
            if (!s_sdl_controllers[p] && s_sdl_had[p] && !memcmp(&s_sdl_last_guid[p], &guid, sizeof guid)) { port = p; back = 1; }
        for (int p = 0; p < PAD_MAX_HOST_PORTS && port < 0; p++)
            if (!s_sdl_controllers[p]) port = p;
        if (port < 0) break;
        SDL_Gamepad* gp = SDL_OpenGamepad(id);
        if (!gp) {
            fprintf(stderr, "[cellPad] SDL gamepad %u: open failed: %s\n", (unsigned)id, SDL_GetError());
            continue;
        }
        s_sdl_controllers[port] = gp;
        s_sdl_ids[port] = id;
        s_sdl_last_guid[port] = guid;
        s_sdl_had[port] = 1;
        memset(&s_rumble[port], 0, sizeof s_rumble[port]);
        SDL_RumbleGamepad(gp, 0, 0, 0);
        SDL_SetGamepadLED(gp, 0xFF, 0x33, 0x00);
        pad_sdl_use_layout(gp);
        SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, true);
        SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true);
        const char* name = SDL_GetGamepadName(gp);
        const bool rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gp),
                                                   SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        fprintf(stderr, "[cellPad] SDL gamepad %s on port %d: %s (instance %u)%s\n",
                back ? "reconnected" : "connected", port, name ? name : "?", (unsigned)id,
                rumble ? "" : " (no rumble)");
    }
    SDL_free(ids);
}

static void pad_sdl_hotplug(int force)
{
    int changed = force;
    SDL_Event ev[32];
    int got;
    while ((got = SDL_PeepEvents(ev, 32, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION,
                                 SDL_EVENT_JOYSTICK_AXIS_MOTION + 0xFF)) > 0)
        for (int i = 0; i < got; i++)
            if (ev[i].type == SDL_EVENT_JOYSTICK_ADDED || ev[i].type == SDL_EVENT_JOYSTICK_REMOVED ||
                ev[i].type == SDL_EVENT_GAMEPAD_ADDED || ev[i].type == SDL_EVENT_GAMEPAD_REMOVED)
                changed = 1;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p] && !SDL_GamepadConnected(s_sdl_controllers[p])) {
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

static void pad_poll_sdl(void)
{
    SDL_UpdateGamepads();
    pad_sdl_hotplug(0);
    static int was_focused = 1;
    const int focused = pad_host_window_focused(), lost = was_focused && !focused;
    was_focused = focused;

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        SDL_Gamepad* gp = s_sdl_controllers[i];
        if (!gp || !focused) {
            if (gp && lost) SDL_RumbleGamepad(gp, 0, 0, 0);
            pad_host_release(&s_host_state[i]);
            memset(&s_rumble[i], 0, sizeof s_rumble[i]);
            continue;
        }

        s_host_state[i].connected = 1;

        if (s_rumble[i].sent_ms && pad_now_ms() - s_rumble[i].sent_ms >= PAD_RUMBLE_RENEW_MS)
            pad_sdl_rumble_send(i);

        u16 btns = 0;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK))           btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START))          btns |= CELL_PAD_CTRL_START;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP))        btns |= CELL_PAD_CTRL_UP;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH))          btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))           btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH))          btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST))           btns |= CELL_PAD_CTRL_SQUARE;

        int lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        if (btns & ~s_host_state[i].buttons) pad_sdl_use_layout(gp);
        s_host_state[i].buttons = btns;

        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));

        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

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

        if (s_port_setting[i] & CELL_PAD_SETTING_SENSOR_ON)
            pad_sdl_sensors(gp, s_host_state[i].sensor);
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMEPAD) == 0) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) s_sdl_ids[p] = 0;

    pad_sdl_hotplug(1);
}

#endif

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

static void pad_poll_backend(void)
{
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    if (!pad_lock_try()) return;
    s_last_ms = now;
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL
    pad_poll_sdl();
#endif
    if (!s_host_state[0].connected) pad_poll_keyboard();
    pad_track_assign();
    pad_unlock();
}

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        pad_host_release(&s_host_state[i]);

    pad_lock();
    pad_init_backend();
    pad_unlock();

    pad_poll_backend();
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_port_reported[i] = s_host_state[i].connected || i == 0;
        s_port_changed[i] = 0;
    }

    return CELL_OK;
}

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    pad_poll_backend();

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;
    }

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    {
        if (port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;
            else                        s_data_fresh[port_no] = 0;
        }
    }
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;
      } }

    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[256]; static int n_ev = 0;
      static unsigned long long t0 = 0;
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

    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
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

    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        memcpy(&data->button[CELL_PAD_BTN_OFFSET_SENSOR_X], hs->sensor, sizeof hs->sensor);
    }

emit:
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
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

    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

    if (port_no >= PAD_MAX_HOST_PORTS)
        return CELL_OK;

    u16 lo, hi;
    pad_rumble_levels(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL],
                      param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE], &lo, &hi);
    s_rumble[port_no].lo = lo;
    s_rumble[port_no].hi = hi;

#if PAD_BACKEND_XINPUT
    if (s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)lo;
        vib.wRightMotorSpeed = (WORD)hi;
        if (p_xi_set_state) p_xi_set_state((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL
    pad_lock();
    pad_sdl_rumble_send((int)port_no);
    pad_unlock();
#endif

    return CELL_OK;
}
