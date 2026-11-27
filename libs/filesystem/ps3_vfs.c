#include "ps3_vfs.h"
#include "ps3_aes.h"
#include "../../runtime/memory/vm_track.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../runtime/platform/win32_dirent.h"
#include "../../runtime/platform/ps3_utf8_fs.h"

#ifdef _WIN32
#define VFS_SEEK _fseeki64
#define VFS_TELL _ftelli64
#else
#include <fcntl.h>
#define VFS_SEEK fseeko
#define VFS_TELL ftello
#endif
typedef ps3_host_stat_t host_stat_t;

static const uint8_t PKG_KEY[16] = { 0x2E,0x7B,0x71,0xD7,0xC9,0xC9,0xA1,0x4E,0xA3,0x22,0x1F,0x18,0x88,0x28,0xB8,0xF8 };

typedef struct {
    char* key;
    char* path;
    const char* name;
    uint64_t offset, size;
    int is_dir;
} vfs_entry;

typedef struct {
    char* prefix;
    size_t prefix_len;
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    int pkg;
    ps3_aes_key key;
    uint8_t riv[16];
    uint8_t dbg_key[64];
    uint64_t data_offset;
    uint64_t data_size;
    vfs_entry* entries;
    size_t count, cap;
    vfs_entry root;
} vfs_mount;

static vfs_mount* g_mounts[64];
static int g_mount_count;

struct ps3_vfile {
    FILE* host;
    const vfs_mount* m;
    const vfs_entry* e;
    uint64_t pos;
    uint8_t dirty;
};

typedef struct {
    char* name;
    int is_dir;
} vdir_item;

struct ps3_vdir {
    vdir_item* items;
    size_t count, next;
};

static int image_read(const vfs_mount* m, uint64_t off, void* dst, size_t n)
{
#ifdef _WIN32
    uint8_t* out = (uint8_t*)dst;
    while (n) {
        OVERLAPPED o = { 0 };
        o.Offset = (DWORD)off;
        o.OffsetHigh = (DWORD)(off >> 32);
        const DWORD want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
        DWORD got = 0;
        if (!ReadFile(m->h, out, want, &got, &o) || got != want) return -1;
        out += got;
        off += got;
        n -= got;
    }
    return 0;
#else
    uint8_t* out = (uint8_t*)dst;
    while (n) {
        const ssize_t got = pread(m->fd, out, n, (off_t)off);
        if (got <= 0) return -1;
        out += got;
        off += (uint64_t)got;
        n -= (size_t)got;
    }
    return 0;
#endif
}

static uint32_t vfs_rol(uint32_t v, int c) { return v << c | v >> (32 - c); }

