// Comprehensive NVN object lifecycle, memory safety, and concurrency test suite (Phase 2).
// Covers all 18 required unit test scenarios from Section H of the unified master plan.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../runtime/nvn/objects.c"
#include "nvn_test_harness.h"

// Minimal observable stubs for backend hooks
static _Atomic unsigned g_destroyed;
static _Atomic unsigned g_pool_written;
static _Atomic unsigned g_presented;
static NvnTexture *g_last_presented;

void backend_texture_destroyed(NvnTexture *t) {
    if (t) atomic_fetch_add(&g_destroyed, 1);
}
void backend_pool_written(uint64_t addr, uint64_t size) {
    (void)addr; (void)size;
    atomic_fetch_add(&g_pool_written, 1);
}
void backend_present(NvnTexture *t) {
    g_last_presented = t;
    atomic_fetch_add(&g_presented, 1);
}
void backend_init(void) {}
void backend_execute(uint64_t handle) { (void)handle; }
void renderer_sync(void) {}
void renderer_flush(void) {}
void renderer_submit(void) {}
uint64_t renderer_fence_seq(void) { return 42; }
void renderer_wait_seq(uint64_t seq) { (void)seq; }
int renderer_wait_seq_timeout(uint64_t seq, uint64_t ns) { (void)seq; (void)ns; return 0; }
void engage_timing_apply(Cpu *c) { (void)c; }
void engage_present_pace(void) {}
uint64_t plat_now_ns(void) { return 1000; }
void nvn_command_finalize(uint64_t h) { (void)h; }
void rt_fatal_trace(void) { _exit(99); }

static uint64_t call1(void (*fn)(Cpu *), uint64_t a0) {
    Cpu c = {0}; c.x[0] = a0; c.x[30] = 0x7000123400ull; fn(&c); return c.x[0];
}
static uint64_t call2(void (*fn)(Cpu *), uint64_t a0, uint64_t a1) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[30] = 0x7000123400ull; fn(&c); return c.x[0];
}
static uint64_t call3(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[30] = 0x7000123400ull; fn(&c); return c.x[0];
}
static uint64_t call4(void (*fn)(Cpu *), uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[30] = 0x7000123400ull; fn(&c); return c.x[0];
}

// Helper: initialize a 2D RGBA8 texture backed by a real memory pool
static void init_pool(uint64_t pool_buf[32], void *storage, size_t size) {
    uint64_t bld[8] = {0};
    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)bld);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)bld, (uintptr_t)storage, size);
    assert(call2(hle_nvnMemoryPoolInitialize, (uintptr_t)pool_buf, (uintptr_t)bld) == 1);
}

static void init_tex2d(uint64_t tex_buf[16], uint64_t pool_buf[32],
                       uint32_t w, uint32_t h, uint32_t levels, uint32_t flags, uint32_t target, uint32_t stride) {
    uint64_t tb[16] = {0};
    call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb);
    call2(hle_nvnTextureBuilderSetFlags, (uintptr_t)tb, flags);
    call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb, target);
    call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb, w, h);
    call2(hle_nvnTextureBuilderSetLevels, (uintptr_t)tb, levels);
    call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb, 0x25);  // RGBA8
    if (stride) call2(hle_nvnTextureBuilderSetStride, (uintptr_t)tb, stride);
    call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb, (uintptr_t)pool_buf, 0);
    assert(call2(hle_nvnTextureInitialize, (uintptr_t)tex_buf, (uintptr_t)tb) == 1);
}

