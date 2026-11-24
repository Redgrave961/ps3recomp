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
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  #define IID_IMMDeviceEnumerator   ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator  ps3r_CLSID_MMDeviceEnumerator
  #define IID_IMMNotificationClient ps3r_IID_IMMNotificationClient
  #define IID_IAudioClient          ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient    ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL
  #include <SDL3/SDL.h>
#endif

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
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

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

static unsigned long long g_audio_dropped;
static unsigned g_audio_holes[CELL_AUDIO_PORT_MAX];
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 8];
static u32   s_out_channels = 2;

#if AUDIO_BACKEND_SDL

static SDL_AudioStream* s_sdl_audio_stream = NULL;
static u32              s_sdl_host_channels_cache = 0;
static volatile int     s_sdl_reopen = 0;
static unsigned long long s_sdl_reopen_last_ns = 0;

static u32 sdl_query_host_channels(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return 2;
    }
    SDL_AudioSpec dev_spec;
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev_spec, &frames)) {
        if (dev_spec.channels >= 8) return 8;
        if (dev_spec.channels >= 6) return 6;
    }
    return 2;
}

u32 ps3_audio_host_channels(void)
{
    u32 c = __atomic_load_n(&s_sdl_host_channels_cache, __ATOMIC_RELAXED);
    if (!c) {
        c = sdl_query_host_channels();
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

static int sdl_open_stream(void)
{
    const u32 ch = sdl_query_host_channels();
    __atomic_store_n(&s_sdl_host_channels_cache, ch, __ATOMIC_RELAXED);
    s_out_channels = ch;
    char frames[16];
    snprintf(frames, sizeof frames, "%d", CELL_AUDIO_BLOCK_SAMPLES);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, (int)s_out_channels, CELL_AUDIO_SAMPLE_RATE };
    s_sdl_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s_sdl_audio_stream && s_out_channels > 2) {
        s_out_channels = 2;
        __atomic_store_n(&s_sdl_host_channels_cache, 2, __ATOMIC_RELAXED);
        const SDL_AudioSpec fallback = { SDL_AUDIO_F32, 2, CELL_AUDIO_SAMPLE_RATE };
        s_sdl_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &fallback, NULL, NULL);
    }
    if (!s_sdl_audio_stream) {
        printf("[cellAudio] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return -1;
    }

    printf("[cellAudio] Output: %u channels @ %d Hz\n", s_out_channels, CELL_AUDIO_SAMPLE_RATE);
    SDL_ResumeAudioStreamDevice(s_sdl_audio_stream);
    return 0;
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
    if (s_sdl_audio_stream) {
        SDL_DestroyAudioStream(s_sdl_audio_stream);
        s_sdl_audio_stream = NULL;
    }
}

static void audio_backend_poll_reopen(void)
{
    const int want = __atomic_exchange_n(&s_sdl_reopen, 0, __ATOMIC_ACQ_REL);
    if (!want && s_sdl_audio_stream) return;

    const unsigned long long now = ps3_sync_now_ns();
    if (!want && now - s_sdl_reopen_last_ns < 500000000ull) return;
    s_sdl_reopen_last_ns = now;

    const u32 new_ch = sdl_query_host_channels();
    const int unbound = !s_sdl_audio_stream || SDL_GetAudioStreamDevice(s_sdl_audio_stream) == 0;
    if (!unbound && new_ch == s_out_channels) return;

    if (s_sdl_audio_stream) {
        SDL_DestroyAudioStream(s_sdl_audio_stream);
        s_sdl_audio_stream = NULL;
    }
    sdl_open_stream();
}

static void audio_backend_submit(const float* samples, u32 num_samples)
{
    if (s_sdl_audio_stream)
        SDL_PutAudioStreamData(s_sdl_audio_stream, samples,
                               (int)(num_samples * s_out_channels * sizeof(float)));
}

static u32 audio_backend_queued_samples(void)
{
    if (!s_sdl_audio_stream) return 0;
    const int queued = SDL_GetAudioStreamQueued(s_sdl_audio_stream);
    return queued > 0 ? (u32)queued / (s_out_channels * sizeof(float)) : 0;
}