static void vfs_sha1_64(const uint8_t in[64], uint8_t out[20])
{
    static const uint32_t iv[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    uint32_t h[5];
    memcpy(h, iv, sizeof h);
    uint8_t pad[64] = { 0x80 };
    pad[62] = 0x02;
    const uint8_t* blocks[2] = { in, pad };
    for (int blk = 0; blk < 2; blk++) {
        uint32_t w[80];
        const uint8_t* p = blocks[blk];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = vfs_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], d = h[2], e = h[3], f = h[4];
        for (int i = 0; i < 80; i++) {
            const uint32_t t = i < 20 ? ((b & d) | (~b & e)) + 0x5A827999u
                             : i < 40 ? (b ^ d ^ e) + 0x6ED9EBA1u
                             : i < 60 ? ((b & d) | (b & e) | (d & e)) + 0x8F1BBCDCu
                                      : (b ^ d ^ e) + 0xCA62C1D6u;
            const uint32_t tmp = vfs_rol(a, 5) + t + f + w[i];
            f = e; e = d; d = vfs_rol(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += d; h[3] += e; h[4] += f;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static void debug_pkg_xor(const vfs_mount* m, uint64_t blk_index, uint8_t* buf, size_t n)
{
    uint8_t kbuf[64], digest[20];
    memcpy(kbuf, m->dbg_key, 64);
    for (size_t done = 0; done < n; done += 16, blk_index++) {
        for (int b = 0; b < 8; b++) kbuf[0x38 + b] = (uint8_t)(blk_index >> (56 - b * 8));
        vfs_sha1_64(kbuf, digest);
        const size_t chunk = n - done < 16 ? n - done : 16;
        for (size_t i = 0; i < chunk; i++) buf[done + i] ^= digest[i];
    }
}

static int pkg_read(const vfs_mount* m, uint64_t off, void* dst, size_t n)
{
    uint8_t* out = (uint8_t*)dst;
    uint8_t block[65536];
    while (n) {
        const uint64_t base = off & ~(uint64_t)15;
        const size_t lead = (size_t)(off - base);
        size_t span = lead + n;
        if (span > sizeof block) span = sizeof block;
        const size_t padded = (span + 15) & ~(size_t)15;
        size_t to_read = padded;
        if (m->data_size && base < m->data_size && base + to_read > m->data_size)
            to_read = (size_t)(m->data_size - base);
        if (to_read < span) to_read = span;
        if (image_read(m, m->data_offset + base, block, to_read)) return -1;
        if (to_read < padded) memset(block + to_read, 0, padded - to_read);
        if (m->pkg == 2) debug_pkg_xor(m, base / 16, block, padded);
        else ps3_aes_ctr(&m->key, m->riv, base / 16, block, block, padded);
        const size_t take = span - lead;
        memcpy(out, block + lead, take);
        out += take;
        off += take;
        n -= take;
    }
    return 0;
}

static int entry_read(const vfs_mount* m, const vfs_entry* e, uint64_t off, void* dst, size_t n)
{
    return m->pkg ? pkg_read(m, e->offset + off, dst, n) : image_read(m, e->offset + off, dst, n);
}

static size_t normalize_into(const char* path, char* out, size_t cap)
{
    const size_t n = strlen(path);
    if (n + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = path[i] == '\\' ? '/' : (char)tolower((unsigned char)path[i]);
        if (c == '/' && o && out[o - 1] == '/') continue;
        out[o++] = c;
    }
    while (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return o + 1;
}

static char* normalize(const char* path)
{
    const size_t n = strlen(path);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    normalize_into(path, out, n + 1);
    return out;
}

static int entry_cmp(const void* a, const void* b)
{
    return strcmp(((const vfs_entry*)a)->key, ((const vfs_entry*)b)->key);
}

static int add_entry(vfs_mount* m, const char* rel, uint64_t offset, uint64_t size, int is_dir)
{
    if (m->count == m->cap) {
        const size_t cap = m->cap ? m->cap * 2 : 256;
        vfs_entry* grown = (vfs_entry*)realloc(m->entries, cap * sizeof *grown);
        if (!grown) return -1;
        m->entries = grown;
        m->cap = cap;
    }
    vfs_entry* e = &m->entries[m->count];
    const size_t n = strlen(rel);
    e->key = normalize(rel);
    e->path = (char*)malloc(n + 1);
    if (!e->key || !e->path) {
        free(e->key);
        free(e->path);
        return -1;
    }
    memcpy(e->path, rel, n + 1);
    e->offset = offset;
    e->size = size;
    e->is_dir = is_dir;
    m->count++;
    return 0;
}

static int finish_mount(vfs_mount* m)
{
    const size_t files = m->count;
    for (size_t i = 0; i < files; i++) {
        for (const char* s = strchr(m->entries[i].path, '/'); s; s = strchr(s + 1, '/')) {
            char parent[1024];
            const size_t n = (size_t)(s - m->entries[i].path);
            if (n >= sizeof parent) return -1;
            memcpy(parent, m->entries[i].path, n);
            parent[n] = 0;
            if (add_entry(m, parent, 0, 0, 1)) return -1;
        }
    }
    qsort(m->entries, m->count, sizeof *m->entries, entry_cmp);
    size_t kept = 0;
    for (size_t i = 0; i < m->count; i++) {
        if (kept && !strcmp(m->entries[kept - 1].key, m->entries[i].key)) {
            if (m->entries[i].is_dir) {
                free(m->entries[i].key);
                free(m->entries[i].path);
                m->entries[i].key = NULL;
                m->entries[i].path = NULL;
                continue;
            }
            free(m->entries[kept - 1].key);
            free(m->entries[kept - 1].path);
            m->entries[kept - 1] = m->entries[i];
            m->entries[i].key = NULL;
            m->entries[i].path = NULL;
            continue;
        }
        if (kept != i) {
            m->entries[kept] = m->entries[i];
            m->entries[i].key = NULL;
            m->entries[i].path = NULL;
        }
        kept++;
    }
    m->count = kept;
    for (size_t i = 0; i < m->count; i++) {
        const char* slash = strrchr(m->entries[i].path, '/');
        m->entries[i].name = slash ? slash + 1 : m->entries[i].path;
    }
    m->root.key = (char*)"";
    m->root.is_dir = 1;
    return 0;
}

static vfs_mount* open_image(const char* host_dir, const char* image)
{
    vfs_mount* m = (vfs_mount*)calloc(1, sizeof *m);
    if (!m) return NULL;
#ifdef _WIN32
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, image, -1, wide, 1024) ||
        (m->h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL)) == INVALID_HANDLE_VALUE) {
        free(m);
        return NULL;
    }
#else
    if ((m->fd = open(image, O_RDONLY)) < 0) {
        free(m);
        return NULL;
    }
    posix_fadvise(m->fd, 0, 0, POSIX_FADV_RANDOM);
#endif
    m->prefix = normalize(host_dir);
    m->prefix_len = m->prefix ? strlen(m->prefix) : 0;
    return m;
}

static void close_image(vfs_mount* m)
{
#ifdef _WIN32
    CloseHandle(m->h);
#else
    close(m->fd);
#endif
    for (size_t i = 0; i < m->count; i++) {
        free(m->entries[i].key);
        free(m->entries[i].path);
    }
    free(m->entries);
    free(m->prefix);
    free(m);
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int iso_walk(vfs_mount* m, uint32_t lba, uint32_t size, const char* dir, int depth, int joliet)
{
    if (depth > 32 || size > (64u << 20)) return -1;
    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) return -1;
    if (image_read(m, (uint64_t)lba * 2048, data, size)) {
        free(data);
        return -1;
    }
    int rc = 0;
    for (uint32_t pos = 0; pos < size && !rc;) {
        const uint8_t len = data[pos];
        if (!len) {
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (pos + len > size || len < 34) {
            rc = -1;
            break;
        }
        const uint8_t* r = data + pos;
        pos += len;
        const uint8_t name_len = r[32];
        if (33u + name_len > len || (name_len == 1 && r[33] <= 1)) continue;
        char name[256], path[1024];
        if (joliet) {
            size_t o = 0;
            for (uint8_t i = 0; i + 1 < name_len && o + 3 < sizeof name; i += 2) {
                const unsigned c = (unsigned)r[33 + i] << 8 | r[34 + i];
                if (c < 0x80) name[o++] = (char)c;
                else if (c < 0x800) { name[o++] = (char)(0xC0 | c >> 6); name[o++] = (char)(0x80 | (c & 0x3F)); }
                else { name[o++] = (char)(0xE0 | c >> 12); name[o++] = (char)(0x80 | (c >> 6 & 0x3F)); name[o++] = (char)(0x80 | (c & 0x3F)); }
            }
            name[o] = 0;
        } else {
            memcpy(name, r + 33, name_len);
            name[name_len] = 0;
        }
        char* semi = strchr(name, ';');
        if (semi) *semi = 0;
        size_t nl = strlen(name);
        if (nl && name[nl - 1] == '.') name[--nl] = 0;
        snprintf(path, sizeof path, "%s%s%s", dir, *dir ? "/" : "", name);
        if (r[25] & 2) {
            rc = add_entry(m, path, 0, 0, 1);
            if (!rc) rc = iso_walk(m, le32(r + 2), le32(r + 10), path, depth + 1, joliet);
        } else if (m->count && !m->entries[m->count - 1].is_dir && !strcmp(m->entries[m->count - 1].path, path)) {
            m->entries[m->count - 1].size += le32(r + 10);
        } else {
            rc = add_entry(m, path, (uint64_t)le32(r + 2) * 2048, le32(r + 10), 0);
        }
    }
    free(data);
    return rc;
}

static int add_mount(vfs_mount* m)
{
    if (g_mount_count == (int)(sizeof g_mounts / sizeof g_mounts[0]) || finish_mount(m)) return -1;
    g_mounts[g_mount_count++] = m;
    return 0;
}

void ps3_vfs_unmount_all(void)
{
    while (g_mount_count) close_image(g_mounts[--g_mount_count]);
}

int ps3_vfs_mount_iso(const char* host_dir, const char* iso_path)
{
    vfs_mount* m = open_image(host_dir, iso_path);
    if (!m || !m->prefix) {
        if (m) close_image(m);
        return PS3_VFS_EOPEN;
    }
    uint8_t vd[2048], root[34] = { 0 };
    int found = 0, joliet = 0;
    for (uint32_t s = 16; s < 64 && !image_read(m, (uint64_t)s * 2048, vd, sizeof vd) && !memcmp(vd + 1, "CD001", 5) && vd[0] != 255; s++) {
        const int is_joliet = vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E');
        if ((vd[0] == 1 && !found) || is_joliet) {
            memcpy(root, vd + 156, sizeof root);
            found = 1;
            joliet = is_joliet;
        }
    }
    if (!found || iso_walk(m, le32(root + 2), le32(root + 10), "", 0, joliet) || add_mount(m)) {
        close_image(m);
        return PS3_VFS_EFORMAT;
    }
    return PS3_VFS_OK;
}

static vfs_mount* open_pkg(const char* game_dir, const char* dir_name, const char* pkg_path, char content_id[37], int* err)
{
    vfs_mount* m = open_image("", pkg_path);
    *err = PS3_VFS_EOPEN;
    if (!m) return NULL;
    *err = PS3_VFS_EFORMAT;
    uint8_t h[0x80];
    if (image_read(m, 0, h, sizeof h) || memcmp(h, "\x7FPKG", 4) || (h[4] != 0x80 && h[4] != 0x00)) {
        close_image(m);
        return NULL;
    }
    const uint32_t items = ps3_be32(h + 0x14);
    m->pkg = h[4] == 0x80 ? 1 : 2;
    m->data_offset = ps3_be64(h + 0x20);
    const uint64_t data_size = ps3_be64(h + 0x28);
    m->data_size = data_size;
    if (m->pkg == 1) {
        memcpy(m->riv, h + 0x70, 16);
        ps3_aes128_init(&m->key, PKG_KEY);
    } else {
        memset(m->dbg_key, 0, 64);
        memcpy(m->dbg_key + 0x00, h + 0x60, 8);
        memcpy(m->dbg_key + 0x08, h + 0x60, 8);
        memcpy(m->dbg_key + 0x10, h + 0x68, 8);
        memcpy(m->dbg_key + 0x18, h + 0x68, 8);
    }
    char cid[37];
    memcpy(cid, h + 0x30, 36);
    cid[36] = 0;
    if (content_id) memcpy(content_id, cid, 37);
    char prefix[1100];
    if (dir_name && *dir_name) snprintf(prefix, sizeof prefix, "%s/%s", game_dir, dir_name);
    else snprintf(prefix, sizeof prefix, "%s/%.9s", game_dir, cid + 7);
    free(m->prefix);
    m->prefix = normalize(prefix);
    m->prefix_len = m->prefix ? strlen(m->prefix) : 0;
    const uint64_t table = (uint64_t)items * 0x20;
    uint8_t* t = table <= data_size && items < 100000 ? (uint8_t*)malloc(table ? (size_t)table : 1) : NULL;
    int rc = m->prefix && t && (!table || !pkg_read(m, 0, t, (size_t)table)) ? 0 : -1;
    for (uint32_t i = 0; i < items && !rc; i++) {
        const uint8_t* e = t + i * 0x20;
        const uint32_t name_off = ps3_be32(e), name_len = ps3_be32(e + 4);
        const uint64_t off = ps3_be64(e + 8), size = ps3_be64(e + 16);
        const uint8_t ftype = e[0x1B] & 0x1F;
        const int is_dir = (ftype == 4 || ftype == 0x12);
        char name[512];
        if (!name_len || name_len >= sizeof name || name_off > data_size || (uint64_t)name_len > data_size - name_off ||
            (!is_dir && size > 0 && (off > data_size || size > data_size - off)) ||
            pkg_read(m, name_off, name, name_len)) {
            rc = -1;
            break;
        }
        name[name_len] = 0;
        char* p = name;
        while (*p == '/' || *p == '\\') p++;
        size_t nl = strlen(p);
        while (nl && (p[nl - 1] == '/' || p[nl - 1] == '\\')) p[--nl] = 0;
        if (!nl) continue;
        rc = add_entry(m, p, is_dir ? 0 : off, is_dir ? 0 : size, is_dir);
    }
    free(t);
    if (rc) {
        close_image(m);
        return NULL;
    }
    return m;
}

int ps3_vfs_mount_pkg_as(const char* game_dir, const char* dir_name, const char* pkg_path, char content_id[37])
{
    int err;
    vfs_mount* m = open_pkg(game_dir, dir_name, pkg_path, content_id, &err);
    if (!m) return err;
    if (add_mount(m)) {
        close_image(m);
        return PS3_VFS_EFORMAT;
    }
    return PS3_VFS_OK;
}

int ps3_vfs_mount_pkg(const char* game_dir, const char* pkg_path, char content_id[37])
{
    return ps3_vfs_mount_pkg_as(game_dir, NULL, pkg_path, content_id);
}

int ps3_vfs_pkg_info(const char* pkg_path, char content_id[37], int* has_eboot)
{
    int err;
    vfs_mount* m = open_pkg("", NULL, pkg_path, content_id, &err);
    if (!m) return err;
    *has_eboot = 0;
    for (size_t i = 0; i < m->count && !*has_eboot; i++) *has_eboot = !strcmp(m->entries[i].key, "usrdir/eboot.bin");
    close_image(m);
    return PS3_VFS_OK;
}

int ps3_vfs_enum_last_mount(void (*cb)(const char* full_path, void* user), void* user)
{
    if (!cb || g_mount_count <= 0) return -1;
    const vfs_mount* m = g_mounts[g_mount_count - 1];
    char full[1400];
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].is_dir) continue;
        snprintf(full, sizeof full, "%s/%s", m->prefix, m->entries[i].path);
        cb(full, user);
    }
    return 0;
}