// --- Isolated child helpers ---
static void child_get_width(void *arg) {
    call1(hle_nvnTextureGetWidth, (uint64_t)(uintptr_t)arg);
}
static void child_get_levels_addr(void *arg) {
    uint64_t addr = *(const uint64_t *)arg;
    call1(hle_nvnTextureGetLevels, addr);
}
static void child_finalize_null(void *arg) {
    (void)arg;
    call1(hle_nvnTextureFinalize, 0);
}
static void child_present_null_tex(void *arg) {
    (void)arg;
    uint64_t win_guest[16] = {0};
    uint64_t wb[16] = {0};
    uint64_t zero_tex[16] = {0};
    uint64_t tex_ptrs[2] = {(uintptr_t)zero_tex, 0};
    call1(hle_nvnWindowBuilderSetDefaults, (uintptr_t)wb);
    call3(hle_nvnWindowBuilderSetTextures, (uintptr_t)wb, 2, (uintptr_t)tex_ptrs);
    call2(hle_nvnWindowInitialize, (uintptr_t)win_guest, (uintptr_t)wb);
    call3(hle_nvnQueuePresentTexture, 0, (uintptr_t)win_guest, 0);
}
static void child_corrupt_unityfs_strict(void *arg) {
    (void)arg;
    setenv("ENGAGE_NVN_STRICT", "1", 1);
    uint64_t corrupt[16] = {0};
    corrupt[0] = 0x53;
    corrupt[1] = 0x0053467974696e55ull;  // "UnityFS\0"
    call1(hle_nvnTextureGetLevels, (uintptr_t)corrupt);
}
static void child_sampler_copy_double_finalize(void *arg) {
    (void)arg;
    uint64_t sb[12] = {0}, s1[12] = {0}, s2[12] = {0};
    call1(hle_nvnSamplerBuilderSetDefaults, (uintptr_t)sb);
    call3(hle_nvnSamplerBuilderSetMinMagFilter, (uintptr_t)sb, 1, 1);
    call2(hle_nvnSamplerInitialize, (uintptr_t)s1, (uintptr_t)sb);
    memcpy(s2, s1, sizeof s2);
    // Prevent compiler from eliding the raw host pointer copy / double-free
    __asm__ volatile("" : "+m"(s1), "+m"(s2) :: "memory");
    call1(hle_nvnSamplerFinalize, (uintptr_t)s1);
    call1(hle_nvnSamplerFinalize, (uintptr_t)s2);  // double-free today!
}

// ================================================================== T01..T18

static void child_get_width_strict(void *arg) {
    setenv("ENGAGE_NVN_STRICT", "1", 1);
    (void)call1(hle_nvnTextureGetWidth, (uintptr_t)arg);
}

static void t01_zeroed_guest_texture(void) {
    uint64_t zero[16] = {0};
    unsigned before = atomic_load(&g_destroyed);
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)zero) == 0, "T01 zeroed texture GetLevels == 0");
    NVN_CHECK(call1(hle_nvnTextureGetWidth, (uintptr_t)zero) == 0, "T01 zeroed texture GetWidth == 0");
    call1(hle_nvnTextureFinalize, (uintptr_t)zero);
    NVN_CHECK(atomic_load(&g_destroyed) == before, "T01 zeroed texture Finalize does not destroy backend");
}

static void t02_null_pointers(void) {
    NVN_CHECK(call1(hle_nvnTextureGetLevels, 0) == 0, "T02 GetLevels(NULL) returns 0 without crash");
    int rc_fin = nvn_run_isolated(child_finalize_null, NULL);
    NVN_XFAIL_UNTIL(3, rc_fin == 0, "T02 nvnTextureFinalize(NULL) is a safe no-op (no SIGSEGV)");
    int rc_pres = nvn_run_isolated(child_present_null_tex, NULL);
    NVN_XFAIL_UNTIL(4, rc_pres == 0 || rc_pres == 99, "T02 nvnQueuePresentTexture with uninitialized tex does not SIGSEGV");
}

static void t03_out_of_window_addresses(void) {
    uint64_t addrs[3] = {0x1000ull, 0xDEADBEEF0000ull, UINT64_MAX - 8ull};
    for (int i = 0; i < 3; i++) {
        int rc = nvn_run_isolated(child_get_levels_addr, &addrs[i]);
        NVN_XFAIL_UNTIL(3, rc == 0 || rc == 99, "T03 out-of-window / overflow address does not SIGSEGV");
    }
}

static void t04_t05_unmapped_and_noperm(void) {
    void *pg = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pg != MAP_FAILED);
    uint64_t addr = (uintptr_t)pg;
    int rc = nvn_run_isolated(child_get_levels_addr, &addr);
    NVN_XFAIL_UNTIL(3, rc == 0 || rc == 99, "T04/T05 PROT_NONE/unmapped page does not SIGSEGV");
    munmap(pg, 4096);
}

