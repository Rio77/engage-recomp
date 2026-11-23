// Phase 6: Shadow-model lifecycle fuzzer for the NVN object subsystem.
// Checks invariants I1-I5 across randomized sequences of Texture, TextureView,
// MemoryPool, Sync, Sampler, TexturePool, SamplerPool, Program, and RT_ON_RESET.
//
// Modes:
//   - Standalone deterministic seeded runner (default): runs 200,000 operations
//     (override with NVN_FUZZ_ITERS=<n>, NVN_FUZZ_SEED=<hex>).
//   - libFuzzer target (-DFUZZ_LIBFUZZER): exposes LLVMFuzzerTestOneInput.
#include <assert.h>
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

#define N_POOLS    8
#define N_TEXS     32
#define N_SYNCS    16
#define N_SAMPLERS 16
#define DESC_CAP   64
#define CANARY_U64 0xD1E5A7E5C0FFEE99ull

typedef struct {
    uint64_t pre[2];
    uint8_t body[0x100];
    uint64_t post[2];
} GuardedPool;

typedef struct {
    uint64_t pre[2];
    uint8_t body[0x80];
    uint64_t post[2];
} GuardedTex;

typedef struct {
    uint64_t pre[2];
    uint8_t body[0x10];
    uint64_t post[2];
} GuardedView;

typedef struct {
    uint64_t pre[2];
    uint8_t body[0x40];
    uint64_t post[2];
} GuardedSync;

typedef struct {
    uint64_t pre[2];
    uint8_t body[0x60];
    uint64_t post[2];
} GuardedSampler;

typedef struct {
    uint64_t pre[2];
    uint8_t body[0x20];
    uint64_t post[2];
} GuardedDescPool;

static uint8_t g_pool_backing[N_POOLS][4096 * 16] __attribute__((aligned(4096)));
static GuardedPool g_pools[N_POOLS];
static GuardedTex g_texs[N_TEXS];
static GuardedSync g_syncs[N_SYNCS];
static GuardedSampler g_samplers[N_SAMPLERS];
static GuardedDescPool g_tpool, g_spool;

typedef enum {
    ST_ZERO = 0,
    ST_LIVE_ORIG,
    ST_LIVE_COPY,
    ST_FINALIZED,
    ST_STALE_COPY,
    ST_STALE_RESET,
    ST_CORRUPT,
} ShadowTexKind;

typedef struct {
    ShadowTexKind kind;
    uint32_t levels;
    uint32_t width;
    uint64_t debug_id;
    int origin_idx;  // for copies: which origin slot it was copied from
    uint64_t origin_gen_id;  // monotonic shadow birth id
} ShadowTex;

typedef struct {
    bool live;
    bool finalized;
} ShadowPool;

typedef struct {
    bool live;
    bool finalized;
} ShadowSync;

typedef struct {
    bool live;
    uint32_t min_f, mag_f;
} ShadowSampler;

static ShadowPool s_pools[N_POOLS];
static ShadowTex s_texs[N_TEXS];
static ShadowSync s_syncs[N_SYNCS];
static ShadowSampler s_samplers[N_SAMPLERS];
static bool s_desc_live = false;
static uint64_t s_next_birth = 1;

static void init_canaries(uint64_t pre[2], uint64_t post[2]) {
    pre[0] = pre[1] = CANARY_U64;
    post[0] = post[1] = CANARY_U64;
}
static void check_canaries(const uint64_t pre[2], const uint64_t post[2], const char *what) {
    if (pre[0] != CANARY_U64 || pre[1] != CANARY_U64 || post[0] != CANARY_U64 || post[1] != CANARY_U64) {
        fprintf(stderr, "FUZZ INVARIANT I1 FAIL: canary smashed on %s\n", what);
        abort();
    }
}

static uint64_t call1(HostFn fn, uint64_t a0) {
    Cpu c = {0}; c.x[0] = a0; c.x[30] = 0x7000F00000ull;
    fn(&c); return c.x[0];
}
static uint64_t call2(HostFn fn, uint64_t a0, uint64_t a1) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[30] = 0x7000F00000ull;
    fn(&c); return c.x[0];
}
static uint64_t call3(HostFn fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[30] = 0x7000F00000ull;
    fn(&c); return c.x[0];
}
static uint64_t call4(HostFn fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu c = {0}; c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[30] = 0x7000F00000ull;
    fn(&c); return c.x[0];
}

