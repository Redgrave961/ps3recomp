#ifndef PS3_LOG_H
#define PS3_LOG_H

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define PS3_ISATTY_STDERR() _isatty(_fileno(stderr))
#else
#include <unistd.h>
#define PS3_ISATTY_STDERR() isatty(fileno(stderr))
#endif

#ifdef __cplusplus
static inline int ps3_log_verbose(void)
#else
static __inline int ps3_log_verbose(void)
#endif
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PS3_VERBOSE");
        if (e) v = (e[0] && e[0] != '0') ? 1 : 0;
        else   v = !PS3_ISATTY_STDERR();
    }
    return v;
}

#endif
