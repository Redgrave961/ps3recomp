#ifndef PS3RECOMP_RSX_CORE_H
#define PS3RECOMP_RSX_CORE_H

#include "ps3emu/ps3types.h"
#include "../../runtime/memory/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_LOCAL_BASE          0xC0000000u
#define RSX_LOCAL_SIZE_MAX      0x10000000u

#define RSX_SEMAPHORE_EA        (VM_HLE_INJECT_BASE + 0x0000u)
#define RSX_SEMAPHORE_SIZE      0x1000u
#define RSX_NOTIFY_EA           (VM_HLE_INJECT_BASE + 0x1000u)
#define RSX_REPORT_EA           (VM_HLE_INJECT_BASE + 0x1400u)
#define RSX_REPORT_SIZE         0x8000u
#define RSX_CONTROL_EA          (VM_HLE_INJECT_BASE + 0xA000u)
#define RSX_PUT_EA              (RSX_CONTROL_EA + 0x0u)
#define RSX_GET_EA              (RSX_CONTROL_EA + 0x4u)
#define RSX_REF_EA              (RSX_CONTROL_EA + 0x8u)
#define RSX_GCM_BLOCK_EA        (VM_HLE_INJECT_BASE + 0xB000u)
#define RSX_IO_TABLE_EA         (VM_HLE_INJECT_BASE + 0xC000u)
#define RSX_EA_TABLE_EA         (VM_HLE_INJECT_BASE + 0xE000u)
#define RSX_IO_PAGES            512u

#define RSX_LOCATION_LOCAL      0
#define RSX_LOCATION_MAIN       1

#define RSX_DMA_LOCAL           0xFEED0000u
#define RSX_DMA_MAIN            0xFEED0001u
#define RSX_DMA_REPORT_LOCAL    0x66626660u
#define RSX_DMA_REPORT_MAIN     0xBAD68000u
#define RSX_DMA_NOTIFY_MAIN_0   0x6660420Fu
#define RSX_DMA_SEMAPHORE_RW    0x66606660u
#define RSX_DMA_SEMAPHORE_R     0x66616661u

enum rsx_event {
    RSX_EVENT_VBLANK = 0,
    RSX_EVENT_FLIP,
    RSX_EVENT_QUEUE,
    RSX_EVENT_USER,
    RSX_EVENT_COUNT
};

typedef struct rsx_display_buffer {
    u32 offset;
    u32 pitch;
    u32 width;
    u32 height;
} rsx_display_buffer;

typedef struct rsx_renderer {
    void* user;
    int  (*open)(void* user);
    void (*methods)(void* user, u32 reg, u32 inc, const u32* be_args, u32 count);
    void (*flush)(void* user);
    void (*sync)(void* user);
    void (*reset)(void* user);
    int  (*report)(void* user, u32 type, u32 ea);
    void (*present)(void* user, u32 buffer, const rsx_display_buffer* display);
    void (*read_back)(void* user, u32 ea, u32 size);
    void (*wrote)(void* user, u32 ea, u32 size);
    void (*vblank)(void* user, u64 count);
    void (*close)(void* user);
} rsx_renderer;

typedef void (*rsx_event_sink)(enum rsx_event event, u32 arg);
typedef int (*rsx_vblank_clock)(void* user);

void rsx_core_init(u32 local_size, u32 main_size);
void rsx_core_set_renderer(const rsx_renderer* renderer);
void rsx_core_set_event_sink(rsx_event_sink sink);
void rsx_core_set_vblank_clock(rsx_vblank_clock clock, void* user);
void rsx_core_start(void);
void rsx_core_fifo_reset(u32 put, u32 get);
void rsx_core_doorbell(void);
void rsx_core_wait_get_outside(u32 lo, u32 hi);

u32  rsx_core_local_size(void);
int  rsx_io_map(u32 ea, u32 io, u32 size);
void rsx_io_unmap(u32 io, u32 size);
u32  rsx_io_to_ea(u32 io);
u32  rsx_ea_to_io(u32 ea);
u32  rsx_address(u32 location, u32 offset, u32 size);
u32  rsx_location_ea(int local, u32 offset);

void rsx_core_set_display_buffer(u32 id, const rsx_display_buffer* buffer);
int  rsx_core_display_buffer(u32 id, rsx_display_buffer* out);
u32  rsx_core_display_buffer_count(void);
u32  rsx_core_current_display_buffer(void);
void rsx_core_set_vsync(int vsync);
void rsx_core_flip_immediate(u32 buffer);
u32  rsx_core_flip_status(void);
void rsx_core_set_flip_status(u32 status);
u64  rsx_core_last_flip_time(void);
u64  rsx_core_vblank_count(void);
u64  rsx_core_last_vblank_time(void);
u64  rsx_core_timestamp(void);
u64  rsx_core_system_time(void);
u32  rsx_core_frames_presented(void);

void rsx_core_raise(enum rsx_event event, u32 arg);

void rsx_mem_store(u32 ea, const void* src, u32 size);
void rsx_core_read_back(u32 ea, u32 size);
void rsx_core_wrote(u32 ea, u32 size);

#ifdef __cplusplus
}
#endif

#endif