static void fuzz_reset_all(void) {
    texture_identities_reset();
    pool_identities_reset();
    sync_identities_reset();
    desc_pools_reset();
    program_identities_reset();
    nvn_diag_reset();

    for (int i = 0; i < N_POOLS; i++) {
        init_canaries(g_pools[i].pre, g_pools[i].post);
        memset(g_pools[i].body, 0, sizeof g_pools[i].body);
        s_pools[i] = (ShadowPool){0};
    }
    for (int i = 0; i < N_TEXS; i++) {
        init_canaries(g_texs[i].pre, g_texs[i].post);
        memset(g_texs[i].body, 0, sizeof g_texs[i].body);
        s_texs[i] = (ShadowTex){0};
    }
    for (int i = 0; i < N_SYNCS; i++) {
        init_canaries(g_syncs[i].pre, g_syncs[i].post);
        memset(g_syncs[i].body, 0, sizeof g_syncs[i].body);
        s_syncs[i] = (ShadowSync){0};
    }
    for (int i = 0; i < N_SAMPLERS; i++) {
        init_canaries(g_samplers[i].pre, g_samplers[i].post);
        memset(g_samplers[i].body, 0, sizeof g_samplers[i].body);
        s_samplers[i] = (ShadowSampler){0};
    }
    init_canaries(g_tpool.pre, g_tpool.post);
    memset(g_tpool.body, 0, sizeof g_tpool.body);
    init_canaries(g_spool.pre, g_spool.post);
    memset(g_spool.body, 0, sizeof g_spool.body);
    s_desc_live = false;
}

static void mark_copies_stale(uint64_t birth_id) {
    for (int i = 0; i < N_TEXS; i++) {
        if (s_texs[i].kind == ST_LIVE_COPY && s_texs[i].origin_gen_id == birth_id) {
            s_texs[i].kind = ST_STALE_COPY;
        }
    }
}

