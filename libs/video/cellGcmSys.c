#include "cellGcmSys.h"
#include "rsx_core.h"
#include "rsx_tiling.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/ppu/ppu_memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include "../../runtime/platform/win32_compat.h"
#endif

#include <string.h>

#define GCM_SEGMENT        0x8000u
#define GCM_CALLBACK_OPD   (RSX_GCM_BLOCK_EA + 0x000u)
#define GCM_CALLBACK_CODE  (RSX_GCM_BLOCK_EA + 0x010u)
#define GCM_CONTEXT_EA     (RSX_GCM_BLOCK_EA + 0x020u)
#define GCM_DISPLAY_EA     (RSX_GCM_BLOCK_EA + 0x040u)
#define GCM_TILE_EA        (RSX_GCM_BLOCK_EA + 0x0C0u)
#define GCM_ZCULL_EA       (RSX_GCM_BLOCK_EA + 0x1C0u)

#define GCM_METHOD_FLIP             0xFEACu

extern uint32_t g_ps3_sdk_version;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

typedef struct gcm_tile {
    u32 location, offset, size, pitch, comp, base, bank;
    int bound;
} gcm_tile;

static struct {
    int initialized;
    u32 local_size;
    u32 main_size;
    u32 io_address;
    u32 io_size;
    u32 cmd_fragments;
    u32 command_words;
    u32 segment_words;
    volatile u32 handler[RSX_EVENT_COUNT];
    u8 map_pages[4096];
    gcm_tile tile[CELL_GCM_MAX_TILE_COUNT];
    SRWLOCK io_lock;
} g = { .command_words = 0x400u, .segment_words = 0x100u, .io_lock = SRWLOCK_INIT };

static u32 gcm_local_size(u32 sdk)
{
    if (sdk >= 0x00220000u) return 0x0F900000u;
    if (sdk >= 0x00200000u) return 0x0F200000u;
    if (sdk >= 0x00190000u) return 0x0EA00000u;
    if (sdk >= 0x00180000u) return 0x0E800000u;
    return 0x0E000000u;
}

static void gcm_event(enum rsx_event event, u32 arg)
{
    const u32 opd = __atomic_load_n(&g.handler[event], __ATOMIC_ACQUIRE);
    if (opd) ppu_guest_call(opd, arg, 0, 0, 0, 0, 0, 0, 0);
}

static u32 gcm_io(u32 ea)
{
    return rsx_ea_to_io(ea);
}

static void gcm_put(u32 io)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vm_write32(RSX_PUT_EA, io);
    rsx_core_doorbell();
}

static s32 gcm_callback(u32 ctx, u32 count)
{
    (void)count;
    const u32 cur = vm_read32(ctx + 8u);
    const u32 io_cur = gcm_io(cur);
    if (io_cur == 0xFFFFFFFFu) return (s32)CELL_GCM_ERROR_FAILURE;
    gcm_put(io_cur);
    const u32 base = g.io_address;
    const u32 range = (cur - base) / GCM_SEGMENT;
    u32 begin, end;
    if (range + 1u >= g.cmd_fragments) {
        begin = base + 0x1000u;
        end = base + GCM_SEGMENT - 4u;
    } else {
        begin = base + (range + 1u) * GCM_SEGMENT;
        end = base + (range + 2u) * GCM_SEGMENT - 4u;
    }
    vm_write32(cur, 0x20000000u | gcm_io(begin));
    vm_write32(ctx + 0u, begin);
    vm_write32(ctx + 8u, begin);
    vm_write32(ctx + 4u, end);
    rsx_core_wait_get_outside(gcm_io(begin), gcm_io(end));
    return CELL_OK;
}

static void gcm_callback_entry(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)gcm_callback((u32)ctx->gpr[3], (u32)ctx->gpr[4]);
}

static s32 gcm_reserve(u32 ctx, u32 words)
{
    if (vm_read32(ctx + 8u) + words * 4u < vm_read32(ctx + 4u)) return CELL_OK;
    const u32 opd = vm_read32(ctx + 12u);
    if (opd == GCM_CALLBACK_OPD) return gcm_callback(ctx, words);
    return (s32)ppu_guest_call(opd, ctx, words, 0, 0, 0, 0, 0, 0);
}

static void gcm_emit(u32 ctx, u32 reg, const u32* args, u32 n)
{
    const u32 cur = vm_read32(ctx + 8u);
    vm_write32(cur, (n << 18) | reg);
    for (u32 i = 0; i < n; i++) vm_write32(cur + 4u + i * 4u, args[i]);
    vm_write32(ctx + 8u, cur + 4u + n * 4u);
}

static u32 gcm_stack_arg(u32 index)
{
    const u32 sp = (u32)g_active_ctx->gpr[1];
    return vm_read32(sp + 0x30u + index * 8u + 4u);
}