static void t06_corrupt_alien_unityfs(void) {
    // Under ENGAGE_NVN_STRICT=1, "UnityFS\0" corruption must NOT silently return 0!
    int rc = nvn_run_isolated(child_corrupt_unityfs_strict, NULL);
    NVN_XFAIL_UNTIL(4, rc == 99, "T06 corrupt 'UnityFS\\0' detected under ENGAGE_NVN_STRICT=1 (not silent 0)");
}

static void t07_recycled_generation_and_bounded_table(void) {
    static uint8_t storage[65536];
    uint64_t pool[32] = {0};
    init_pool(pool, storage, sizeof storage);
    uint64_t tex[16] = {0}, saved_first[16] = {0};

    // Cycle > 4096 textures to verify no unbounded table exhaustion / stale reuse
    init_tex2d(tex, pool, 32, 32, 3, 0, 1, 0);
    memcpy(saved_first, tex, sizeof saved_first);
    call1(hle_nvnTextureFinalize, (uintptr_t)tex);

    for (int i = 0; i < 4500; i++) {
        init_tex2d(tex, pool, 16, 16, 2, 0, 1, 0);
        call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    }
    // Check whether host slots were recycled (not leaked indefinitely):
    unsigned entries = nvn_texture_allocated_slots();
    NVN_XFAIL_UNTIL(3, entries < 4200, "T07 texture identity table recycles finalized slots (bounded memory)");
    // After 4500 cycles (> quarantine 1024), saved_first's slot was recycled with a new generation:
    NVN_XFAIL_UNTIL(3, nvn_texture_lookup((uintptr_t)saved_first).state == TEX_ST_STALE_RECYCLED,
                    "T07 recycled slot rejects obsolete generation handle (no ABA aliasing)");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    texture_identities_reset();
}

static void t08_reinit_over_live(void) {
    static uint8_t storage[65536];
    uint64_t pool[32] = {0}, tex[16] = {0};
    init_pool(pool, storage, sizeof storage);

    init_tex2d(tex, pool, 32, 32, 3, 0, 1, 0);
    unsigned destroyed_before = atomic_load(&g_destroyed);
    // Re-initialize directly over a live NVNtexture without prior Finalize
    init_tex2d(tex, pool, 64, 64, 5, 0, 1, 0);
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)tex) == 5, "T08 reinit updates levels");
    NVN_XFAIL_UNTIL(4, atomic_load(&g_destroyed) == destroyed_before + 1,
                    "T08 reinit over live texture retires previous backend resource cleanly");

    call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    texture_identities_reset();
}

static void t09_double_finalize_and_sentinel(void) {
    static uint8_t storage[65536];
    uint64_t pool[32] = {0}, tex[16] = {0};
    init_pool(pool, storage, sizeof storage);

    init_tex2d(tex, pool, 32, 32, 4, 0, 1, 0);
    unsigned d0 = atomic_load(&g_destroyed);
    call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    NVN_CHECK(atomic_load(&g_destroyed) == d0 + 1, "T09 first Finalize destroys backend once");
    // Finalize should leave a non-zero finalized sentinel in [0..16) to distinguish from ZEROED_GUEST
    int has_sentinel = (tex[0] != 0 || tex[1] != 0);
    NVN_XFAIL_UNTIL(4, has_sentinel, "T09 Finalize writes finalized sentinel (distinguishable from zeroed)");

    call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    NVN_CHECK(atomic_load(&g_destroyed) == d0 + 1, "T09 second Finalize is safe no-op");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    texture_identities_reset();
}

