/*
 * ps3recomp - cellSysutil HLE implementation
 *
 * System callbacks, parameter queries, BGM control, system cache,
 * and disc game check.
 */

#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>   /* the host UI language */
#endif
#include <errno.h>
#include <sys/stat.h>   /* mkdir -- preparing the /dev_hdd1 cache directory */

void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);
#ifdef _WIN32
#  include <direct.h>
#  ifndef mkdir
#    define mkdir(p, m) _mkdir(p)
#  endif
#endif

/* Guest-memory stores — out-params from the recompiled title are guest VM
 * addresses, not host pointers. */
extern void vm_write8(uint64_t addr, uint8_t v);
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Guest callback dispatch hook (set by the game's host code at startup).
 * -----------------------------------------------------------------------*/
ps3_guest_caller_fn g_ps3_guest_caller = NULL;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Stored as a guest OPD address rather than a host function pointer:
     * the recompiled game passes its OPD addr through Register, and the
     * dispatcher hook resolves the OPD when actually invoking. The
     * CellSysutilCallback typedef maps to a pointer, so we cast through
     * uintptr_t at the API boundary. */
    uint32_t            guest_opd;
    uint32_t            userdata;     /* guest pointer; opaque to the runtime */
    int                 registered;
} SysutilCallbackSlot;

static SysutilCallbackSlot s_callbacks[CELL_SYSUTIL_MAX_CALLBACKS];

/* ---------------------------------------------------------------------------
 * Sysutil event queue.
 *
 * The OS would deliver events asynchronously (XMB open/close, drawing
 * begin/end, save-data complete, etc.). Games drain them by calling
 * cellSysutilCheckCallback periodically. We accept queued events from
 * the host side via cellSysutilQueueEvent() and drain them here.
 * -----------------------------------------------------------------------*/

typedef struct {
    int      slot;
    uint32_t status;
    uint32_t param;
} SysutilEvent;

#define SYSUTIL_EVENT_QUEUE_SIZE 32
static SysutilEvent s_event_queue[SYSUTIL_EVENT_QUEUE_SIZE];
static int          s_event_head = 0;  /* next read */
static int          s_event_tail = 0;  /* next write */

void cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param)
{
    int next = (s_event_tail + 1) % SYSUTIL_EVENT_QUEUE_SIZE;
    if (next == s_event_head) {
        printf("[cellSysutil] event queue full — dropping status=0x%X\n", status);
        return;
    }
    s_event_queue[s_event_tail].slot   = slot;
    s_event_queue[s_event_tail].status = status;
    s_event_queue[s_event_tail].param  = param;
    s_event_tail = next;
}

/* Dialog and other one-shot completions carry their own OPD, rather than a
 * registered sysutil slot. Detach one batch before invoking guest code so a
 * callback may safely enqueue another completion for the next poll. */
#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_completion_lock = SRWLOCK_INIT;
#define COMPLETION_LOCK() AcquireSRWLockExclusive(&s_completion_lock)
#define COMPLETION_UNLOCK() ReleaseSRWLockExclusive(&s_completion_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_completion_lock = PTHREAD_MUTEX_INITIALIZER;
#define COMPLETION_LOCK() pthread_mutex_lock(&s_completion_lock)
#define COMPLETION_UNLOCK() pthread_mutex_unlock(&s_completion_lock)
#endif
/* MinGW GCC ignores __declspec(thread), so it is for MSVC only. */
#if defined(_MSC_VER)
#define COMPLETION_TLS __declspec(thread)
#else
#define COMPLETION_TLS _Thread_local
#endif

typedef struct GuestCompletion {
    struct GuestCompletion* next;
    u32 opd;
    u64 args[8];
} GuestCompletion;
static GuestCompletion* s_completion_head;
static GuestCompletion* s_completion_tail;

s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    if (!opd) return CELL_OK;
    GuestCompletion* item = malloc(sizeof(*item));
    if (!item) return (s32)CELL_ENOMEM;
    item->next = NULL; item->opd = opd; memcpy(item->args, args, sizeof(item->args));
    COMPLETION_LOCK();
    if (s_completion_tail) s_completion_tail->next = item;
    else s_completion_head = item;
    s_completion_tail = item;
    COMPLETION_UNLOCK();
    return CELL_OK;
}

s32 cellSysutilQueueGuestCallback(u32 opd, u64 arg0, u64 arg1)
{
    const u64 args[8] = {arg0, arg1, 0, 0, 0, 0, 0, 0};
    return cellSysutilQueueGuestCallbackArgs(opd, args);
}