static const vfs_entry* lookup(const char* path, const vfs_mount** owner)
{
    char stack_buf[1024];
    char* heap_key = NULL;
    char* key = normalize_into(path, stack_buf, sizeof stack_buf) ? stack_buf : (heap_key = normalize(path));
    if (!key) return NULL;
    const vfs_entry* found = NULL;
    for (int i = g_mount_count - 1; i >= 0 && !found; i--) {
        const vfs_mount* m = g_mounts[i];
        if (strncmp(key, m->prefix, m->prefix_len)) continue;
        if (!key[m->prefix_len]) {
            found = &m->root;
        } else if (key[m->prefix_len] == '/') {
            vfs_entry probe;
            probe.key = key + m->prefix_len + 1;
            found = (const vfs_entry*)bsearch(&probe, m->entries, m->count, sizeof *m->entries, entry_cmp);
        }
        if (found) *owner = m;
    }
    free(heap_key);
    return found;
}

int ps3_vfs_stat(const char* path, ps3_vstat* st)
{
    host_stat_t hs;
    if (ps3_utf8_stat64(path, &hs) == 0) {
        st->size = (uint64_t)hs.st_size;
        st->is_dir = (hs.st_mode & S_IFMT) == S_IFDIR;
        st->atime = (int64_t)hs.st_atime;
        st->mtime = (int64_t)hs.st_mtime;
        st->ctime = (int64_t)hs.st_ctime;
        return 0;
    }
    const vfs_mount* m = NULL;
    const vfs_entry* e = lookup(path, &m);
    if (!e) return -1;
    memset(st, 0, sizeof *st);
    st->size = e->size;
    st->is_dir = e->is_dir;
    return 0;
}