static void t10_t11_t12_value_copies_and_compare(void) {
    static uint8_t storage[65536];
    uint64_t pool[32] = {0}, orig[16] = {0}, copy1[16] = {0}, copy2[16] = {0};
    init_pool(pool, storage, sizeof storage);

    init_tex2d(orig, pool, 64, 64, 4, 0, 1, 0);
    memcpy(copy1, orig, sizeof copy1);
    memcpy(copy2, orig, sizeof copy2);

    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)orig) == 4, "T10 orig GetLevels == 4");
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)copy1) == 4, "T10 copy1 GetLevels == 4");
    NVN_CHECK(call1(hle_nvnTextureGetWidth, (uintptr_t)copy1) == 64, "T10 copy1 GetWidth == 64");
    NVN_XFAIL_UNTIL(4, call2(hle_nvnTextureCompare, (uintptr_t)orig, (uintptr_t)copy1) == 1,
                    "T10 nvnTextureCompare returns 1 for value copies");

    // T11: Finalize original while copy1 & copy2 remain active
    call1(hle_nvnTextureFinalize, (uintptr_t)orig);
    NVN_CHECK(nvn_texture_host((uintptr_t)orig) == NULL, "T11 finalized orig no longer resolves as live");
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)copy1) == 4, "T11 copy1 retains metadata after orig finalize");

    // T12: Finalize copy1 while copy2 remains active
    call1(hle_nvnTextureFinalize, (uintptr_t)copy1);
    NVN_CHECK(nvn_texture_host((uintptr_t)copy1) == NULL, "T12 finalized copy1 no longer resolves as live");
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)copy2) == 4, "T12 copy2 retains metadata after copy1 finalize");

    call1(hle_nvnTextureFinalize, (uintptr_t)copy2);
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    texture_identities_reset();
}

static void t13_t14_t15_reinit_reset_and_post_finalize(void) {
    static uint8_t storage[65536];
    uint64_t pool[32] = {0}, addr[16] = {0}, old_copy[16] = {0};
    init_pool(pool, storage, sizeof storage);

    // T13: Init -> copy -> Finalize -> Re-init at same address
    init_tex2d(addr, pool, 32, 32, 3, 0, 1, 0);
    uint64_t id1 = call1(hle_nvnTextureGetDebugID, (uintptr_t)addr);
    memcpy(old_copy, addr, sizeof old_copy);
    call1(hle_nvnTextureFinalize, (uintptr_t)addr);

    // T15: Post-finalize GetWidth returns 0 in non-strict mode, FATAL (exit 99) in strict mode; WriteTexels is rejected
    NVN_CHECK(call1(hle_nvnTextureGetWidth, (uintptr_t)addr) == 0, "T15 post-finalize GetWidth mitigated to 0");
    int rc_w = nvn_run_isolated(child_get_width_strict, addr);
    NVN_CHECK(rc_w == 99, "T15 post-finalize GetWidth strict mode fails cleanly");
    int32_t reg[6] = {0, 0, 0, 4, 4, 1};
    uint8_t px[64] = {0};
    unsigned pw0 = atomic_load(&g_pool_written);
    call4(hle_nvnTextureWriteTexels, (uintptr_t)addr, 0, (uintptr_t)reg, (uintptr_t)px);
    NVN_CHECK(atomic_load(&g_pool_written) == pw0, "T15 post-finalize WriteTexels rejected safely");

    init_tex2d(addr, pool, 64, 64, 6, 0, 1, 0);
    uint64_t id2 = call1(hle_nvnTextureGetDebugID, (uintptr_t)addr);
    NVN_CHECK(id2 != id1, "T13 reused address gets fresh debug_id");
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)addr) == 6, "T13 reused address returns new levels");
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)old_copy) == 3, "T13 old copy does not alias new texture");

    // T14: Runtime reset invalidates all handles
    texture_identities_reset();
    NVN_CHECK(nvn_texture_host((uintptr_t)addr) == NULL, "T14 reset invalidates live original");
    NVN_CHECK(nvn_texture_host((uintptr_t)old_copy) == NULL, "T14 reset invalidates live copy");

    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
}

// T16: Multithreaded Init / GetLevels / RegisterTexture / Finalize
typedef struct {
    uint64_t *pool;
    uint64_t *tpool;
    int tid;
    uint64_t ids[200];
} MtArg;

static void *mt_worker(void *arg) {
    MtArg *a = arg;
    for (int i = 0; i < 200; i++) {
        uint64_t tex[16] = {0};
        init_tex2d(tex, a->pool, 16, 16, 2, 0, 1, 0);
        a->ids[i] = call1(hle_nvnTextureGetDebugID, (uintptr_t)tex);
        assert(call1(hle_nvnTextureGetLevels, (uintptr_t)tex) == 2);
        call4(hle_nvnTexturePoolRegisterTexture, (uintptr_t)a->tpool, (uint64_t)(a->tid * 200 + i), (uintptr_t)tex, 0);
        call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    }
    return NULL;
}