static void drain_guest_completions(void)
{
    static COMPLETION_TLS int draining;
    if (draining || !g_ps3_guest_caller) return;
    draining = 1;
    COMPLETION_LOCK();
    GuestCompletion* item = s_completion_head;
    s_completion_head = s_completion_tail = NULL;
    COMPLETION_UNLOCK();
    while (item) {
        GuestCompletion* next = item->next;
        g_ps3_guest_caller(item->opd, item->args[0], item->args[1], item->args[2],
            item->args[3], item->args[4], item->args[5], item->args[6], item->args[7]);
        free(item);
        item = next;
    }
    draining = 0;
}

static char s_cache_path[CELL_SYSCACHE_PATH_MAX];
static int s_cache_mounted = 0;


/* ---------------------------------------------------------------------------
 * Core callbacks & params
 * -----------------------------------------------------------------------*/

s32 cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata)
{
    /* `func` and `userdata` are guest VM addresses; reinterpret without
     * touching them. NULL func means "unregister this slot" — some games
     * (notably flOw) call Register with func=NULL as a clear-on-poll. */
    uint32_t func_addr     = (uint32_t)(uintptr_t)func;
    uint32_t userdata_addr = (uint32_t)(uintptr_t)userdata;

    printf("[cellSysutil] RegisterCallback(slot=%d, func=0x%08X)\n",
           slot, func_addr);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    if (func_addr == 0) {
        /* Treat as Unregister — matches observed game behaviour. */
        s_callbacks[slot].guest_opd  = 0;
        s_callbacks[slot].userdata   = 0;
        s_callbacks[slot].registered = 0;
        return CELL_OK;
    }

    s_callbacks[slot].guest_opd  = func_addr;
    s_callbacks[slot].userdata   = userdata_addr;
    s_callbacks[slot].registered = 1;

    return CELL_OK;
}

s32 cellSysutilUnregisterCallback(s32 slot)
{
    printf("[cellSysutil] UnregisterCallback(slot=%d)\n", slot);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    s_callbacks[slot].guest_opd  = 0;
    s_callbacks[slot].userdata   = 0;
    s_callbacks[slot].registered = 0;

    return CELL_OK;
}

/* Whether the title actually pumps sysutil. cellMsgDialog needs to know: it
 * defers a dialog answer to this pump (which is where hardware delivers it) and
 * must not defer into a pump that never runs. */
static int s_pump_seen = 0;

s32 cellSysutilCheckCallback(void)
{
    /* Give a no-button dialog the title has left open past its grace period
     * its dismissal. Done before the drain so the callback it queues is
     * delivered on this same poll. */
    { extern void cellMsgDialog_tick(void); cellMsgDialog_tick(); }
    /* Turn psnr replies and pushes into queued NP callbacks first, so this
     * same poll delivers them. A no-op when no psnr server is configured. */
    { extern void np_psnr_pump(void); np_psnr_pump(); }
    drain_guest_completions();
    {
      if (!s_pump_seen) {
          s_pump_seen = 1;
          printf("[cellSysutil] CheckCallback: the title pumps sysutil%c", 10);
      }
    }
    /* Drain the event queue, dispatching each event into guest code via
     * the registered ps3_guest_caller hook. Standard PS3 sysutil callback
     * signature is:
     *   void cb(uint64_t status, uint64_t param, void* userdata)
     * which maps to PPC r3=status, r4=param, r5=userdata; r6 is unused.
     *
     * Stops when the queue empties OR when no guest caller is installed
     * (in which case we drop pending events to avoid stalling).
     */
    while (s_event_head != s_event_tail) {
        SysutilEvent e = s_event_queue[s_event_head];
        s_event_head = (s_event_head + 1) % SYSUTIL_EVENT_QUEUE_SIZE;

        if (e.slot < 0 || e.slot >= CELL_SYSUTIL_MAX_CALLBACKS) continue;
        if (!s_callbacks[e.slot].registered) continue;
        if (!s_callbacks[e.slot].guest_opd) continue;

        if (g_ps3_guest_caller) {
            g_ps3_guest_caller(s_callbacks[e.slot].guest_opd,
                               (uint64_t)e.status,
                               (uint64_t)e.param,
                               (uint64_t)s_callbacks[e.slot].userdata,
                               0, 0, 0, 0, 0);
        }
    }
    return CELL_OK;
}

