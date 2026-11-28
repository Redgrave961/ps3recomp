#include "cellPngDec.h"
#include "../filesystem/ps3_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

#include "../../runtime/ppu/ppu_memory.h"

static void vm_read_str(u32 addr, char* dst, size_t cap)
{
    size_t i = 0;
    if (!addr) { if (cap) dst[0] = 0; return; }
    for (; i < cap - 1; i++) {
        char c = (char)vm_read8(addr + (u32)i);
        dst[i] = c;
        if (!c) return;
    }
    dst[i] = 0;
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "stb_image.h"

#define PNGDEC_MAX_HANDLES      4
#define PNGDEC_MAX_SUBHANDLES   8

typedef struct {
    int  in_use;
} PngDecMainState;

typedef struct {
    int               in_use;
    u32               main_handle;
    CellPngDecSrc     src;
    CellPngDecInfo    info;
    CellPngDecInParam in_param;
    int               header_read;
    int               param_set;
    u8*               src_data;
    u32               src_size;
    int               src_allocated;
    char              filename[CELL_PNGDEC_MAX_PATH];
} PngDecSubState;

static PngDecMainState s_png_main[PNGDEC_MAX_HANDLES];
static PngDecSubState  s_png_sub[PNGDEC_MAX_SUBHANDLES];

static int pngdec_load_source(PngDecSubState* sub)
{
    if (sub->src_data) return 0;

    if (sub->src.srcSelect == CELL_PNGDEC_BUFFER) {
        u32 guest_addr = (u32)sub->src.streamPtr;
        if (!vm_base) {
            printf("[cellPngDec] ERROR: vm_base is NULL, cannot translate guest addr 0x%08X\n", guest_addr);
            return -1;
        }
        sub->src_data = vm_base + guest_addr;
        sub->src_size = sub->src.streamSize;
        sub->src_allocated = 0;
        return 0;
    }

    if (sub->src.srcSelect == CELL_PNGDEC_FILE) {
        char host_path[1024];
        const int translated = sys_fs_translate_path(sub->src.fileName, host_path, (int)sizeof(host_path));

        ps3_vfile* f = translated ? ps3_vfile_open(host_path, "rb") : NULL;
        if (!f) {
            printf("[cellPngDec] Cannot open file: %s (host: %s)\n",
                   sub->src.fileName, host_path);
            return -1;
        }

        ps3_vstat st;
        ps3_vfile_fstat(f, &st);
        long total_size = (long)st.size;

        long read_offset = (long)sub->src.fileOffset;
        long read_size;

        if (sub->src.fileSize > 0 && (long)sub->src.fileSize < (total_size - read_offset))
            read_size = (long)sub->src.fileSize;
        else
            read_size = total_size - read_offset;

        if (read_size <= 0) {
            ps3_vfile_close(f);
            printf("[cellPngDec] File is empty or offset past end: %s\n", host_path);
            return -1;
        }

        ps3_vfile_seek(f, read_offset, SEEK_SET);

        sub->src_data = (u8*)malloc((size_t)read_size);
        if (!sub->src_data) {
            ps3_vfile_close(f);
            return -1;
        }

        sub->src_size = (u32)ps3_vfile_read(sub->src_data, (size_t)read_size, f);
        sub->src_allocated = 1;
        ps3_vfile_close(f);

        printf("[cellPngDec] Loaded %u bytes from '%s'\n", sub->src_size, host_path);
        return 0;
    }

    return -1;
}

static void pngdec_free_source(PngDecSubState* sub)
{
    if (sub->src_allocated && sub->src_data)
        free(sub->src_data);
    sub->src_data = NULL;
    sub->src_size = 0;
    sub->src_allocated = 0;
}

s32 cellPngDecDestroy(CellPngDecMainHandle mainHandle)
{
    printf("[cellPngDec] Destroy(handle=%u)\n", mainHandle);

    if (mainHandle >= PNGDEC_MAX_HANDLES || !s_png_main[mainHandle].in_use)
        return CELL_PNGDEC_ERROR_ARG;

    for (u32 i = 0; i < PNGDEC_MAX_SUBHANDLES; i++) {
        if (s_png_sub[i].in_use && s_png_sub[i].main_handle == mainHandle) {
            pngdec_free_source(&s_png_sub[i]);
            s_png_sub[i].in_use = 0;
        }
    }

    s_png_main[mainHandle].in_use = 0;
    return CELL_OK;
}

s32 cellPngDecOpen(CellPngDecMainHandle mainHandle,
                   u32 subHandle_addr, u32 src_addr, u32 openInfo_addr)
{
    if (mainHandle >= PNGDEC_MAX_HANDLES || !s_png_main[mainHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!subHandle_addr || !src_addr)
        return CELL_PNGDEC_ERROR_ARG;

    u32 srcSelect = vm_read32(src_addr + 0);
    printf("[cellPngDec] Open(main=%u, srcSelect=%u)\n", mainHandle, srcSelect);

    for (u32 i = 0; i < PNGDEC_MAX_SUBHANDLES; i++) {
        if (!s_png_sub[i].in_use) {
            PngDecSubState* sub = &s_png_sub[i];
            memset(sub, 0, sizeof(PngDecSubState));
            sub->in_use = 1;
            sub->main_handle = mainHandle;
            sub->src.srcSelect = srcSelect;
            sub->src.fileOffset = (u64)vm_read64(src_addr + 8);
            sub->src.fileSize   = vm_read32(src_addr + 16);
            sub->src.streamPtr  = vm_read32(src_addr + 20);
            sub->src.streamSize = vm_read32(src_addr + 24);
            if (srcSelect == CELL_PNGDEC_FILE) {
                u32 name_addr = vm_read32(src_addr + 4);
                vm_read_str(name_addr, sub->filename, sizeof(sub->filename));
                sub->src.fileName = sub->filename;
                printf("[cellPngDec]   file: '%s'\n", sub->filename);
            } else if (srcSelect == CELL_PNGDEC_BUFFER) {
                printf("[cellPngDec]   buffer: guest_addr=0x%08X size=%u\n",
                       (u32)sub->src.streamPtr, sub->src.streamSize);
            }

            vm_write32(subHandle_addr, i);
            if (openInfo_addr)
                vm_write32(openInfo_addr + 0, 0);
            return CELL_OK;
        }
    }

    return CELL_PNGDEC_ERROR_BUSY;
}

static void pngdec_write_info(u32 addr, const CellPngDecInfo* in)
{
    vm_write32(addr + 0,  in->imageWidth);
    vm_write32(addr + 4,  in->imageHeight);
    vm_write32(addr + 8,  in->numComponents);
    vm_write32(addr + 12, in->colorSpace);
    vm_write32(addr + 16, in->bitDepth);
    vm_write32(addr + 20, in->interlaceMethod);
    vm_write32(addr + 24, in->chunkInformation);
}

s32 cellPngDecReadHeader(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u32 info_addr)
{
    (void)mainHandle;
    printf("[cellPngDec] ReadHeader(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!info_addr)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];

    if (pngdec_load_source(sub) < 0)
        return CELL_PNGDEC_ERROR_OPEN_FILE;

    {
        int w, h, comp;
        if (!stbi_info_from_memory(sub->src_data, (int)sub->src_size, &w, &h, &comp)) {
            printf("[cellPngDec] stbi_info failed: %s\n", stbi_failure_reason());
            return CELL_PNGDEC_ERROR_HEADER;
        }

        sub->info.imageWidth    = (u32)w;
        sub->info.imageHeight   = (u32)h;
        sub->info.numComponents = (u32)comp;
        sub->info.bitDepth      = 8;
        sub->info.interlaceMethod = 0;
        sub->info.chunkInformation = 0;

        switch (comp) {
            case 1: sub->info.colorSpace = CELL_PNGDEC_GRAYSCALE; break;
            case 2: sub->info.colorSpace = CELL_PNGDEC_GRAYSCALE_ALPHA; break;
            case 3: sub->info.colorSpace = CELL_PNGDEC_RGB; break;
            case 4: sub->info.colorSpace = CELL_PNGDEC_RGBA; break;
            default: sub->info.colorSpace = CELL_PNGDEC_RGBA; break;
        }

        printf("[cellPngDec]   %ux%u, %u components\n", (u32)w, (u32)h, (u32)comp);

        sub->header_read = 1;
        pngdec_write_info(info_addr, &sub->info);
        return CELL_OK;
    }
}

s32 cellPngDecSetParameter(CellPngDecMainHandle mainHandle,
                           CellPngDecSubHandle subHandle,
                           u32 inParam_addr, u32 outParam_addr)
{
    (void)mainHandle;

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!inParam_addr || !outParam_addr)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];
    if (!sub->header_read)
        return CELL_PNGDEC_ERROR_SEQ;

    sub->in_param.commandPtr        = vm_read32(inParam_addr + 0);
    sub->in_param.outputMode        = vm_read32(inParam_addr + 4);
    sub->in_param.outputColorSpace  = vm_read32(inParam_addr + 8);
    sub->in_param.outputBitDepth    = vm_read32(inParam_addr + 12);
    sub->in_param.outputPackFlag    = vm_read32(inParam_addr + 16);
    sub->in_param.outputAlphaSelect = vm_read32(inParam_addr + 20);
    sub->in_param.outputColorAlpha  = vm_read32(inParam_addr + 24);
    sub->param_set = 1;

    printf("[cellPngDec] SetParameter(sub=%u, outputColorSpace=%u)\n",
           subHandle, sub->in_param.outputColorSpace);

    u32 out_cs = sub->in_param.outputColorSpace;
    u32 out_comp;
    switch (out_cs) {
        case CELL_PNGDEC_COLOR_SPACE_GRAYSCALE: out_comp = 1; break;
        case CELL_PNGDEC_COLOR_SPACE_RGB:       out_comp = 3; break;
        case CELL_PNGDEC_COLOR_SPACE_RGBA:      out_comp = 4; break;
        case CELL_PNGDEC_COLOR_SPACE_ARGB:      out_comp = 4; break;
        default:                                 out_comp = 4; out_cs = CELL_PNGDEC_COLOR_SPACE_RGBA; break;
    }

    u64 widthByte = (u64)(sub->info.imageWidth * out_comp);
    vm_write64(outParam_addr + 0,  widthByte);
    vm_write32(outParam_addr + 8,  sub->info.imageWidth);
    vm_write32(outParam_addr + 12, sub->info.imageHeight);
    vm_write32(outParam_addr + 16, out_comp);
    vm_write32(outParam_addr + 20, 8);
    vm_write32(outParam_addr + 24, 0);
    vm_write32(outParam_addr + 28, out_cs);
    vm_write32(outParam_addr + 32, (u32)(widthByte * sub->info.imageHeight));

    return CELL_OK;
}

static void pngdec_write_outinfo(u32 addr, u32 status)
{
    if (!addr) return;
    vm_write32(addr + 0,  0);
    vm_write32(addr + 4,  0);
    vm_write32(addr + 8,  0);
    vm_write32(addr + 12, status);
}

s32 cellPngDecDecodeData(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u32 data_addr, u32 dataCtrlParam_addr, u32 dataOutInfo_addr)
{
    (void)mainHandle;
    printf("[cellPngDec] DecodeData(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!data_addr)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];
    if (!sub->header_read || !sub->param_set)
        return CELL_PNGDEC_ERROR_SEQ;

    {
        int w, h, comp;
        int desired_comp;

        switch (sub->in_param.outputColorSpace) {
            case CELL_PNGDEC_COLOR_SPACE_GRAYSCALE: desired_comp = 1; break;
            case CELL_PNGDEC_COLOR_SPACE_RGB:       desired_comp = 3; break;
            case CELL_PNGDEC_COLOR_SPACE_RGBA:      desired_comp = 4; break;
            case CELL_PNGDEC_COLOR_SPACE_ARGB:      desired_comp = 4; break;
            default:                                 desired_comp = 4; break;
        }

        u8* pixels = stbi_load_from_memory(sub->src_data, (int)sub->src_size,
                                            &w, &h, &comp, desired_comp);
        if (!pixels) {
            printf("[cellPngDec] stbi_load failed: %s\n", stbi_failure_reason());
            pngdec_write_outinfo(dataOutInfo_addr, CELL_PNGDEC_DEC_STATUS_STOP);
            return CELL_PNGDEC_ERROR_STREAM_FORMAT;
        }

        u32 row_bytes = (u32)w * (u32)desired_comp;

        if (sub->in_param.outputColorSpace == CELL_PNGDEC_COLOR_SPACE_ARGB && desired_comp == 4) {
            u32 total_bytes = row_bytes * (u32)h;
            for (u32 i = 0; i < total_bytes; i += 4) {
                u8 r = pixels[i+0], g = pixels[i+1], b = pixels[i+2], a = pixels[i+3];
                pixels[i+0] = a; pixels[i+1] = r; pixels[i+2] = g; pixels[i+3] = b;
            }
        }

        u64 dst_pitch = dataCtrlParam_addr ? vm_read64(dataCtrlParam_addr + 0) : 0;
        if (dst_pitch < row_bytes) dst_pitch = row_bytes;
        for (u32 y = 0; y < (u32)h; y++)
            memcpy(vm_base + data_addr + (u64)y * dst_pitch,
                   pixels + (u64)y * row_bytes, row_bytes);
        if (h) vm_track_write(data_addr, (u64)(h - 1) * dst_pitch + row_bytes);
        stbi_image_free(pixels);

        printf("[cellPngDec]   decoded %ux%u -> guest 0x%08X (pitch %llu, row %u)\n",
               (u32)w, (u32)h, data_addr, (unsigned long long)dst_pitch, row_bytes);

        pngdec_write_outinfo(dataOutInfo_addr, CELL_PNGDEC_DEC_STATUS_FINISH);
        return CELL_OK;
    }
}

s32 cellPngDecClose(CellPngDecMainHandle mainHandle,
                    CellPngDecSubHandle subHandle)
{
    (void)mainHandle;
    printf("[cellPngDec] Close(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    pngdec_free_source(&s_png_sub[subHandle]);
    s_png_sub[subHandle].in_use = 0;
    return CELL_OK;
}