static void verify_invariants(void) {
    // I1: Canaries intact across every object slot
    for (int i = 0; i < N_POOLS; i++) check_canaries(g_pools[i].pre, g_pools[i].post, "MemoryPool");
    for (int i = 0; i < N_TEXS; i++) check_canaries(g_texs[i].pre, g_texs[i].post, "Texture");
    for (int i = 0; i < N_SYNCS; i++) check_canaries(g_syncs[i].pre, g_syncs[i].post, "Sync");
    for (int i = 0; i < N_SAMPLERS; i++) check_canaries(g_samplers[i].pre, g_samplers[i].post, "Sampler");
    check_canaries(g_tpool.pre, g_tpool.post, "TexturePool");
    check_canaries(g_spool.pre, g_spool.post, "SamplerPool");

    // I2 + I3 + I4: Texture shadow checks
    for (int i = 0; i < N_TEXS; i++) {
        uint64_t addr = (uintptr_t)g_texs[i].body;
        NvnTexLookup lk = nvn_texture_lookup(addr);
        switch (s_texs[i].kind) {
        case ST_ZERO:
            assert(lk.state == TEX_ST_ZEROED);
            assert(call1(hle_nvnTextureGetLevels, addr) == 0);
            break;
        case ST_LIVE_ORIG:
            assert(lk.state == TEX_ST_LIVE_ORIG && lk.tex != NULL);
            assert(lk.tex->pool == NULL);  // I2: never retain raw NvnPool*
            assert(call1(hle_nvnTextureGetLevels, addr) == s_texs[i].levels);
            assert(call1(hle_nvnTextureGetWidth, addr) == s_texs[i].width);
            assert(call1(hle_nvnTextureGetDebugID, addr) == s_texs[i].debug_id);
            break;
        case ST_LIVE_COPY:
            assert(lk.state == TEX_ST_LIVE_COPY && lk.tex != NULL);
            assert(lk.tex->pool == NULL);
            assert(call1(hle_nvnTextureGetLevels, addr) == s_texs[i].levels);
            break;
        case ST_FINALIZED:
            assert(lk.state == TEX_ST_FINALIZED_INST);
            assert(nvn_texture_host(addr) == NULL);
            assert(call1(hle_nvnTextureGetLevels, addr) == 0);
            break;
        case ST_STALE_COPY:
            // Either STALE_COPY (still in quarantine: preserved metadata) or STALE_RECYCLED (after >1024 finalizes)
            assert(lk.state == TEX_ST_STALE_COPY || lk.state == TEX_ST_STALE_RECYCLED);
            if (lk.state == TEX_ST_STALE_COPY) {
                assert(nvn_texture_host(addr) == lk.tex);
                assert(call1(hle_nvnTextureGetLevels, addr) == s_texs[i].levels);
            } else {
                assert(nvn_texture_host(addr) == NULL);
                assert(call1(hle_nvnTextureGetLevels, addr) == 0);
            }
            break;
        case ST_STALE_RESET:
            assert(lk.state == TEX_ST_STALE_RESET);
            assert(nvn_texture_host(addr) == NULL);
            assert(call1(hle_nvnTextureGetLevels, addr) == 0);
            break;
        case ST_CORRUPT:
            assert(lk.state == TEX_ST_CORRUPT);
            assert(nvn_texture_host(addr) == NULL);
            assert(call1(hle_nvnTextureGetLevels, addr) == 0);
            break;
        }
    }

    // I3: Finalized sync guest word[0] stays 0
    for (int i = 0; i < N_SYNCS; i++) {
        if (s_syncs[i].finalized) {
            uint64_t w0 = 0;
            memcpy(&w0, g_syncs[i].body, 8);
            assert(w0 == 0);
        }
    }

    // I5: Bounded texture slot table (never grows past ~quarantine + live slots)
    assert(nvn_texture_allocated_slots() <= NVN_TEX_QUARANTINE + N_TEXS + 64);
}