/* value_ea is a GUEST address, and it is spelt as one. It used to be declared
 * s32* and immediately cast back to a guest EA, with a comment explaining that
 * dereferencing it as a host pointer faults -- true, and no help to a caller
 * who reads the declaration instead of the body. A host that translated the
 * argument first, which is what a pointer parameter asks for, handed this the
 * address of a host stack local; the cast truncated it to 32 bits and
 * vm_write32 stored four bytes at whatever guest address that spelt. It varies
 * with stack layout, so it lands somewhere different every run and nowhere
 * near the guest's variable, and nothing on the way says a word.
 *
 * A u32 cannot be passed a host pointer by accident. */
/* The console language the title is told (dub, subtitles, menus), in order:
 * PS3_LANG if set -- a CELL_SYSUTIL_LANG_* number or a code: en, en-gb, ja,
 * fr, es, de, it, nl, pt (Portugal), pt-br, ru, ko, zh-t, zh-s, fi, sv, da,
 * no, pl, tr; else the host's UI language (Windows: the user's UI LANGID;
 * elsewhere LC_ALL, LC_MESSAGES, LANG); else English (US), the old fixed
 * answer. A title only switches if its data carries that language. */
static int sysutil_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* "pt_BR.UTF-8" -> "pt-br", "zh_TW" -> "zh-t", "nb_NO" -> "no", "fr_FR" -> "fr";
 * "C"/"POSIX"/empty -> "" (no preference). */
static void sysutil_locale_code(const char* loc, char* out, size_t n)
{
    out[0] = 0;
    if (!loc || !*loc || !strcmp(loc, "C") || !strcmp(loc, "POSIX") || n < 6) return;
    char ll[4] = {0}, cc[4] = {0};
    size_t i = 0;
    for (; i < 3 && loc[i] && loc[i] != '_' && loc[i] != '-' && loc[i] != '.' && loc[i] != '@'; i++)
        ll[i] = (char)((loc[i] >= 'A' && loc[i] <= 'Z') ? loc[i] - 'A' + 'a' : loc[i]);
    if (loc[i] == '_' || loc[i] == '-') {
        size_t j = 0;
        for (i++; j < 3 && loc[i] && loc[i] != '.' && loc[i] != '@'; i++, j++)
            cc[j] = (char)((loc[i] >= 'A' && loc[i] <= 'Z') ? loc[i] - 'A' + 'a' : loc[i]);
    }
    if (!strcmp(ll, "c") || !strcmp(ll, "pos")) { out[0] = 0; return; }   /* C.UTF-8, POSIX */
    if (!strcmp(ll, "zh")) {
        const int hant = strstr(loc, "Hant") || strstr(loc, "hant");
        snprintf(out, n, "%s", (hant || !strcmp(cc, "tw") || !strcmp(cc, "hk") || !strcmp(cc, "mo")) ? "zh-t" : "zh-s");
    } else if (!strcmp(ll, "nb") || !strcmp(ll, "nn")) {
        snprintf(out, n, "no");
    } else if (!strcmp(ll, "pt") && !strcmp(cc, "br")) {
        snprintf(out, n, "pt-br");
    } else if (!strcmp(ll, "en") && (!strcmp(cc, "gb") || !strcmp(cc, "au") || !strcmp(cc, "nz") ||
                                     !strcmp(cc, "ie") || !strcmp(cc, "za") || !strcmp(cc, "in"))) {
        /* the regions that got the UK English release */
        snprintf(out, n, "en-gb");
    } else {
        snprintf(out, n, "%s", ll);
    }
}