static void t16_multithreaded_queries(void) {
    static uint8_t storage[65536];
    uint64_t pool[32] = {0}, tpool[4] = {0};
    init_pool(pool, storage, sizeof storage);
    call4(hle_nvnTexturePoolInitialize, (uintptr_t)tpool, (uintptr_t)pool, 0, 1024);

    pthread_t th[4];
    MtArg args[4];
    for (int i = 0; i < 4; i++) {
        args[i] = (MtArg){.pool = pool, .tpool = tpool, .tid = i};
        assert(pthread_create(&th[i], NULL, mt_worker, &args[i]) == 0);
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);

    // Check uniqueness of all 800 debug_ids (catches non-atomic next_debug_id++ race if it collides)
    int dupes = 0;
    for (int a = 0; a < 4; a++)
        for (int i = 0; i < 200; i++)
            for (int b = a; b < 4; b++)
                for (int j = (a == b ? i + 1 : 0); j < 200; j++)
                    if (args[a].ids[i] == args[b].ids[j]) dupes++;
    NVN_CHECK(dupes == 0 || NVN_TEST_PHASE < 3, "T16 multithreaded debug_id uniqueness (atomic next_debug_id)");
    if (dupes > 0 && NVN_TEST_PHASE < 3) {
        printf("  [XFAIL phase 3] T16 observed %d duplicate debug_ids from non-atomic next_debug_id++\n", dupes);
        g_nvn_xfail++;
    }

    call1(hle_nvnTexturePoolFinalize, (uintptr_t)tpool);
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    desc_pools_reset();
    texture_identities_reset();
}

static void t17_storage_size_pool_and_sync_lifetime(void) {
    static uint8_t storage[262144];
    uint64_t pool[32] = {0}, tb[16] = {0}, tex[16] = {0};
    init_pool(pool, storage, sizeof storage);

    // Linear texture (flags & 0x10): builder_storage_size vs nvnTextureGetStorageSize
    call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb);
    call2(hle_nvnTextureBuilderSetFlags, (uintptr_t)tb, 0x10);  // LINEAR
    call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb, 1);    // 2D
    call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb, 100, 64);
    call2(hle_nvnTextureBuilderSetLevels, (uintptr_t)tb, 1);
    call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb, 0x25); // RGBA8 (4 B/px)
    call2(hle_nvnTextureBuilderSetStride, (uintptr_t)tb, 512);  // 512 * 64 = 32768
    uint64_t bsz = call1(hle_nvnTextureBuilderGetStorageSize, (uintptr_t)tb);
    call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb, (uintptr_t)pool, 0);
    call2(hle_nvnTextureInitialize, (uintptr_t)tex, (uintptr_t)tb);
    uint64_t tsz = call1(hle_nvnTextureGetStorageSize, (uintptr_t)tex);
    NVN_CHECK(bsz == 32768, "T17 linear TextureBuilderGetStorageSize == 32768");
    NVN_XFAIL_UNTIL(4, tsz == bsz, "T17 linear nvnTextureGetStorageSize matches builder_storage_size");

    // CMD_FENCE_SYNC guest memory corruption after nvnSyncFinalize + guest reuse
    uint64_t sync_mem[8] = {0};
    call2(hle_nvnSyncInitialize, (uintptr_t)sync_mem, 0x1234);
    call1(hle_nvnSyncFinalize, (uintptr_t)sync_mem);
    // Simulate deferred execution hitting finalized sync whose guest memory is now zeroed/reused:
    NVN_CHECK(sync_mem[0] == 0, "T17 finalized sync guest word is 0 (before Phase 5 handle)");
    nvn_sync_set_seq((uintptr_t)sync_mem, 99);
    // In Phase 2, sync_of() allocates a new NvnSync* and overwrites sync_mem[0] (guest corruption!).
    NVN_XFAIL_UNTIL(5, sync_mem[0] == 0, "T17 deferred nvn_sync_set_seq does NOT corrupt finalized guest memory");
    if (sync_mem[0] != 0) call1(hle_nvnSyncFinalize, (uintptr_t)sync_mem);

    // Dangling NvnTexture.pool after nvnMemoryPoolFinalize (checked structurally without dereferencing freed pool)
    NvnTexture *ht = nvn_texture_host((uintptr_t)tex);
    NVN_CHECK(ht != NULL, "T17 host texture valid before pool finalize");
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    // In Phase 5, NvnTexture should reference pool by generational handle, not raw freed NvnPool*.
    NVN_XFAIL_UNTIL(5, ht->pool == NULL, "T17 NvnTexture does not hold raw dangling NvnPool* after pool finalize");

    call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    texture_identities_reset();
}

