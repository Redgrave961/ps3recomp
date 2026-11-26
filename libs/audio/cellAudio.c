#include "../../runtime/memory/vm.h"
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"
#include "../../runtime/platform/ps3_sync.h"

#if defined(_WIN32)
  #define AUDIO_BACKEND_SDL     0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL     1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  #ifndef DEFINE_AUDIO_GUID
    #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
            const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMNotificationClient,0x7991EEC9,0x7E89,0x4D85,0x83,0x90,0x6C,0x70,0x3C,0xEC,0x60,0xC0);
  DEFINE_AUDIO_GUID(ps3r_IID_IUnknown,             0x00000000,0x0000,0x0000,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient3,        0x7ED4EE07,0x8E67,0x4CD4,0x8C,0x1A,0x2B,0x7A,0x59,0x87,0xAD,0x42);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  #define IID_IMMDeviceEnumerator   ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator  ps3r_CLSID_MMDeviceEnumerator
  #define IID_IMMNotificationClient ps3r_IID_IMMNotificationClient
  #define IID_IAudioClient          ps3r_IID_IAudioClient
  #define IID_IAudioClient3         ps3r_IID_IAudioClient3
  #define IID_IAudioRenderClient    ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL
  #include <SDL3/SDL.h>
#endif

#ifdef _WIN32
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;
    u32                  buf_size;
    u64                  read_index;
    u64                  port_addr;
    u64                  read_idx_addr;
} AudioPortSlot;

static int           s_audio_initialized = 0;
static AudioPortSlot s_ports[CELL_AUDIO_PORT_MAX];
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;
static u32           s_out_channels = 2;

static unsigned long long g_audio_underruns = 0;
static unsigned g_audio_holes[CELL_AUDIO_PORT_MAX];

static int audio_env_buf_ms(void)
{
    const char* e = getenv("PS3_AUDIO_BUF_MS");
    const int ms = e ? atoi(e) : 0;
    return (ms >= 10 && ms <= 500) ? ms : 0;
}

static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static unsigned audio_backend_device_frames(void);
static u32      audio_backend_sample_rate(void);

static void audio_mix_one_block(float* out, u32 out_ch)
{
    const u32 total_samples = CELL_AUDIO_BLOCK_SAMPLES * out_ch;
    memset(out, 0, total_samples * sizeof(float));

    static int s_peak_on = -1;
    static int s_rate_on = -1;
    static int s_mute    = -1;
    if (s_peak_on < 0) s_peak_on = getenv("AUDIO_PEAK") ? 1 : 0;
    if (s_rate_on < 0) s_rate_on = getenv("AUDIO_RATE") ? 1 : 0;
    if (s_mute    < 0) s_mute    = getenv("PS3_MUTE")   ? 1 : 0;

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                const u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                        port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) {
                    if (port->buffer[s] != 0.0f) { nz = 1; break; }
                }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        const u32 nch    = (u32)port->param.nChannel;
        const u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f || level > 1.0f) level = 1.0f;

        const u32 block_idx = (u32)(port->read_index % nblock);
        float* src = port->buffer + block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;

        if (s_peak_on) {
            static unsigned s_ring_cnt[CELL_AUDIO_PORT_MAX];
            if ((++s_ring_cnt[p] % 400u) == 0u) {
                char map[36];
                u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16) {
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            }
        }

        if (s_rate_on) {
            static u32 prev_nz[CELL_AUDIO_PORT_MAX];
            int nz = 0;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 2) {
                if (src[s2] != 0.0f) { nz = 1; break; }
            }
            if (!nz && prev_nz[p]) g_audio_holes[p]++;
            prev_nz[p] = (u32)nz;
        }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float* dst = &out[s * out_ch];
            if (out_ch == 8) {
                if (nch == 8) {
                    for (u32 c = 0; c < 8; c++) dst[c] += ld_be_f32(&src[s * 8 + c]) * level;
                } else if (nch >= 2) {
                    dst[0] += ld_be_f32(&src[s * nch + 0]) * level;
                    dst[1] += ld_be_f32(&src[s * nch + 1]) * level;
                } else {
                    const float m = ld_be_f32(&src[s]) * level;
                    dst[0] += m;
                    dst[1] += m;
                }
            } else if (out_ch == 6) {
                if (nch == 8) {
                    dst[0] += ld_be_f32(&src[s * 8 + 0]) * level;
                    dst[1] += ld_be_f32(&src[s * 8 + 1]) * level;
                    dst[2] += ld_be_f32(&src[s * 8 + 2]) * level;
                    dst[3] += ld_be_f32(&src[s * 8 + 3]) * level;
                    dst[4] += (ld_be_f32(&src[s * 8 + 4]) + ld_be_f32(&src[s * 8 + 6]) * 0.707f) * level;
                    dst[5] += (ld_be_f32(&src[s * 8 + 5]) + ld_be_f32(&src[s * 8 + 7]) * 0.707f) * level;
                } else if (nch >= 2) {
                    dst[0] += ld_be_f32(&src[s * nch + 0]) * level;
                    dst[1] += ld_be_f32(&src[s * nch + 1]) * level;
                } else {
                    const float m = ld_be_f32(&src[s]) * level;
                    dst[0] += m;
                    dst[1] += m;
                }
            } else {
                float left, right;
                if (nch >= 2) {
                    left  = ld_be_f32(&src[s * nch + 0]) * level;
                    right = ld_be_f32(&src[s * nch + 1]) * level;
                } else {
                    left = right = ld_be_f32(&src[s]) * level;
                }
                if (nch == 8) {
                    const float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                    const float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                    const float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                    const float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                    const float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                    const float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                    left  += center + lfe + rl + sl;
                    right += center + lfe + rr + sr;
                }
                dst[0] += left;
                dst[1] += right;
            }
        }

        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    mutex_unlock(&s_audio_mutex);

    for (u32 i = 0; i < total_samples; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }

    if (s_peak_on) {
        static unsigned s_pk_n = 0;
        float pk = 0.0f;
        for (u32 i = 0; i < total_samples; i++) {
            const float a = fabsf(out[i]);
            if (a > pk) pk = a;
        }
        if ((++s_pk_n % 200u) == 0u || (pk > 0.001f && s_pk_n < 40u))
            fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", s_pk_n, pk);
    }

    {
        static FILE* wf = (FILE*)-1;
        if (wf == (FILE*)-1) {
            const char* e = getenv("AUDIO_WAV");
            wf = e ? fopen(e, "wb") : NULL;
        }
        if (wf) fwrite(out, sizeof(float), total_samples, wf);
    }

    if (s_rate_on) {
        static unsigned long long t0 = 0;
        static unsigned n = 0, z = 0;
        static unsigned long long u0 = 0;
        const unsigned long long now = ps3_sync_now_ns() / 1000000ull;
        if (!t0) t0 = now;
        n++;
        int nz = 0;
        for (u32 i = 0; i < total_samples; i++) {
            if (out[i] != 0.0f) { nz = 1; break; }
        }
        if (!nz) z++;
        if (now - t0 >= 5000) {
            const unsigned dev_frames = audio_backend_device_frames();
            const u32 dev_rate = audio_backend_sample_rate();
            fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, underruns %llu, device buf %u @ %u Hz (%.1f ms), holes %u+%u\n",
                    n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0,
                    g_audio_underruns - u0, dev_frames, dev_rate,
                    dev_rate ? dev_frames * 1000.0 / dev_rate : 0.0,
                    g_audio_holes[0], g_audio_holes[1]);
            g_audio_holes[0] = g_audio_holes[1] = 0;
            n = z = 0;
            u0 = g_audio_underruns;
            t0 = now;
        }
    }

    if (s_mute)
        memset(out, 0, total_samples * sizeof(float));
}

