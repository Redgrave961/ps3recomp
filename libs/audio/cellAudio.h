#ifndef PS3RECOMP_CELL_AUDIO_H
#define PS3RECOMP_CELL_AUDIO_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_AUDIO_PORT_MAX                 8
#define CELL_AUDIO_BLOCK_SAMPLES            256
#define CELL_AUDIO_BLOCK_8                  8
#define CELL_AUDIO_BLOCK_16                 16
#define CELL_AUDIO_BLOCK_32                 32

#define CELL_AUDIO_PORT_2CH                 2
#define CELL_AUDIO_PORT_8CH                 8

#define CELL_AUDIO_SAMPLE_RATE              48000

#define CELL_AUDIO_STATUS_READY             1
#define CELL_AUDIO_STATUS_RUN               2

typedef struct CellAudioPortParam {
    u64 nChannel;
    u64 nBlock;
    u64 attr;
    float level;
} CellAudioPortParam;

typedef struct CellAudioPortConfig {
    u64 readIndexAddr;
    u32 status;
    u64 nChannel;
    u64 nBlock;
    u32 portSize;
    u64 portAddr;
} CellAudioPortConfig;

s32 cellAudioInit(void);

s32 cellAudioQuit(void);

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum);

s32 cellAudioPortClose(u32 portNum);

s32 cellAudioPortStart(u32 portNum);

s32 cellAudioPortStop(u32 portNum);

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config);

#ifdef __cplusplus
}
#endif

#endif