static void t18_sampler_copy_double_finalize(void) {
    int rc = nvn_run_isolated(child_sampler_copy_double_finalize, NULL);
    NVN_XFAIL_UNTIL(5, rc == 0, "T18 value-copied NVNsampler double finalize does not crash / double-free");
}

static void t19_chapter_end_corrupt_depth_getters(void) {
    // Reproduces Engage chapter-end scene transition at 0x7000a72738..0x7000a7278c:
    // Unity frees the depth NVNtexture page and reuses it for other C++ objects,
    // then calls GetLevels -> TextureViewSetLevels(-1, 1) -> GetSamples -> GetTarget.
    uint64_t corrupt[8] = {0x03c70f0b201fc400ull, 0x03d70f0c7b830014ull, 0, 0, 0, 0, 0, 0};
    NVN_CHECK(call1(hle_nvnTextureGetLevels, (uintptr_t)corrupt) == 0, "T19 corrupt GetLevels returns 0");
    NVN_CHECK(call1(hle_nvnTextureGetSamples, (uintptr_t)corrupt) == 0, "T19 corrupt GetSamples returns 0");
    NVN_CHECK(call1(hle_nvnTextureGetTarget, (uintptr_t)corrupt) == 0, "T19 corrupt GetTarget returns 0");
    NVN_CHECK(call1(hle_nvnTextureGetWidth, (uintptr_t)corrupt) == 0, "T19 corrupt GetWidth returns 0");
    NVN_CHECK(call1(hle_nvnTextureGetHeight, (uintptr_t)corrupt) == 0, "T19 corrupt GetHeight returns 0");
    NVN_CHECK(call1(hle_nvnTextureGetFormat, (uintptr_t)corrupt) == 0, "T19 corrupt GetFormat returns 0");
    NVN_CHECK(call1(hle_nvnTextureGetStorageSize, (uintptr_t)corrupt) == 0, "T19 corrupt GetStorageSize returns 0");

    uint32_t sw[4] = {9, 9, 9, 9};
    Cpu c = {0};
    c.x[0] = (uintptr_t)corrupt;
    c.x[1] = (uintptr_t)&sw[0];
    c.x[2] = (uintptr_t)&sw[1];
    c.x[3] = (uintptr_t)&sw[2];
    c.x[4] = (uintptr_t)&sw[3];
    hle_nvnTextureGetSwizzle(&c);
    NVN_CHECK(sw[0] == 0 && sw[1] == 0 && sw[2] == 0 && sw[3] == 0, "T19 corrupt GetSwizzle zeroes outputs");

    NvnTextureView view;
    call1(hle_nvnTextureViewSetDefaults, (uintptr_t)&view);
    call3(hle_nvnTextureViewSetLevels, (uintptr_t)&view, (uint64_t)(uint32_t)-1, 1);
    NVN_CHECK(view.base_level == 0 && view.num_levels == 1, "T19 TextureViewSetLevels clamps negative base_level to 0");
}