#if AUDIO_BACKEND_SDL

static SDL_AudioStream*   s_sdl_audio_stream = NULL;
static u32                s_sdl_host_channels_cache = 0;
static u32                s_sdl_dev_rate = CELL_AUDIO_SAMPLE_RATE;
static int                s_sdl_extra_bytes = 0;
static volatile int       s_sdl_reopen = 0;
static unsigned long long s_sdl_reopen_last_ns = 0;

static void sdl_query_host_format(u32* out_ch, u32* out_rate)
{
    u32 ch = 2;
    u32 rate = CELL_AUDIO_SAMPLE_RATE;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            if (out_ch) *out_ch = ch;
            if (out_rate) *out_rate = rate;
            return;
        }
    }
    SDL_AudioSpec dev_spec;
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev_spec, &frames)) {
        if (dev_spec.channels >= 8) ch = 8;
        else if (dev_spec.channels >= 6) ch = 6;
        if (dev_spec.freq >= 22050 && dev_spec.freq <= 384000) rate = (u32)dev_spec.freq;
    }
    if (out_ch) *out_ch = ch;
    if (out_rate) *out_rate = rate;
}

u32 ps3_audio_host_channels(void)
{
    u32 c = __atomic_load_n(&s_sdl_host_channels_cache, __ATOMIC_RELAXED);
    if (!c) {
        sdl_query_host_format(&c, NULL);
        __atomic_store_n(&s_sdl_host_channels_cache, c, __ATOMIC_RELAXED);
    }
    return c;
}

static bool SDLCALL sdl_audio_event_watch(void* userdata, SDL_Event* event)
{
    (void)userdata;
    if (event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED ||
        event->type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED ||
        event->type == SDL_EVENT_AUDIO_DEVICE_ADDED) {
        __atomic_store_n(&s_sdl_reopen, 1, __ATOMIC_RELEASE);
    }
    return true;
}