/* The host's UI language as a PS3_LANG code, or "" when it has none. */
static void sysutil_host_language(char* out, size_t n)
{
    out[0] = 0;
#ifdef _WIN32
    const LANGID id = GetUserDefaultUILanguage();
    const unsigned prim = id & 0x3FFu, sub = (unsigned)id >> 10;
    static const struct { unsigned prim; const char* code; } w[] = {
        {0x11, "ja"}, {0x09, "en"}, {0x0C, "fr"}, {0x0A, "es"}, {0x07, "de"},
        {0x10, "it"}, {0x13, "nl"}, {0x16, "pt"}, {0x19, "ru"}, {0x12, "ko"},
        {0x04, "zh"}, {0x0B, "fi"}, {0x1D, "sv"}, {0x06, "da"}, {0x14, "no"},
        {0x15, "pl"}, {0x1F, "tr"},
    };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
        if (w[i].prim == prim) { snprintf(out, n, "%s", w[i].code); break; }
    /* UK, Australia, New Zealand, Ireland, South Africa, India: the regions
     * that got the UK English release */
    if (prim == 0x09 && (sub == 0x02 || sub == 0x03 || sub == 0x05 || sub == 0x06 ||
                         sub == 0x07 || sub == 0x10)) snprintf(out, n, "en-gb");
    if (prim == 0x16 && sub == 0x01) snprintf(out, n, "pt-br");          /* Portuguese (Brazil) */
    /* Traditional: Taiwan, Hong Kong, Macao and the neutral zh-Hant (0x7C04) */
    if (prim == 0x04) snprintf(out, n, (sub == 0x01 || sub == 0x03 || sub == 0x05 || sub == 0x1F) ? "zh-t" : "zh-s");
#else
    const char* vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (unsigned i = 0; i < 3 && !out[0]; i++) sysutil_locale_code(getenv(vars[i]), out, n);
#ifdef __APPLE__
    /* An app started from the Finder has no LANG: ask for the user's first
     * preferred language ("pt-BR", "en-GB", "zh-Hant-TW"...). */
    if (!out[0]) {
        CFArrayRef langs = CFLocaleCopyPreferredLanguages();
        if (langs) {
            if (CFArrayGetCount(langs) > 0) {
                char buf[32];
                CFStringRef l = (CFStringRef)CFArrayGetValueAtIndex(langs, 0);
                if (CFStringGetCString(l, buf, sizeof buf, kCFStringEncodingUTF8)) {
                    if (!strncmp(buf, "zh-Hant", 7)) snprintf(out, n, "zh-t");
                    else if (!strncmp(buf, "zh-Hans", 7)) snprintf(out, n, "zh-s");
                    else sysutil_locale_code(buf, out, n);
                }
            }
            CFRelease(langs);
        }
    }
#endif
#endif
}

static s32 sysutil_language(void)
{
    static s32 lang = -1;
    if (lang >= 0) return lang;
    static const struct { const char* code; s32 id; } k[] = {
        {"ja", CELL_SYSUTIL_LANG_JAPANESE}, {"en", CELL_SYSUTIL_LANG_ENGLISH_US},
        {"en-us", CELL_SYSUTIL_LANG_ENGLISH_US}, {"fr", CELL_SYSUTIL_LANG_FRENCH},
        {"es", CELL_SYSUTIL_LANG_SPANISH}, {"de", CELL_SYSUTIL_LANG_GERMAN},
        {"it", CELL_SYSUTIL_LANG_ITALIAN}, {"nl", CELL_SYSUTIL_LANG_DUTCH},
        {"pt", CELL_SYSUTIL_LANG_PORTUGUESE_PT}, {"pt-pt", CELL_SYSUTIL_LANG_PORTUGUESE_PT},
        {"ru", CELL_SYSUTIL_LANG_RUSSIAN}, {"ko", CELL_SYSUTIL_LANG_KOREAN},
        {"zh-t", CELL_SYSUTIL_LANG_CHINESE_T}, {"zh-s", CELL_SYSUTIL_LANG_CHINESE_S},
        {"fi", CELL_SYSUTIL_LANG_FINNISH}, {"sv", CELL_SYSUTIL_LANG_SWEDISH},
        {"da", CELL_SYSUTIL_LANG_DANISH}, {"no", CELL_SYSUTIL_LANG_NORWEGIAN},
        {"pl", CELL_SYSUTIL_LANG_POLISH}, {"pt-br", CELL_SYSUTIL_LANG_PORTUGUESE_BR},
        {"en-gb", CELL_SYSUTIL_LANG_ENGLISH_GB}, {"tr", CELL_SYSUTIL_LANG_TURKISH},
    };
    lang = CELL_SYSUTIL_LANG_ENGLISH_US;
    const char* e = getenv("PS3_LANG");
    const char* from = "default";
    if (e && *e) {
        char* end = NULL;
        long n = strtol(e, &end, 10);
        if (end && *end == '\0' && n >= 0 && n <= CELL_SYSUTIL_LANG_TURKISH) { lang = (s32)n; from = "PS3_LANG"; }
        else {
            unsigned i;
            for (i = 0; i < sizeof k / sizeof k[0]; i++)
                if (sysutil_ieq(e, k[i].code)) { lang = k[i].id; from = "PS3_LANG"; break; }
            if (i == sizeof k / sizeof k[0])
                printf("[cellSysutil] PS3_LANG=%s not recognised, using English\n", e);
        }
    } else {
        char host[8];
        sysutil_host_language(host, sizeof host);
        for (unsigned i = 0; host[0] && i < sizeof k / sizeof k[0]; i++)
            if (sysutil_ieq(host, k[i].code)) { lang = k[i].id; from = "host"; break; }
    }
    printf("[cellSysutil] system language %d (%s)\n", (int)lang, from);
    return lang;
}