s32 _cellGcmInitBody(u32 context, u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    if (g.initialized) return (s32)CELL_GCM_ERROR_FAILURE;
    g.local_size = gcm_local_size(g_ps3_sdk_version);
    g.main_size = 0x10000000u;
    rsx_core_init(g.local_size, g.main_size);
    rsx_core_set_event_sink(gcm_event);
    memset(g.map_pages, 0, sizeof g.map_pages);
    if (!ioSize || rsx_io_map(ioAddress, 0, ioSize) != 0) return (s32)CELL_GCM_ERROR_FAILURE;
    g.map_pages[ioAddress >> 20] = (u8)(ioSize >> 20);
    rsx_core_fifo_reset(0x1000u, 0x1000u);
    g.io_address = ioAddress;
    g.io_size = ioSize;
    g.cmd_fragments = cmdSize / GCM_SEGMENT;
    memset(vm_base + RSX_GCM_BLOCK_EA, 0, 0x1000u);
    vm_write32(GCM_CALLBACK_OPD, GCM_CALLBACK_CODE);
    vm_write32(GCM_CALLBACK_OPD + 4u, 0);
    ppu_register_function(GCM_CALLBACK_CODE, gcm_callback_entry);
    vm_write32(GCM_CONTEXT_EA + 0u, ioAddress + 0x1000u);
    vm_write32(GCM_CONTEXT_EA + 4u, ioAddress + GCM_SEGMENT - 4u);
    vm_write32(GCM_CONTEXT_EA + 8u, ioAddress + 0x1000u);
    vm_write32(GCM_CONTEXT_EA + 12u, GCM_CALLBACK_OPD);
    vm_write32(context, GCM_CONTEXT_EA);
    rsx_core_start();
    g.initialized = 1;
    return CELL_OK;
}

void cellGcmGetConfiguration(u32 config)
{
    if (!g.initialized) return;
    vm_write32(config + 0u, RSX_LOCAL_BASE);
    vm_write32(config + 4u, g.io_address);
    vm_write32(config + 8u, g.local_size);
    vm_write32(config + 12u, g.io_size);
    vm_write32(config + 16u, 650000000u);
    vm_write32(config + 20u, 500000000u);
}

u32 cellGcmGetControlRegister(void) { return RSX_CONTROL_EA; }
u32 cellGcmGetDefaultCommandWordSize(void) { return g.command_words; }
u32 cellGcmGetDefaultSegmentWordSize(void) { return g.segment_words; }
s32 cellGcmSetDefaultFifoSize(u32 bufferSize, u32 segmentSize) { (void)bufferSize; (void)segmentSize; return CELL_OK; }

s32 cellGcmSetDisplayBuffer(u8 id, u32 offset, u32 pitch, u32 width, u32 height)
{
    if (id >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) return (s32)CELL_GCM_ERROR_FAILURE;
    const u32 ea = GCM_DISPLAY_EA + id * 16u;
    vm_write32(ea + 0u, offset);
    vm_write32(ea + 4u, pitch);
    vm_write32(ea + 8u, width);
    vm_write32(ea + 12u, height);
    const rsx_display_buffer b = { offset, pitch, width, height };
    rsx_core_set_display_buffer(id, &b);
    return CELL_OK;
}

void cellGcmSetFlipMode(u32 mode) { rsx_core_set_vsync(mode == CELL_GCM_DISPLAY_VSYNC); }
u32 cellGcmGetFlipStatus(void) { return rsx_core_flip_status(); }
void cellGcmResetFlipStatus(void) { rsx_core_set_flip_status(CELL_GCM_FLIP_STATUS_WAITING); }
u64 cellGcmGetLastFlipTime(void) { return rsx_core_last_flip_time(); }
u64 cellGcmGetLastSecondVTime(void) { return 0; }

s32 cellGcmSetFlipImmediate(u8 id)
{
    if (id >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) return (s32)CELL_GCM_ERROR_FAILURE;
    rsx_core_flip_immediate(id);
    return CELL_OK;
}

static s32 gcm_prepare_flip(u32 ctx, u32 id, int reserve)
{
    if (id >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) return (s32)CELL_GCM_ERROR_FAILURE;
    if (reserve) {
        const s32 r = gcm_reserve(ctx, 2);
        if (r) return r;
    }
    gcm_emit(ctx, GCM_METHOD_FLIP, &id, 1);
    rsx_core_flip_emitted();
    if (reserve && ctx == GCM_CONTEXT_EA) gcm_put(vm_read32(RSX_PUT_EA) + 8u);
    return (s32)id;
}

s32 cellGcmSetPrepareFlip(u32 ctx, u32 id) { return gcm_prepare_flip(ctx, id, 1); }

