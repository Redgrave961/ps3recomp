#include "pad_bindings.h"
#include "cellPad.h"
#include "../../runtime/platform/ps3_sync.h"
#include "../video/vk/rsx_vk.h"
#include <SDL3/SDL.h>
#include <math.h>

#define PAD_MOUSE_BUTTONS 32
#define PAD_KEYS 512
#define PAD_INPUTS (PAD_KEYS + PAD_MOUSE_BUTTONS + 2)
#define PAD_WHEEL_PRESS_MS 50
#define PAD_WHEEL_PERIOD_MS 90
#define PAD_MOUSE_TRAVEL 8.0f
#define PAD_MOUSE_STICK_MS 120.0f
#define PAD_MOUSE_STICK_HOLD_MS 250
#define PAD_MOUSE_STICK_GAIN (1.0f / 30.0f)

#define PAD_DEFAULTS { \
    [PAD_CROSS] = { SDL_SCANCODE_SPACE }, \
    [PAD_CIRCLE] = { PAD_INPUT_MOUSE(SDL_BUTTON_RIGHT) }, \
    [PAD_SQUARE] = { PAD_INPUT_MOUSE(SDL_BUTTON_LEFT) }, \
    [PAD_TRIANGLE] = { SDL_SCANCODE_E }, \
    [PAD_L1] = { SDL_SCANCODE_Q, PAD_INPUT_MOUSE(SDL_BUTTON_MIDDLE) }, \
    [PAD_R1] = { SDL_SCANCODE_LSHIFT }, \
    [PAD_L2] = { SDL_SCANCODE_R }, \
    [PAD_R2] = { SDL_SCANCODE_F }, \
    [PAD_R3] = { SDL_SCANCODE_C }, \
    [PAD_START] = { SDL_SCANCODE_ESCAPE }, \
    [PAD_SELECT] = { SDL_SCANCODE_TAB }, \
    [PAD_UP] = { SDL_SCANCODE_UP, SDL_SCANCODE_W }, \
    [PAD_DOWN] = { SDL_SCANCODE_DOWN, SDL_SCANCODE_S }, \
    [PAD_LEFT] = { SDL_SCANCODE_LEFT, SDL_SCANCODE_A }, \
    [PAD_RIGHT] = { SDL_SCANCODE_RIGHT, SDL_SCANCODE_D }, \
    [PAD_MOVE_UP] = { SDL_SCANCODE_W }, \
    [PAD_MOVE_DOWN] = { SDL_SCANCODE_S }, \
    [PAD_MOVE_LEFT] = { SDL_SCANCODE_A }, \
    [PAD_MOVE_RIGHT] = { SDL_SCANCODE_D }, \
    [PAD_LOOK_LEFT] = { PAD_INPUT_WHEEL_DOWN }, \
    [PAD_LOOK_RIGHT] = { PAD_INPUT_WHEEL_UP }, \
    [PAD_MENU_CONFIRM] = { SDL_SCANCODE_RETURN }, \
    [PAD_MENU_BACK] = { SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE }, \
    [PAD_MENU_PREV] = { SDL_SCANCODE_Q }, \
    [PAD_MENU_NEXT] = { SDL_SCANCODE_E }, \
    [PAD_MENU_EXTRA] = { SDL_SCANCODE_R }, \
}

static const int16_t k_defaults[PAD_ACTION_COUNT][PAD_BINDING_SLOTS] = PAD_DEFAULTS;
static int16_t s_bind[PAD_ACTION_COUNT][PAD_BINDING_SLOTS] = PAD_DEFAULTS;