static void exec_op(uint8_t op, uint8_t a, uint8_t b, uint8_t c_arg) {
    switch (op % 15) {
    case 0: {  // Pool init
        int pi = a % N_POOLS;
        uint64_t bld[8] = {0};
        call2(hle_nvnMemoryPoolBuilderSetDevice, (uintptr_t)bld, 0x1234);
        call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)bld);
        call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)bld, (uintptr_t)g_pool_backing[pi], sizeof g_pool_backing[pi]);
        assert(call2(hle_nvnMemoryPoolInitialize, (uintptr_t)g_pools[pi].body, (uintptr_t)bld) == 1);
        s_pools[pi].live = true;
        s_pools[pi].finalized = false;
        break;
    }
    case 1: {  // Pool finalize
        int pi = a % N_POOLS;
        call1(hle_nvnMemoryPoolFinalize, (uintptr_t)g_pools[pi].body);
        s_pools[pi].live = false;
        s_pools[pi].finalized = true;
        break;
    }
    case 2:
    case 3: {  // Texture init (including reinit over live)
        int ti = a % N_TEXS;
        int pi = b % N_POOLS;
        if (!s_pools[pi].live) {
            uint64_t bld[8] = {0};
            call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)bld);
            call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)bld, (uintptr_t)g_pool_backing[pi], sizeof g_pool_backing[pi]);
            call2(hle_nvnMemoryPoolInitialize, (uintptr_t)g_pools[pi].body, (uintptr_t)bld);
            s_pools[pi].live = true; s_pools[pi].finalized = false;
        }
        if (s_texs[ti].kind == ST_LIVE_ORIG) mark_copies_stale(s_texs[ti].origin_gen_id);
        uint32_t lv = 1 + (c_arg % 5);
        uint32_t w = 16u << (b % 4);
        uint64_t tb[16] = {0};
        call1(hle_nvnTextureBuilderSetDefaults, (uintptr_t)tb);
        call2(hle_nvnTextureBuilderSetTarget, (uintptr_t)tb, 1);
        call3(hle_nvnTextureBuilderSetSize2D, (uintptr_t)tb, w, w);
        call2(hle_nvnTextureBuilderSetLevels, (uintptr_t)tb, lv);
        call2(hle_nvnTextureBuilderSetFormat, (uintptr_t)tb, 0x25);
        call3(hle_nvnTextureBuilderSetStorage, (uintptr_t)tb, (uintptr_t)g_pools[pi].body, 0);
        assert(call2(hle_nvnTextureInitialize, (uintptr_t)g_texs[ti].body, (uintptr_t)tb) == 1);
        s_texs[ti].kind = ST_LIVE_ORIG;
        s_texs[ti].levels = lv;
        s_texs[ti].width = w;
        s_texs[ti].debug_id = call1(hle_nvnTextureGetDebugID, (uintptr_t)g_texs[ti].body);
        s_texs[ti].origin_idx = ti;
        s_texs[ti].origin_gen_id = s_next_birth++;
        break;
    }
    case 4: {  // Texture finalize (including double finalize & copy finalize)
        int ti = a % N_TEXS;
        ShadowTexKind prev = s_texs[ti].kind;
        uint64_t bid = s_texs[ti].origin_gen_id;
        call1(hle_nvnTextureFinalize, (uintptr_t)g_texs[ti].body);
        if (prev == ST_LIVE_ORIG) {
            s_texs[ti].kind = ST_FINALIZED;
            mark_copies_stale(bid);
        } else if (prev == ST_LIVE_COPY) {
            s_texs[ti].kind = ST_FINALIZED;
            // Finalizing a live copy retires its shared slot too -> origin & other copies become stale
            for (int j = 0; j < N_TEXS; j++) {
                if (j != ti && s_texs[j].origin_gen_id == bid &&
                    (s_texs[j].kind == ST_LIVE_ORIG || s_texs[j].kind == ST_LIVE_COPY)) {
                    s_texs[j].kind = ST_STALE_COPY;
                }
            }
        } else if (prev == ST_STALE_COPY || prev == ST_FINALIZED || prev == ST_STALE_RESET) {
            // Double finalize writes NVN_MAGIC_TEX_FIN only on live/copy/finalized; for stale copy, keeps sentinel or finalized
            NvnTexLookup lk = nvn_texture_lookup((uintptr_t)g_texs[ti].body);
            if (lk.state == TEX_ST_FINALIZED_INST) s_texs[ti].kind = ST_FINALIZED;
        }
        break;
    }
    case 5: {  // Unity value copy (memcpy)
        int src = a % N_TEXS, dst = b % N_TEXS;
        if (src == dst) break;
        if (s_texs[dst].kind == ST_LIVE_ORIG) {
            // Don't orphan an active origin without finalizing first in shadow model
            break;
        }
        memcpy(g_texs[dst].body, g_texs[src].body, sizeof g_texs[dst].body);
        s_texs[dst] = s_texs[src];
        if (s_texs[src].kind == ST_LIVE_ORIG) s_texs[dst].kind = ST_LIVE_COPY;
        break;
    }
    case 6: {  // Texture compare & storage size queries
        int t0 = a % N_TEXS, t1 = b % N_TEXS;
        uint64_t eq = call2(hle_nvnTextureCompare, (uintptr_t)g_texs[t0].body, (uintptr_t)g_texs[t1].body);
        if (s_texs[t0].kind == ST_LIVE_ORIG && s_texs[t1].kind == ST_LIVE_COPY &&
            s_texs[t0].origin_gen_id == s_texs[t1].origin_gen_id) {
            assert(eq == 1);
        }
        if (s_texs[t0].kind == ST_LIVE_ORIG) {
            assert(call1(hle_nvnTextureGetStorageSize, (uintptr_t)g_texs[t0].body) > 0);
        }
        break;
    }
    case 7: {  // Alien corruption ("UnityFS\0") on a non-live slot
        int ti = a % N_TEXS;
        if (s_texs[ti].kind == ST_LIVE_ORIG) break;
        memcpy(g_texs[ti].body, "UnityFS\0\x00\x00\x00\x06\x32\x30\x32\x31", 16);
        s_texs[ti].kind = ST_CORRUPT;
        break;
    }
    case 8: {  // Zeroing a non-live slot
        int ti = a % N_TEXS;
        if (s_texs[ti].kind == ST_LIVE_ORIG) break;
        memset(g_texs[ti].body, 0, sizeof g_texs[ti].body);
        s_texs[ti].kind = ST_ZERO;
        break;
    }
    case 9: {  // Guarded 16-byte NVNtextureView operations
        GuardedView v1, v2;
        init_canaries(v1.pre, v1.post); init_canaries(v2.pre, v2.post);
        call1(hle_nvnTextureViewSetDefaults, (uintptr_t)v1.body);
        call3(hle_nvnTextureViewSetLevels, (uintptr_t)v1.body, a % 4, 1 + (b % 4));
        call3(hle_nvnTextureViewSetLayers, (uintptr_t)v1.body, 0, 1 + (c_arg % 3));
        call2(hle_nvnTextureViewSetFormat, (uintptr_t)v1.body, 0x25);
        memcpy(v2.body, v1.body, sizeof v2.body);
        assert(call2(hle_nvnTextureViewCompare, (uintptr_t)v1.body, (uintptr_t)v2.body) == 1);
        check_canaries(v1.pre, v1.post, "TextureView1");
        check_canaries(v2.pre, v2.post, "TextureView2");
        break;
    }
    case 10: {  // Sync init / finalize / deferred set_seq
        int si = a % N_SYNCS;
        if ((b & 3) == 0) {
            assert(call2(hle_nvnSyncInitialize, (uintptr_t)g_syncs[si].body, 0x1234) == 1);
            s_syncs[si].live = true; s_syncs[si].finalized = false;
        } else if ((b & 3) == 1) {
            call1(hle_nvnSyncFinalize, (uintptr_t)g_syncs[si].body);
            s_syncs[si].live = false; s_syncs[si].finalized = true;
        } else {
            uint64_t tok = 0, ep = 0;
            nvn_sync_pack_handle((uintptr_t)g_syncs[si].body, &tok, &ep);
            nvn_sync_set_seq((uintptr_t)g_syncs[si].body, 42 + c_arg);
            if (tok) nvn_sync_set_seq_token((uintptr_t)g_syncs[si].body, tok, ep, 99 + c_arg);
        }
        break;
    }
    case 11: {  // Sampler init / copy / finalize / compare
        int si = a % N_SAMPLERS, sj = b % N_SAMPLERS;
        if ((c_arg & 3) == 0) {
            uint64_t sb[12] = {0};
            call1(hle_nvnSamplerBuilderSetDefaults, (uintptr_t)sb);
            call3(hle_nvnSamplerBuilderSetMinMagFilter, (uintptr_t)sb, a % 4, b % 2);
            assert(call2(hle_nvnSamplerInitialize, (uintptr_t)g_samplers[si].body, (uintptr_t)sb) == 1);
            s_samplers[si].live = true;
        } else if ((c_arg & 3) == 1 && si != sj) {
            memcpy(g_samplers[sj].body, g_samplers[si].body, sizeof g_samplers[sj].body);
            s_samplers[sj] = s_samplers[si];
        } else if ((c_arg & 3) == 2) {
            call1(hle_nvnSamplerFinalize, (uintptr_t)g_samplers[si].body);
            s_samplers[si].live = false;
        } else {
            (void)call2(hle_nvnSamplerCompare, (uintptr_t)g_samplers[si].body, (uintptr_t)g_samplers[sj].body);
        }
        break;
    }
    case 12:
    case 13: {  // Descriptor pools register + seqlock read
        if (!s_desc_live) {
            if (!s_pools[0].live) {
                uint64_t bld[8] = {0};
                call1(hle_nvnMemoryPoolBuilderSetDefaults, (uintptr_t)bld);
                call3(hle_nvnMemoryPoolBuilderSetStorage, (uintptr_t)bld, (uintptr_t)g_pool_backing[0], sizeof g_pool_backing[0]);
                call2(hle_nvnMemoryPoolInitialize, (uintptr_t)g_pools[0].body, (uintptr_t)bld);
                s_pools[0].live = true; s_pools[0].finalized = false;
            }
            assert(call4(hle_nvnTexturePoolInitialize, (uintptr_t)g_tpool.body, (uintptr_t)g_pools[0].body, 0, DESC_CAP) == 1);
            assert(call4(hle_nvnSamplerPoolInitialize, (uintptr_t)g_spool.body, (uintptr_t)g_pools[0].body, 0x1000, DESC_CAP) == 1);
            s_desc_live = true;
        }
        int id = a % DESC_CAP;
        int ti = b % N_TEXS;
        call4(hle_nvnTexturePoolRegisterTexture, (uintptr_t)g_tpool.body, id, (uintptr_t)g_texs[ti].body, 0);
        NvnDescPool *tp = HOST(NvnDescPool, (uintptr_t)g_tpool.body);
        NvnTexEntry ent;
        bool ok = nvn_tex_pool_read(tp, (uint32_t)id, &ent);
        if (s_texs[ti].kind == ST_LIVE_ORIG || s_texs[ti].kind == ST_LIVE_COPY) {
            assert(ok && ent.tex != NULL && ent.tex->levels == s_texs[ti].levels);
        }
        int si = c_arg % N_SAMPLERS;
        call3(hle_nvnSamplerPoolRegisterSampler, (uintptr_t)g_spool.body, id, (uintptr_t)g_samplers[si].body);
        NvnDescPool *sp = HOST(NvnDescPool, (uintptr_t)g_spool.body);
        NvnSamplerDesc sd;
        assert(nvn_sampler_pool_read(sp, (uint32_t)id, &sd));
        break;
    }
    case 14: {  // Rare epoch reset (1 in 16 chance when op==14)
        if ((c_arg & 15) == 0) {
            texture_identities_reset();
            pool_identities_reset();
            sync_identities_reset();
            desc_pools_reset();
            for (int i = 0; i < N_TEXS; i++) {
                if (s_texs[i].kind == ST_LIVE_ORIG || s_texs[i].kind == ST_LIVE_COPY ||
                    s_texs[i].kind == ST_STALE_COPY || s_texs[i].kind == ST_FINALIZED)
                    s_texs[i].kind = ST_STALE_RESET;
            }
            for (int i = 0; i < N_POOLS; i++) { s_pools[i].live = false; s_pools[i].finalized = false; }
            for (int i = 0; i < N_SYNCS; i++) { s_syncs[i].live = false; }
            s_desc_live = false;
        }
        break;
    }
    }
}