static void SDLCALL sdl_audio_callback(void* userdata, SDL_AudioStream* stream,
                                       int additional_amount, int total_amount)
{
    (void)userdata;
    int need = additional_amount;
    if (s_sdl_extra_bytes > 0) {
        const int queued = SDL_GetAudioStreamQueued(stream);
        const int want = total_amount + s_sdl_extra_bytes - (queued > 0 ? queued : 0);
        if (want > need) need = want;
    }
    if (need <= 0) return;

    const u32 out_ch = s_out_channels;
    const int block_bytes = (int)(CELL_AUDIO_BLOCK_SAMPLES * out_ch * sizeof(float));
    float block[CELL_AUDIO_BLOCK_SAMPLES * 8];

    while (need > 0) {
        audio_mix_one_block(block, out_ch);
        SDL_PutAudioStreamData(stream, block, block_bytes);
        need -= block_bytes;
    }
}

static unsigned audio_backend_device_frames(void)
{
    SDL_AudioSpec spec;
    int frames = 0;
    if (s_sdl_audio_stream)
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_sdl_audio_stream), &spec, &frames);
    return (unsigned)frames;
}

static u32 audio_backend_sample_rate(void)
{
    return s_sdl_dev_rate;
}

static int sdl_open_stream(void)
{
    u32 ch = 2, dev_rate = CELL_AUDIO_SAMPLE_RATE;
    sdl_query_host_format(&ch, &dev_rate);
    __atomic_store_n(&s_sdl_host_channels_cache, ch, __ATOMIC_RELAXED);
    s_out_channels = ch;

    /* Scale the requested device quantum to ~5.33 ms at any host rate (44.1-384 kHz). */
    u32 want_frames = (u32)(((u64)dev_rate * CELL_AUDIO_BLOCK_SAMPLES + CELL_AUDIO_SAMPLE_RATE / 2u) / CELL_AUDIO_SAMPLE_RATE);
    if (want_frames < 128u) want_frames = 128u;
    char frames_hint[16];
    snprintf(frames_hint, sizeof frames_hint, "%u", want_frames);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_hint);

    const SDL_AudioSpec spec = { SDL_AUDIO_F32, (int)s_out_channels, CELL_AUDIO_SAMPLE_RATE };
    s_sdl_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_audio_callback, NULL);
    if (!s_sdl_audio_stream && s_out_channels > 2) {
        s_out_channels = 2;
        __atomic_store_n(&s_sdl_host_channels_cache, 2, __ATOMIC_RELAXED);
        const SDL_AudioSpec fallback = { SDL_AUDIO_F32, 2, CELL_AUDIO_SAMPLE_RATE };
        s_sdl_audio_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &fallback, sdl_audio_callback, NULL);
    }
    if (!s_sdl_audio_stream) {
        printf("[cellAudio] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_AudioSpec actual_spec;
    int dev_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_sdl_audio_stream), &actual_spec, &dev_frames) &&
        actual_spec.freq > 0) {
        s_sdl_dev_rate = (u32)actual_spec.freq;
    } else {
        s_sdl_dev_rate = dev_rate;
    }

    const int extra_ms = audio_env_buf_ms();
    s_sdl_extra_bytes = extra_ms > 0
        ? (int)((u32)(extra_ms * CELL_AUDIO_SAMPLE_RATE / 1000) * s_out_channels * sizeof(float))
        : 0;

    printf("[cellAudio] SDL3 output: %u ch @ %u Hz (pull, device %d frames / %.1f ms)\n",
           s_out_channels, s_sdl_dev_rate, dev_frames,
           s_sdl_dev_rate ? dev_frames * 1000.0 / s_sdl_dev_rate : 0.0);
    SDL_ResumeAudioStreamDevice(s_sdl_audio_stream);
    return 0;
}

static void sdl_close_stream(void)
{
    if (s_sdl_audio_stream) {
        SDL_DestroyAudioStream(s_sdl_audio_stream);
        s_sdl_audio_stream = NULL;
    }
}

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }
    __atomic_store_n(&s_sdl_reopen, 0, __ATOMIC_RELAXED);
    SDL_AddEventWatch(sdl_audio_event_watch, NULL);
    return sdl_open_stream();
}

static void audio_backend_shutdown(void)
{
    SDL_RemoveEventWatch(sdl_audio_event_watch, NULL);
    sdl_close_stream();
}

static void audio_backend_poll_reopen(void)
{
    const int want = __atomic_exchange_n(&s_sdl_reopen, 0, __ATOMIC_ACQ_REL);
    if (!want && s_sdl_audio_stream) return;

    const unsigned long long now = ps3_sync_now_ns();
    if (!want && now - s_sdl_reopen_last_ns < 500000000ull) return;
    s_sdl_reopen_last_ns = now;

    u32 new_ch = 2, new_rate = CELL_AUDIO_SAMPLE_RATE;
    sdl_query_host_format(&new_ch, &new_rate);
    const int unbound = !s_sdl_audio_stream || SDL_GetAudioStreamDevice(s_sdl_audio_stream) == 0;
    if (!unbound && new_ch == s_out_channels && new_rate == s_sdl_dev_rate) return;

    sdl_close_stream();
    sdl_open_stream();
}

