#ifndef PS3EMU_MILESTONE_H
#define PS3EMU_MILESTONE_H

#ifdef __cplusplus
extern "C" {
#endif

extern int g_ps3_ms_enabled;

void ps3_ms(const char* key);

void ps3_msf(const char* fmt, ...);

void ps3_ms_kv(const char* key, long long value);

void ps3_ms_dump(void);

#define ps3_ms(key) do { if (__builtin_expect(__atomic_load_n(&g_ps3_ms_enabled, __ATOMIC_RELAXED) != 0, 0)) (ps3_ms)(key); } while (0)
#define ps3_msf(...) do { if (__builtin_expect(__atomic_load_n(&g_ps3_ms_enabled, __ATOMIC_RELAXED) != 0, 0)) (ps3_msf)(__VA_ARGS__); } while (0)
#define ps3_ms_kv(key, value) do { if (__builtin_expect(__atomic_load_n(&g_ps3_ms_enabled, __ATOMIC_RELAXED) != 0, 0)) (ps3_ms_kv)((key), (value)); } while (0)

#ifdef __cplusplus
}
#endif

#endif