#ifdef FUZZ_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    setenv("ENGAGE_NVN_DIAG", "off", 1);
    unsetenv("ENGAGE_NVN_STRICT");
    fuzz_reset_all();
    for (size_t i = 0; i + 4 <= size; i += 4) {
        exec_op(data[i], data[i + 1], data[i + 2], data[i + 3]);
    }
    verify_invariants();
    return 0;
}
#else
static uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (*s = x);
}

int main(void) {
    setenv("ENGAGE_NVN_DIAG", "off", 1);
    unsetenv("ENGAGE_NVN_STRICT");
    uint64_t iters = 200000;
    const char *e_it = getenv("NVN_FUZZ_ITERS");
    if (e_it && *e_it) iters = strtoull(e_it, NULL, 10);
    uint64_t seed = 0x5EED20261122CAFEull;
    const char *e_sd = getenv("NVN_FUZZ_SEED");
    if (e_sd && *e_sd) seed = strtoull(e_sd, NULL, 16);

    fuzz_reset_all();
    for (uint64_t i = 0; i < iters; i++) {
        uint64_t r = xorshift64(&seed);
        exec_op((uint8_t)r, (uint8_t)(r >> 8), (uint8_t)(r >> 16), (uint8_t)(r >> 24));
        if ((i & 255) == 255) verify_invariants();
    }
    verify_invariants();
    fuzz_reset_all();
    printf("PASS: fuzz_nvn_lifecycle (%llu randomized ops, invariants I1-I5 verified)\n",
           (unsigned long long)iters);
    return 0;
}
#endif
