#ifndef PS3RECOMP_RSX_VK_H
#define PS3RECOMP_RSX_VK_H

#include "../rsx_core.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rsx_vk_display { RSX_VK_WINDOWED, RSX_VK_BORDERLESS, RSX_VK_FULLSCREEN };

typedef struct rsx_vk_mode {
    u32 width;
    u32 height;
    float refresh;
} rsx_vk_mode;

const rsx_renderer* rsx_vk_renderer(const char* title);
void rsx_vk_set_resolution_scale(u32 num, u32 den);
void rsx_vk_set_display(enum rsx_vk_display display);
void rsx_vk_on_display_toggle(void (*listener)(enum rsx_vk_display display));
void rsx_vk_set_vsync(int vsync);
u32 rsx_vk_display_modes(rsx_vk_mode* out, u32 capacity);
void rsx_vk_on_display_modes(void (*listener)(void));

#ifdef __cplusplus
}
#endif

#endif