static void audio_mix_loop(void)
{
    ps3_thread_name("audio mix");
    printf("[cellAudio] Audio monitor thread started\n");

    const unsigned long long block_ns = 1000000000ull * CELL_AUDIO_BLOCK_SAMPLES / CELL_AUDIO_SAMPLE_RATE;
    unsigned long long t_start = 0;
    unsigned long long blocks = 0;
    float fallback_block[CELL_AUDIO_BLOCK_SAMPLES * 8];

    while (s_mix_thread_running) {
        audio_backend_poll_reopen();
        if (s_sdl_audio_stream) {
            t_start = 0;
            blocks = 0;
            SDL_Delay(50);
            continue;
        }

        const unsigned long long now = ps3_sync_now_ns();
        if (!t_start) { t_start = now; blocks = 0; }
        const unsigned long long due = t_start + blocks * block_ns;
        if (now < due) {
            ps3_sleep_us(1000);
            continue;
        }
        if (now - due > 50000000ull) t_start = now - blocks * block_ns;
        blocks++;
        audio_mix_one_block(fallback_block, s_out_channels);
    }

    printf("[cellAudio] Audio monitor thread stopped\n");
}

#endif /* AUDIO_BACKEND_SDL */

#if AUDIO_BACKEND_WASAPI

static IMMDeviceEnumerator* s_wasapi_enum          = NULL;
static IAudioClient*        s_wasapi_client        = NULL;
static IAudioRenderClient*  s_wasapi_render        = NULL;
static HANDLE               s_wasapi_event         = NULL;
static UINT32               s_wasapi_buf_frames    = 0;
static UINT32               s_wasapi_period_frames = 0;
static UINT32               s_wasapi_target_frames = 0;
static u32                  s_wasapi_rate          = CELL_AUDIO_SAMPLE_RATE;
static unsigned long long   s_wasapi_stream_blocks = 0;
static int                  s_wasapi_low_latency   = 0;
static int                  s_wasapi_no_low_lat    = 0;
static volatile LONG        s_wasapi_reopen        = 0;
static volatile LONG        s_wasapi_host_channels_cache = 0;
static unsigned long long   s_wasapi_reopen_last_ns = 0;

static float  s_wasapi_stage[CELL_AUDIO_BLOCK_SAMPLES * 8];
static u32    s_wasapi_stage_pos = 0;
static u32    s_wasapi_stage_len = 0;
static float  s_wasapi_rs_hist[4][8];
static double s_wasapi_rs_phase = 0.0;

static const GUID k_subformat_ieee_float = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

static unsigned audio_backend_device_frames(void)
{
    return (unsigned)s_wasapi_target_frames;
}

static u32 audio_backend_sample_rate(void)
{
    return s_wasapi_rate;
}

static void wasapi_request_reopen(void)
{
    InterlockedExchange(&s_wasapi_reopen, 1);
    HANDLE ev = s_wasapi_event;
    if (ev) SetEvent(ev);
}

static HRESULT STDMETHODCALLTYPE wasapi_notify_qi(IMMNotificationClient* self, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (memcmp(riid, &ps3r_IID_IUnknown, sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IMMNotificationClient, sizeof(GUID)) == 0) {
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG   STDMETHODCALLTYPE wasapi_notify_addref(IMMNotificationClient* self) { (void)self; return 1; }
static ULONG   STDMETHODCALLTYPE wasapi_notify_release(IMMNotificationClient* self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_state(IMMNotificationClient* self, LPCWSTR id, DWORD state) { (void)self; (void)id; (void)state; return S_OK; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_added(IMMNotificationClient* self, LPCWSTR id) { (void)self; (void)id; if (!s_wasapi_client) wasapi_request_reopen(); return S_OK; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_removed(IMMNotificationClient* self, LPCWSTR id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_default(IMMNotificationClient* self, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)self; (void)id;
    if (flow == eRender && role == eConsole) {
        s_wasapi_no_low_lat = 0;
        wasapi_request_reopen();
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE wasapi_notify_prop(IMMNotificationClient* self, LPCWSTR id, const PROPERTYKEY key) { (void)self; (void)id; (void)key; return S_OK; }

static IMMNotificationClientVtbl s_wasapi_notify_vtbl = {
    wasapi_notify_qi,
    wasapi_notify_addref,
    wasapi_notify_release,
    wasapi_notify_state,
    wasapi_notify_added,
    wasapi_notify_removed,
    wasapi_notify_default,
    wasapi_notify_prop,
};
static IMMNotificationClient s_wasapi_notify = { &s_wasapi_notify_vtbl };

static u32 wasapi_query_host_channels(void)
{
    u32 ch = 2;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return ch;
    IMMDeviceEnumerator* en = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&en)) && en) {
        IMMDevice* dev = NULL;
        if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) && dev) {
            IAudioClient* cli = NULL;
            if (SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&cli)) && cli) {
                WAVEFORMATEX* mix = NULL;
                if (SUCCEEDED(cli->lpVtbl->GetMixFormat(cli, &mix)) && mix) {
                    if (mix->nChannels >= 8) ch = 8;
                    else if (mix->nChannels >= 6) ch = 6;
                    CoTaskMemFree(mix);
                }
                cli->lpVtbl->Release(cli);
            }
            dev->lpVtbl->Release(dev);
        }
        en->lpVtbl->Release(en);
    }
    return ch;
}

