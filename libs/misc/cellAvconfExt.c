#include "cellAvconfExt.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdlib.h>

#include <stdio.h>
#include <string.h>

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option)
{
    (void)audioOut;
    (void)option;

    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM) {
        if (fs & CELL_AUDIO_OUT_FS_48KHZ) return CELL_AUDIO_OUT_CHNUM_6;
        return CELL_AUDIO_OUT_CHNUM_2;
    }

    if (type == CELL_AUDIO_OUT_CODING_TYPE_AC3 || type == CELL_AUDIO_OUT_CODING_TYPE_DTS)
        return CELL_AUDIO_OUT_CHNUM_6;

    return 0;
}

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info)
{
    (void)audioOut;
    (void)deviceIndex;

    printf("[cellAvconfExt] AudioOutGetDeviceInfo(out=%u, dev=%u)\n",
           audioOut, deviceIndex);

    if (!info)
        return CELL_EINVAL;
    info = GUEST_PTR(info, CellAudioOutDeviceInfo*);

    memset(info, 0, sizeof(CellAudioOutDeviceInfo));

    info->portType = 0;
    info->state = 2;
    info->availableModeCount = 3;
    info->availableModes[0].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[0].channel = CELL_AUDIO_OUT_CHNUM_2;
    info->availableModes[0].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[1].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[1].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[1].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[2].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[2].channel = CELL_AUDIO_OUT_CHNUM_8;
    info->availableModes[2].fs      = CELL_AUDIO_OUT_FS_48KHZ;

    return CELL_OK;
}

s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize)
{
    (void)audioOut;
    (void)option;
    (void)optionSize;

    printf("[cellAvconfExt] AudioOutGetConfiguration()\n");

    if (!config)
        return CELL_EINVAL;
    config = GUEST_PTR(config, CellAudioOutConfiguration*);

    memset(config, 0, sizeof(CellAudioOutConfiguration));
    config->channel = CELL_AUDIO_OUT_CHNUM_2;
    config->encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM;

    return CELL_OK;
}

s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, void* state)
{
    (void)deviceIndex;
    uint32_t ea = (uint32_t)(uintptr_t)state;
    printf("[cellAvconfExt] AudioOutGetState(audioOut=%u)\n", audioOut);
    if (!ea)
        return CELL_EINVAL;

    vm_write8(ea + 0, 0);
    vm_write8(ea + 1, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    vm_write8(ea + 2, 0);
    vm_write8(ea + 3, 0);
    vm_write32(ea + 4, 0);
    vm_write8(ea + 8, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    vm_write8(ea + 9, CELL_AUDIO_OUT_CHNUM_2);
    vm_write8(ea + 10, CELL_AUDIO_OUT_FS_48KHZ);
    vm_write8(ea + 11, 0);
    vm_write32(ea + 12, 0);
    return CELL_OK;
}

s32 cellAudioOutGetNumberOfDevice(u32 audioOut)
{
    (void)audioOut;
    return 1;
}

s32 cellAudioOutConfigure(u32 audioOut, void* config, void* option, u32 waitForEvent)
{
    (void)config; (void)option; (void)waitForEvent;
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u) -> ok\n", audioOut);
    return 0;
}