static const uint16_t k_buttons[PAD_ACTION_COUNT] = {
    [PAD_CROSS] = CELL_PAD_CTRL_CROSS, [PAD_CIRCLE] = CELL_PAD_CTRL_CIRCLE,
    [PAD_SQUARE] = CELL_PAD_CTRL_SQUARE, [PAD_TRIANGLE] = CELL_PAD_CTRL_TRIANGLE,
    [PAD_L1] = CELL_PAD_CTRL_L1, [PAD_R1] = CELL_PAD_CTRL_R1, [PAD_L2] = CELL_PAD_CTRL_L2, [PAD_R2] = CELL_PAD_CTRL_R2,
    [PAD_L3] = CELL_PAD_CTRL_L3, [PAD_R3] = CELL_PAD_CTRL_R3, [PAD_START] = CELL_PAD_CTRL_START, [PAD_SELECT] = CELL_PAD_CTRL_SELECT,
    [PAD_UP] = CELL_PAD_CTRL_UP, [PAD_DOWN] = CELL_PAD_CTRL_DOWN, [PAD_LEFT] = CELL_PAD_CTRL_LEFT, [PAD_RIGHT] = CELL_PAD_CTRL_RIGHT,
    [PAD_MENU_CONFIRM] = CELL_PAD_CTRL_CROSS, [PAD_MENU_BACK] = CELL_PAD_CTRL_CIRCLE,
    [PAD_MENU_PREV] = CELL_PAD_CTRL_L1, [PAD_MENU_NEXT] = CELL_PAD_CTRL_R1, [PAD_MENU_EXTRA] = CELL_PAD_CTRL_TRIANGLE,
};

static const uint16_t k_face[3][PAD_TRIANGLE + 1] = {
    { CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_SQUARE, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_TRIANGLE },
    { CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_SQUARE, CELL_PAD_CTRL_TRIANGLE },
    { CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_SQUARE, CELL_PAD_CTRL_TRIANGLE },
};

typedef struct pad_wheel {
    int pending;
    uint64_t release, next;
} pad_wheel;

static ps3_lock s_lock;
static uint32_t s_version;
static int s_kbm_last;
static float s_travel;
static int s_style;
static int s_capture = -1;
static int s_capture_slot;
static int s_armed;
static int s_hold;
static int s_swap_action = -1;
static int s_swap_input;
static pad_wheel s_wheel[2];
static int (*s_menu_probe)(void);
static int (*s_layout_probe)(void);
static int s_layout;
static uint8_t s_down[PAD_INPUTS];
static uint8_t s_menu_press[PAD_INPUTS];
static uint8_t s_press_in_menu[PAD_INPUTS];
static uint32_t s_seen[PAD_INPUTS];
static uint32_t s_polls;
static int (*s_route_probe)(void);

int pad_kbm_last(void)
{
    return __atomic_load_n(&s_kbm_last, __ATOMIC_RELAXED);
}

uint32_t pad_bindings_version(void)
{
    return __atomic_load_n(&s_version, __ATOMIC_ACQUIRE);
}

int pad_binding(int action, int slot)
{
    if (action < 0 || action >= PAD_ACTION_COUNT || slot < 0 || slot >= PAD_BINDING_SLOTS) return PAD_INPUT_NONE;
    return __atomic_load_n(&s_bind[action][slot], __ATOMIC_RELAXED);
}

const char* pad_input_name(int input)
{
    static const char* const mouse[] = { "Left Mouse", "Middle Mouse", "Right Mouse", "Mouse 4", "Mouse 5", "Mouse 6", "Mouse 7", "Mouse 8" };
    switch (input) {
    case PAD_INPUT_NONE: return "";
    case PAD_INPUT_WHEEL_UP: return "Wheel Up";
    case PAD_INPUT_WHEEL_DOWN: return "Wheel Down";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_ESCAPE: return "Esc";
    case SDL_SCANCODE_LSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: return "Ctrl";
    case SDL_SCANCODE_LALT: return "Alt";
    default: break;
    }
    if (input > PAD_INPUT_MOUSE(0))
        return input - PAD_INPUT_MOUSE(1) < (int)SDL_arraysize(mouse) ? mouse[input - PAD_INPUT_MOUSE(1)] : "Mouse";
    return SDL_GetScancodeName((SDL_Scancode)input);
}

int pad_gamepad_style(void)
{
    return __atomic_load_n(&s_style, __ATOMIC_RELAXED);
}

void pad_gamepad_seen(int sdl_gamepad_type, int used)
{
    int style = 0;
    switch (sdl_gamepad_type) {
    case SDL_GAMEPAD_TYPE_STANDARD:
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
    case SDL_GAMEPAD_TYPE_STEAM:
        style = 1;
        break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        style = 2;
        break;
    default:
        break;
    }
    __atomic_store_n(&s_style, style, __ATOMIC_RELAXED);
    if (used) __atomic_store_n(&s_kbm_last, 0, __ATOMIC_RELAXED);
}