u32 ps3_audio_host_channels(void)
{
    LONG c = InterlockedCompareExchange(&s_wasapi_host_channels_cache, 0, 0);
    if (!c) {
        c = (LONG)wasapi_query_host_channels();
        InterlockedExchange(&s_wasapi_host_channels_cache, c);
    }
    return (u32)c;
}

static void fill_wasapi_format(WAVEFORMATEXTENSIBLE* wfxe, u32 channels)
{
    memset(wfxe, 0, sizeof(*wfxe));
    wfxe->Format.nChannels       = (WORD)channels;
    wfxe->Format.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfxe->Format.wBitsPerSample  = 32;
    wfxe->Format.nBlockAlign     = (WORD)(channels * 4u);
    wfxe->Format.nAvgBytesPerSec = CELL_AUDIO_SAMPLE_RATE * wfxe->Format.nBlockAlign;
    if (channels > 2) {
        wfxe->Format.wFormatTag          = WAVE_FORMAT_EXTENSIBLE;
        wfxe->Format.cbSize              = 22;
        wfxe->Samples.wValidBitsPerSample = 32;
        wfxe->dwChannelMask              = (channels >= 8) ? 0x63Fu : 0x3Fu;
        wfxe->SubFormat                  = k_subformat_ieee_float;
    } else {
        wfxe->Format.wFormatTag          = WAVE_FORMAT_IEEE_FLOAT;
        wfxe->Format.cbSize              = 0;
    }
}

static int wasapi_mix_is_native_f32(const WAVEFORMATEX* mix, u32 out_ch)
{
    /* Use native IAudioClient3 + Catmull-Rom for 44.1-384 kHz; below 44.1 kHz
       (e.g. Bluetooth HFP 16/22/32 kHz), let Windows AUTOCONVERTPCM low-pass. */
    if (!mix || mix->nSamplesPerSec < 44100u || mix->nSamplesPerSec > 384000u ||
        mix->nChannels != out_ch || mix->wBitsPerSample != 32)
        return 0;
    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return 1;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)mix;
        return memcmp(&ext->SubFormat, &k_subformat_ieee_float, sizeof(GUID)) == 0;
    }
    return 0;
}

static UINT32 wasapi_ps3_block_out_frames(u32 rate)
{
    return (UINT32)(((u64)CELL_AUDIO_BLOCK_SAMPLES * rate + CELL_AUDIO_SAMPLE_RATE - 1u) / CELL_AUDIO_SAMPLE_RATE);
}

static void wasapi_close_stream(void)
{
    if (s_wasapi_client) s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
    s_wasapi_buf_frames    = 0;
    s_wasapi_period_frames = 0;
    s_wasapi_target_frames = 0;
    s_wasapi_rate          = CELL_AUDIO_SAMPLE_RATE;
    s_wasapi_stream_blocks = 0;
    s_wasapi_low_latency   = 0;
    s_wasapi_stage_pos     = 0;
    s_wasapi_stage_len     = 0;
    memset(s_wasapi_rs_hist, 0, sizeof(s_wasapi_rs_hist));
    s_wasapi_rs_phase      = 0.0;
}

static int wasapi_try_low_latency(IMMDevice* device, const WAVEFORMATEX* mix, HANDLE ev)
{
    IAudioClient3* ac3 = NULL;
    if (FAILED(s_wasapi_client->lpVtbl->QueryInterface(s_wasapi_client, &IID_IAudioClient3, (void**)&ac3)) || !ac3)
        return 0;

    UINT32 def_p = 0, fund_p = 0, min_p = 0, max_p = 0;
    HRESULT hr = ac3->lpVtbl->GetSharedModeEnginePeriod(ac3, mix, &def_p, &fund_p, &min_p, &max_p);
    if (FAILED(hr) || !min_p || !def_p) {
        ac3->lpVtbl->Release(ac3);
        return 0;
    }

    /* Pick the smallest valid period >= ~4 ms at any host rate (44.1-384 kHz)
       to match PS3's 5.33 ms cadence while leaving headroom on loaded CPUs. */
    const UINT32 min_want = (UINT32)(((u64)mix->nSamplesPerSec * 4ull) / 1000ull);
    UINT32 period = min_p;
    if (fund_p > 0 && period < min_want) {
        const UINT32 steps = (min_want - period + fund_p - 1u) / fund_p;
        period += steps * fund_p;
    }
    if (period > def_p) period = def_p;

    hr = ac3->lpVtbl->InitializeSharedAudioStream(
        ac3, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, mix, NULL);
    ac3->lpVtbl->Release(ac3);
    if (FAILED(hr)) {
        /* Activate a clean IAudioClient for the fallback path. */
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&s_wasapi_client);
        return 0;
    }

    s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, ev);
    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);
    s_wasapi_rate          = (u32)mix->nSamplesPerSec;
    s_wasapi_period_frames = period;
    const UINT32 min_target = wasapi_ps3_block_out_frames(s_wasapi_rate);
    UINT32 target = period * 2u;
    if (target < min_target) target = min_target;
    if (target > s_wasapi_buf_frames) target = s_wasapi_buf_frames;
    s_wasapi_target_frames = target;
    s_wasapi_low_latency   = 1;
    return 1;
}

