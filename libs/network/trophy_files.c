/*
 * trophy_files -- a title's PS3 trophy files on the host. See trophy_files.h
 * for the formats. Written from the public format documentation; nothing here
 * comes from an emulator's code.
 */
#include "trophy_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- big-endian helpers ---- */

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

/* ---- TROPHY.TRP ---- */

#define TRP_MAGIC 0xDCA24D00u

int trp_open(const char* path, trp_index* out)
{
    memset(out, 0, sizeof *out);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[0x40];
    int ok = fread(h, 1, sizeof h, f) == sizeof h && rd32(h) == TRP_MAGIC;
    const uint64_t fsize = ok ? rd64(h + 8) : 0;
    const uint32_t count = ok ? rd32(h + 16) : 0, esz = ok ? rd32(h + 20) : 0;
    if (ok && (esz < 48 || count > TROPHY_TRP_MAX_FILES)) ok = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint8_t e[48];
        if (fseek(f, (long)(0x40 + (uint64_t)i * esz), SEEK_SET) || fread(e, 1, 48, f) != 48) { ok = 0; break; }
        trp_entry* d = &out->e[out->n];
        memcpy(d->name, e, 32);
        d->name[32] = 0;
        d->offset = rd64(e + 32);
        d->size = rd64(e + 40);
        if (!d->name[0] || d->offset + d->size > fsize || d->size > (64u << 20)) continue;   /* skip a bad one */
        out->n++;
    }
    fclose(f);
    if (!ok) { out->n = 0; return 0; }
    snprintf(out->path, sizeof out->path, "%s", path);
    out->file_size = fsize;
    out->entry_count = count;
    out->entry_size = esz;
    return 1;
}

const trp_entry* trp_find_entry(const trp_index* t, const char* name)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->e[i].name, name)) return &t->e[i];
    return NULL;
}

char* trp_read_entry_data(const trp_index* t, const char* name, size_t* size)
{
    const trp_entry* e = trp_find_entry(t, name);
    if (!e) return NULL;
    FILE* f = fopen(t->path, "rb");
    if (!f) return NULL;
    char* d = (char*)malloc((size_t)e->size + 1);
    if (d && (fseek(f, (long)e->offset, SEEK_SET) || fread(d, 1, (size_t)e->size, f) != (size_t)e->size)) { free(d); d = NULL; }
    fclose(f);
    if (!d) return NULL;
    d[e->size] = 0;
    if (size) *size = (size_t)e->size;
    return d;
}

/* ---- TROP*.SFM ---- */

