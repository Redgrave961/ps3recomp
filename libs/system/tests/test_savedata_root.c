#include "../cellSaveData.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(uint32_t a) { (void)a; }
void spu_lockline_unlock(uint32_t a) { (void)a; }

ps3_guest_caller_fn g_ps3_guest_caller;

int main(void)
{
    char root[] = "/tmp/ps3recomp-gamedata-root-XXXXXX";
    assert(mkdtemp(root));
    setenv("PS3_GAMEDATA_ROOT", root, 1);

    char path[1024], want[1024];
    build_save_path(path, sizeof(path), "TEST00000");
    snprintf(want, sizeof(want), "%s/dev_hdd0/home/00000001/savedata/TEST00000", root);
    assert(strcmp(path, want) == 0);

    setenv("PS3_GAMEDATA_ROOT", "/nonexistent", 1);
    build_save_path(path, sizeof(path), "TEST00000");
    snprintf(want, sizeof(want), "%s/dev_hdd0/home/00000001/savedata/TEST00000", root);
    assert(strcmp(path, want) == 0);

    char cmd[1100];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    puts("PS3_GAMEDATA_ROOT moves the save root, adopted once");
}