void _cellGcmSetFlipCommand(u32 ctx, u32 id) { gcm_prepare_flip(ctx, id, 0); }

void cellGcmSetFlipHandler(u32 handler) { __atomic_store_n(&g.handler[RSX_EVENT_FLIP], handler, __ATOMIC_RELEASE); }
void cellGcmSetVBlankHandler(u32 handler) { __atomic_store_n(&g.handler[RSX_EVENT_VBLANK], handler, __ATOMIC_RELEASE); }
void cellGcmSetUserHandler(u32 handler) { __atomic_store_n(&g.handler[RSX_EVENT_USER], handler, __ATOMIC_RELEASE); }
s32 cellGcmSetSecondVHandler(u32 handler) { return CELL_OK; }
void cellGcmSetGraphicsHandler(u32 handler) {}
void cellGcmSetSecondVFrequency(u32 freq) { (void)freq; }

u32 cellGcmGetLabelAddress(u8 index) { return RSX_SEMAPHORE_EA + 0x10u * index; }

static u32 gcm_report_ea(u32 index, u32 location)
{
    if (location == CELL_GCM_LOCATION_MAIN) {
        if (index >= CELL_GCM_MAX_MAIN_REPORT_COUNT) return 0;
        return rsx_io_to_ea(0x0E000000u + index * 0x10u);
    }
    if (index >= CELL_GCM_MAX_LOCAL_REPORT_COUNT) return 0;
    return RSX_REPORT_EA + index * 0x10u;
}

u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    const u32 ea = gcm_report_ea(index, location);
    return ea ? vm_read64(ea) : 0;
}

u64 cellGcmGetTimeStamp(u32 index) { return cellGcmGetTimeStampLocation(index, CELL_GCM_LOCATION_LOCAL); }

s32 cellGcmAddressToOffset(u32 address, u32 offset)
{
    u32 result;
    if (address - RSX_LOCAL_BASE < g.local_size) {
        result = address - RSX_LOCAL_BASE;
    } else {
        result = rsx_ea_to_io(address);
        if (result == 0xFFFFFFFFu || result >= g.main_size) return (s32)CELL_GCM_ERROR_FAILURE;
    }
    vm_write32(offset, result);
    return CELL_OK;
}

s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32 address)
{
    const u32 ea = rsx_io_to_ea(ioOffset);
    if (!ea) return (s32)CELL_GCM_ERROR_FAILURE;
    vm_write32(address, ea);
    return CELL_OK;
}

static s32 gcm_map(u32 ea, u32 io, u32 size)
{
    if (!size || (ea & 0xFFFFFu) || (io & 0xFFFFFu) || (size & 0xFFFFFu)) return (s32)CELL_GCM_ERROR_FAILURE;
    if (!g.initialized || io + size > g.main_size) return (s32)CELL_GCM_ERROR_FAILURE;
    if (rsx_io_map(ea, io, size) != 0) return (s32)CELL_GCM_ERROR_FAILURE;
    g.map_pages[ea >> 20] = (u8)(size >> 20);
    return CELL_OK;
}

