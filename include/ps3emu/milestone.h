#ifndef PS3EMU_MILESTONE_H
#define PS3EMU_MILESTONE_H

#ifdef __cplusplus
extern "C" {
#endif

void ps3_ms(const char* key);

void ps3_msf(const char* fmt, ...);

void ps3_ms_kv(const char* key, long long value);

void ps3_ms_dump(void);

#ifdef __cplusplus
}
#endif

#endif
