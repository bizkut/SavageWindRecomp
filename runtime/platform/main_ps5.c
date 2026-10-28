// PS5 game entry: the game on its guest thread, the VideoOut window and renderer on the main thread (ui_run).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gamelib.h"
#include "../hle.h"
#include "../kernel.h"
#include "../paths.h"
#include <ps5platform/klog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
bool ps5_exefs_init(const char *directory, char *why, size_t why_size);
const char *g_romfs_dir = "/data/savagewind/game/romfs";
uint64_t g_program_id;

static void exit_title(void) {
    fflush(stderr);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
}
// No in-place reboot (runtime/main.c: back to the start screen, or the game again): the title has no start screen,
// and nothing on the PS5 asks for one. SAVAGEWIND_REBOOT_AT is a desktop test (a title has no environment).
int rt_reboot_requested(void) { return 0; }
void reboot_test_frame(uint64_t frame) { (void)frame; }

static void module_init(Cpu *c, Module *m) {
    if (m->init) guest_call(c, m->init);
    else for (uint64_t i = 0; i < m->init_array_sz / 8; i++) {
        uint64_t fn = ((uint64_t *)(uintptr_t)m->init_array)[i];
        if (fn) guest_call(c, fn);
    }
}
static void init_sdk(Cpu *c) { module_init(c, loader_module("sdk")); }
static void init_main(Cpu *c) {
    for (int i = g_nmodules - 1; i >= 0; i--)
        if (strcmp(g_modules[i].name, "sdk")) module_init(c, &g_modules[i]);
}
static void fini_main(Cpu *c) {
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (!strcmp(m->name, "sdk")) continue;
        if (m->fini) guest_call(c, m->fini);
        else for (uint64_t j = m->fini_array_sz / 8; j-- > 0;) {
            uint64_t fn = ((uint64_t *)(uintptr_t)m->fini_array)[j];
            if (fn) guest_call(c, fn);
        }
    }
}

int main(void) {
    sceKernelDebugOutText(0, "savagewind: PPSA90212 M3 1.9.0 startup\n");
    if (ps5_klog_capture_stderr("savagewind: ") != 0) {
        sceKernelDebugOutText(0, "savagewind: cannot capture stderr\n");
        exit_title();
    }
    if (!plat_reserve_guest(GUEST_LO, GUEST_HI)) {
        fprintf(stderr, "cannot reserve guest address space\n");
        exit_title();
    }
    plat_init();
    ps5_logf("[input] no PS5 controller backend before M4: player 1's controller is connected with nothing pressed\n");
    rt_paths_init();
    char why[512] = "";
    if (!gamelib_load(NULL, why, sizeof why) || !ps5_exefs_init("/data/savagewind/import", why, sizeof why)) {
        fprintf(stderr, "startup failed: %s\n", why);
        exit_title();
    }
    loader_load("exefs");
    extern void fs_title_version_patch(void);
    extern void settings_load(void);
    extern void language_booted(void);
    fs_title_version_patch();
    settings_load();
    language_booted();
    kernel_init();
    dispatch_init();
    size_t size = 0;
    uint8_t *npdm = loader_read_file("exefs/main.npdm", &size, 0x10000);
    if (!npdm || size < 0x80 || memcmp(npdm, "META", 4)) {
        fprintf(stderr, "package main.npdm is missing or invalid\n");
        free(npdm);
        exit_title();
    }
    uint32_t stack_size, aci;
    memcpy(&stack_size, npdm + 0x1c, 4);
    int prio = npdm[0xe];
    memcpy(&aci, npdm + 0x70, 4);
    if ((uint64_t)aci + 0x18 <= size && !memcmp(npdm + aci, "ACI0", 4)) memcpy(&g_program_id, npdm + aci + 0x10, 8);
    free(npdm);
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    bool has_fini = start != 0;
    if (!start) start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) { fprintf(stderr, "nn::init::Start is missing\n"); exit_title(); }
    uint64_t sdk = hle_register("<rtld_init_sdk>", init_sdk);
    uint64_t game = hle_register("<rtld_init_main>", init_main);
    uint64_t fini = has_fini ? hle_register("<rtld_fini_main>", fini_main) : 0;
    KThread *thread = kernel_create_main_thread(start, stack_size, prio);
    fprintf(stderr, "[ps5] entering linked game code at %llx\n", (unsigned long long)start);
    kernel_run_main(thread, thread->handle, 0, sdk, game, fini);
    extern int ui_run(void);  // runtime/nvn/vulkan/device.c: the display plane and device, then the window loop
    ui_run();
    exit_title();
}
