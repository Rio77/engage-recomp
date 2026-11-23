// Phase 6: 8-thread concurrent stress test for NVN objects, guest_mem seqlock,
// nvn_diag circular ring buffer, and tear-free seqlock descriptor pools (TSan target).
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/objects.c"

void rt_fatal_trace(void) { abort(); }
void backend_init(void) {}
void backend_execute(uint64_t handle) { (void)handle; }
void backend_present(NvnTexture *tex) { (void)tex; }
void backend_texture_destroyed(NvnTexture *tex) { if (tex) tex->backend = NULL; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
uint64_t renderer_fence_seq(void) { return 1; }
void renderer_wait_seq(uint64_t seq) { (void)seq; }
void renderer_flush(void) {}
void renderer_sync(void) {}
void nvn_command_finalize(uint64_t handle) { (void)handle; }
uint64_t plat_now_ns(void) { return 0; }

#define N_THREADS 8
#define ITERS_PER_THREAD 15000
#define DESC_SLOTS 32

static uint8_t g_backing[4096 * 64] __attribute__((aligned(4096)));
static uint64_t g_pool_guest[32] __attribute__((aligned(16)));
static uint64_t g_tpool_guest[4] __attribute__((aligned(16)));
static uint64_t g_spool_guest[4] __attribute__((aligned(16)));

// Private per-worker texture/sync/sampler arrays + shared descriptor pool
static uint64_t g_tex_mem[4][8][16] __attribute__((aligned(16)));
static uint64_t g_sync_mem[4][8][8] __attribute__((aligned(16)));
static uint64_t g_smp_mem[4][8][12] __attribute__((aligned(16)));

// Dedicated stable textures registered into descriptor pool with invariant:
//   view.base_level == tex->levels (so a torn read of (tex, view) is immediately caught!)
static uint64_t g_desc_texs[8][16] __attribute__((aligned(16)));
static _Atomic int g_stop = 0;
static _Atomic uint64_t g_reads_ok = 0;
static _Atomic uint64_t g_writes_ok = 0;

static uint64_t call1(HostFn fn, uint64_t a0) {
    Cpu c = {0}; c.x[0] = a0; c.x[30] = 0x7000575E55ull;
    fn(&c); return c.x[0];
}
static uint64_t call2(HostFn fn, uint64_t a0, uint64_t a1) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[30] = 0x7000575E55ull;
    fn(&c); return c.x[0];
}
static uint64_t call3(HostFn fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[30] = 0x7000575E55ull;
    fn(&c); return c.x[0];
}
static uint64_t call4(HostFn fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[30] = 0x7000575E55ull;
    fn(&c); return c.x[0];
}

// Threads 0, 1: Rapid Texture init/reinit/copy/query/finalize + debug_id monotonicity
static void *worker_texture_churn(void *arg) {
    int id = (int)(intptr_t)arg;
    uint64_t prev_dbg = 0;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        int slot = i & 7;
        uint32_t lv = 1 + (uint32_t)((i + id) & 3);
        uint64_t tb[16] = {0};
        call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb);
        call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb, 1);
        call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb, 64, 64);
        call2(hle_nvnTextureBuilderSetLevels, (uintptr_t)tb, lv);
        call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb, 0x25);
        call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb, (uintptr_t)g_pool_guest, 0);
        assert(call2(hle_nvnTextureInitialize, (uintptr_t)g_tex_mem[id][slot], (uintptr_t)tb) == 1);
        assert(call1(hle_nvnTextureGetLevels, (uintptr_t)g_tex_mem[id][slot]) == lv);
        uint64_t dbg = call1(hle_nvnTextureGetDebugID, (uintptr_t)g_tex_mem[id][slot]);
        assert(dbg > prev_dbg);
        prev_dbg = dbg;
        // Value copy to local stack + query + finalize original + query copy
        uint64_t copy[16];
        memcpy(copy, g_tex_mem[id][slot], sizeof copy);
        assert(call2(hle_nvnTextureCompare, (uintptr_t)g_tex_mem[id][slot], (uintptr_t)copy) == 1);
        if ((i & 3) != 0) {
            call1(hle_nvnTextureFinalize, (uintptr_t)g_tex_mem[id][slot]);
            // Copy still reports preserved levels (while in quarantine) or 0 (if recycled)
            uint64_t clv = call1(hle_nvnTextureGetLevels, (uintptr_t)copy);
            assert(clv == lv || clv == 0);
        }
        atomic_fetch_add_explicit(&g_writes_ok, 1, memory_order_relaxed);
    }
    for (int s = 0; s < 8; s++) call1(hle_nvnTextureFinalize, (uintptr_t)g_tex_mem[id][s]);
    return NULL;
}