static void t20_msaa_storage_size_and_alignment(void) {
    static uint8_t storage[8 * 1024 * 1024];
    uint64_t pool[32] = {0}, tb1[16] = {0}, tb4[16] = {0}, tex[16] = {0};
    init_pool(pool, storage, sizeof storage);

    call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb1);
    call2(hle_nvnTextureBuilderSetFlags, (uintptr_t)tb1, 0x08);  // COMPRESSIBLE
    call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb1, 1);    // 2D
    call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb1, 256, 256);
    call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb1, 0x25); // RGBA8
    uint64_t sz1 = call1(hle_nvnTextureBuilderGetStorageSize, (uintptr_t)tb1);
    uint64_t al1 = call1(hle_nvnTextureBuilderGetStorageAlignment, (uintptr_t)tb1);

    call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb4);
    call2(hle_nvnTextureBuilderSetFlags, (uintptr_t)tb4, 0x08);
    call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb4, 5);    // 2D_MULTISAMPLE
    call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb4, 256, 256);
    call2(hle_nvnTextureBuilderSetSamples, (uintptr_t)tb4, 4);
    call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb4, 0x25);
    uint64_t sz4 = call1(hle_nvnTextureBuilderGetStorageSize, (uintptr_t)tb4);
    uint64_t al4 = call1(hle_nvnTextureBuilderGetStorageAlignment, (uintptr_t)tb4);

    call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb4, (uintptr_t)pool, 0);
    call2(hle_nvnTextureInitialize, (uintptr_t)tex, (uintptr_t)tb4);
    uint64_t tsz4 = call1(hle_nvnTextureGetStorageSize, (uintptr_t)tex);

    NVN_CHECK(sz1 == 256 * 256 * 4, "T20 1x RGBA8 256x256 == 256 KB");
    NVN_CHECK(sz4 >= sz1 * 4, "T20 4x MSAA 256x256 >= 4x single-sample size");
    NVN_CHECK(tsz4 == sz4, "T20 texture GetStorageSize matches MSAA builder size");
    NVN_CHECK(al1 == 65536, "T20 compressible texture alignment == 65536");
    NVN_CHECK(al4 == 65536, "T20 MSAA texture alignment == 65536");

    call1(hle_nvnTextureFinalize, (uintptr_t)tex);
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)pool);
    texture_identities_reset();
    pool_identities_reset();
}

static void *sync_signaler_thread(void *arg) {
    uint64_t *sync_mem = (uint64_t *)arg;
    struct timespec slp = {0, 2000000}; // 2 ms
    nanosleep(&slp, NULL);
    nvn_sync_set_seq((uintptr_t)sync_mem, 123);
    return NULL;
}

static void t21_sync_wait_unsignaled_and_signaled(void) {
    uint64_t sync_mem[8] = {0};
    NVN_CHECK(call2(hle_nvnSyncInitialize, (uintptr_t)sync_mem, 0x1234) == 1, "T21 SyncInitialize == 1");
    // Unsignaled with 0 timeout -> immediate TIMEOUT (2)
    NVN_CHECK(call2(hle_nvnSyncWait, (uintptr_t)sync_mem, 0) == 2, "T21 unsignaled poll timeout=0 returns 2");
    // Unsignaled with short timeout -> TIMEOUT (2)
    NVN_CHECK(call2(hle_nvnSyncWait, (uintptr_t)sync_mem, 1000000ull) == 2, "T21 unsignaled short timeout returns 2");
    // Unsignaled with 200 ms timeout while another thread signals at 2 ms -> succeeds (stub returns 0)
    pthread_t th;
    pthread_create(&th, NULL, sync_signaler_thread, sync_mem);
    uint64_t r = call2(hle_nvnSyncWait, (uintptr_t)sync_mem, 200000000ull);
    pthread_join(th, NULL);
    NVN_CHECK(r == 0, "T21 unsignaled wait wakes up once signaled by peer thread");
    call1(hle_nvnSyncFinalize, (uintptr_t)sync_mem);
    sync_identities_reset();
}

int main(void) {
    printf("=== test_nvn_lifecycle (NVN_TEST_PHASE=%d) ===\n", NVN_TEST_PHASE);
    t01_zeroed_guest_texture();
    t02_null_pointers();
    t03_out_of_window_addresses();
    t04_t05_unmapped_and_noperm();
    t06_corrupt_alien_unityfs();
    t07_recycled_generation_and_bounded_table();
    t08_reinit_over_live();
    t09_double_finalize_and_sentinel();
    t10_t11_t12_value_copies_and_compare();
    t13_t14_t15_reinit_reset_and_post_finalize();
    t16_multithreaded_queries();
    t17_storage_size_pool_and_sync_lifetime();
    t18_sampler_copy_double_finalize();
    t19_chapter_end_corrupt_depth_getters();
    t20_msaa_storage_size_and_alignment();
    t21_sync_wait_unsignaled_and_signaled();
    return nvn_summary("test_nvn_lifecycle");
}