s32 cellGcmMapMainMemory(u32 ea, u32 size, u32 offset)
{
    if (!size || (ea & 0xFFFFFu) || (size & 0xFFFFFu)) return (s32)CELL_GCM_ERROR_FAILURE;
    AcquireSRWLockExclusive(&g.io_lock);
    const u32 pages = size >> 20, end = g.main_size >> 20;
    s32 r = (s32)CELL_GCM_ERROR_NO_IO_PAGE_TABLE;
    for (u32 io = 0, run = 0; io < end; io++) {
        run = rsx_io_to_ea(io << 20) ? 0 : run + 1u;
        if (run == pages) {
            const u32 first = (io + 1u - pages) << 20;
            r = gcm_map(ea, first, size);
            if (r == CELL_OK) vm_write32(offset, first);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g.io_lock);
    return r;
}

static s32 gcm_unmap_io(u32 io)
{
    const u32 ea = rsx_io_to_ea(io & ~0xFFFFFu);
    if (!ea) return (s32)CELL_GCM_ERROR_FAILURE;
    const u32 pages = g.map_pages[ea >> 20];
    if (!pages) return (s32)CELL_GCM_ERROR_FAILURE;
    rsx_io_unmap(io & ~0xFFFFFu, pages << 20);
    g.map_pages[ea >> 20] = 0;
    return CELL_OK;
}

s32 cellGcmUnmapIoAddress(u32 io)
{
    AcquireSRWLockExclusive(&g.io_lock);
    const s32 r = gcm_unmap_io(io);
    ReleaseSRWLockExclusive(&g.io_lock);
    return r;
}

void cellGcmSortRemapEaIoAddress(void) {}

static void gcm_tile_publish(u32 index)
{
    const gcm_tile* t = &g.tile[index];
    const u32 ea = GCM_TILE_EA + index * 16u;
    vm_write32(ea + 0u, (t->location + 1u) | (t->bank << 4) | ((t->offset / 0x10000u) << 16) | (t->location << 31));
    vm_write32(ea + 4u, ((t->offset + t->size - 1u) / 0x10000u) << 16 | (t->location << 31));
    vm_write32(ea + 8u, (t->pitch / 0x100u) << 8);
    vm_write32(ea + 12u, t->base | ((t->base + ((t->size - 1u) / 0x10000u)) << 13) | (t->comp << 26) | (1u << 30));
    if (t->bound) rsx_tiling_set(index, t->location, t->offset, t->size, t->pitch, t->bank);
    else rsx_tiling_set(index, 0, 0, 0, 0, 0);
}

static s32 gcm_set_tile(u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base, u8 bank, int bind)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT || base >= 2048u || bank >= 4u) return (s32)CELL_GCM_ERROR_INVALID_VALUE;
    if ((offset & 0xFFFFu) || (size & 0xFFFFu) || (pitch & 0xFFu)) return (s32)CELL_GCM_ERROR_INVALID_ALIGNMENT;
    if (location >= 2u || (comp != 0u && (comp < 7u || comp > 12u))) return (s32)CELL_GCM_ERROR_INVALID_ENUM;
    gcm_tile* t = &g.tile[index];
    t->location = location;
    t->offset = offset;
    t->size = size;
    t->pitch = pitch;
    t->comp = comp;
    t->base = base;
    t->bank = bank;
    if (bind) t->bound = pitch > 0;
    gcm_tile_publish(index);
    return CELL_OK;
}

s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base, u8 bank)
{
    return gcm_set_tile(index, location, offset, size, pitch, comp, base, bank, 0);
}

s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) return (s32)CELL_GCM_ERROR_INVALID_VALUE;
    g.tile[index].bound = 1;
    gcm_tile_publish(index);
    return CELL_OK;
}

s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) return (s32)CELL_GCM_ERROR_INVALID_VALUE;
    g.tile[index].bound = 0;
    gcm_tile_publish(index);
    return CELL_OK;
}

u32 cellGcmGetTiledPitchSize(u32 size)
{
    static const u32 pitches[] = {
        0x00000, 0x00200, 0x00300, 0x00400, 0x00500, 0x00600, 0x00700, 0x00800,
        0x00A00, 0x00C00, 0x00D00, 0x00E00, 0x01000, 0x01400, 0x01800, 0x01A00,
        0x01C00, 0x02000, 0x02800, 0x03000, 0x03400, 0x03800, 0x04000, 0x05000,
        0x06000, 0x06800, 0x07000, 0x08000, 0x0A000, 0x0C000, 0x0D000, 0x0E000,
        0x10000 };
    for (u32 i = 0; i + 1u < sizeof pitches / sizeof pitches[0]; i++)
        if (pitches[i] < size && size <= pitches[i + 1u]) return pitches[i + 1u];
    return 0;
}

static void gcm_zcull(u8 index, u32 offset, u32 width, u32 height, u32 cull_start, u32 z_format,
                      u32 aa_format, u32 zcull_dir)
{
    const u32 zcull_format = gcm_stack_arg(8), s_func = gcm_stack_arg(9);
    const u32 s_ref = gcm_stack_arg(10), s_mask = gcm_stack_arg(11);
    const u32 ea = GCM_ZCULL_EA + index * 24u;
    vm_write32(ea + 0u, 1u | (z_format << 4) | (aa_format << 8));
    vm_write32(ea + 4u, ((width >> 6) << 22) | ((height >> 6) << 6));
    vm_write32(ea + 8u, cull_start & ~0xFFFu);
    vm_write32(ea + 12u, offset);
    vm_write32(ea + 16u, (zcull_dir << 1) | (zcull_format << 2) | ((s_func & 0xFu) << 12) | (s_ref << 16) | (s_mask << 24));
    vm_write32(ea + 20u, 0x2000u | (0x20u << 16));
}

s32 cellGcmBindZcull(u8 index, u32 offset, u32 width, u32 height, u32 cullStart, u32 zFormat,
                     u32 aaFormat, u32 zCullDir)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) return (s32)CELL_GCM_ERROR_INVALID_VALUE;
    gcm_zcull(index, offset, width, height, cullStart, zFormat, aaFormat, zCullDir);
    return CELL_OK;
}

s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) return (s32)CELL_GCM_ERROR_INVALID_VALUE;
    vm_write32(GCM_ZCULL_EA + index * 24u, 0);
    return CELL_OK;
}

void _cellGcmFunc15(u32 context) { (void)context; }
