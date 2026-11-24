#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guest_mem.h"
#include "hle.h"
#include "hle/cheatvm.h"

Module g_modules[4];
int g_nmodules = 1;

static char g_lib_path[512];
const char *gamelib_path(void) { return g_lib_path; }
bool gamebuild_available(void) { return false; }
uint64_t input_buttons_now(void) { return 0; }
void rt_fatal_trace(void) { abort(); }

typedef struct {
    uint8_t pad[0x2000];
} FakeMem;

int main(void) {
    char tmpdir[] = "/tmp/test_cheatvm_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    char orig_cwd[512];
    assert(getcwd(orig_cwd, sizeof orig_cwd) != NULL);
    assert(chdir(tmpdir) == 0);

    snprintf(g_lib_path, sizeof g_lib_path, "%s/engagegame.so", tmpdir);

    FakeMem *fake_main = calloc(1, sizeof *fake_main);
    FakeMem *fake_heap = calloc(1, sizeof *fake_heap);
    assert(fake_main && fake_heap);
    uint64_t main_base = (uint64_t)(uintptr_t)fake_main;
    uint64_t heap_base = (uint64_t)(uintptr_t)fake_heap;

    g_modules[0] = (Module){
        .name = "main",
        .base = main_base,
        .size = sizeof *fake_main,
        .text_off = 0,
        .text_size = 0x1000,
        .data_off = 0x1000,
        .data_size = 0x1000,
    };

    /* Set up pointer chain: main + 0x1100 -> heap + 0x200 -> +(0x10) -> heap + 0x400 -> +(0x8) -> u32 */
    uint64_t ptr1 = heap_base + 0x200;
    uint64_t ptr2 = heap_base + 0x400;
    memcpy(fake_main->pad + 0x1100, &ptr1, sizeof ptr1);
    memcpy(fake_heap->pad + 0x200 + 0x10, &ptr2, sizeof ptr2);

    uint64_t win_lo = (main_base < heap_base ? main_base : heap_base);
    uint64_t win_hi = (main_base > heap_base ? main_base : heap_base) + sizeof(FakeMem);
    GuestRegion regs[2];
    if (main_base < heap_base) {
        regs[0] = (GuestRegion){.addr = main_base, .size = sizeof *fake_main, .perm = GMEM_PERM_RW};
        regs[1] = (GuestRegion){.addr = heap_base, .size = sizeof *fake_heap, .perm = GMEM_PERM_RW};
    } else {
        regs[0] = (GuestRegion){.addr = heap_base, .size = sizeof *fake_heap, .perm = GMEM_PERM_RW};
        regs[1] = (GuestRegion){.addr = main_base, .size = sizeof *fake_main, .perm = GMEM_PERM_RW};
    }
    guest_mem_test_set_regions(regs, 2, win_lo, win_hi);

    FILE *vf = fopen("engagegame.so.cheats", "w");
    assert(vf != NULL);
    fprintf(vf, "00000100 0b0107e0\n");
    fclose(vf);

    FILE *cf = fopen("cheats.txt", "w");
    assert(cf != NULL);
    fprintf(cf,
        "[Exp 2x]\n"
        "# Double XP using 040B0000\n"
        "040B0000 00000100 0B0107E0\n"
        "\n"
        "[Reg Modified]\n"
        "400B0000 00000000 00001200\n"
        "040B0000 00000000 12345678\n"
        "\n"
        "[Safe Chain]\n"
        "580F0000 00001100\n"
        "580F1000 00000010\n"
        "780F0000 00000008\n"
        "640F0000 00000000 0098967F\n"
        "\n"
        "[Broken Chain]\n"
        "580F0000 00001200\n"
        "580F1000 00000010\n"
        "640F0000 00000000 0000270F\n");
    fclose(cf);

    assert(cheatvm_count() == 4);

    CheatInfo info = {0};
    assert(cheatvm_info(0, &info));
    assert(strcmp(info.name, "Exp 2x") == 0);
    assert(!info.unsupported);
    assert(!info.needs_gen);

    /* Cheat 1 writes reg B before opcode 0, so it is NOT treated as static .text patch. */
    assert(cheatvm_info(1, &info));
    assert(strcmp(info.name, "Reg Modified") == 0);
    assert(!info.needs_gen);

    /* Enable Exp 2x -> cheat_word returns patched instruction, then off -> 0. */
    cheatvm_set(0, true);
    cheatvm_frame();
    assert(cheat_word(main_base + 0x100) == 0x0B0107E0u);
    cheatvm_set(0, false);
    assert(cheat_word(main_base + 0x100) == 0u);

    /* Enable Reg Modified -> writes runtime memory safely without touching code table. */
    cheatvm_set(1, true);
    cheatvm_frame();
    uint32_t reg_written = 0;
    memcpy(&reg_written, fake_main->pad + 0x1200, sizeof reg_written);
    assert(reg_written == 0x12345678u);
    cheatvm_set(1, false);

    /* Safe pointer chain writes 9999999 to heap + 0x408. */
    cheatvm_set(2, true);
    cheatvm_frame();
    uint32_t gold = 0;
    memcpy(&gold, fake_heap->pad + 0x408, sizeof gold);
    assert(gold == 0x0098967Fu);
    cheatvm_set(2, false);

    /* Broken pointer chain (points to 0x12345678, out of mapped regions) must NOT crash. */
    cheatvm_set(3, true);
    cheatvm_frame();
    cheatvm_set(3, false);

    guest_mem_test_clear();
    assert(chdir(orig_cwd) == 0);
    remove("cheats.txt");
    free(fake_main);
    free(fake_heap);
    printf("PASS: test_cheatvm (040B0000 zero_regs, RegModified, SafeChain, BrokenChain)\n");
    return 0;
}