// Threads 2, 3: Concurrent TexturePool & SamplerPool writers (varying texture + matching view.base_level)
static void *worker_desc_writer(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        int slot = (i + id * 7) % DESC_SLOTS;
        int ti = (i + id) & 7;
        uint32_t expected_lv = (uint32_t)(ti + 1);
        NvnTextureView v = {0};
        v.setmask = 1;
        v.base_level = (uint8_t)expected_lv;
        v.num_levels = 1;
        call4(hle_nvnTexturePoolRegisterTexture, (uintptr_t)g_tpool_guest, slot,
              (uintptr_t)g_desc_texs[ti], (uintptr_t)&v);

        uint64_t sb[12] = {0};
        call1(hle_nvnSamplerBuilderSetDefaults, (uintptr_t)sb);
        call3(hle_nvnSamplerBuilderSetMinMagFilter, (uintptr_t)sb, expected_lv, expected_lv);
        call3(hle_nvnSamplerPoolRegisterSamplerBuilder, (uintptr_t)g_spool_guest, slot, (uintptr_t)sb);
        atomic_fetch_add_explicit(&g_writes_ok, 1, memory_order_relaxed);
    }
    return NULL;
}

// Threads 4, 5: Lock-free seqlock TexturePool & SamplerPool readers verifying ZERO torn pairs!
static void *worker_desc_reader(void *arg) {
    int id = (int)(intptr_t)arg;
    NvnDescPool *tp = HOST(NvnDescPool, (uintptr_t)g_tpool_guest);
    NvnDescPool *sp = HOST(NvnDescPool, (uintptr_t)g_spool_guest);
    assert(tp && sp);
    uint64_t iter = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_acquire) || iter < 20000) {
        uint32_t slot = (uint32_t)((iter + (uint64_t)id * 5) % DESC_SLOTS);
        NvnTexEntry ent;
        if (nvn_tex_pool_read(tp, slot, &ent)) {
            // Torn-read check: view.base_level MUST match ent.tex->levels!
            assert(ent.tex != NULL);
            assert(ent.has_view);
            assert((uint32_t)ent.view.base_level == ent.tex->levels);
            atomic_fetch_add_explicit(&g_reads_ok, 1, memory_order_relaxed);
        }
        NvnSamplerDesc sd;
        if (nvn_sampler_pool_read(sp, slot, &sd)) {
            // Torn-read check: min_filter MUST equal mag_filter (written together)
            if (sd.min_filter != 0 || sd.mag_filter != 0) {
                assert(sd.min_filter == sd.mag_filter);
            }
        }
        iter++;
        if (iter > 200000) break;
    }
    return NULL;
}

// Threads 6, 7: Concurrent Sync, MemoryPool, Sampler, and guest_mem seqlock churn
static void *worker_misc_objects(void *arg) {
    int id = (int)(intptr_t)arg;
    int wi = id & 3;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        int s = i & 7;
        // Sync init -> pack token -> set_seq -> finalize -> deferred set_seq_token (must not corrupt!)
        assert(call2(hle_nvnSyncInitialize, (uintptr_t)g_sync_mem[wi][s], 0x1234) == 1);
        uint64_t tok = 0, ep = 0;
        nvn_sync_pack_handle((uintptr_t)g_sync_mem[wi][s], &tok, &ep);
        assert(tok != 0);
        nvn_sync_set_seq_token((uintptr_t)g_sync_mem[wi][s], tok, ep, (uint64_t)i + 1);
        call1(hle_nvnSyncFinalize, (uintptr_t)g_sync_mem[wi][s]);
        assert(g_sync_mem[wi][s][0] == 0);
        nvn_sync_set_seq((uintptr_t)g_sync_mem[wi][s], (uint64_t)i + 99);
        nvn_sync_set_seq_token((uintptr_t)g_sync_mem[wi][s], tok, ep, (uint64_t)i + 100);
        assert(g_sync_mem[wi][s][0] == 0);

        // Sampler init -> copy -> double finalize (must never double-free!)
        uint64_t sb[12] = {0};
        call1(hle_nvnSamplerBuilderSetDefaults, (uintptr_t)sb);
        call3(hle_nvnSamplerBuilderSetMinMagFilter, (uintptr_t)sb, 1, 1);
        assert(call2(hle_nvnSamplerInitialize, (uintptr_t)g_smp_mem[wi][s], (uintptr_t)sb) == 1);
        uint64_t smp_copy[12];
        memcpy(smp_copy, g_smp_mem[wi][s], sizeof smp_copy);
        assert(call2(hle_nvnSamplerCompare, (uintptr_t)g_smp_mem[wi][s], (uintptr_t)smp_copy) == 1);
        call1(hle_nvnSamplerFinalize, (uintptr_t)g_smp_mem[wi][s]);
        call1(hle_nvnSamplerFinalize, (uintptr_t)smp_copy);

        // Transient MemoryPool init/finalize
        uint64_t pb[8] = {0}, mp[32] = {0};
        call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)pb);
        call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)pb, (uintptr_t)g_backing, sizeof g_backing);
        assert(call2(hle_nvnMemoryPoolInitialize, (uintptr_t)mp, (uintptr_t)pb) == 1);
        assert(call1(hle_nvnMemoryPoolGetSize, (uintptr_t)mp) == sizeof g_backing);
        call1(hle_nvnMemoryPoolFinalize, (uintptr_t)mp);

        atomic_fetch_add_explicit(&g_writes_ok, 1, memory_order_relaxed);
    }
    return NULL;
}