static int audio_backend_room(void)
{
    if (!s_sdl_audio_stream) return -1;
    return audio_backend_queued_samples() < CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static unsigned audio_backend_device_frames(void)
{
    SDL_AudioSpec spec;
    int frames = 0;
    if (s_sdl_audio_stream) SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_sdl_audio_stream), &spec, &frames);
    return (unsigned)frames;
}

#endif

#if AUDIO_BACKEND_WASAPI

static IMMDeviceEnumerator* s_wasapi_enum   = NULL;
static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;
static volatile LONG        s_wasapi_reopen = 0;
static volatile LONG        s_wasapi_host_channels_cache = 0;
static unsigned long long   s_wasapi_reopen_last_ns = 0;

static const GUID k_subformat_ieee_float = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

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
static ULONG STDMETHODCALLTYPE wasapi_notify_addref(IMMNotificationClient* self) { (void)self; return 1; }
static ULONG STDMETHODCALLTYPE wasapi_notify_release(IMMNotificationClient* self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_state(IMMNotificationClient* self, LPCWSTR id, DWORD state) { (void)self; (void)id; (void)state; return S_OK; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_added(IMMNotificationClient* self, LPCWSTR id) { (void)self; (void)id; if (!s_wasapi_client) wasapi_request_reopen(); return S_OK; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_removed(IMMNotificationClient* self, LPCWSTR id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE wasapi_notify_default(IMMNotificationClient* self, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)self; (void)id;
    if (flow == eRender && role == eConsole) wasapi_request_reopen();
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

static void wasapi_close_stream(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
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
    s_wasapi_buf_frames = 0;
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
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    if (local_enum) enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    u32 ch = 2;
    WAVEFORMATEX* mix = NULL;
    if (SUCCEEDED(s_wasapi_client->lpVtbl->GetMixFormat(s_wasapi_client, &mix)) && mix) {
        if (mix->nChannels >= 8) ch = 8;
        else if (mix->nChannels >= 6) ch = 6;
        CoTaskMemFree(mix);
    }
    s_out_channels = ch;
    InterlockedExchange(&s_wasapi_host_channels_cache, (LONG)ch);

    WAVEFORMATEXTENSIBLE wfxe;
    fill_wasapi_format(&wfxe, s_out_channels);

    REFERENCE_TIME buf_duration = 600000;
    { const char* e = getenv("PS3_AUDIO_BUF_MS"); const int ms = e ? atoi(e) : 0;
      if (ms >= 10 && ms <= 500) buf_duration = (REFERENCE_TIME)ms * 10000; }
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
        buf_duration, 0, &wfxe.Format, NULL);

    if (FAILED(hr) && s_out_channels > 2) {
        s_out_channels = 2;
        InterlockedExchange(&s_wasapi_host_channels_cache, 2);
        fill_wasapi_format(&wfxe, 2);
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
            buf_duration, 0, &wfxe.Format, NULL);
    }

    if (FAILED(hr)) {
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            convert, buf_duration, 0, &wfxe.Format, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            wasapi_close_stream();
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        wasapi_close_stream();
        return -1;
    }

    printf("[cellAudio] WASAPI output: %u channels @ %d Hz\n", s_out_channels, CELL_AUDIO_SAMPLE_RATE);
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

static void audio_backend_submit(const float* samples, u32 num_samples)
{
    if (!s_wasapi_render || !s_wasapi_client) return;

    UINT32 padding = 0;
    HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        if (wasapi_device_lost(hr)) wasapi_request_reopen();
        return;
    }

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, samples, num_samples * s_out_channels * sizeof(float));
        hr = s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                                    mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        if (wasapi_device_lost(hr)) wasapi_request_reopen();
    } else if (wasapi_device_lost(hr)) {
        wasapi_request_reopen();
    }
}

static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        if (wasapi_device_lost(hr)) {
            wasapi_request_reopen();
            return -1;
        }
        static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}

#endif