static void utf8_put(char** o, char* end, unsigned c)
{
    char b[4];
    int n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 0x3F)); b[2] = (char)(0x80 | (c >> 6 & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
    if (*o + n >= end) return;
    memcpy(*o, b, (size_t)n);
    *o += n;
}

/* Text between a and b, entities decoded, whitespace trimmed. */
static void xml_text(const char* a, const char* b, char* out, size_t cap)
{
    char* o = out;
    char* end = out + cap;
    while (a < b && (*a == ' ' || *a == '\n' || *a == '\r' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\n' || b[-1] == '\r' || b[-1] == '\t')) b--;
    while (a < b && o + 1 < end) {
        if (*a == '&') {
            const char* semi = memchr(a, ';', (size_t)(b - a));
            if (semi && semi - a <= 10) {
                const size_t k = (size_t)(semi - a - 1);
                unsigned c = 0;
                if (k == 3 && !strncmp(a + 1, "amp", 3)) c = '&';
                else if (k == 2 && !strncmp(a + 1, "lt", 2)) c = '<';
                else if (k == 2 && !strncmp(a + 1, "gt", 2)) c = '>';
                else if (k == 4 && !strncmp(a + 1, "quot", 4)) c = '"';
                else if (k == 4 && !strncmp(a + 1, "apos", 4)) c = '\'';
                else if (k >= 2 && a[1] == '#') c = (unsigned)strtoul(a + (a[2] == 'x' || a[2] == 'X' ? 3 : 2), NULL, a[2] == 'x' || a[2] == 'X' ? 16 : 10);
                if (c) { utf8_put(&o, end, c); a = semi + 1; continue; }
            }
        }
        *o++ = *a++;
    }
    *o = 0;
}

/* <tag>text</tag> inside [p, end): 1 when found. */
static int xml_elem(const char* p, const char* end, const char* tag, char* out, size_t cap)
{
    char open[40], close[40];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const size_t ol = strlen(open);
    for (const char* s = p; s + ol <= end; s++) {
        if (memcmp(s, open, ol)) continue;
        const char* a = s + ol;
        for (const char* e = a; e + strlen(close) <= end; e++)
            if (!memcmp(e, close, strlen(close))) { xml_text(a, e, out, cap); return 1; }
        return 0;
    }
    return 0;
}

/* attr="value" inside the tag [p, end). */
static int xml_attr(const char* p, const char* end, const char* name, char* out, size_t cap)
{
    const size_t nl = strlen(name);
    for (const char* s = p; s + nl + 2 <= end; s++) {
        if (memcmp(s, name, nl) || s[nl] != '=' || (s[nl + 1] != '"' && s[nl + 1] != '\'')) continue;
        if (s > p && s[-1] != ' ' && s[-1] != '\t' && s[-1] != '\n' && s[-1] != '\r') continue;
        const char q = s[nl + 1];
        const char* a = s + nl + 2;
        const char* e = memchr(a, q, (size_t)(end - a));
        if (!e) return 0;
        xml_text(a, e, out, cap);
        return 1;
    }
    return 0;
}

static trophy_def* set_get(trophy_set* s, int id)
{
    for (int i = 0; i < s->n; i++)
        if (s->t[i].id == id) return &s->t[i];
    if (s->n >= TROPHY_FILES_MAX) return NULL;
    trophy_def* d = &s->t[s->n++];
    memset(d, 0, sizeof *d);
    d->id = id;
    d->grade = 'B';
    d->pid = -1;
    return d;
}

int trophy_sfm_parse(const char* xml, trophy_set* into)
{
    if (!xml) return into->n;
    const char* end = xml + strlen(xml);
    char buf[256];
    const char* conf = strstr(xml, "<trophyconf");
    if (!conf) return into->n;
    const char* first = strstr(conf, "<trophy ");
    const char* head_end = first ? first : end;
    if (xml_elem(conf, head_end, "npcommid", buf, sizeof buf) && buf[0])
        snprintf(into->comm_id, sizeof into->comm_id, "%s", buf);
    if (xml_elem(conf, head_end, "title-name", buf, sizeof buf) && buf[0])
        snprintf(into->title, sizeof into->title, "%s", buf);
    {
        char detail[sizeof into->title_detail];
        if (xml_elem(conf, head_end, "title-detail", detail, sizeof detail) && detail[0])
            snprintf(into->title_detail, sizeof into->title_detail, "%s", detail);
    }
    for (const char* p = first; p; p = strstr(p + 1, "<trophy ")) {
        const char* gt = strchr(p, '>');
        if (!gt) break;
        if (!xml_attr(p, gt, "id", buf, sizeof buf)) continue;
        trophy_def* d = set_get(into, atoi(buf));
        if (!d) break;
        if (xml_attr(p, gt, "ttype", buf, sizeof buf) && buf[0]) d->grade = buf[0];
        if (xml_attr(p, gt, "pid", buf, sizeof buf) && buf[0]) d->pid = atoi(buf);
        if (xml_attr(p, gt, "hidden", buf, sizeof buf)) d->hidden = !strcmp(buf, "yes");
        if (gt[-1] == '/') continue;                           /* <trophy .../>: attributes only */
        const char* close = strstr(gt, "</trophy>");
        if (!close) close = end;
        xml_elem(gt, close, "name", d->name, sizeof d->name);
        xml_elem(gt, close, "detail", d->detail, sizeof d->detail);
    }
    return into->n;
}

int trophy_set_load(trophy_set* s, const trp_index* t, const char* lang_file)
{
    memset(s, 0, sizeof *s);
    char* conf = trp_read_entry_data(t, "TROPCONF.SFM", NULL);
    trophy_sfm_parse(conf, s);
    free(conf);
    char* text = lang_file ? trp_read_entry_data(t, lang_file, NULL) : NULL;
    if (!text) text = trp_read_entry_data(t, "TROP.SFM", NULL);
    trophy_sfm_parse(text, s);
    free(text);
    return s->n;
}

const trophy_def* trophy_set_find(const trophy_set* s, int id)
{
    for (int i = 0; i < s->n; i++)
        if (s->t[i].id == id) return &s->t[i];
    return NULL;
}

void trophy_lang_file(int ps3_lang, char* out, size_t n)
{
    snprintf(out, n, "TROP_%02d.SFM", ps3_lang);
}