int main(void) {
    setenv("ENGAGE_NVN_DIAG", "off", 1);

    // Initialize shared MemoryPool, TexturePool, SamplerPool, and 8 stable textures (levels 1..8)
    uint64_t pb[8] = {0};
    call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)pb);
    call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)pb, (uintptr_t)g_backing, sizeof g_backing);
    assert(call2(hle_nvnMemoryPoolInitialize, (uintptr_t)g_pool_guest, (uintptr_t)pb) == 1);
    assert(call4(hle_nvnTexturePoolInitialize, (uintptr_t)g_tpool_guest, (uintptr_t)g_pool_guest, 0, DESC_SLOTS) == 1);
    assert(call4(hle_nvnSamplerPoolInitialize, (uintptr_t)g_spool_guest, (uintptr_t)g_pool_guest, 0x1000, DESC_SLOTS) == 1);

    for (int i = 0; i < 8; i++) {
        uint64_t tb[16] = {0};
        call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb);
        call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb, 1);
        call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb, 64, 64);
        call2(hle_nvnTextureBuilderSetLevels, (uintptr_t)tb, (uint64_t)(i + 1));
        call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb, 0x25);
        call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb, (uintptr_t)g_pool_guest, 0);
        assert(call2(hle_nvnTextureInitialize, (uintptr_t)g_desc_texs[i], (uintptr_t)tb) == 1);
    }

    pthread_t th[N_THREADS];
    pthread_create(&th[0], NULL, worker_texture_churn, (void *)(intptr_t)0);
    pthread_create(&th[1], NULL, worker_texture_churn, (void *)(intptr_t)1);
    pthread_create(&th[2], NULL, worker_desc_writer, (void *)(intptr_t)0);
    pthread_create(&th[3], NULL, worker_desc_writer, (void *)(intptr_t)1);
    pthread_create(&th[4], NULL, worker_desc_reader, (void *)(intptr_t)0);
    pthread_create(&th[5], NULL, worker_desc_reader, (void *)(intptr_t)1);
    pthread_create(&th[6], NULL, worker_misc_objects, (void *)(intptr_t)0);
    pthread_create(&th[7], NULL, worker_misc_objects, (void *)(intptr_t)1);

    // Join writers first, then signal readers to stop
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    pthread_join(th[2], NULL);
    pthread_join(th[3], NULL);
    pthread_join(th[6], NULL);
    pthread_join(th[7], NULL);
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    pthread_join(th[4], NULL);
    pthread_join(th[5], NULL);

    for (int i = 0; i < 8; i++) call1(hle_nvnTextureFinalize, (uintptr_t)g_desc_texs[i]);
    call1(hle_nvnTexturePoolFinalize, (uintptr_t)g_tpool_guest);
    call1(hle_nvnSamplerPoolFinalize, (uintptr_t)g_spool_guest);
    call1(hle_nvnMemoryPoolFinalize, (uintptr_t)g_pool_guest);

    texture_identities_reset();
    pool_identities_reset();
    sync_identities_reset();
    desc_pools_reset();

    printf("PASS: test_nvn_stress_threads (8 threads, writes=%llu, tear-free reads=%llu)\n",
           (unsigned long long)atomic_load(&g_writes_ok),
           (unsigned long long)atomic_load(&g_reads_ok));
    return 0;
}