s32 ps3_sysutil_language(void) { return sysutil_language(); }

s32 cellSysutilGetSystemParamInt(s32 id, u32 value_ea)
{
    if (!value_ea)
        return CELL_SYSUTIL_ERROR_VALUE;

    s32 v = 0;
    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_LANG:
        v = sysutil_language();
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN:
        v = CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT:    v = 0; break; /* YYYYMMDD */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT:    v = 0; break; /* 24-hour */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE:       v = 0; break; /* UTC */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME:     v = 0; break; /* No DST */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL:           v = 0; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT: v = 0; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_LICENSE_AREA: {
        /* The console's licence area (a game picks region variants of its assets by it). $PS3_LICENSE_AREA (a number,
         * the CELL_SYSUTIL_LICENSE_AREA_* values) wins; ASSUMPTION: without it, North America. */
        const char* e = getenv("PS3_LICENSE_AREA");
        v = (e && *e) ? (s32)atoi(e) : CELL_SYSUTIL_LICENSE_AREA_A;
        break;
    }
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT:   v = 1; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ:  v = 0; break; /* 60Hz */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE:     v = 1; break; /* Rumble on */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE:  v = 0; break; /* US/101 */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF:    v = 0; break; /* Disabled */
    default:
        printf("[cellSysutil] GetSystemParamInt: unknown id 0x%04X\n", id);
        v = 0;
        break;
    }

    vm_write32(value_ea, (uint32_t)v);
    return CELL_OK;
}

/* buf_ea is a GUEST address, for the same reason and with the same history as
 * the Int form above: the body always treated it as one, and only the
 * declaration said otherwise. */
