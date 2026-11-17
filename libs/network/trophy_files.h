/*
 * trophy_files -- a title's PS3 trophy files on the host (game-agnostic).
 *
 *   TROPHY.TRP     the trophy pack a game ships (PS3_GAME/TROPDIR/<id>_<nn>/):
 *                  the definitions (TROPCONF.SFM), their text (TROP.SFM and
 *                  TROP_<language>.SFM) and the icons (ICON0.PNG, TROP<nnn>.PNG)
 *
 * The formats, as the public documentation has them:
 *
 * TROPHY.TRP (big-endian): a 0x40-byte header -- magic 0xDCA24D00, version,
 * file size (u64), entry count, entry size (0x40), a dev flag, a SHA-1 (checked
 * from version 2 on) and padding -- then the entries: name[32], offset (u64),
 * size (u64), flags (u32), padding. A PS3 pack stores its files as they are.
 *
 * TROP*.SFM: XML after a signature comment. <trophyconf> holds <npcommid>,
 * <title-name>, and per trophy <trophy id="000" hidden="no" ttype="P" pid="-1">
 * with <name> and <detail> (TROPCONF.SFM in a pack may carry the attributes
 * only; the text is in TROP.SFM / TROP_xx.SFM). ttype: P(latinum) G S B.
 */
#ifndef PS3RECOMP_TROPHY_FILES_H
#define PS3RECOMP_TROPHY_FILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TROPHY_FILES_MAX     128     /* SCE_NP_TROPHY_MAX_NUM_TROPHIES */
#define TROPHY_TRP_MAX_FILES 512

typedef struct trophy_def {
    int  id;
    char grade;          /* 'P', 'G', 'S', 'B' ('B' when the file says nothing) */
    int  pid;            /* the platinum it counts towards; -1 = none */
    int  hidden;
    char name[128];      /* UTF-8, XML entities decoded */
    char detail[1024];
} trophy_def;

typedef struct trophy_set {
    char       comm_id[16];   /* <npcommid>, e.g. "NPWR01730_00" */
    char       title[128];
    char       title_detail[1024];
    int        n;             /* trophies in t[], in file order */
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

/* ---- TROPHY.TRP ---- */
int   trp_open(const char* path, trp_index* out);                  /* 1 = a trophy pack */
const trp_entry* trp_find_entry(const trp_index* t, const char* name);
char* trp_read_entry_data(const trp_index* t, const char* name, size_t* size);  /* malloc'd, NUL-terminated */

/* ---- TROP*.SFM ---- */
/* Parse a trophyconf XML. With `into` already filled (structure from TROPCONF.SFM),
 * a text file only adds names, details and the title; trophies it lists that `into`
 * lacks are added. Returns the trophy count. */
int trophy_sfm_parse(const char* xml, trophy_set* into);
/* The set from a pack: TROPCONF.SFM, then the text of `lang_file` (e.g. "TROP_02.SFM"),
 * else TROP.SFM. Returns the trophy count (0 = none). */
int trophy_set_load(trophy_set* s, const trp_index* t, const char* lang_file);
const trophy_def* trophy_set_find(const trophy_set* s, int id);
/* The TROP_xx.SFM a PS3 system language id uses ("TROP_01.SFM" for 1). */
void trophy_lang_file(int ps3_lang, char* out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