void pad_set_menu_probe(int (*probe)(void))
{
    __atomic_store_n(&s_menu_probe, probe, __ATOMIC_RELEASE);
}

int pad_menu_active(void)
{
    int (*probe)(void) = __atomic_load_n(&s_menu_probe, __ATOMIC_ACQUIRE);
    return probe ? probe() : 0;
}

void pad_set_layout_probe(int (*probe)(void))
{
    __atomic_store_n(&s_layout_probe, probe, __ATOMIC_RELEASE);
}

void pad_set_mouse_route_probe(int (*probe)(void))
{
    __atomic_store_n(&s_route_probe, probe, __ATOMIC_RELEASE);
}

static uint16_t pad_face(int action)
{
    return action <= PAD_TRIANGLE ? k_face[__atomic_load_n(&s_layout, __ATOMIC_RELAXED)][action] : k_buttons[action];
}

static int pad_menu_layer(int action)
{
    return action >= PAD_MENU_CONFIRM || (action >= PAD_UP && action <= PAD_RIGHT);
}

static int pad_first_binding(int action)
{
    for (int s = 0; s < PAD_BINDING_SLOTS; s++)
        if (pad_binding(action, s)) return pad_binding(action, s);
    return PAD_INPUT_NONE;
}

int pad_prompt_input(int action, int menu)
{
    for (int a = PAD_MENU_CONFIRM; menu && a < PAD_ACTION_COUNT; a++)
        if (k_buttons[a] == k_buttons[action] && pad_first_binding(a)) return pad_first_binding(a);
    for (int a = PAD_CROSS; !menu && action <= PAD_TRIANGLE && a <= PAD_TRIANGLE; a++)
        if (pad_face(a) == k_buttons[action]) return pad_first_binding(a);
    return pad_first_binding(action);
}

static int pad_input_valid(int input)
{
    return (input >= 0 && input < PAD_KEYS) || (input > PAD_INPUT_MOUSE(0) && input <= PAD_INPUT_MOUSE(PAD_MOUSE_BUTTONS))
        || input == PAD_INPUT_WHEEL_UP || input == PAD_INPUT_WHEEL_DOWN;
}

static void pad_store(int action, int slot, int input)
{
    __atomic_store_n(&s_bind[action][slot], (int16_t)input, __ATOMIC_RELAXED);
}

void pad_binding_set(int action, int slot, int input)
{
    if (action < 0 || action >= PAD_ACTION_COUNT || slot < 0 || slot >= PAD_BINDING_SLOTS || !pad_input_valid(input)) return;
    ps3_lock_acquire(&s_lock);
    pad_store(action, slot, input);
    ps3_lock_release(&s_lock);
    __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
}

void pad_bindings_reset(void)
{
    ps3_lock_acquire(&s_lock);
    for (int a = 0; a < PAD_ACTION_COUNT; a++)
        for (int s = 0; s < PAD_BINDING_SLOTS; s++) pad_store(a, s, k_defaults[a][s]);
    ps3_lock_release(&s_lock);
    __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
}

static void pad_bind(int action, int slot, int input)
{
    ps3_lock_acquire(&s_lock);
    const int old = s_bind[action][slot];
    for (int a = 0; a < PAD_ACTION_COUNT; a++)
        for (int s = 0; s < PAD_BINDING_SLOTS; s++)
            if (input && s_bind[a][s] == input && (a != action || s != slot) && pad_menu_layer(a) == pad_menu_layer(action)) {
                pad_store(a, s, old);
                if (a != action) {
                    __atomic_store_n(&s_swap_input, old, __ATOMIC_RELAXED);
                    __atomic_store_n(&s_swap_action, a, __ATOMIC_RELEASE);
                }
            }
    pad_store(action, slot, input);
    ps3_lock_release(&s_lock);
    __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
}

void pad_capture(int action, int slot)
{
    __atomic_store_n(&s_swap_action, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_armed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_capture_slot, slot >= 0 && slot < PAD_BINDING_SLOTS ? slot : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_capture, action >= 0 && action < PAD_ACTION_COUNT ? action : -1, __ATOMIC_RELEASE);
}

int pad_capturing(void)
{
    return __atomic_load_n(&s_capture, __ATOMIC_ACQUIRE) >= 0;
}