s32 cellSysutilGetSystemParamString(s32 id, u32 buf_ea, u32 bufsize)
{
    if (!buf_ea || bufsize == 0)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME: {
        const char* s = "ps3recomp_user";
        u32 i;
        for (i = 0; s[i] && i < bufsize - 1; i++) vm_write8(buf_ea + i, (uint8_t)s[i]);
        vm_write8(buf_ea + i, 0);
        break;
    }
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME: {
        const char* s = "User";
        u32 i;
        for (i = 0; s[i] && i < bufsize - 1; i++) vm_write8(buf_ea + i, (uint8_t)s[i]);
        vm_write8(buf_ea + i, 0);
        break;
    }
    default:
        printf("[cellSysutil] GetSystemParamString: unknown id 0x%04X\n", id);
        vm_write8(buf_ea, 0);
        break;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * BGM playback control
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * System cache
 * -----------------------------------------------------------------------*/

s32 cellSysCacheMount(char* param)
{
    /* The argument is a GUEST EA of CellSysCacheParam:
     *   +0x00 char cacheId[32]       (IN)
     *   +0x20 char getCachePath[136] (OUT)
     *   +0xA8 void* reserved
     * The old code strncpy'd through the raw EA as a host char* -> host AV
     * (LBP: write fault at 0xD00109F0, a guest thread-stack address). */
    uint32_t ea = (uint32_t)(uintptr_t)param;
    if (!ea)
        return CELL_EINVAL;

    char cache_id[33];
    memcpy(cache_id, vm_base + ea, 32);
    cache_id[32] = '\0';
    printf("[cellSysutil] SysCacheMount(id='%s')\n", cache_id);

    snprintf(s_cache_path, CELL_SYSCACHE_PATH_MAX, "/dev_hdd1/cache/%s", cache_id);
    size_t n = strlen(s_cache_path);
    if (n > 135) n = 135;
    memcpy(vm_base + ea + 0x20, s_cache_path, n);
    vm_base[ea + 0x20 + n] = '\0';
    s_cache_mounted = 1;

    /* The guest path above is only half the mount: /dev_hdd1 must also exist as
     * a WRITABLE host directory, or a title's first cache open fails against the
     * read-only disc root and it never creates its media threads. Create the
     * cache directory where /dev_hdd1 maps to (PS3_HDD1_ROOT); without that root /dev_hdd1 is not remapped, so there is
     * nothing to prepare. */
    { const char* root = getenv("PS3_HDD1_ROOT");
      if (root && *root) {
          char gpath[512], hpath[1024];
          sys_fs_translate_path("/dev_hdd1/cache", hpath, (int)sizeof hpath);
          mkdir(hpath, 0777);
          snprintf(gpath, sizeof gpath, "/dev_hdd1/cache/%s", cache_id);
          sys_fs_translate_path(gpath, hpath, (int)sizeof hpath);
          if (mkdir(hpath, 0777) == 0 || errno == EEXIST)
              printf("[cellSysutil] SysCacheMount: host cache dir %s ready\n", hpath);
          else
              printf("[cellSysutil] SysCacheMount: host cache dir %s NOT created (errno=%d)\n",
                     hpath, errno); } }

    return CELL_OK;   /* CELL_SYSCACHE_RET_OK_CLEARED */
}

/* ---------------------------------------------------------------------------
 * Disc game check
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * cellSysconf
 *
 * The accessory settings screen (audio devices, Bluetooth headsets) the system
 * overlays on a title. There is no such screen here, so Open finishes at
 * once: callback(CELL_OK, userdata) is queued as if the user had closed it,
 * and it runs on the title's next cellSysutilCheckCallback like every other
 * dialog completion, never from inside Open.
 *
 * Abort closes whatever settings screen is up; with none ever left open it is
 * a successful no-op. Resistance: Fall of Man calls it whenever it tears its
 * front end down -- starting the campaign, quitting, entering online play --
 * and it reached the game as an unresolved import.
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Misc utilities
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Host quit: the window's close button, Cmd+Q, a host menu's "Quit game".
 *
 * What the console's "Quit Game" does first: every registered sysutil
 * callback gets CELL_SYSUTIL_REQUEST_EXITGAME, and the title tidies up and
 * calls sys_process_exit, which ends the process here (exit()). A title that
 * has not done so within PS3_QUIT_GRACE_MS (default 3000) is ended for it,
 * as the console does: once no save is being written (cellSaveData.c
 * ps3_savedata_busy, waited on for up to 10 s), with stdio flushed and the
 * atexit handlers run, exit(0) -- a normal end for whatever launched the
 * game. Runs on its own thread, so the caller (the main thread, the frame
 * clock) returns at once; a second request is ignored.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static DWORD WINAPI host_quit_thread(LPVOID unused)
#else
#include <unistd.h>
static void* host_quit_thread(void* unused)
#endif
{
    (void)unused;
    extern int ps3_savedata_busy(void);   /* cellSaveData.c */
    int grace = 3000;
    { const char* e = getenv("PS3_QUIT_GRACE_MS"); if (e && *e) grace = atoi(e); }
    if (grace < 0) grace = 0;
    int asked = 0;
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; ++slot) {
        if (!s_callbacks[slot].registered || !s_callbacks[slot].guest_opd) continue;
        const u64 args[8] = { CELL_SYSUTIL_REQUEST_EXITGAME, 0, s_callbacks[slot].userdata, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs(s_callbacks[slot].guest_opd, args);
        asked++;
    }
    printf("[quit] REQUEST_EXITGAME to %d sysutil callback(s); waiting up to %d ms for the game\n",
           asked, grace);
    fflush(stdout);
    const int step = 50;
    for (int t = 0; t < grace; t += step) {
#ifdef _WIN32
        Sleep(step);
#else
        usleep(step * 1000);
#endif
    }
    int waited = 0;
    while (ps3_savedata_busy() && waited < 10000) {
#ifdef _WIN32
        Sleep(step);
#else
        usleep(step * 1000);
#endif
        waited += step;
    }
    printf("[quit] the game did not exit on its own; ending it (save in flight: %s, waited %d ms) -> exit 0\n",
           ps3_savedata_busy() ? "yes, gave up" : "no", waited);
    fflush(NULL);
    exit(0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static volatile int s_host_quit;

int ps3_host_quitting(void) { return __atomic_load_n(&s_host_quit, __ATOMIC_ACQUIRE); }

void ps3_host_quit(const char* why)
{
    if (__atomic_exchange_n(&s_host_quit, 1, __ATOMIC_ACQ_REL)) return;
    printf("[quit] requested (%s)\n", why ? why : "host");
    fflush(stdout);
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, host_quit_thread, NULL, 0, NULL);
    if (h) { CloseHandle(h); return; }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, host_quit_thread, NULL) == 0) { pthread_detach(th); return; }
#endif
    fflush(NULL);
    exit(0);
}
