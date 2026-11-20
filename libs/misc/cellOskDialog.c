#include "cellOskDialog.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"
#include "../guest_struct.h"

static int  s_osk_initialized = 0;
static int  s_osk_loaded = 0;
static char s_default_response[CELL_OSK_MAX_TEXT_LENGTH + 1] = "";

static CellOskDialogChar s_result_text[CELL_OSK_MAX_TEXT_LENGTH + 1];
static u32               s_result_length = 0;

static void osk_ascii_to_utf16(const char* src, CellOskDialogChar* dst,
                                u32 maxChars)
{
    u32 i;
    for (i = 0; i < maxChars && src[i] != '\0'; i++)
        dst[i] = (CellOskDialogChar)src[i];
    dst[i] = 0;
}

static void osk_utf16_to_ascii(const CellOskDialogChar* src, char* dst,
                                u32 maxChars)
{
    u32 i;
    for (i = 0; i < maxChars && src[i] != 0; i++)
        dst[i] = (src[i] < 128) ? (char)src[i] : '?';
    dst[i] = '\0';
}

enum {
    OSKIF_O_MESSAGE = 0, OSKIF_O_INIT_TEXT = 4,
    OSKRET_O_RESULT = 0, OSKRET_O_NUMCHARS = 4, OSKRET_O_PRESULTSTRING = 8
};

static u32 osk_guest_utf16(u32 ea, CellOskDialogChar* dst, u32 max)
{
    u32 n = 0;
    if (!ea)
        return 0;
    while (n < max) {
        u16 c = vm_read16(ea + n * 2);
        if (!c)
            break;
        dst[n++] = c;
    }
    dst[n] = 0;
    return n;
}

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo)
{
    (void)container; (void)dialogParam;

    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_ALREADY_LOADED;

    s_osk_loaded = 1;

    u32 info_ea    = GUEST_EA(inputFieldInfo);
    u32 message_ea = info_ea ? vm_read32(info_ea + OSKIF_O_MESSAGE)   : 0;
    u32 init_ea    = info_ea ? vm_read32(info_ea + OSKIF_O_INIT_TEXT) : 0;

    static CellOskDialogChar host_init[CELL_OSK_MAX_TEXT_LENGTH + 1];
    u32 init_len = osk_guest_utf16(init_ea, host_init, CELL_OSK_MAX_TEXT_LENGTH);

    if (message_ea) {
        static CellOskDialogChar host_msg[CELL_OSK_MAX_TEXT_LENGTH + 1];
        osk_guest_utf16(message_ea, host_msg, CELL_OSK_MAX_TEXT_LENGTH);
        char prompt[256];
        osk_utf16_to_ascii(host_msg, prompt, sizeof(prompt) - 1);
        printf("[cellOskDialog] LoadAsync - Prompt: \"%s\"\n", prompt);
    } else {
        printf("[cellOskDialog] LoadAsync - (no prompt)\n");
    }

    if (init_ea) {
        char init[256];
        osk_utf16_to_ascii(host_init, init, sizeof(init) - 1);
        printf("[cellOskDialog] Initial text: \"%s\"\n", init);
    }

    memset(s_result_text, 0, sizeof(s_result_text));
    if (s_default_response[0] != '\0') {
        osk_ascii_to_utf16(s_default_response, s_result_text,
                           CELL_OSK_MAX_TEXT_LENGTH);
        s_result_length = (u32)strlen(s_default_response);
        printf("[cellOskDialog] Returning default response: \"%s\"\n",
               s_default_response);
    } else if (init_ea) {
        memcpy(s_result_text, host_init,
               (init_len + 1) * sizeof(CellOskDialogChar));
        s_result_length = init_len;
    } else {
        s_result_length = 0;
    }

    return CELL_OK;
}

s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result)
{
    if (!s_osk_initialized)
        return CELL_OSK_ERROR_NOT_INITIALIZED;

    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    printf("[cellOskDialog] UnloadAsync()\n");

    if (result) {
        u32 res_ea = GUEST_EA(result);
        vm_write32(res_ea + OSKRET_O_RESULT,
                   CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK);
        vm_write32(res_ea + OSKRET_O_NUMCHARS, s_result_length);
        u32 str_ea = vm_read32(res_ea + OSKRET_O_PRESULTSTRING);
        if (str_ea && s_result_length > 0) {
            for (u32 i = 0; i <= s_result_length; i++)
                vm_write16(str_ea + i * 2, s_result_text[i]);
        }
    }

    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogAbort(void)
{
    printf("[cellOskDialog] Abort()\n");

    if (!s_osk_loaded)
        return CELL_OSK_ERROR_DIALOG_NOT_LOADED;

    s_osk_loaded = 0;
    return CELL_OK;
}

s32 cellOskDialogSetInitialKeyLayout(u32 layout)
{
    (void)layout;
    printf("[cellOskDialog] SetInitialKeyLayout(%u)\n", layout);
    return CELL_OK;
}

s32 cellOskDialogSetLayoutMode(u32 mode)
{
    (void)mode;
    printf("[cellOskDialog] SetLayoutMode(%u)\n", mode);
    return CELL_OK;
}
