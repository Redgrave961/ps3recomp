#include "cellMsgDialog.h"
#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"

static const char* guest_str(const void* p)
{
    uint32_t ea = (uint32_t)(uintptr_t)p;
    return (ea && vm_base) ? (const char*)(vm_base + ea) : "<null>";
}

static s32 queue_dialog_callback(CellMsgDialogCallback cb, int32_t result,
                                 void* userdata)
{
    return cellSysutilQueueGuestCallback((u32)(uintptr_t)cb,
        (u64)(int64_t)result, (u64)(uintptr_t)userdata);
}

static int                   s_dialog_open = 0;
static CellMsgDialogCallback s_callback    = NULL;
static void*                 s_userdata    = NULL;

static ULONGLONG s_nobutton_deadline = 0;

static unsigned nobutton_grace_ms(void)
{
    static int ms = -1;
    if (ms < 0) { const char* e = getenv("MSGDIALOG_AUTODISMISS_MS");
                  ms = e ? atoi(e) : 2000; if (ms < 0) ms = 0; }
    return (unsigned)ms;
}

#define MAX_PROGRESS_BARS 2

static u32 s_progress[MAX_PROGRESS_BARS];

static int msg_offers_to_abort(const char* msg)
{
    static const char* const words[] = { "cancel", "quit", "abort", NULL };
    if (!msg) return 0;
    for (int w = 0; words[w]; w++) {
        const size_t n = strlen(words[w]);
        for (const char* p = msg; *p; p++) {
            size_t i = 0;
            while (i < n && p[i] && (char)tolower((unsigned char)p[i]) == words[w][i]) i++;
            if (i == n) return 1;
        }
    }
    return 0;
}

s32 cellMsgDialogOpen2(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam)
{
    printf("[cellMsgDialog] Open2(type=0x%08X, msg='%s')\n",
           type, guest_str(msgString));

    if (s_dialog_open) {
        printf("[cellMsgDialog] WARNING: dialog already open, closing previous\n");
    }

    s_dialog_open = 1;
    s_callback    = callback;
    s_userdata    = userdata;
    memset(s_progress, 0, sizeof(s_progress));

    if (msgString) {
        printf("========================================\n");
        printf("[DIALOG] %s\n", guest_str(msgString));
        printf("========================================\n");
    }

    u32 button_type = type & 0x00000030;
    int has_progress = (type & 0x0000F000) != 0;

    static int nobutton_waits = -1;
    if (nobutton_waits < 0) nobutton_waits = getenv("MSGDIALOG_NOBUTTON_WAITS") ? 1 : 0;
    int stays_open = has_progress ||
                     (nobutton_waits &&
                      button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE);

    if (!stays_open) {
        s32 result = CELL_MSGDIALOG_BUTTON_OK;

        if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
            static int forced = -1;
            if (forced < 0) {
                const char* e = getenv("MSGDIALOG_ANSWER");
                if      (e && (e[0] == 'n' || e[0] == 'N')) forced = 1;
                else if (e && (e[0] == 'a' || e[0] == 'A')) forced = 2;
                else                                        forced = 0;
            }
            int auto_no = (forced == 2) ? msg_offers_to_abort(guest_str(msgString)) : 0;
            int no_ = (forced == 2) ? auto_no : forced;
            result = no_ ? CELL_MSGDIALOG_BUTTON_NO : CELL_MSGDIALOG_BUTTON_YES;
            printf("[cellMsgDialog] Auto-responding: %s%s\n", no_ ? "NO" : "YES",
                   auto_no ? " (prompt offers to abort)" : "");
        } else if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) {
            result = CELL_MSGDIALOG_BUTTON_OK;
            printf("[cellMsgDialog] Auto-responding: OK\n");
        } else {
            result = CELL_MSGDIALOG_BUTTON_NONE;
            printf("[cellMsgDialog] Auto-responding: NONE (no buttons)\n");
        }

        s_dialog_open = 0;
        if (s_callback) {
            CellMsgDialogCallback cb = s_callback;
            s_callback = NULL;
            return queue_dialog_callback(cb, result, s_userdata);
        }
    } else {
        printf("[cellMsgDialog] %s dialog opened (waits for Close/Abort)\n",
               has_progress ? "Progress bar" : "No-button");
        s_nobutton_deadline = (!has_progress && nobutton_grace_ms())
            ? GetTickCount64() + nobutton_grace_ms() : 0;
    }

    return CELL_OK;
}

s32 cellMsgDialogClose(float delayMs)
{
    printf("[cellMsgDialog] Close(delay=%.1f ms)\n", delayMs);

    if (!s_dialog_open) {
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_dialog_open = 0;
    s_nobutton_deadline = 0;

    if (s_callback) {
        CellMsgDialogCallback cb = s_callback;
        s_callback = NULL;
        return queue_dialog_callback(cb, CELL_MSGDIALOG_BUTTON_NONE, s_userdata);
    }

    return CELL_OK;
}

s32 cellMsgDialogAbort(void)
{
    printf("[cellMsgDialog] Abort()\n");

    s_dialog_open = 0;
    s_nobutton_deadline = 0;
    s_callback = NULL;
    s_userdata = NULL;

    return CELL_OK;
}

s32 cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    s_progress[progressBarIndex] += delta;
    if (s_progress[progressBarIndex] > 100)
        s_progress[progressBarIndex] = 100;

    printf("[cellMsgDialog] ProgressBar[%u] = %u%%\n",
           progressBarIndex, s_progress[progressBarIndex]);

    return CELL_OK;
}

void cellMsgDialog_tick(void)
{
    if (!s_dialog_open || !s_nobutton_deadline) return;
    if (GetTickCount64() < s_nobutton_deadline) return;
    s_nobutton_deadline = 0;
    s_dialog_open = 0;
    printf("[cellMsgDialog] no-button dialog auto-dismissed after %u ms\n",
           nobutton_grace_ms());
    if (s_callback) {
        CellMsgDialogCallback cb = s_callback;
        s_callback = NULL;
        queue_dialog_callback(cb, CELL_MSGDIALOG_BUTTON_NONE, s_userdata);
    }
}
