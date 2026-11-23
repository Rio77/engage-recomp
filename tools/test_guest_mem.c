// Unit tests for centralized guest_mem validation (Phase 3).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "guest_mem.h"
#include "rt.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static _Atomic int g_stop;

static void *writer_thread(void *arg) {
    uint8_t *arena = arg;
    uint64_t base = (uintptr_t)arena;
    for (int i = 0; i < 20000; i++) {
        GuestRegion regs[2] = {
            {base, 4096, 5, GMEM_PERM_RW},
            {base + 8192, 4096, 5, (i & 1) ? GMEM_PERM_RW : GMEM_PERM_R},
        };
        guest_mem_test_set_regions(regs, 2, base, base + 16384);
    }
    atomic_store(&g_stop, 1);
    return NULL;
}

static void *reader_thread(void *arg) {
    uint8_t *arena = arg;
    uint64_t base = (uintptr_t)arena;
    while (!atomic_load(&g_stop)) {
        // Region 0 is always RW
        assert(guest_mem_validate_range(base + 64, 16, 8, GMEM_PERM_RW) == GMEM_OK);
        // Gap [4096, 8192) is ALWAYS unmapped even though inside window
        assert(guest_mem_validate_range(base + 4096, 16, 8, GMEM_PERM_R) == GMEM_UNMAPPED);
        // Region 1 is always readable, sometimes not writable
        GuestMemStatus st = guest_mem_validate_range(base + 8192, 16, 8, GMEM_PERM_RW);
        assert(st == GMEM_OK || st == GMEM_NO_PERM);
    }
    return NULL;
}

int main(void) {
    // Allocate a 16 KB synthetic guest window
    uint8_t *arena = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(arena != MAP_FAILED);
    uint64_t base = (uintptr_t)arena;

    // 1. Kernel/region snapshot mode tests (4-level validation)
    GuestRegion regs[3] = {
        {base + 0x0000, 0x1000, 3, GMEM_PERM_R},   // read-only page
        {base + 0x1000, 0x1000, 5, GMEM_PERM_RW},  // contiguous RW page
        {base + 0x3000, 0x1000, 5, GMEM_PERM_RW},  // gap at [0x2000..0x3000)
    };
    guest_mem_test_set_regions(regs, 3, base, base + 0x4000);

    assert(guest_mem_validate_range(0, 16, 8, GMEM_PERM_R) == GMEM_NULL);
    assert(guest_mem_validate_range(UINT64_MAX - 4, 16, 1, GMEM_PERM_R) == GMEM_OVERFLOW);
    assert(guest_mem_validate_range(base + 0x1001, 16, 8, GMEM_PERM_R) == GMEM_MISALIGNED);
    assert(guest_mem_validate_range(base - 0x1000, 16, 8, GMEM_PERM_R) == GMEM_OUT_OF_WINDOW);
    assert(guest_mem_validate_range(base + 0x4000, 16, 8, GMEM_PERM_R) == GMEM_OUT_OF_WINDOW);

    // Level 2: Inside window, logically unmapped in kernel.c (even though OS page is RW!)
    assert(guest_mem_validate_range(base + 0x2000, 16, 8, GMEM_PERM_R) == GMEM_UNMAPPED);
    // Spanning mapped -> unmapped gap must fail
    assert(guest_mem_validate_range(base + 0x1FF8, 16, 8, GMEM_PERM_R) == GMEM_UNMAPPED);

    // Level 3: Permissions R vs RW
    assert(guest_mem_validate_range(base + 0x0000, 16, 8, GMEM_PERM_R) == GMEM_OK);
    assert(guest_mem_validate_range(base + 0x0000, 16, 8, GMEM_PERM_RW) == GMEM_NO_PERM);
    assert(guest_mem_validate_range(base + 0x1000, 16, 8, GMEM_PERM_RW) == GMEM_OK);
    // Spanning R -> RW: readable OK, writable NO_PERM
    assert(guest_mem_validate_range(base + 0x0FF8, 16, 8, GMEM_PERM_R) == GMEM_OK);
    assert(guest_mem_validate_range(base + 0x0FF8, 16, 8, GMEM_PERM_RW) == GMEM_NO_PERM);

    // Checked read/write
    uint64_t val = 0x1122334455667788ull, out = 0;
    assert(guest_mem_write_checked(base + 0x1000, &val, 8, 8) == GMEM_OK);
    assert(guest_mem_read_checked(base + 0x1000, &out, 8, 8) == GMEM_OK && out == val);
    assert(guest_mem_write_checked(base + 0x0000, &val, 8, 8) == GMEM_NO_PERM);
    assert(guest_mem_read_checked(base + 0x2000, &out, 8, 8) == GMEM_UNMAPPED);

    // TLS cache invalidation when region snapshot changes (unmapping 0x1000)
    assert(guest_mem_validate_range(base + 0x1000, 16, 8, GMEM_PERM_RW) == GMEM_OK);  // warms TLS
    GuestRegion shrunk[1] = {{base + 0x0000, 0x1000, 3, GMEM_PERM_R}};
    guest_mem_test_set_regions(shrunk, 1, base, base + 0x4000);
    assert(guest_mem_validate_range(base + 0x1000, 16, 8, GMEM_PERM_R) == GMEM_UNMAPPED);

    // Concurrent seqlock stress
    atomic_store(&g_stop, 0);
    pthread_t w, r1, r2;
    assert(pthread_create(&w, NULL, writer_thread, arena) == 0);
    assert(pthread_create(&r1, NULL, reader_thread, arena) == 0);
    assert(pthread_create(&r2, NULL, reader_thread, arena) == 0);
    pthread_join(w, NULL);
    pthread_join(r1, NULL);
    pthread_join(r2, NULL);

    // 2. Standalone probe mode: PROT_NONE must return GMEM_UNMAPPED without SIGSEGV
    guest_mem_test_clear();
    void *noaccess = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(noaccess != MAP_FAILED);
    assert(guest_mem_validate_range((uintptr_t)noaccess, 16, 8, GMEM_PERM_R) == GMEM_UNMAPPED);
    assert(guest_mem_validate_range(0x1000, 16, 8, GMEM_PERM_R) == GMEM_OUT_OF_WINDOW);
    assert(guest_mem_validate_range(base, 16, 8, GMEM_PERM_R) == GMEM_OK);
    munmap(noaccess, 4096);
    munmap(arena, 16384);

    puts("PASS: guest_mem 4-level validation, TLS invalidation, seqlock concurrency and non-faulting probe");
    return 0;
}