ps3_vfile* ps3_vfile_adopt(FILE* f)
{
    if (!f) return NULL;
    ps3_vfile* v = (ps3_vfile*)calloc(1, sizeof *v);
    if (!v) {
        fclose(f);
        return NULL;
    }
    v->host = f;
    return v;
}

ps3_vfile* ps3_vfile_open(const char* path, const char* mode)
{
    FILE* f = ps3_utf8_fopen(path, mode);
    if (f) return ps3_vfile_adopt(f);
    if (strpbrk(mode, "wa+")) return NULL;
    const vfs_mount* m = NULL;
    const vfs_entry* e = lookup(path, &m);
    if (!e || e->is_dir) return NULL;
    ps3_vfile* v = (ps3_vfile*)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->m = m;
    v->e = e;
    return v;
}

size_t ps3_vfile_read(void* dst, size_t n, ps3_vfile* f)
{
    if (!f->host) {
        if (f->pos >= f->e->size) return 0;
        if (n > f->e->size - f->pos) n = (size_t)(f->e->size - f->pos);
    }
    vm_io_begin(dst, n);
    const size_t got = f->host ? fread(dst, 1, n, f->host) : entry_read(f->m, f->e, f->pos, dst, n) ? 0 : n;
    vm_io_end(dst, n);
    if (!f->host) f->pos += got;
    return got;
}

