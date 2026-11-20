#include "ps3_dev_flash.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void ps3_dev_flash_path(char* out, size_t cap, const char* roots, const char* rel)
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
        snprintf(out, cap, "%.*s/%s", (int)n, entry, rel);
        struct stat st;
        if (stat(out, &st) == 0) return;
    }
    snprintf(out, cap, "%.*s/%s", (int)first_n, first, rel);
}
