#ifndef PS3_STDIO_H
#define PS3_STDIO_H

#ifdef __cplusplus
extern "C" {
#endif

void ps3_stdio_init(void);

void ps3_stdio_flush(void);

void ps3_stdio_flush_crash(void);

#ifdef __cplusplus
}
#endif

#endif