size_t ps3_vfile_write(const void* src, size_t n, ps3_vfile* f)
{
    if (!f->host) return 0;
    vm_commit_host(src, n);
    const size_t wrote = fwrite(src, 1, n, f->host);
    if (wrote) f->dirty = 1;
    return wrote;
}

int ps3_vfile_seek(ps3_vfile* f, int64_t offset, int whence)
{
    if (f->host) return VFS_SEEK(f->host, offset, whence);
    const int64_t base = whence == SEEK_END ? (int64_t)f->e->size : whence == SEEK_CUR ? (int64_t)f->pos : 0;
    if (base + offset < 0) return -1;
    f->pos = (uint64_t)(base + offset);
    return 0;
}

int64_t ps3_vfile_tell(ps3_vfile* f)
{
    return f->host ? (int64_t)VFS_TELL(f->host) : (int64_t)f->pos;
}

int ps3_vfile_fstat(ps3_vfile* f, ps3_vstat* st)
{
    memset(st, 0, sizeof *st);
    if (!f->host) {
        st->size = f->e->size;
        return 0;
    }
    if (f->dirty) {
        fflush(f->host);
        f->dirty = 0;
    }
    host_stat_t hs;
#ifdef _WIN32
    if (_fstat64(_fileno(f->host), &hs) == 0) {
#else
    if (fstat(fileno(f->host), &hs) == 0) {
#endif
        st->size  = (uint64_t)hs.st_size;
        st->atime = (int64_t)hs.st_atime;
        st->mtime = (int64_t)hs.st_mtime;
        st->ctime = (int64_t)hs.st_ctime;
    } else {
        const int64_t cur = VFS_TELL(f->host);
        VFS_SEEK(f->host, 0, SEEK_END);
        st->size = (uint64_t)VFS_TELL(f->host);
        VFS_SEEK(f->host, cur, SEEK_SET);
    }
    return 0;
}

FILE* ps3_vfile_host(ps3_vfile* f)
{
    return f->host;
}

void ps3_vfile_close(ps3_vfile* f)
{
    if (!f) return;
    if (f->host) fclose(f->host);
    free(f);
}

static int vdir_add(ps3_vdir* d, const char* name, int is_dir, size_t* cap)
{
    for (size_t i = 0; i < d->count; i++) {
        const char* a = d->items[i].name;
        const char* b = name;
        while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (!*a && !*b) return 0;
    }
    if (d->count == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        vdir_item* grown = (vdir_item*)realloc(d->items, *cap * sizeof *grown);
        if (!grown) return -1;
        d->items = grown;
    }
    const size_t n = strlen(name);
    char* copy = (char*)malloc(n + 1);
    if (!copy) return -1;
    memcpy(copy, name, n + 1);
    d->items[d->count].name = copy;
    d->items[d->count].is_dir = is_dir;
    d->count++;
    return 0;
}

ps3_vdir* ps3_vdir_open(const char* path)
{
    ps3_vdir* d = (ps3_vdir*)calloc(1, sizeof *d);
    if (!d) return NULL;
    size_t cap = 0;
    int exists = 0;
    DIR* h = opendir(path);
    if (h) {
        exists = 1;
        for (struct dirent* e; (e = readdir(h));) {
            char full[1300];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            const int dot = e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]));
            host_stat_t st;
            vdir_add(d, e->d_name, dot || (ps3_utf8_stat64(full, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR), &cap);
        }
        closedir(h);
    }
    char* key = normalize(path);
    for (int i = 0; key && i < g_mount_count; i++) {
        const vfs_mount* m = g_mounts[i];
        if (strncmp(key, m->prefix, m->prefix_len) || (key[m->prefix_len] && key[m->prefix_len] != '/')) continue;
        const char* rel = key[m->prefix_len] ? key + m->prefix_len + 1 : "";
        const size_t rl = strlen(rel);
        if (*rel) {
            vfs_entry probe;
            probe.key = (char*)rel;
            const vfs_entry* self = (const vfs_entry*)bsearch(&probe, m->entries, m->count, sizeof *m->entries, entry_cmp);
            if (!self || !self->is_dir) continue;
        }
        if (!exists) {
            vdir_add(d, ".", 1, &cap);
            vdir_add(d, "..", 1, &cap);
        }
        exists = 1;
        size_t j = 0;
        if (rl) {
            size_t lo = 0, hi = m->count;
            while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                const int cmp = strncmp(m->entries[mid].key, rel, rl);
                if (cmp < 0 || (cmp == 0 && (unsigned char)m->entries[mid].key[rl] < (unsigned char)'/')) lo = mid + 1;
                else hi = mid;
            }
            j = lo;
        }
        for (; j < m->count; j++) {
            const vfs_entry* e = &m->entries[j];
            if (rl && (strncmp(e->key, rel, rl) || e->key[rl] != '/')) break;
            const char* tail = e->key + (rl ? rl + 1 : 0);
            if (!*tail || strchr(tail, '/')) continue;
            vdir_add(d, e->name, e->is_dir, &cap);
        }
    }
    free(key);
    if (!exists) {
        ps3_vdir_close(d);
        return NULL;
    }
    return d;
}

const char* ps3_vdir_next(ps3_vdir* d, int* is_dir)
{
    if (d->next >= d->count) return NULL;
    if (is_dir) *is_dir = d->items[d->next].is_dir;
    return d->items[d->next++].name;
}

void ps3_vdir_close(ps3_vdir* d)
{
    if (!d) return;
    for (size_t i = 0; i < d->count; i++) free(d->items[i].name);
    free(d->items);
    free(d);
}
