#ifndef PS3RECOMP_TROPHY_FILES_H
#define PS3RECOMP_TROPHY_FILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TROPHY_FILES_MAX     128
#define TROPHY_TRP_MAX_FILES 512

typedef struct trophy_def {
    int  id;
    char grade;
    int  pid;
    int  hidden;
    char name[128];
    char detail[1024];
} trophy_def;

typedef struct trophy_set {
    char       comm_id[16];
    char       title[128];
    char       title_detail[1024];
    int        n;
    trophy_def t[TROPHY_FILES_MAX];
} trophy_set;

typedef struct trp_entry {
    char     name[33];
    uint64_t offset, size;
} trp_entry;

typedef struct trp_index {
    char      path[1024];
    uint64_t  file_size;
    uint32_t  entry_count, entry_size;
    int       n;
    trp_entry e[TROPHY_TRP_MAX_FILES];
} trp_index;

int   trp_open(const char* path, trp_index* out);
const trp_entry* trp_find_entry(const trp_index* t, const char* name);
char* trp_read_entry_data(const trp_index* t, const char* name, size_t* size);

int trophy_sfm_parse(const char* xml, trophy_set* into);
int trophy_set_load(trophy_set* s, const trp_index* t, const char* lang_file);
const trophy_def* trophy_set_find(const trophy_set* s, int id);
void trophy_lang_file(int ps3_lang, char* out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