static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        { static int hole_on = -1; static u32 prev_nz[CELL_AUDIO_PORT_MAX];
          if (hole_on < 0) hole_on = getenv("AUDIO_RATE") ? 1 : 0;
          if (hole_on) { int nz = 0;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 2) if (src[s2] != 0.0f) { nz = 1; break; }
            if (!nz && prev_nz[p]) g_audio_holes[p]++;
            prev_nz[p] = nz; } }

        const u32 out_ch = s_out_channels;
        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float* dst = &s_mix_buffer[s * out_ch];
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
                    float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                    float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                    float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                    float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                    float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                    float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
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

    const u32 total_samples = CELL_AUDIO_BLOCK_SAMPLES * s_out_channels;
    for (u32 i = 0; i < total_samples; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

static float audio_mix_peak(void)
{
    float pk = 0.0f;
    const u32 total_samples = CELL_AUDIO_BLOCK_SAMPLES * s_out_channels;
    for (u32 i = 0; i < total_samples; i++) {
        const float a = s_mix_buffer[i] < 0 ? -s_mix_buffer[i] : s_mix_buffer[i];
        if (a > pk) pk = a;
    }
    return pk;
}

static void audio_dump_mix(void)
{
    static FILE* wf = (FILE*)-1;
    if (wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); wf = e ? fopen(e, "wb") : NULL; }
    if (wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * s_out_channels, wf);
}

static unsigned long long g_audio_underruns;

static void audio_mix_loop(void)
{
    ps3_thread_name("audio mix");
    printf("[cellAudio] Mixing thread started\n");

#ifdef _WIN32
    const HRESULT co_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    { HMODULE avrt = LoadLibraryW(L"avrt.dll");
      typedef HANDLE (WINAPI *set_mm_fn)(LPCWSTR, LPDWORD);
      set_mm_fn set_mm = avrt ? (set_mm_fn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
      DWORD task = 0;
      if (set_mm && !set_mm(L"Pro Audio", &task))
          fprintf(stderr, "[cellAudio] MMCSS Pro Audio unavailable (%lu)%c", (unsigned long)GetLastError(), 10); }
#else
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#endif

    const unsigned long long block_ns = 1000000000ull * CELL_AUDIO_BLOCK_SAMPLES / CELL_AUDIO_SAMPLE_RATE;
    unsigned long long t_start = ps3_sync_now_ns();
    unsigned long long blocks = 0;
    while (s_mix_thread_running) {
        audio_backend_poll_reopen();
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
#if AUDIO_BACKEND_WASAPI
        if (room >= 0 && blocks > 64 &&
            (UINT32)room + CELL_AUDIO_BLOCK_SAMPLES / 2 >= s_wasapi_buf_frames) g_audio_underruns++;
#else
        if (room >= 0 && blocks > 64 && !audio_backend_queued_samples()) g_audio_underruns++;
#endif
        if (room < 0) {
            const unsigned long long now = ps3_sync_now_ns(), due = t_start + blocks * block_ns;
            if (now < due) { ps3_sleep_us(1000); continue; }
            if (now - due > 50000000ull) t_start = now - blocks * block_ns;
        }
        blocks++;
        audio_mix_one_block();
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; const float pk = audio_mix_peak();
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        audio_dump_mix();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static unsigned long long t0; static unsigned n, z; static unsigned long long d0, u0;
            const unsigned long long now = ps3_sync_now_ns() / 1000000ull; if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * s_out_channels; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, underruns %llu, device buf %u, holes %u+%u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                        g_audio_underruns - u0,
#if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, g_audio_holes[0], g_audio_holes[1], 10);
#else
                        audio_backend_device_frames(), g_audio_holes[0], g_audio_holes[1], 10);
#endif
                g_audio_holes[0] = g_audio_holes[1] = 0;
                n = z = 0; d0 = g_audio_dropped; u0 = g_audio_underruns; t0 = now; } } }
    }

#ifdef _WIN32
    if (SUCCEEDED(co_hr)) CoUninitialize();
#endif
    printf("[cellAudio] Mixing thread stopped\n");
}

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
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
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

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
    }

    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

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

    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);
    u64 nblk = vm_read64(param_ea + 8);
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
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
    port->read_index  = 0;

    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
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

    s_ports[portNum].buffer = NULL;

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

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    uint32_t cfg = (uint32_t)(uintptr_t)config;
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

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}
