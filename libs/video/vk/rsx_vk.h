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

typedef struct rsx_vk_texture_patch {
    u32 (*variant)(void);
    int (*apply)(u32 variant, u32 gcm_format, u32 width, u32 height, const uint8_t* guest, uint64_t guest_size,
                 uint8_t* staging);
} rsx_vk_texture_patch;

const rsx_renderer* rsx_vk_renderer(const char* title, const char* cache_path, const char* icon_path);
void rsx_vk_set_texture_patch(const rsx_vk_texture_patch* patch);
void rsx_vk_set_resolution_scale(u32 num, u32 den);
void rsx_vk_set_display(enum rsx_vk_display display);
void rsx_vk_on_display_toggle(void (*listener)(enum rsx_vk_display display));
void rsx_vk_set_frame_rate(int free_flips, u32 hz);
void rsx_vk_mouse_motion(float* dx, float* dy);
u32 rsx_vk_mouse_input(int* wheel, float* travel);
u32 rsx_vk_display_modes(rsx_vk_mode* out, u32 capacity);
void rsx_vk_on_display_modes(void (*listener)(void));

#ifdef __cplusplus
}
#endif

#endif
