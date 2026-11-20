#ifndef PS3RECOMP_CELL_AVCONF_EXT_H
#define PS3RECOMP_CELL_AVCONF_EXT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_AUDIO_OUT_CODING_TYPE_LPCM    0
#define CELL_AUDIO_OUT_CODING_TYPE_AC3     4
#define CELL_AUDIO_OUT_CODING_TYPE_DTS     8

#define CELL_AUDIO_OUT_CHNUM_2             2
#define CELL_AUDIO_OUT_CHNUM_6             6
#define CELL_AUDIO_OUT_CHNUM_8             8

#define CELL_AUDIO_OUT_FS_48KHZ            0x04

typedef struct CellAudioOutSoundMode {
    u8  type;
    u8  channel;
    u8  fs;
    u8  reserved;
} CellAudioOutSoundMode;

typedef struct CellAudioOutDeviceInfo {
    u8  portType;
    u8  availableModeCount;
    u8  state;
    u8  reserved[3];
    CellAudioOutSoundMode availableModes[16];
} CellAudioOutDeviceInfo;

typedef struct CellAudioOutConfiguration {
    u8  channel;
    u8  encoder;
    u8  reserved[10];
    u32 downMixer;
} CellAudioOutConfiguration;

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option);

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info);
s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize);
s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, void* state);

s32 cellAudioOutGetNumberOfDevice(u32 audioOut);

#ifdef __cplusplus
}
#endif

#endif