static int wasapi_open_stream(void)
{
    HRESULT hr;
    IMMDeviceEnumerator* enumerator = s_wasapi_enum;
    int local_enum = 0;
    if (!enumerator) {
        hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                              &IID_IMMDeviceEnumerator, (void**)&enumerator);
        if (FAILED(hr)) {
            printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
            return -1;
        }
        local_enum = 1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (local_enum) enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&s_wasapi_client);
    if (FAILED(hr)) {
        device->lpVtbl->Release(device);
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    u32 ch = 2;
    WAVEFORMATEX* mix = NULL;
    if (SUCCEEDED(s_wasapi_client->lpVtbl->GetMixFormat(s_wasapi_client, &mix)) && mix) {
        if (mix->nChannels >= 8) ch = 8;
        else if (mix->nChannels >= 6) ch = 6;
    }
    s_out_channels = ch;
    InterlockedExchange(&s_wasapi_host_channels_cache, (LONG)ch);

    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    const int env_ms = audio_env_buf_ms();

    int initialized = 0;
    if (!env_ms && !s_wasapi_no_low_lat && s_wasapi_event &&
        wasapi_mix_is_native_f32(mix, s_out_channels)) {
        initialized = wasapi_try_low_latency(device, mix, s_wasapi_event);
    }

    if (mix) CoTaskMemFree(mix);

    if (!initialized) {
        if (!s_wasapi_client) {
            device->lpVtbl->Release(device);
            wasapi_close_stream();
            return -1;
        }
        WAVEFORMATEXTENSIBLE wfxe;
        fill_wasapi_format(&wfxe, s_out_channels);

        const REFERENCE_TIME buf_duration = (REFERENCE_TIME)(env_ms ? env_ms : 60) * 10000;
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
        const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
            buf_duration, 0, &wfxe.Format, NULL);

        if (FAILED(hr) && s_out_channels > 2) {
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            if (SUCCEEDED(device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&s_wasapi_client)) && s_wasapi_client) {
                s_out_channels = 2;
                InterlockedExchange(&s_wasapi_host_channels_cache, 2);
                fill_wasapi_format(&wfxe, 2);
                hr = s_wasapi_client->lpVtbl->Initialize(
                    s_wasapi_client, AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
                    buf_duration, 0, &wfxe.Format, NULL);
            }
        }

        if (FAILED(hr)) {
            if (s_wasapi_client) {
                s_wasapi_client->lpVtbl->Release(s_wasapi_client);
                s_wasapi_client = NULL;
            }
            if (SUCCEEDED(device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&s_wasapi_client)) && s_wasapi_client) {
                hr = s_wasapi_client->lpVtbl->Initialize(
                    s_wasapi_client, AUDCLNT_SHAREMODE_SHARED,
                    convert, buf_duration, 0, &wfxe.Format, NULL);
            }
            if (FAILED(hr) || !s_wasapi_client) {
                device->lpVtbl->Release(device);
                printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
                wasapi_close_stream();
                return -1;
            }
            if (s_wasapi_event) { CloseHandle(s_wasapi_event); s_wasapi_event = NULL; }
        } else if (s_wasapi_event) {
            s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
        }

        s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);
        REFERENCE_TIME def_hns = 100000; /* 10 ms fallback */
        s_wasapi_client->lpVtbl->GetDevicePeriod(s_wasapi_client, &def_hns, NULL);
        s_wasapi_period_frames = (UINT32)((def_hns * CELL_AUDIO_SAMPLE_RATE + 9999999ll) / 10000000ll);
        if (s_wasapi_period_frames < CELL_AUDIO_BLOCK_SAMPLES)
            s_wasapi_period_frames = CELL_AUDIO_BLOCK_SAMPLES;

        if (env_ms) {
            s_wasapi_target_frames = s_wasapi_buf_frames;
        } else {
            /* Keep ~1 period + 1 PS3 block (~15 ms) queued inside the 60 ms ring;
               if a saturated CPU ever underruns, target auto-widens inside the ring. */
            UINT32 target = s_wasapi_period_frames + CELL_AUDIO_BLOCK_SAMPLES;
            if (target > s_wasapi_buf_frames) target = s_wasapi_buf_frames;
            s_wasapi_target_frames = target;
        }
        s_wasapi_rate = CELL_AUDIO_SAMPLE_RATE;
        s_wasapi_low_latency = 0;
    }

    device->lpVtbl->Release(device);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        wasapi_close_stream();
        return -1;
    }

    printf("[cellAudio] WASAPI output: %u ch @ %u Hz (%s, period %u / target %u / buf %u frames, %.1f ms)\n",
           s_out_channels, s_wasapi_rate,
           s_wasapi_low_latency ? "IAudioClient3" : "shared",
           (unsigned)s_wasapi_period_frames, (unsigned)s_wasapi_target_frames,
           (unsigned)s_wasapi_buf_frames,
           s_wasapi_target_frames * 1000.0 / s_wasapi_rate);
    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static int audio_backend_init(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    InterlockedExchange(&s_wasapi_reopen, 0);
    if (!s_wasapi_enum) {
        hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                              &IID_IMMDeviceEnumerator, (void**)&s_wasapi_enum);
        if (SUCCEEDED(hr) && s_wasapi_enum) {
            s_wasapi_enum->lpVtbl->RegisterEndpointNotificationCallback(s_wasapi_enum, &s_wasapi_notify);
        }
    }
    return wasapi_open_stream();
}