int pad_last_swap(int* input)
{
    const int action = __atomic_load_n(&s_swap_action, __ATOMIC_ACQUIRE);
    *input = __atomic_load_n(&s_swap_input, __ATOMIC_RELAXED);
    return action;
}

static int pad_first_down(const bool* keys, int numkeys, uint32_t mouse)
{
    for (int k = 1; k < numkeys && k < PAD_KEYS; k++)
        if (keys[k]) return k;
    for (int b = 1; b <= PAD_MOUSE_BUTTONS; b++)
        if (mouse & SDL_BUTTON_MASK(b)) return PAD_INPUT_MOUSE(b);
    return PAD_INPUT_NONE;
}

static void pad_capture_step(int action, int input)
{
    if (!__atomic_load_n(&s_armed, __ATOMIC_RELAXED)) {
        if (input == PAD_INPUT_NONE) __atomic_store_n(&s_armed, 1, __ATOMIC_RELAXED);
        return;
    }
    if (input == PAD_INPUT_NONE || !__atomic_compare_exchange_n(&s_capture, &action, -1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    if (input != SDL_SCANCODE_ESCAPE)
        pad_bind(action, __atomic_load_n(&s_capture_slot, __ATOMIC_RELAXED), input == SDL_SCANCODE_DELETE ? PAD_INPUT_NONE : input);
    s_hold = 1;
}

static int pad_wheel_down(pad_wheel* w, uint64_t now)
{
    if (w->pending && now >= w->next) {
        w->pending--;
        w->release = now + PAD_WHEEL_PRESS_MS;
        w->next = now + PAD_WHEEL_PERIOD_MS;
    }
    return now < w->release;
}

static int pad_menu_bound(int input)
{
    for (int a = 0; a < PAD_ACTION_COUNT; a++)
        for (int s = 0; s < PAD_BINDING_SLOTS; s++)
            if (pad_menu_layer(a) && __atomic_load_n(&s_bind[a][s], __ATOMIC_RELAXED) == input) return 1;
    return 0;
}

static int pad_input_layer(int input, const bool* keys, int numkeys, uint32_t mouse, const int wheel[2], int menu)
{
    if (input == PAD_INPUT_NONE) return 0;
    const int index = input >= PAD_INPUT_WHEEL_UP ? PAD_KEYS + PAD_MOUSE_BUTTONS + input - PAD_INPUT_WHEEL_UP
                    : input > PAD_INPUT_MOUSE(0) ? PAD_KEYS + input - PAD_INPUT_MOUSE(1) : input;
    if (s_seen[index] != s_polls) {
        s_seen[index] = s_polls;
        const int down = input >= PAD_INPUT_WHEEL_UP ? wheel[input - PAD_INPUT_WHEEL_UP]
                       : input > PAD_INPUT_MOUSE(0) ? (mouse & SDL_BUTTON_MASK(input - PAD_INPUT_MOUSE(0))) != 0
                       : input < numkeys && keys[input];
        if (down && !s_down[index]) {
            s_menu_press[index] = menu && pad_menu_bound(input);
            s_press_in_menu[index] = menu != 0;
        }
        s_down[index] = (uint8_t)down;
    }
    return !s_down[index] ? 0 : s_menu_press[index] ? 2 : 1 + 2 * s_press_in_menu[index];
}

static uint8_t pad_axis(int negative, int positive)
{
    return negative == positive ? 128 : negative ? 0 : 255;
}

void pad_kbm_poll(int focused, pad_kbm_state* out)
{
    int notches;
    float travel;
    const uint32_t mouse = rsx_vk_mouse_input(&notches, &travel);
    out->buttons = 0;
    out->lx = out->ly = out->rx = out->ry = 128;
    if (!focused) {
        s_wheel[0].pending = s_wheel[1].pending = 0;
        return;
    }
    s_travel = __atomic_load_n(&s_kbm_last, __ATOMIC_RELAXED) ? 0.f : s_travel + travel;
    if (s_travel > PAD_MOUSE_TRAVEL) __atomic_store_n(&s_kbm_last, 1, __ATOMIC_RELAXED);
    s_wheel[notches < 0].pending += notches < 0 ? -notches : notches;
    int numkeys = 0;
    const bool* keys = SDL_GetKeyboardState(&numkeys);
    const int pressed = pad_first_down(keys, numkeys, mouse);
    const int capture = __atomic_load_n(&s_capture, __ATOMIC_ACQUIRE);
    if (capture >= 0 || s_hold) {
        const int wheel = s_wheel[0].pending ? PAD_INPUT_WHEEL_UP : s_wheel[1].pending ? PAD_INPUT_WHEEL_DOWN : PAD_INPUT_NONE;
        s_wheel[0].pending = s_wheel[1].pending = 0;
        if (capture >= 0) pad_capture_step(capture, pressed ? pressed : wheel);
        else s_hold = pressed != PAD_INPUT_NONE;
        return;
    }
    const uint64_t now = SDL_GetTicks();
    const int wheel[2] = { pad_wheel_down(&s_wheel[0], now), pad_wheel_down(&s_wheel[1], now) };
    const int menu = pad_menu_active();
    int (*layout_probe)(void) = __atomic_load_n(&s_layout_probe, __ATOMIC_ACQUIRE);
    const int layout = layout_probe ? layout_probe() : 0;
    if (layout != s_layout) {
        __atomic_store_n(&s_layout, layout, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
    }
    int down[PAD_ACTION_COUNT] = { 0 }, game[PAD_ACTION_COUNT] = { 0 };
    s_polls++;
    for (int a = 0; a < PAD_ACTION_COUNT; a++)
        for (int s = 0; s < PAD_BINDING_SLOTS; s++) {
            const int layer = pad_input_layer(__atomic_load_n(&s_bind[a][s], __ATOMIC_RELAXED), keys, numkeys, mouse, wheel, menu);
            down[a] |= pad_menu_layer(a) ? layer == 2 : layer == 1 || layer == 3;
            game[a] |= layer == 1;
        }
    for (int a = 0; a < PAD_ACTION_COUNT; a++)
        if (down[a]) out->buttons |= game[a] ? pad_face(a) : a == PAD_MENU_CONFIRM && menu == PAD_CONTEXT_START ? CELL_PAD_CTRL_START : k_buttons[a];
    out->lx = pad_axis(down[PAD_MOVE_LEFT], down[PAD_MOVE_RIGHT]);
    out->ly = pad_axis(down[PAD_MOVE_UP], down[PAD_MOVE_DOWN]);
    out->rx = pad_axis(down[PAD_LOOK_LEFT], down[PAD_LOOK_RIGHT]);
    out->ry = pad_axis(down[PAD_LOOK_UP], down[PAD_LOOK_DOWN]);
    int (*route_probe)(void) = __atomic_load_n(&s_route_probe, __ATOMIC_ACQUIRE);
    const int route = route_probe ? route_probe() : PAD_MOUSE_RIGHT_STICK;
    if (!menu && route != PAD_MOUSE_CAMERA) {
        static float vx, vy;
        static uint64_t last, moved;
        float dx, dy;
        rsx_vk_mouse_motion(&dx, &dy);
        if (dx != 0.f || dy != 0.f) moved = now;
        const float decay = moved != now && now - moved < PAD_MOUSE_STICK_HOLD_MS ? 1.f : expf(-(float)(now - last) / PAD_MOUSE_STICK_MS);
        last = now;
        vx = vx * decay + dx * PAD_MOUSE_STICK_GAIN;
        vy = vy * decay + dy * PAD_MOUSE_STICK_GAIN;
        const float m = sqrtf(vx * vx + vy * vy);
        if (m > 1.f) {
            vx /= m;
            vy /= m;
        }
        const int left = route == PAD_MOUSE_LEFT_STICK;
        uint8_t* x = left ? &out->lx : &out->rx;
        uint8_t* y = left ? &out->ly : &out->ry;
        if (*x == 128 && *y == 128 && m > 0.05f) {
            *x = (uint8_t)lrintf(128.f + vx * 127.f);
            *y = (uint8_t)lrintf(128.f + vy * 127.f);
        }
    }
    static pad_kbm_state prev = { 0, 128, 128, 128, 128 };
    if ((out->buttons & ~prev.buttons) || (out->lx != prev.lx && out->lx != 128) || (out->ly != prev.ly && out->ly != 128)
        || (out->rx != prev.rx && out->rx != 128) || (out->ry != prev.ry && out->ry != 128))
        __atomic_store_n(&s_kbm_last, 1, __ATOMIC_RELAXED);
    prev = *out;
}
