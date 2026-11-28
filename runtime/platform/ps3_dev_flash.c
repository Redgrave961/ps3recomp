#include "ps3_dev_flash.h"
#include "ps3_utf8_fs.h"

#include <stdio.h>
#include <string.h>

int ps3_dev_flash_path(char* out, size_t cap, const char* roots, const char* rel)
{
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const char* first = roots;
    size_t first_n = 0;
    for (const char* r = roots; r; ) {
        const char* e = strchr(r, sep);
        const size_t n = e ? (size_t)(e - r) : strlen(r);
        const char* entry = r;
        r = e ? e + 1 : NULL;
        if (!n) continue;
        if (!first_n) { first = entry; first_n = n; }
        const int len = snprintf(out, cap, "%.*s/%s", (int)n, entry, rel);
        ps3_host_stat_t st;
        if (len >= 0 && (size_t)len < cap && ps3_utf8_stat64(out, &st) == 0) return 1;
    }
    const int len = snprintf(out, cap, "%.*s/%s", (int)first_n, first, rel);
    return len >= 0 && (size_t)len < cap;
}
