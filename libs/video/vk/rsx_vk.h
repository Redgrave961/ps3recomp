#ifndef PS3RECOMP_RSX_VK_H
#define PS3RECOMP_RSX_VK_H

#include "../rsx_core.h"

#ifdef __cplusplus
extern "C" {
#endif

enum rsx_vk_display { RSX_VK_WINDOWED, RSX_VK_BORDERLESS, RSX_VK_FULLSCREEN };
enum rsx_vk_aa { RSX_VK_AA_OFF, RSX_VK_AA_SMAA };

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
void rsx_vk_set_resolution_scale(u32 num_x, u32 den_x, u32 num_y, u32 den_y);
void rsx_vk_set_aa(enum rsx_vk_aa aa);
void rsx_vk_set_display(enum rsx_vk_display display);
void rsx_vk_on_display_toggle(void (*listener)(enum rsx_vk_display display));
void rsx_vk_set_frame_rate(int free_flips, u32 hz);
void rsx_vk_mouse_motion(float* dx, float* dy);
u32 rsx_vk_mouse_input(int* wheel, float* travel);
u32 rsx_vk_display_modes(rsx_vk_mode* out, u32 capacity);
int rsx_vk_desktop_mode(rsx_vk_mode* out);
void rsx_vk_on_display_modes(void (*listener)(void));
void rsx_vk_window_size(u32* w, u32* h);
void rsx_vk_on_window_resize(void (*listener)(u32 w, u32 h));
void rsx_vk_resize_window(u32 w, u32 h);

enum rsx_vk_upscale {
    RSX_VK_UPSCALE_OFF = 0,
    RSX_VK_UPSCALE_FSR_NATIVE = 1,
    RSX_VK_UPSCALE_FSR_QUALITY = 2,
    RSX_VK_UPSCALE_FSR_BALANCED = 3,
    RSX_VK_UPSCALE_FSR_PERFORMANCE = 4,
    RSX_VK_UPSCALE_DLAA = 5,
    RSX_VK_UPSCALE_DLSS_QUALITY = 6,
    RSX_VK_UPSCALE_DLSS_BALANCED = 7,
    RSX_VK_UPSCALE_DLSS_PERFORMANCE = 8,
};

enum rsx_vk_upscale_cap {
    RSX_VK_UPSCALE_CAP_FSR = 1,
    RSX_VK_UPSCALE_CAP_DLSS = 2,
};

typedef struct rsx_vk_upscale_frame {
    float inv_curr_vp[16];
    float prev_vp[16];
    float jitter_x;
    float jitter_y;
    float near_clip;
    float far_clip;
    float fov_y;
    float dt_ms;
    float sharpness;
    float vel_scale_x;
    float vel_scale_y;
    float prev_jitter_x;
    float prev_jitter_y;
    int   reset;
} rsx_vk_upscale_frame;

void rsx_vk_set_upscaler(int mode);
int  rsx_vk_upscaler_active(void);
int  rsx_vk_upscaler_caps(void);
const char* rsx_vk_upscaler_fsr_version(void);
void rsx_vk_upscale_render_size(u32 display_w, u32 display_h, int mode, u32* out_w, u32* out_h);
void rsx_vk_upscale_jitter(int mode, u32 render_w, u32 display_w, u32 frame_index, float* out_jx, float* out_jy);
void rsx_vk_upscale_capture_velocity(void);
void rsx_vk_upscale_trigger(const rsx_vk_upscale_frame* frame);

#ifdef __cplusplus
}
#endif

#endif
