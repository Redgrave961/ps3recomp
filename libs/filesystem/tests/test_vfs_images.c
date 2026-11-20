#include "../ps3_self.h"
#include "../ps3_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_files, g_absent;
static uint64_t g_bytes;

static uint8_t* slurp_host(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    *size = (size_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc(*size ? *size : 1);
    if (d && fread(d, 1, *size, f) != *size) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

static void compare_file(const char* mounted, const char* extracted)
{
    ps3_vfile* v = ps3_vfile_open(mounted, "rb");
    FILE* f = fopen(extracted, "rb");
    if (v && !f) {
        printf("not in the extracted copy: %s\n", mounted);
        g_absent++;
        ps3_vfile_close(v);
        return;
    }
    if (!v || !f) {
        printf("FAIL open %s (%s) / %s (%s)\n", mounted, v ? "ok" : "missing", extracted, f ? "ok" : "missing");
        g_failed++;
        ps3_vfile_close(v);
        if (f) fclose(f);
        return;
    }
    static uint8_t a[1 << 20], b[1 << 20];
    for (;;) {
        const size_t na = ps3_vfile_read(a, sizeof a, v), nb = fread(b, 1, sizeof b, f);
        if (na != nb || memcmp(a, b, na)) {
            printf("FAIL content %s\n", mounted);
            g_failed++;
            break;
        }
        g_bytes += na;
        if (!na) break;
    }
    g_files++;
    ps3_vfile_close(v);
    fclose(f);
}

static void walk(const char* mounted, const char* extracted)
{
    ps3_vdir* d = ps3_vdir_open(mounted);
    if (!d) {
        printf("FAIL opendir %s\n", mounted);
        g_failed++;
        return;
    }
    int is_dir;
    for (const char* name; (name = ps3_vdir_next(d, &is_dir));) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char m[1200], e[1200];
        snprintf(m, sizeof m, "%s/%s", mounted, name);
        snprintf(e, sizeof e, "%s/%s", extracted, name);
        ps3_vstat st;
        if (ps3_vfs_stat(m, &st) || st.is_dir != is_dir) {
            printf("FAIL stat %s\n", m);
            g_failed++;
        }
        if (is_dir) walk(m, e);
        else compare_file(m, e);
    }
    ps3_vdir_close(d);
}

int main(int argc, char** argv)
{
    if (argc < 6) {
        printf("usage: %s <iso> <disc root> <hdd0 game dir> <EBOOT.elf> <pkg>...\n", argv[0]);
        return 2;
    }
    const char* bdvd = "Z:/vfs_test/dev_bdvd";
    const char* game = "Z:/vfs_test/dev_hdd0/game";
    if (ps3_vfs_mount_iso(bdvd, argv[1])) {
        printf("FAIL mount iso\n");
        return 1;
    }
    char title_dir[1100] = "";
    for (int i = 5; i < argc; i++) {
        char cid[37];
        if (ps3_vfs_mount_pkg(game, argv[i], cid)) {
            printf("FAIL mount %s\n", argv[i]);
            return 1;
        }
        snprintf(title_dir, sizeof title_dir, "%s/%.9s", game, cid + 7);
    }
    walk(bdvd, argv[2]);
    walk(title_dir, argv[3]);

    char eboot[1200];
    snprintf(eboot, sizeof eboot, "%s/USRDIR/EBOOT.BIN", title_dir);
    ps3_vfile* v = ps3_vfile_open(eboot, "rb");
    ps3_vstat st;
    uint8_t* self = v && !ps3_vfile_fstat(v, &st) ? (uint8_t*)malloc((size_t)st.size) : NULL;
    const int read_ok = self && ps3_vfile_read(self, (size_t)st.size, v) == st.size;
    ps3_vfile_close(v);
    uint8_t* elf = NULL;
    size_t elf_size = 0, ref_size = 0;
    uint8_t* ref = slurp_host(argv[4], &ref_size);
    const int rc = read_ok ? ps3_self_decrypt(self, (size_t)st.size, &elf, &elf_size) : -100;
    const int same = rc == 0 && ref && elf_size == ref_size && !memcmp(elf, ref, ref_size);
    printf("EBOOT decrypt rc=%d size=%zu %s\n", rc, elf_size, same ? "identical" : "DIFFERENT");
    if (!same) g_failed++;
    printf("%d files, %llu bytes compared, %d not in the extracted copy, %d failures\n", g_files, (unsigned long long)g_bytes, g_absent, g_failed);
    return g_failed ? 1 : 0;
}