static void audio_backend_shutdown(void)
{
    wasapi_close_stream();
    if (s_wasapi_enum) {
        s_wasapi_enum->lpVtbl->UnregisterEndpointNotificationCallback(s_wasapi_enum, &s_wasapi_notify);
        s_wasapi_enum->lpVtbl->Release(s_wasapi_enum);
        s_wasapi_enum = NULL;
    }
}

static void audio_backend_poll_reopen(void)
{
    const LONG want = InterlockedExchange(&s_wasapi_reopen, 0);
    if (!want && s_wasapi_client) return;

    const unsigned long long now = ps3_sync_now_ns();
    if (!want && now - s_wasapi_reopen_last_ns < 500000000ull) return;
    s_wasapi_reopen_last_ns = now;

    wasapi_close_stream();
    wasapi_open_stream();
}

static int wasapi_device_lost(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

static void audio_mix_loop(void)
{
    ps3_thread_name("audio mix");
    printf("[cellAudio] Mixing thread started\n");

    const HRESULT co_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    HANDLE mmcss = NULL;
    typedef HANDLE (WINAPI *set_mm_fn)(LPCWSTR, LPDWORD);
    typedef BOOL   (WINAPI *rev_mm_fn)(HANDLE);
    if (avrt) {
        set_mm_fn set_mm = (set_mm_fn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        DWORD task = 0;
        if (set_mm) {
            mmcss = set_mm(L"Pro Audio", &task);
            if (!mmcss)
                fprintf(stderr, "[cellAudio] MMCSS Pro Audio unavailable (%lu)\n", (unsigned long)GetLastError());
        }
    }

    const unsigned long long block_ns = 1000000000ull * CELL_AUDIO_BLOCK_SAMPLES / CELL_AUDIO_SAMPLE_RATE;
    unsigned long long t_start = 0;
    unsigned long long blocks = 0;
    unsigned low_lat_underruns = 0;

    while (s_mix_thread_running) {
        audio_backend_poll_reopen();

        if (!s_wasapi_client || !s_wasapi_render) {
            const unsigned long long now = ps3_sync_now_ns();
            if (!t_start) { t_start = now; blocks = 0; }
            const unsigned long long due = t_start + blocks * block_ns;
            if (now < due) { ps3_sleep_us(1000); continue; }
            if (now - due > 50000000ull) t_start = now - blocks * block_ns;
            blocks++;
            audio_mix_one_block(s_wasapi_stage, s_out_channels);
            continue;
        }
        t_start = 0;

        UINT32 padding = 0;
        HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
        if (FAILED(hr)) {
            if (wasapi_device_lost(hr)) wasapi_request_reopen();
            else {
                static int _n = 0;
                if (PS3_LOG_FIRST(_n, 4))
                    fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX\n", (unsigned long)hr);
                Sleep(1);
            }
            continue;
        }

        if (s_wasapi_stream_blocks > 64 && padding == 0 && s_wasapi_buf_frames > s_wasapi_period_frames) {
            g_audio_underruns++;
            if (s_wasapi_target_frames < s_wasapi_buf_frames) {
                UINT32 next = s_wasapi_target_frames + wasapi_ps3_block_out_frames(s_wasapi_rate);
                if (next > s_wasapi_buf_frames) next = s_wasapi_buf_frames;
                s_wasapi_target_frames = next;
            } else if (s_wasapi_low_latency && ++low_lat_underruns >= 8u) {
                s_wasapi_no_low_lat = 1;
                low_lat_underruns = 0;
                wasapi_request_reopen();
                continue;
            }
        }

        UINT32 need = (padding < s_wasapi_target_frames) ? (s_wasapi_target_frames - padding) : 0;
        const UINT32 avail = s_wasapi_buf_frames - padding;
        if (need > avail) need = avail;

        if (need == 0) {
            if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
            else Sleep(1);
            continue;
        }

        const u32 out_ch = s_out_channels;
        if (s_wasapi_rate == CELL_AUDIO_SAMPLE_RATE) {
            while (need > 0 && s_mix_thread_running) {
                if (s_wasapi_stage_pos >= s_wasapi_stage_len) {
                    s_wasapi_stream_blocks++;
                    audio_mix_one_block(s_wasapi_stage, out_ch);
                    s_wasapi_stage_pos = 0;
                    s_wasapi_stage_len = CELL_AUDIO_BLOCK_SAMPLES;
                }
                UINT32 take = s_wasapi_stage_len - s_wasapi_stage_pos;
                if (take > need) take = need;

                BYTE* buf = NULL;
                hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, take, &buf);
                if (SUCCEEDED(hr) && buf) {
                    memcpy(buf, &s_wasapi_stage[s_wasapi_stage_pos * out_ch],
                           take * out_ch * sizeof(float));
                    hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, take, 0);
                    if (FAILED(hr) && wasapi_device_lost(hr)) { wasapi_request_reopen(); break; }
                    s_wasapi_stage_pos += take;
                    need -= take;
                } else {
                    if (wasapi_device_lost(hr)) wasapi_request_reopen();
                    break;
                }
            }
        } else {
            BYTE* buf = NULL;
            hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, need, &buf);
            if (SUCCEEDED(hr) && buf) {
                float* dst = (float*)buf;
                const double step = (double)CELL_AUDIO_SAMPLE_RATE / (double)s_wasapi_rate;
                for (UINT32 f = 0; f < need; f++) {
                    while (s_wasapi_rs_phase >= 1.0) {
                        if (s_wasapi_stage_pos >= s_wasapi_stage_len) {
                            s_wasapi_stream_blocks++;
                            audio_mix_one_block(s_wasapi_stage, out_ch);
                            s_wasapi_stage_pos = 0;
                            s_wasapi_stage_len = CELL_AUDIO_BLOCK_SAMPLES;
                        }
                        memcpy(s_wasapi_rs_hist[0], s_wasapi_rs_hist[1], out_ch * sizeof(float));
                        memcpy(s_wasapi_rs_hist[1], s_wasapi_rs_hist[2], out_ch * sizeof(float));
                        memcpy(s_wasapi_rs_hist[2], s_wasapi_rs_hist[3], out_ch * sizeof(float));
                        memcpy(s_wasapi_rs_hist[3], &s_wasapi_stage[s_wasapi_stage_pos * out_ch], out_ch * sizeof(float));
                        s_wasapi_stage_pos++;
                        s_wasapi_rs_phase -= 1.0;
                    }
                    const float t = (float)s_wasapi_rs_phase;
                    float* out_f = dst + f * out_ch;
                    for (u32 c = 0; c < out_ch; c++) {
                        const float y0 = s_wasapi_rs_hist[0][c];
                        const float y1 = s_wasapi_rs_hist[1][c];
                        const float y2 = s_wasapi_rs_hist[2][c];
                        const float y3 = s_wasapi_rs_hist[3][c];
                        const float c1 = 0.5f * (y2 - y0);
                        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                        float v = ((c3 * t + c2) * t + c1) * t + y1;
                        if (v > 1.0f) v = 1.0f;
                        else if (v < -1.0f) v = -1.0f;
                        out_f[c] = v;
                    }
                    s_wasapi_rs_phase += step;
                }
                hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, need, 0);
                if (FAILED(hr) && wasapi_device_lost(hr)) wasapi_request_reopen();
            } else if (wasapi_device_lost(hr)) {
                wasapi_request_reopen();
            }
        }
    }

    if (mmcss && avrt) {
        rev_mm_fn rev_mm = (rev_mm_fn)(void*)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        if (rev_mm) rev_mm(mmcss);
    }
    if (avrt) FreeLibrary(avrt);
    if (SUCCEEDED(co_hr)) CoUninitialize();
    printf("[cellAudio] Mixing thread stopped\n");
}

