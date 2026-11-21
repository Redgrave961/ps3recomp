#ifndef PS3RECOMP_PAD_BINDINGS_H
#define PS3RECOMP_PAD_BINDINGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAD_INPUT_NONE 0
#define PAD_INPUT_MOUSE(button) (0x1000 + (button))
#define PAD_INPUT_WHEEL_UP 0x1100
#define PAD_INPUT_WHEEL_DOWN 0x1101
#define PAD_BINDING_SLOTS 2
#define PAD_CONTEXT_UI 1
#define PAD_CONTEXT_START 2

enum pad_action {
    PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3,
    PAD_START, PAD_SELECT,
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_MOVE_UP, PAD_MOVE_DOWN, PAD_MOVE_LEFT, PAD_MOVE_RIGHT,
    PAD_LOOK_UP, PAD_LOOK_DOWN, PAD_LOOK_LEFT, PAD_LOOK_RIGHT,
    PAD_MENU_CONFIRM, PAD_MENU_BACK, PAD_MENU_PREV, PAD_MENU_NEXT, PAD_MENU_EXTRA,
    PAD_ACTION_COUNT
};

int pad_kbm_last(void);
uint32_t pad_bindings_version(void);
int pad_binding(int action, int slot);
const char* pad_input_name(int input);

int pad_gamepad_style(void);
int pad_menu_active(void);
int pad_prompt_input(int action, int menu);

typedef struct pad_kbm_state {
    uint16_t buttons;
    uint8_t lx, ly, rx, ry;
} pad_kbm_state;

void pad_kbm_poll(int focused, pad_kbm_state* out);
void pad_gamepad_seen(int sdl_gamepad_type, int used);
void pad_set_menu_probe(int (*probe)(void));
void pad_set_layout_probe(int (*probe)(void));
enum pad_mouse_route { PAD_MOUSE_RIGHT_STICK, PAD_MOUSE_LEFT_STICK, PAD_MOUSE_CAMERA };
void pad_set_mouse_route_probe(int (*probe)(void));
void pad_binding_set(int action, int slot, int input);
void pad_bindings_reset(void);
void pad_capture(int action, int slot);
int pad_capturing(void);
int pad_last_swap(int* input);

#ifdef __cplusplus
}
#endif

#endif