#endif /* AUDIO_BACKEND_WASAPI */

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    audio_mix_loop();
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    audio_mix_loop();
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;
#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func, NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_wasapi_event) SetEvent(s_wasapi_event);
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

s32 cellAudioInit(void)
{
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0)
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");

    if (audio_start_mix_thread() < 0)
        printf("[cellAudio] WARNING: Could not start mixing thread\n");

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    const uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    const uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    const u64 nch  = vm_read64(param_ea + 0);
    const u64 nblk = vm_read64(param_ea + 8);
    const u64 attr = vm_read64(param_ea + 16);
    float init_level = 1.0f;
    if (attr & 0x1000ull) { /* CELL_AUDIO_PORTATTR_INITLEVEL */
        const u32 lv_bits = vm_read32(param_ea + 24);
        memcpy(&init_level, &lv_bits, 4);
        if (!(init_level > 0.0f && init_level <= 1.0f)) init_level = 1.0f;
    }
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) { found = (s32)i; break; }
    }
    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->param.attr     = attr;
    port->param.level    = init_level;
    port->read_index     = 0;

    const u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    const u32 guest_buf  = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    const u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);
    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }
    s_ports[portNum].buffer  = NULL;
    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    const uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    vm_write32(cfg +  0, (u32)port->read_idx_addr);
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);
    vm_write64(cfg +  8, port->param.nChannel);
    vm_write64(cfg + 16, port->param.nBlock);
    vm_write32(cfg + 24, port->buf_size);
    vm_write32(cfg + 28, (u32)port->port_addr);

    {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 24))
            fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                    portNum, port->running ? "RUN" : "READY",
                    (u32)port->port_addr, (u32)port->read_idx_addr);
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}
