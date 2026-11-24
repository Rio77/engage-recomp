// NVN objects, builders, and state objects (everything except command recording).
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../guest_mem.h"
#include "../kernel.h"
#include "diag.h"
#include "nvn.h"

void renderer_sync(void);

static _Atomic uint64_t next_debug_id = 1;
static uint64_t alloc_debug_id(void) {
    return atomic_fetch_add_explicit(&next_debug_id, 1, memory_order_relaxed);
}

// Lock-free paged generational texture slot table (Phase 3).
// Guest NVNtexture [0..16) holds an NvnGuestHandle (no raw host pointers):
//   - Survives Unity value copies (memcpy)
//   - O(1) lock-free lookup on hot path via acquire loads
//   - Bounded host memory via FIFO quarantine recycling of finalized slots
//   - Instant O(1) invalidation on RT_ON_RESET via g_tex_epoch
#define NVN_TEX_PAGE_SHIFT 10u
#define NVN_TEX_PAGE_SIZE  (1u << NVN_TEX_PAGE_SHIFT)
#define NVN_TEX_PAGE_MASK  (NVN_TEX_PAGE_SIZE - 1u)
#define NVN_TEX_MAX_PAGES  64u
#define NVN_TEX_MAX_SLOTS  (NVN_TEX_PAGE_SIZE * NVN_TEX_MAX_PAGES)
#define NVN_TEX_QUARANTINE 1024u
#define NVN_TEX_QRING_CAP  2048u  // power of 2 > NVN_TEX_QUARANTINE

enum {
    TEX_SLOT_FREE = 0,
    TEX_SLOT_ALIVE = 1,
    TEX_SLOT_RETIRED = 2,
};

typedef struct NvnTexSlot {
    _Atomic uint32_t generation;
    _Atomic uint32_t state;
    uint64_t origin_addr;
    NvnTexture tex;
} NvnTexSlot;

static _Atomic(NvnTexSlot *) g_tex_pages[NVN_TEX_MAX_PAGES];
static _Atomic uint16_t g_tex_epoch = 1;
static pthread_mutex_t g_tex_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_tex_next_fresh = 0;
static uint32_t g_tex_qring[NVN_TEX_QRING_CAP];
static uint32_t g_tex_qhead = 0, g_tex_qtail = 0, g_tex_qcount = 0;

static uint16_t nvn_handle_compute_tag(const NvnGuestHandle *h, uint8_t obj_kind) {
    uint64_t x = ((uint64_t)h->magic << 32) ^ (uint64_t)h->slot_index;
    x ^= ((uint64_t)h->generation << 16) ^ ((uint64_t)h->rt_epoch << 8) ^ obj_kind;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x ^= x >> 31;
    uint16_t t = (uint16_t)(x ^ (x >> 16));
    return t ? t : 1u;
}

static NvnTexSlot *tex_slot_at(uint32_t idx) {
    if (idx >= NVN_TEX_MAX_SLOTS) return NULL;
    uint32_t p = idx >> NVN_TEX_PAGE_SHIFT;
    NvnTexSlot *page = atomic_load_explicit(&g_tex_pages[p], memory_order_acquire);
    return page ? &page[idx & NVN_TEX_PAGE_MASK] : NULL;
}

static NvnTexSlot *tex_slot_alloc(uint64_t guest_addr, NvnGuestHandle *out_hdr) {
    pthread_mutex_lock(&g_tex_alloc_lock);
    uint32_t idx = UINT32_MAX;
    if (g_tex_qcount >= NVN_TEX_QUARANTINE) {
        idx = g_tex_qring[g_tex_qhead];
        g_tex_qhead = (g_tex_qhead + 1u) & (NVN_TEX_QRING_CAP - 1u);
        g_tex_qcount--;
    } else if (g_tex_next_fresh < NVN_TEX_MAX_SLOTS) {
        idx = g_tex_next_fresh++;
    } else if (g_tex_qcount > 0) {
        idx = g_tex_qring[g_tex_qhead];
        g_tex_qhead = (g_tex_qhead + 1u) & (NVN_TEX_QRING_CAP - 1u);
        g_tex_qcount--;
    } else {
        pthread_mutex_unlock(&g_tex_alloc_lock);
        FATAL("NVN texture slot table exhausted (%u slots)\n", NVN_TEX_MAX_SLOTS);
    }

    uint32_t p = idx >> NVN_TEX_PAGE_SHIFT;
    NvnTexSlot *page = atomic_load_explicit(&g_tex_pages[p], memory_order_relaxed);
    if (!page) {
        page = calloc(NVN_TEX_PAGE_SIZE, sizeof(NvnTexSlot));
        if (!page) {
            pthread_mutex_unlock(&g_tex_alloc_lock);
            FATAL("NVN texture page allocation failed\n");
        }
        atomic_store_explicit(&g_tex_pages[p], page, memory_order_release);
    }
    NvnTexSlot *s = &page[idx & NVN_TEX_PAGE_MASK];
    uint32_t gen = atomic_load_explicit(&s->generation, memory_order_relaxed) + 1u;
    if (!gen) gen = 1u;
    // Publish FREE + bumped generation before wiping slot payload so stale readers bail immediately.
    atomic_store_explicit(&s->state, TEX_SLOT_FREE, memory_order_release);
    atomic_store_explicit(&s->generation, gen, memory_order_release);
    s->origin_addr = guest_addr;
    memset(&s->tex, 0, sizeof s->tex);

    uint16_t ep = atomic_load_explicit(&g_tex_epoch, memory_order_relaxed);
    pthread_mutex_unlock(&g_tex_alloc_lock);

    NvnGuestHandle h = {
        .magic = NVN_MAGIC_TEX_LIVE,
        .slot_index = idx,
        .generation = gen,
        .rt_epoch = ep,
        .tag16 = 0,
    };
    h.tag16 = nvn_handle_compute_tag(&h, NVN_OBJ_TEXTURE);
    *out_hdr = h;
    return s;
}

static void tex_slot_publish_live(NvnTexSlot *s) {
    atomic_store_explicit(&s->state, TEX_SLOT_ALIVE, memory_order_release);
}

static bool tex_slot_retire(uint32_t idx, uint32_t gen) {
    pthread_mutex_lock(&g_tex_alloc_lock);
    NvnTexSlot *s = tex_slot_at(idx);
    bool retired_now = false;
    if (s && atomic_load_explicit(&s->generation, memory_order_relaxed) == gen &&
        atomic_load_explicit(&s->state, memory_order_relaxed) == TEX_SLOT_ALIVE) {
        atomic_store_explicit(&s->state, TEX_SLOT_RETIRED, memory_order_release);
        if (g_tex_qcount < NVN_TEX_QRING_CAP) {
            g_tex_qring[g_tex_qtail] = idx;
            g_tex_qtail = (g_tex_qtail + 1u) & (NVN_TEX_QRING_CAP - 1u);
            g_tex_qcount++;
        }
        retired_now = true;
    }
    pthread_mutex_unlock(&g_tex_alloc_lock);
    return retired_now;
}

static unsigned nvn_texture_allocated_slots(void) {
    pthread_mutex_lock(&g_tex_alloc_lock);
    unsigned n = g_tex_next_fresh;
    pthread_mutex_unlock(&g_tex_alloc_lock);
    return n;
}

NvnTexLookup nvn_texture_lookup(uint64_t guest) {
    NvnTexLookup out = {0};
    NvnGuestHandle h;
    GuestMemStatus ms = guest_mem_read_checked(guest, &h, sizeof h, 8);
    out.mem_status = ms;
    if (ms != GMEM_OK) {
        out.state = TEX_ST_BAD_ADDR;
        return out;
    }
    out.hdr = h;
    out.raw_valid = true;

    if ((h.magic | h.slot_index | h.generation | h.rt_epoch | h.tag16) == 0) {
        out.state = TEX_ST_ZEROED;
        return out;
    }
    if ((h.magic != NVN_MAGIC_TEX_LIVE && h.magic != NVN_MAGIC_TEX_FIN) ||
        h.tag16 != nvn_handle_compute_tag(&h, NVN_OBJ_TEXTURE)) {
        out.state = TEX_ST_CORRUPT;
        return out;
    }
    if (h.rt_epoch != atomic_load_explicit(&g_tex_epoch, memory_order_acquire)) {
        out.state = TEX_ST_STALE_RESET;
        return out;
    }
    NvnTexSlot *s = tex_slot_at(h.slot_index);
    if (!s) {
        out.state = TEX_ST_CORRUPT;
        return out;
    }
    uint32_t gen = atomic_load_explicit(&s->generation, memory_order_acquire);
    uint32_t st = atomic_load_explicit(&s->state, memory_order_acquire);
    if (gen != h.generation || st == TEX_SLOT_FREE) {
        out.state = TEX_ST_STALE_RECYCLED;
        return out;
    }
    if (h.magic == NVN_MAGIC_TEX_FIN) {
        out.state = TEX_ST_FINALIZED_INST;
        out.tex = &s->tex;
        return out;
    }
    out.tex = &s->tex;
    if (st == TEX_SLOT_RETIRED) {
        out.state = TEX_ST_STALE_COPY;
        return out;
    }
    out.state = (guest == s->origin_addr) ? TEX_ST_LIVE_ORIG : TEX_ST_LIVE_COPY;
    return out;
}

NvnTexture *nvn_texture_host(uint64_t guest) {
    NvnTexLookup lk = nvn_texture_lookup(guest);
    if (lk.state == TEX_ST_LIVE_ORIG || lk.state == TEX_ST_LIVE_COPY || lk.state == TEX_ST_STALE_COPY)
        return lk.tex;
    return NULL;
}

static NvnTexture *texture_require(uint64_t guest) {
    NvnTexLookup lk = nvn_texture_lookup(guest);
    if (lk.state == TEX_ST_LIVE_ORIG || lk.state == TEX_ST_LIVE_COPY || lk.state == TEX_ST_STALE_COPY)
        return lk.tex;
    uint64_t q0 = 0, q1 = 0;
    if (lk.raw_valid) {
        memcpy(&q0, &lk.hdr, 8);
        memcpy(&q1, (const uint8_t *)&lk.hdr + 8, 8);
    }
    nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_TEXTURE, (uint8_t)lk.state, guest,
                    lk.hdr.slot_index, lk.hdr.generation, lk.hdr.rt_epoch, 0, 0,
                    lk.raw_valid, q0, q1);
    nvn_diag_dump_for_addr("texture_require", guest, lk.hdr.slot_index, lk.hdr.generation, 0);
    FATAL("[texture-life] invalid NVN texture used: guest=%llx state=%u mem=%s\n",
          (unsigned long long)guest, (unsigned)lk.state, guest_mem_status_name(lk.mem_status));
}

// Helper for unit tests (e.g. test_texture_identity.c) to bind a synthetic texture into guest memory.
static NvnTexture *texture_identity_bind_test(uint64_t guest, uint32_t levels) {
    NvnGuestHandle hdr;
    NvnTexSlot *s = tex_slot_alloc(guest, &hdr);
    s->tex.debug_id = alloc_debug_id();
    s->tex.levels = levels;
    tex_slot_publish_live(s);
    uint8_t zeros[112] = {0};
    guest_mem_write_checked(guest, &hdr, sizeof hdr, 8);
    guest_mem_write_checked(guest + 16, zeros, sizeof zeros, 8);
    return &s->tex;
}

// main.c calls reset hooks only after stopping all guest threads and releasing
// renderer resources, so no queued command or copied object can still use these.
RT_ON_RESET(texture_identities_reset) {
    pthread_mutex_lock(&g_tex_alloc_lock);
    uint16_t next_ep = (uint16_t)(atomic_load_explicit(&g_tex_epoch, memory_order_relaxed) + 1u);
    if (!next_ep) next_ep = 1u;
    atomic_store_explicit(&g_tex_epoch, next_ep, memory_order_release);
    for (uint32_t p = 0; p < NVN_TEX_MAX_PAGES; p++) {
        NvnTexSlot *page = atomic_exchange_explicit(&g_tex_pages[p], NULL, memory_order_acq_rel);
        free(page);
    }
    g_tex_next_fresh = 0;
    g_tex_qhead = g_tex_qtail = g_tex_qcount = 0;
    pthread_mutex_unlock(&g_tex_alloc_lock);
    nvn_diag_record(NVN_EV_RESET, NVN_OBJ_TEXTURE, 0, 0, 0, 0, next_ep, 0, 0, false, 0, 0);
}


// ================================================================== device
// NVNdeviceInfo -> value
static uint32_t device_integer(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 55, [1] = 15,            // SDK 12 NVN API
        [2] = 14, [3] = 65536, [4] = 256, [5] = 8, [6] = 16, [7] = 4, [8] = 16, [9] = 32,
        [10] = 16, [11] = 4, [12] = 4, [13] = 4, [14] = 16, [15] = 32, [16] = 32,
        [17] = 256, [18] = 256,        // reserved texture / sampler descriptors
        [19] = 4, [20] = 8, [21] = 0x10000, [22] = 0x1000,
        [23] = 256, [24] = 256, [25] = 4096, [26] = 0x20000, [27] = 16,
        [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
        [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
        [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
        [40] = 1, [41] = 1, [42] = 1,  // min/max filtering, stencil8, ASTC
        [43] = 0x40000, [44] = 15, [45] = 2048,
        [46] = 1, [47] = 1, [48] = 16, [49] = 5,  // GPU code major/minor range
        [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 1, [57] = 0x1000,
        [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
        [0x5E] = 128, [0x5F] = 32,     // implemented separate binding slots
    };
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) { once = true; backend_init(); }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
NVN(nvnDeviceSetDebugLabel) { (void)c; }
NVN(nvnDeviceGetInteger) {
    uint32_t item = (uint32_t)A(1);
    uint32_t value = device_integer(item);
    *(int32_t *)AP(2) = (int32_t)value;
    if (ENV("ENGAGE_TRACE_NVN_PROPERTIES")) LOG("[engage-nvn] property %#x = %u\n", item, value);
}
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    RET(plat_now_ns());
}
NVN(nvnDeviceGetTimestampInNanoseconds) {  // counter.timestamp: Switch GPU ticks (614.4 MHz, as the counters are written)
    RET((uint64_t)((unsigned __int128)G(uint64_t, A(1), 8) * 625 / 384));
}
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
NVN(nvnDeviceGetTextureHandle) { RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20)); }
NVN(nvnDeviceGetTexelFetchHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
RT_ON_RESET(nvn_device_reset) { window_origin = depth_mode = 0; }  // (the next boot sets its own)
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { nvn_command_finalize(A(1)); }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
NVN(nvnQueueSubmitCommands) {
    int n = (int)A(1);
    if (n <= 0 || n > 65536 || guest_mem_validate_range(A(2), (uint64_t)n * sizeof(uint64_t), 8, GMEM_PERM_R) != GMEM_OK)
        return;
    const uint64_t *handles = AP(2);
    for (int i = 0; i < n; i++) backend_execute(handles[i]);
}
NVN(nvnQueueFlush) {
    (void)c;
#ifndef __APPLE__
    extern bool vk_batch_submits_on(void) __attribute__((weak));
    extern void renderer_submit(void) __attribute__((weak));
    if (vk_batch_submits_on && renderer_submit && vk_batch_submits_on()) renderer_submit();
#endif
}
NVN(nvnQueueFinish) { (void)c; renderer_sync(); }

// ================================================================== generational sync table (Phase 5A)
#define NVN_SYNC_PAGE_SHIFT 8u
#define NVN_SYNC_PAGE_SIZE  (1u << NVN_SYNC_PAGE_SHIFT)
#define NVN_SYNC_PAGE_MASK  (NVN_SYNC_PAGE_SIZE - 1u)
#define NVN_SYNC_MAX_PAGES  256u
#define NVN_SYNC_MAX_SLOTS  (NVN_SYNC_PAGE_SIZE * NVN_SYNC_MAX_PAGES)
#define NVN_SYNC_QUARANTINE 256u
#define NVN_SYNC_QRING_CAP  512u

typedef struct NvnSyncSlot {
    _Atomic uint32_t generation;
    _Atomic uint32_t state;
    NvnSync sync;
} NvnSyncSlot;

static _Atomic(NvnSyncSlot *) g_sync_pages[NVN_SYNC_MAX_PAGES];
static _Atomic uint16_t g_sync_epoch = 1;
static pthread_mutex_t g_sync_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_sync_next_fresh = 0;
static uint32_t g_sync_qring[NVN_SYNC_QRING_CAP];
static uint32_t g_sync_qhead = 0, g_sync_qtail = 0, g_sync_qcount = 0;

static NvnSyncSlot *sync_slot_at(uint32_t idx) {
    if (idx >= NVN_SYNC_MAX_SLOTS) return NULL;
    NvnSyncSlot *page = atomic_load_explicit(&g_sync_pages[idx >> NVN_SYNC_PAGE_SHIFT], memory_order_acquire);
    return page ? &page[idx & NVN_SYNC_PAGE_MASK] : NULL;
}

static NvnSyncSlot *sync_slot_alloc(NvnGuestHandle *out_hdr) {
    pthread_mutex_lock(&g_sync_alloc_lock);
    uint32_t idx = UINT32_MAX;
    if (g_sync_qcount >= NVN_SYNC_QUARANTINE) {
        idx = g_sync_qring[g_sync_qhead];
        g_sync_qhead = (g_sync_qhead + 1u) & (NVN_SYNC_QRING_CAP - 1u);
        g_sync_qcount--;
    } else if (g_sync_next_fresh < NVN_SYNC_MAX_SLOTS) {
        idx = g_sync_next_fresh++;
    } else if (g_sync_qcount > 0) {
        idx = g_sync_qring[g_sync_qhead];
        g_sync_qhead = (g_sync_qhead + 1u) & (NVN_SYNC_QRING_CAP - 1u);
        g_sync_qcount--;
    } else {
        pthread_mutex_unlock(&g_sync_alloc_lock);
        FATAL("NVN sync slot table exhausted (%u slots)\n", NVN_SYNC_MAX_SLOTS);
    }
    uint32_t p = idx >> NVN_SYNC_PAGE_SHIFT;
    NvnSyncSlot *page = atomic_load_explicit(&g_sync_pages[p], memory_order_relaxed);
    if (!page) {
        page = calloc(NVN_SYNC_PAGE_SIZE, sizeof(NvnSyncSlot));
        if (!page) {
            pthread_mutex_unlock(&g_sync_alloc_lock);
            FATAL("NVN sync page allocation failed\n");
        }
        atomic_store_explicit(&g_sync_pages[p], page, memory_order_release);
    }
    NvnSyncSlot *s = &page[idx & NVN_SYNC_PAGE_MASK];
    uint32_t gen = atomic_load_explicit(&s->generation, memory_order_relaxed) + 1u;
    if (!gen) gen = 1u;
    atomic_store_explicit(&s->state, TEX_SLOT_FREE, memory_order_release);
    atomic_store_explicit(&s->generation, gen, memory_order_release);
    atomic_store_explicit(&s->sync.signaled, 0, memory_order_relaxed);
    atomic_store_explicit(&s->sync.seq, 0, memory_order_relaxed);
    atomic_store_explicit(&s->state, TEX_SLOT_ALIVE, memory_order_release);
    uint16_t ep = atomic_load_explicit(&g_sync_epoch, memory_order_relaxed);
    pthread_mutex_unlock(&g_sync_alloc_lock);

    NvnGuestHandle h = {
        .magic = NVN_MAGIC_SYNC_LIVE,
        .slot_index = idx,
        .generation = gen,
        .rt_epoch = ep,
        .tag16 = 0,
    };
    h.tag16 = nvn_handle_compute_tag(&h, NVN_OBJ_SYNC);
    *out_hdr = h;
    return s;
}

static bool sync_slot_retire(uint32_t idx, uint32_t gen) {
    pthread_mutex_lock(&g_sync_alloc_lock);
    NvnSyncSlot *s = sync_slot_at(idx);
    bool ok = false;
    if (s && atomic_load_explicit(&s->generation, memory_order_relaxed) == gen &&
        atomic_load_explicit(&s->state, memory_order_relaxed) == TEX_SLOT_ALIVE) {
        atomic_store_explicit(&s->state, TEX_SLOT_RETIRED, memory_order_release);
        if (g_sync_qcount < NVN_SYNC_QRING_CAP) {
            g_sync_qring[g_sync_qtail] = idx;
            g_sync_qtail = (g_sync_qtail + 1u) & (NVN_SYNC_QRING_CAP - 1u);
            g_sync_qcount++;
        }
        ok = true;
    }
    pthread_mutex_unlock(&g_sync_alloc_lock);
    return ok;
}

RT_ON_RESET(sync_identities_reset) {
    pthread_mutex_lock(&g_sync_alloc_lock);
    uint16_t next_ep = (uint16_t)(atomic_load_explicit(&g_sync_epoch, memory_order_relaxed) + 1u);
    if (!next_ep) next_ep = 1u;
    atomic_store_explicit(&g_sync_epoch, next_ep, memory_order_release);
    for (uint32_t p = 0; p < NVN_SYNC_MAX_PAGES; p++) {
        NvnSyncSlot *page = atomic_exchange_explicit(&g_sync_pages[p], NULL, memory_order_acq_rel);
        free(page);
    }
    g_sync_next_fresh = 0;
    g_sync_qhead = g_sync_qtail = g_sync_qcount = 0;
    pthread_mutex_unlock(&g_sync_alloc_lock);
}

static bool sync_read_handle(uint64_t guest, NvnGuestHandle *out) {
    if (guest_mem_read_checked(guest, out, sizeof *out, 8) != GMEM_OK) return false;
    if (out->magic != NVN_MAGIC_SYNC_LIVE) return false;
    if (out->tag16 != nvn_handle_compute_tag(out, NVN_OBJ_SYNC)) return false;
    if (out->rt_epoch != atomic_load_explicit(&g_sync_epoch, memory_order_acquire)) return false;
    return true;
}

NvnSync *nvn_sync_host(uint64_t guest) {
    NvnGuestHandle h;
    if (!sync_read_handle(guest, &h)) return NULL;
    NvnSyncSlot *s = sync_slot_at(h.slot_index);
    if (!s) return NULL;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != TEX_SLOT_ALIVE) return NULL;
    if (atomic_load_explicit(&s->generation, memory_order_acquire) != h.generation) return NULL;
    return &s->sync;
}

void nvn_sync_pack_handle(uint64_t guest, uint64_t *out_token, uint64_t *out_epoch) {
    *out_token = 0;
    *out_epoch = 0;
    NvnGuestHandle h;
    if (!sync_read_handle(guest, &h)) return;
    NvnSyncSlot *s = sync_slot_at(h.slot_index);
    if (!s || atomic_load_explicit(&s->state, memory_order_acquire) != TEX_SLOT_ALIVE ||
        atomic_load_explicit(&s->generation, memory_order_acquire) != h.generation)
        return;
    *out_token = ((uint64_t)h.slot_index << 32) | (uint64_t)h.generation;
    *out_epoch = (uint64_t)h.rt_epoch;
}

uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);

NVN(nvnQueueFenceSync) {
    NvnSync *s = nvn_sync_host(A(1));
    if (!s) {
        nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_SYNC, 0, A(1), 0, 0, 0, 0, c->x[30], false, 0, 0);
        return;
    }
#ifndef __APPLE__
    extern bool vk_async_submit_on(void) __attribute__((weak));
    extern bool vk_async_enqueue_fence(uint64_t guest, uint64_t token, uint64_t epoch) __attribute__((weak));
    if (vk_async_submit_on && vk_async_enqueue_fence && vk_async_submit_on()) {
        uint64_t token = 0, epoch = 0;
        nvn_sync_pack_handle(A(1), &token, &epoch);
        atomic_store_explicit(&s->seq, 0, memory_order_release);
        atomic_store_explicit(&s->signaled, 0, memory_order_release);
        if (vk_async_enqueue_fence(token ? 0 : A(1), token, epoch)) return;
    }
#endif
    atomic_store_explicit(&s->seq, renderer_fence_seq(), memory_order_release);
    atomic_store_explicit(&s->signaled, 1, memory_order_release);
}

void nvn_sync_set_seq(uint64_t guest, uint64_t seq) {
    // Read-only lookup: never allocates or writes to finalized/reused guest memory.
    NvnSync *s = nvn_sync_host(guest);
    if (!s) return;
    atomic_store_explicit(&s->seq, seq, memory_order_release);
    atomic_store_explicit(&s->signaled, 1, memory_order_release);
}

void nvn_sync_set_seq_token(uint64_t guest, uint64_t token, uint64_t epoch, uint64_t seq) {
    if (token) {
        if ((uint16_t)epoch != atomic_load_explicit(&g_sync_epoch, memory_order_acquire)) return;
        uint32_t idx = (uint32_t)(token >> 32);
        uint32_t gen = (uint32_t)(token & 0xFFFFFFFFu);
        NvnSyncSlot *s = sync_slot_at(idx);
        if (!s) return;
        if (atomic_load_explicit(&s->state, memory_order_acquire) != TEX_SLOT_ALIVE) return;
        if (atomic_load_explicit(&s->generation, memory_order_acquire) != gen) return;
        atomic_store_explicit(&s->sync.seq, seq, memory_order_release);
        atomic_store_explicit(&s->sync.signaled, 1, memory_order_release);
        return;
    }
    if (guest) nvn_sync_set_seq(guest, seq);
}

NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) {
    uint64_t guest = A(0);
    if (guest_mem_validate_range(guest, sizeof(NvnGuestHandle), 8, GMEM_PERM_RW) != GMEM_OK) {
        RET(0);
        return;
    }
    NvnGuestHandle prev;
    if (sync_read_handle(guest, &prev)) sync_slot_retire(prev.slot_index, prev.generation);
    NvnGuestHandle h;
    sync_slot_alloc(&h);
    guest_mem_write_checked(guest, &h, sizeof h, 8);
    nvn_diag_record(NVN_EV_INIT, NVN_OBJ_SYNC, 1, guest, h.slot_index, h.generation, h.rt_epoch, 0, c->x[30], false, 0, 0);
    RET(1);
}
NVN(nvnSyncFinalize) {
    uint64_t guest = A(0);
    NvnGuestHandle h;
    if (sync_read_handle(guest, &h)) {
        sync_slot_retire(h.slot_index, h.generation);
        nvn_diag_record(NVN_EV_FINALIZE, NVN_OBJ_SYNC, 0, guest, h.slot_index, h.generation, h.rt_epoch, 0, c->x[30], false, 0, 0);
    }
    uint8_t zeros[sizeof(NvnGuestHandle)] = {0};
    guest_mem_write_checked(guest, zeros, sizeof zeros, 8);
}
NVN(nvnSyncSetDebugLabel) { (void)c; }
void renderer_flush(void);
NVN(nvnSyncWait) {  // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    NvnSync *sync = nvn_sync_host(A(0));
#ifndef __APPLE__
    static int timed = -1;
    if (timed < 0) {
        const char *e = getenv("FE3H_TIMED_SYNC");
        timed = (!e || atoi(e) != 0) ? 1 : 0;
    }
    if (timed) {
        extern int renderer_wait_seq_timeout(uint64_t seq, uint64_t timeout_ns);
        if (!sync) { RET(0); return; }
        uint64_t timeout_ns = A(1);
        int sig = atomic_load_explicit(&sync->signaled, memory_order_acquire);
        if (!sig && timeout_ns > 0) {
            extern int vk_async_wait_signaled(_Atomic int *flag, uint64_t timeout_ns, uint64_t *rem_ns) __attribute__((weak));
            if (vk_async_wait_signaled) {
                uint64_t rem = timeout_ns;
                sig = vk_async_wait_signaled(&sync->signaled, timeout_ns, &rem);
                timeout_ns = rem;
            } else {
                uint64_t cap = timeout_ns > 2000000000ull ? 2000000000ull : timeout_ns;
                struct timespec ts0, now;
                clock_gettime(CLOCK_MONOTONIC, &ts0);
                uint64_t elapsed = 0;
                while (!sig && elapsed < cap) {
                    struct timespec slp = {0, 100000};
                    nanosleep(&slp, NULL);
                    sig = atomic_load_explicit(&sync->signaled, memory_order_acquire);
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    elapsed = (uint64_t)(now.tv_sec - ts0.tv_sec) * 1000000000ull +
                              (uint64_t)(now.tv_nsec - ts0.tv_nsec);
                }
                if (sig && timeout_ns != UINT64_MAX) {
                    timeout_ns = timeout_ns > elapsed ? timeout_ns - elapsed : 0;
                }
            }
        }
        if (!sig) { RET(2); return; }
        uint64_t seq = atomic_load_explicit(&sync->seq, memory_order_acquire);
        RET(renderer_wait_seq_timeout(seq, timeout_ns));
        return;
    }
#endif
    static int flush = -1;
    static uint64_t n;
    if (flush < 0) flush = getenv("LARECOMP_SYNC_FLUSH") ? 1 : 0;
    if (getenv("LARECOMP_LOG_SYNC") && n++ % 60 == 0) LOG("[nvn] SyncWait #%llu (lr %llx)\n", (unsigned long long)n, c->x[30]);
    static int whole = -1;  // LARECOMP_SYNC_ALL=1: old behaviour, wait for all submitted work
    if (whole < 0) whole = getenv("LARECOMP_SYNC_ALL") ? 1 : 0;
    if (flush) renderer_flush();
    else if (whole) renderer_sync();
    else if (sync) {
        extern void vk_async_drain(void) __attribute__((weak));
        if (!atomic_load_explicit(&sync->signaled, memory_order_acquire) && vk_async_drain) vk_async_drain();
        renderer_wait_seq(atomic_load_explicit(&sync->seq, memory_order_acquire));
    }
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) {
    if (guest_mem_validate_range(A(0), 16, 8, GMEM_PERM_RW) != GMEM_OK) return;
    G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0;
}
NVN(nvnEventBuilderSetStorage) {
    if (guest_mem_validate_range(A(0), 16, 8, GMEM_PERM_RW) != GMEM_OK) return;
    G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2);
}
NVN(nvnEventBuilderGetStorage) {
    uint64_t v = 0; guest_mem_read_checked(A(0), &v, 8, 8); RET(v);
}
NVN(nvnEventBuilderGetMemoryPool) {
    uint64_t v = 0; guest_mem_read_checked(A(0), &v, 8, 8); RET(v);
}
NVN(nvnEventBuilderGetMemoryOffset) {
    uint64_t v = 0; guest_mem_read_checked(A(0) + 8, &v, 8, 8); RET(v);
}
static uint64_t event_addr(uint64_t ev) {
    uint64_t addr = 0;
    guest_mem_read_checked(ev, &addr, sizeof addr, 8);
    return addr;
}
NVN(nvnEventInitialize) {
    if (guest_mem_validate_range(A(0), 24, 8, GMEM_PERM_RW) != GMEM_OK ||
        guest_mem_validate_range(A(1), 16, 8, GMEM_PERM_R) != GMEM_OK) {
        RET(0);
        return;
    }
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    NvnPool *p = HOST(NvnPool, pool);
    if (!p) { RET(0); return; }
    G(uint64_t, A(0), 0) = p->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) {
    uint64_t addr = event_addr(A(0));
    uint32_t v = 0;
    guest_mem_read_checked(addr, &v, sizeof v, 4);
    RET(v);
}
NVN(nvnEventSignal) {
    uint64_t addr = event_addr(A(0));
    uint32_t v = (uint32_t)A(2);
    guest_mem_write_checked(addr, &v, sizeof v, 4);
}
NVN(nvnEventGetMemoryPool) {
    uint64_t v = 0; guest_mem_read_checked(A(0) + 8, &v, 8, 8); RET(v);
}
NVN(nvnEventGetMemoryOffset) {
    uint64_t v = 0; guest_mem_read_checked(A(0) + 16, &v, 8, 8); RET(v);
}

// ================================================================== memory pools (Phase 5A generational)
#define NVN_POOL_PAGE_SHIFT 8u
#define NVN_POOL_PAGE_SIZE  (1u << NVN_POOL_PAGE_SHIFT)
#define NVN_POOL_PAGE_MASK  (NVN_POOL_PAGE_SIZE - 1u)
#define NVN_POOL_MAX_PAGES  256u
#define NVN_POOL_MAX_SLOTS  (NVN_POOL_PAGE_SIZE * NVN_POOL_MAX_PAGES)
#define NVN_POOL_QUARANTINE 256u
#define NVN_POOL_QRING_CAP  512u

typedef struct NvnPoolSlot {
    _Atomic uint32_t generation;
    _Atomic uint32_t state;
    NvnPool pool;
} NvnPoolSlot;

static _Atomic(NvnPoolSlot *) g_pool_pages[NVN_POOL_MAX_PAGES];
static _Atomic uint16_t g_pool_epoch = 1;
static pthread_mutex_t g_pool_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pool_next_fresh = 0;
static uint32_t g_pool_qring[NVN_POOL_QRING_CAP];
static uint32_t g_pool_qhead = 0, g_pool_qtail = 0, g_pool_qcount = 0;

static NvnPoolSlot *pool_slot_at(uint32_t idx) {
    if (idx >= NVN_POOL_MAX_SLOTS) return NULL;
    NvnPoolSlot *page = atomic_load_explicit(&g_pool_pages[idx >> NVN_POOL_PAGE_SHIFT], memory_order_acquire);
    return page ? &page[idx & NVN_POOL_PAGE_MASK] : NULL;
}

static NvnPoolSlot *pool_slot_alloc(NvnGuestHandle *out_hdr) {
    pthread_mutex_lock(&g_pool_alloc_lock);
    uint32_t idx = UINT32_MAX;
    if (g_pool_qcount >= NVN_POOL_QUARANTINE) {
        idx = g_pool_qring[g_pool_qhead];
        g_pool_qhead = (g_pool_qhead + 1u) & (NVN_POOL_QRING_CAP - 1u);
        g_pool_qcount--;
    } else if (g_pool_next_fresh < NVN_POOL_MAX_SLOTS) {
        idx = g_pool_next_fresh++;
    } else if (g_pool_qcount > 0) {
        idx = g_pool_qring[g_pool_qhead];
        g_pool_qhead = (g_pool_qhead + 1u) & (NVN_POOL_QRING_CAP - 1u);
        g_pool_qcount--;
    } else {
        pthread_mutex_unlock(&g_pool_alloc_lock);
        FATAL("NVN memory pool slot table exhausted (%u slots)\n", NVN_POOL_MAX_SLOTS);
    }
    uint32_t p = idx >> NVN_POOL_PAGE_SHIFT;
    NvnPoolSlot *page = atomic_load_explicit(&g_pool_pages[p], memory_order_relaxed);
    if (!page) {
        page = calloc(NVN_POOL_PAGE_SIZE, sizeof(NvnPoolSlot));
        if (!page) {
            pthread_mutex_unlock(&g_pool_alloc_lock);
            FATAL("NVN memory pool page allocation failed\n");
        }
        atomic_store_explicit(&g_pool_pages[p], page, memory_order_release);
    }
    NvnPoolSlot *s = &page[idx & NVN_POOL_PAGE_MASK];
    uint32_t gen = atomic_load_explicit(&s->generation, memory_order_relaxed) + 1u;
    if (!gen) gen = 1u;
    atomic_store_explicit(&s->state, TEX_SLOT_FREE, memory_order_release);
    atomic_store_explicit(&s->generation, gen, memory_order_release);
    memset(&s->pool, 0, sizeof s->pool);
    uint16_t ep = atomic_load_explicit(&g_pool_epoch, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_alloc_lock);

    NvnGuestHandle h = {
        .magic = NVN_MAGIC_POOL_LIVE,
        .slot_index = idx,
        .generation = gen,
        .rt_epoch = ep,
        .tag16 = 0,
    };
    h.tag16 = nvn_handle_compute_tag(&h, NVN_OBJ_POOL);
    *out_hdr = h;
    return s;
}

static bool pool_slot_retire(uint32_t idx, uint32_t gen) {
    pthread_mutex_lock(&g_pool_alloc_lock);
    NvnPoolSlot *s = pool_slot_at(idx);
    bool ok = false;
    if (s && atomic_load_explicit(&s->generation, memory_order_relaxed) == gen &&
        atomic_load_explicit(&s->state, memory_order_relaxed) == TEX_SLOT_ALIVE) {
        atomic_store_explicit(&s->state, TEX_SLOT_RETIRED, memory_order_release);
        if (g_pool_qcount < NVN_POOL_QRING_CAP) {
            g_pool_qring[g_pool_qtail] = idx;
            g_pool_qtail = (g_pool_qtail + 1u) & (NVN_POOL_QRING_CAP - 1u);
            g_pool_qcount++;
        }
        ok = true;
    }
    pthread_mutex_unlock(&g_pool_alloc_lock);
    return ok;
}

RT_ON_RESET(pool_identities_reset) {
    pthread_mutex_lock(&g_pool_alloc_lock);
    uint16_t next_ep = (uint16_t)(atomic_load_explicit(&g_pool_epoch, memory_order_relaxed) + 1u);
    if (!next_ep) next_ep = 1u;
    atomic_store_explicit(&g_pool_epoch, next_ep, memory_order_release);
    for (uint32_t p = 0; p < NVN_POOL_MAX_PAGES; p++) {
        NvnPoolSlot *page = atomic_exchange_explicit(&g_pool_pages[p], NULL, memory_order_acq_rel);
        free(page);
    }
    g_pool_next_fresh = 0;
    g_pool_qhead = g_pool_qtail = g_pool_qcount = 0;
    pthread_mutex_unlock(&g_pool_alloc_lock);
}

static bool pool_read_handle(uint64_t guest, NvnGuestHandle *out) {
    if (guest_mem_read_checked(guest, out, sizeof *out, 8) != GMEM_OK) return false;
    if (out->magic != NVN_MAGIC_POOL_LIVE) return false;
    if (out->tag16 != nvn_handle_compute_tag(out, NVN_OBJ_POOL)) return false;
    if (out->rt_epoch != atomic_load_explicit(&g_pool_epoch, memory_order_acquire)) return false;
    return true;
}

NvnPool *nvn_pool_host(uint64_t guest) {
    NvnGuestHandle h;
    if (!pool_read_handle(guest, &h)) return NULL;
    NvnPoolSlot *s = pool_slot_at(h.slot_index);
    if (!s) return NULL;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != TEX_SLOT_ALIVE) return NULL;
    if (atomic_load_explicit(&s->generation, memory_order_acquire) != h.generation) return NULL;
    return &s->pool;
}

// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset((uint8_t *)AP(0) + 8, 0, 0x18); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    uint64_t guest = A(0), builder = A(1);
    if (guest_mem_validate_range(guest, sizeof(NvnGuestHandle), 8, GMEM_PERM_RW) != GMEM_OK ||
        guest_mem_validate_range(builder, 0x20, 8, GMEM_PERM_R) != GMEM_OK) {
        nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_POOL, 0, guest, 0, 0, 0, 0, c->x[30], false, builder, 0);
        nvn_trace_record(NVN_TR_POOL_INIT_FAIL, guest, builder, 0, 0, c->x[30]);
        uint64_t cnt = 0;
        if (nvn_diag_should_log(NVN_EV_LOOKUP_FAIL, 0xFE, c->x[30], &cnt)) {
            LOG("[nvn-diag] nvnMemoryPoolInitialize validation failed (x%llu): guest=%llx builder=%llx lr=%llx\n",
                (unsigned long long)cnt, (unsigned long long)guest, (unsigned long long)builder,
                (unsigned long long)c->x[30]);
        }
        RET(0);
        return;
    }
    NvnGuestHandle prev;
    if (pool_read_handle(guest, &prev)) pool_slot_retire(prev.slot_index, prev.generation);
    NvnGuestHandle h;
    NvnPoolSlot *slot = pool_slot_alloc(&h);
    NvnPool *p = &slot->pool;
    p->storage = G(uint64_t, builder, 0x10);
    p->size = G(uint64_t, builder, 0x18);
    p->flags = G(uint32_t, builder, 8);
    atomic_store_explicit(&slot->state, TEX_SLOT_ALIVE, memory_order_release);
    if (getenv("LARECOMP_LOG_POOLS"))  // (FE3H makes a transient pool thousands of times a session)
        LOG("[nvn] memory pool storage %llx size %llx flags %#x\n", (unsigned long long)p->storage, (unsigned long long)p->size, p->flags);
#ifndef __APPLE__
    {
        extern void vk_alias_pool_register(uint64_t storage, uint64_t size) __attribute__((weak));
        if (vk_alias_pool_register) vk_alias_pool_register(p->storage, p->size);
    }
#endif
    guest_mem_write_checked(guest, &h, sizeof h, 8);
    nvn_diag_record(NVN_EV_INIT, NVN_OBJ_POOL, 1, guest, h.slot_index, h.generation, h.rt_epoch, 0, c->x[30], false, 0, 0);
    RET(1);
}
NVN(nvnMemoryPoolFinalize) {
    uint64_t guest = A(0);
    NvnGuestHandle h;
    if (pool_read_handle(guest, &h)) {
        NvnPoolSlot *s = pool_slot_at(h.slot_index);
        uint64_t p_storage = s ? s->pool.storage : 0;
        uint64_t p_size = s ? s->pool.size : 0;
        bool retired = pool_slot_retire(h.slot_index, h.generation);
#ifndef __APPLE__
        if (retired && p_size && p_storage >= ALIAS_BASE && p_storage < ALIAS_BASE + ALIAS_SIZE) {
            extern void vk_alias_pool_unregister(uint64_t addr, uint64_t size) __attribute__((weak));
            if (vk_alias_pool_unregister) vk_alias_pool_unregister(p_storage, p_size);
        }
#else
        (void)retired; (void)p_storage; (void)p_size;
#endif
        NvnGuestHandle fin = {
            .magic = NVN_MAGIC_POOL_FIN,
            .slot_index = h.slot_index,
            .generation = h.generation,
            .rt_epoch = h.rt_epoch,
            .tag16 = 0,
        };
        fin.tag16 = nvn_handle_compute_tag(&fin, NVN_OBJ_POOL);
        guest_mem_write_checked(guest, &fin, sizeof fin, 8);
        nvn_diag_record(NVN_EV_FINALIZE, NVN_OBJ_POOL, 0, guest, h.slot_index, h.generation, h.rt_epoch, 0, c->x[30], false, 0, 0);
    }
}
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN(nvnMemoryPoolMap) { NvnPool *p = HOST(NvnPool, A(0)); RET(p ? p->storage : 0); }
NVN(nvnMemoryPoolGetBufferAddress) { NvnPool *p = HOST(NvnPool, A(0)); RET(p ? p->storage : 0); }  // GPU address == CPU address
NVN(nvnMemoryPoolGetSize) { NvnPool *p = HOST(NvnPool, A(0)); RET(p ? p->size : 0); }
NVN(nvnMemoryPoolGetFlags) { NvnPool *p = HOST(NvnPool, A(0)); RET(p ? p->flags : 0); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
    if (p) backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
// MapVirtual(pool, numRequests, const NVNmappingRequest *): {physicalPool*, physicalOffset, virtualOffset, size,
// storageClass} (0x28 bytes). Not implemented: logged.
NVN(nvnMemoryPoolMapVirtual) {
    static int nlog;
    NvnPool *vp = HOST(NvnPool, A(0));
    if (!vp) { RET(0); return; }
    int n = (int)A(1);
    for (int i = 0; i < n && nlog < 40; i++, nlog++) {
        const uint8_t *rq = (const uint8_t *)AP(2) + i * 0x28;
        uint64_t pp = *(const uint64_t *)rq;
        NvnPool *phys = pp ? HOST(NvnPool, pp) : NULL;
        LOG("[nvn] MapVirtual pool storage %llx size %llx flags %#x: phys %llx+%llx -> virt +%llx size %llx class %#x\n",
            (unsigned long long)vp->storage, (unsigned long long)vp->size, vp->flags,
            (unsigned long long)(phys ? phys->storage : 0), (unsigned long long)*(const uint64_t *)(rq + 8),
            (unsigned long long)*(const uint64_t *)(rq + 16), (unsigned long long)*(const uint64_t *)(rq + 24),
            *(const uint32_t *)(rq + 32));
    }
    RET(1);
}

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset((uint8_t *)AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    if (guest_mem_validate_range(A(0), 0x28, 8, GMEM_PERM_RW) != GMEM_OK ||
        guest_mem_validate_range(A(1), 0x20, 8, GMEM_PERM_R) != GMEM_OK) {
        RET(0);
        return;
    }
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    NvnPool *p = HOST(NvnPool, pool);
    if (!p) { RET(0); return; }
    G(uint64_t, A(0), 0) = p->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = alloc_debug_id();
    RET(1);
}
static uint64_t buf_read_u64(uint64_t guest, uint64_t off) {
    uint64_t v = 0;
    if (guest) guest_mem_read_checked(guest + off, &v, sizeof v, 8);
    return v;
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(buf_read_u64(A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(buf_read_u64(A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(buf_read_u64(A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(buf_read_u64(A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(buf_read_u64(A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET(buf_read_u64(A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) {
    uint64_t base = buf_read_u64(A(0), 0);
    if (base) backend_pool_written(base + A(1), A(2));
}
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) {
    TB(uint32_t, 8) = (uint32_t)A(1);
    nvn_trace_record(NVN_TR_TEX_FLAGS, A(0), (uint32_t)A(1),
                     (uint64_t)TB(uint32_t, 0x10) | ((uint64_t)TB(uint32_t, 0x14) << 32),
                     (uint64_t)TB(uint32_t, 0x20) | ((uint64_t)TB(uint32_t, 0x1C) << 32), c->x[30]);
}
NVN(nvnTextureBuilderSetTarget) {
    TB(uint32_t, 0xC) = (uint32_t)A(1);
    nvn_trace_record(NVN_TR_TEX_TARGET, A(0), (uint32_t)A(1),
                     (uint64_t)TB(uint32_t, 0x10) | ((uint64_t)TB(uint32_t, 0x14) << 32),
                     (uint64_t)TB(uint32_t, 0x20) | ((uint64_t)TB(uint32_t, 0x24) << 32), c->x[30]);
}
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize2D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(uint64_t *)AP(1) : 0; }
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) { RET(0); }
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(0); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) {  // 1D array: height is the layer count
        t->layers = G(uint32_t, b, 0x14);
    }
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    // Packaged textures (BNTX) were swizzled offline with their own block height, which can differ
    // from the driver default: NVNpackagedTextureLayout's low word is log2(GOBs per block).
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl && (uint32_t)pl <= 5) t->block_height_log2 = (uint32_t)pl;
    if (pl && getenv("LARECOMP_LOG_PACKAGED")) {
        static int n;
        if (n++ < 4000) LOG("[nvn] packaged layout %016llx fmt %#x %ux%u lv %u -> default bh %u\n", pl, G(uint32_t, b, 0x20), t->width, t->height, t->levels, t->block_height_log2);
    }
}

static bool is_linear(uint64_t b) { return (G(uint32_t, b, 8) & 0x10) || G(uint32_t, b, 0xC) == 10; }

// Scale sample grid for 2D multisample / multisample-array targets (NVN targets 5 and 6)
// using Tegra X1 block-linear sample multipliers so RT pools never under-allocate or alias.
static void msaa_scale_dims(uint32_t target, uint32_t samples, uint32_t *w, uint32_t *h) {
    if ((target != 5 && target != 6) || samples <= 1) return;
    uint32_t sx = samples >= 8 ? 4u : 2u;
    uint32_t sy = samples >= 16 ? 4u : (samples >= 4 ? 2u : 1u);
    *w *= sx;
    *h *= sy;
}

static uint64_t compute_tex_storage_size(uint32_t target, uint32_t flags, uint32_t width, uint32_t height,
                                         uint32_t format, uint32_t samples, uint64_t stride,
                                         const TexLayout *base_layout) {
    if (target == 10) return (uint64_t)width * nvn_format_info(format)->bpb;  // buffer texture
    if (flags & 0x10) return stride * height;
    TexLayout sz = *base_layout;
    if ((target == 5 || target == 6) && samples > 1) {
        msaa_scale_dims(target, samples, &sz.width, &sz.height);
        tegra_layout_init(&sz);
    }
    return tegra_total_size(&sz);
}

static uint64_t builder_storage_size(uint64_t b) {
    TexLayout t = {0};
    uint32_t target = G(uint32_t, b, 0xC);
    uint32_t flags = G(uint32_t, b, 8);
    uint32_t width = G(uint32_t, b, 0x10);
    uint32_t height = G(uint32_t, b, 0x14);
    uint32_t format = G(uint32_t, b, 0x20);
    uint32_t samples = G(uint32_t, b, 0x24);
    uint64_t stride = G(uint64_t, b, 0x30);
    if (target != 10 && !(flags & 0x10)) builder_layout(b, &t);
    return compute_tex_storage_size(target, flags, width, height, format, samples, stride, &t);
}

static uint64_t builder_storage_alignment(uint64_t b) {
    if (is_linear(b)) return 32;
    uint32_t flags = G(uint32_t, b, 8);
    uint32_t target = G(uint32_t, b, 0xC);
    uint32_t samples = G(uint32_t, b, 0x24);
    // NVN compressible textures (flags & 8) and MSAA targets require 64 KB alignment.
    if ((flags & 8u) || ((target == 5 || target == 6) && samples > 1)) return 65536ull;
    TexLayout t = {0};
    builder_layout(b, &t);
    uint64_t blk = 512ull << t.block_height_log2;
    return blk > 512ull ? blk : 512ull;
}

NVN(nvnTextureBuilderGetStorageSize) {
    uint64_t b = A(0);
    uint64_t sz = builder_storage_size(b);
    nvn_trace_record(NVN_TR_TEX_STORAGE_SIZE, b, sz,
                     (uint64_t)G(uint32_t, b, 0x10) | ((uint64_t)G(uint32_t, b, 0x14) << 32),
                     (uint64_t)G(uint32_t, b, 0x20) | ((uint64_t)G(uint32_t, b, 0xC) << 32) |
                     ((uint64_t)G(uint32_t, b, 0x24) << 40), c->x[30]);
    RET(sz);
}
NVN(nvnTextureBuilderGetStorageAlignment) {
    uint64_t b = A(0);
    uint64_t al = builder_storage_alignment(b);
    nvn_trace_record(NVN_TR_TEX_STORAGE_ALIGN, b, al, G(uint32_t, b, 8),
                     (uint64_t)G(uint32_t, b, 0xC) | ((uint64_t)G(uint32_t, b, 0x24) << 32), c->x[30]);
    RET(al);
}

static void texture_fill_from_builder(NvnTexture *t, uint64_t b) {
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->guest_pool = pool;
    t->pool = NULL;  // Phase 5A: never retain a raw NvnPool* pointer that can dangle after pool finalize
    t->pool_slot = 0; t->pool_gen = 0; t->pool_epoch = 0;
    NvnPool *p = NULL;
    if (pool) {
        NvnGuestHandle ph;
        if (pool_read_handle(pool, &ph)) {
            t->pool_slot = ph.slot_index;
            t->pool_gen = ph.generation;
            t->pool_epoch = ph.rt_epoch;
        }
        p = nvn_pool_host(pool);
    }
    t->pool_size = p ? p->size : 0;
    t->pool_offset = G(uint64_t, b, 0x40);
    t->addr = p ? p->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    t->debug_id = alloc_debug_id();
}

static bool nvn_strict_mode(void) {
    const char *e = getenv("ENGAGE_NVN_STRICT");
    return e && *e && strcmp(e, "0") != 0;
}

static uint64_t texture_storage_size(const NvnTexture *t) {
    return compute_tex_storage_size(t->target, t->flags, t->width, t->height,
                                    t->format, t->samples, t->stride, &t->layout);
}

NVN(nvnTextureInitialize) {
    uint64_t guest = A(0), builder = A(1);
    if (guest_mem_validate_range(guest, 128, 8, GMEM_PERM_RW) != GMEM_OK ||
        guest_mem_validate_range(builder, 128, 8, GMEM_PERM_R) != GMEM_OK) {
        nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_TEXTURE, TEX_ST_BAD_ADDR, guest, 0, 0, 0, 0, c->x[30], false, 0, 0);
        RET(0);
        return;
    }
    // Detect re-initialization over an active original NVNtexture without prior Finalize.
    NvnTexLookup prev = nvn_texture_lookup(guest);
    if (prev.state == TEX_ST_LIVE_ORIG && prev.tex) {
        bool retired = tex_slot_retire(prev.hdr.slot_index, prev.hdr.generation);
        nvn_diag_record(NVN_EV_REINIT_OVER_LIVE, NVN_OBJ_TEXTURE, (uint8_t)prev.state, guest,
                        prev.hdr.slot_index, prev.hdr.generation, prev.hdr.rt_epoch, 0, c->x[30], false, 0, 0);
        if (retired) {
            atomic_store_explicit(&prev.tex->finalized, true, memory_order_release);
            backend_texture_destroyed(prev.tex);
        }
    }

    NvnGuestHandle hdr;
    NvnTexSlot *slot = tex_slot_alloc(guest, &hdr);
    NvnTexture *t = &slot->tex;
    atomic_store_explicit(&t->finalized, false, memory_order_release);
    texture_fill_from_builder(t, builder);
    tex_slot_publish_live(slot);
    if (t->flags & 2)
        LOG("[nvn] video texture %p: %ux%u fmt %#x flags %#x target %u addr %llx stride %llu\n", (void *)t, t->width, t->height,
            t->format, t->flags, t->target, (unsigned long long)t->addr, (unsigned long long)t->stride);
    uint8_t zeros[112] = {0};
    guest_mem_write_checked(guest, &hdr, sizeof hdr, 8);
    guest_mem_write_checked(guest + 16, zeros, sizeof zeros, 8);
    nvn_diag_record(NVN_EV_INIT, NVN_OBJ_TEXTURE, TEX_ST_LIVE_ORIG, guest, hdr.slot_index, hdr.generation,
                    hdr.rt_epoch, 0, c->x[30], false, 0, 0);
    RET(1);
}
NVN(nvnTextureFinalize) {
    uint64_t guest = A(0);
    NvnTexLookup lk = nvn_texture_lookup(guest);
    if (lk.state == TEX_ST_BAD_ADDR) {
        nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_TEXTURE, TEX_ST_BAD_ADDR, guest, 0, 0, 0, 0, c->x[30], false, 0, 0);
        return;
    }
    if (lk.state == TEX_ST_LIVE_ORIG || lk.state == TEX_ST_LIVE_COPY || lk.state == TEX_ST_STALE_COPY) {
        bool retired_now = tex_slot_retire(lk.hdr.slot_index, lk.hdr.generation);
        nvn_diag_record(retired_now ? NVN_EV_FINALIZE : NVN_EV_DOUBLE_FINALIZE, NVN_OBJ_TEXTURE, (uint8_t)lk.state,
                        guest, lk.hdr.slot_index, lk.hdr.generation, lk.hdr.rt_epoch, 0, c->x[30], false, 0, 0);
        if (lk.tex) {
            atomic_store_explicit(&lk.tex->finalized, true, memory_order_release);
            backend_texture_destroyed(lk.tex);
        }
        // Write finalized sentinel preserving (slot_index, generation, rt_epoch) so finalized
        // instances remain distinguishable from ZEROED_GUEST.
        NvnGuestHandle fin = lk.hdr;
        fin.magic = NVN_MAGIC_TEX_FIN;
        fin.tag16 = 0;
        fin.tag16 = nvn_handle_compute_tag(&fin, NVN_OBJ_TEXTURE);
        uint8_t zeros[112] = {0};
        guest_mem_write_checked(guest, &fin, sizeof fin, 8);
        guest_mem_write_checked(guest + 16, zeros, sizeof zeros, 8);
        return;
    }
    if (lk.state == TEX_ST_FINALIZED_INST) {
        nvn_diag_record(NVN_EV_DOUBLE_FINALIZE, NVN_OBJ_TEXTURE, (uint8_t)lk.state,
                        guest, lk.hdr.slot_index, lk.hdr.generation, lk.hdr.rt_epoch, 0, c->x[30], false, 0, 0);
        return;  // Keep existing finalized sentinel intact
    }
    // Zeroed / stale / alien: do not overwrite arbitrary alien memory; zeroed is already zero.
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
static NvnTexture *texture_getter_lookup(uint64_t guest, const char *fn_name, uint64_t lr) {
    NvnTexLookup lk = nvn_texture_lookup(guest);
    if (lk.state == TEX_ST_LIVE_ORIG || lk.state == TEX_ST_LIVE_COPY) {
        return lk.tex;
    }
    if (lk.state == TEX_ST_STALE_COPY) {
        uint64_t cnt = 0;
        if (nvn_diag_should_log(NVN_EV_COPY_SEEN, (uint8_t)lk.state, lr, &cnt)) {
            LOG("[nvn-diag] %s on value copy after sibling finalize (x%llu): guest=%llx slot=%u gen=%u lr=%llx\n",
                fn_name, (unsigned long long)cnt, (unsigned long long)guest, lk.hdr.slot_index, lk.hdr.generation,
                (unsigned long long)lr);
        }
        return lk.tex;
    }
    if (lk.state == TEX_ST_ZEROED) {
        // Legitimate pending / uninitialized zeroed struct probed by Unity during streaming.
        uint64_t cnt = 0;
        if (nvn_diag_should_log(NVN_EV_LOOKUP_FAIL, (uint8_t)lk.state, lr, &cnt)) {
            LOG("[texture-life] deferred pending %s (x%llu) guest=%llx lr=%llx\n",
                fn_name, (unsigned long long)cnt, (unsigned long long)guest, (unsigned long long)lr);
        }
        return NULL;
    }
    if (lk.state == TEX_ST_BAD_ADDR && lk.mem_status == GMEM_NULL) {
        return NULL;
    }
    uint64_t q0 = 0, q1 = 0;
    if (lk.raw_valid) {
        memcpy(&q0, &lk.hdr, 8);
        memcpy(&q1, (const uint8_t *)&lk.hdr + 8, 8);
    }
    nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_TEXTURE, (uint8_t)lk.state, guest,
                    lk.hdr.slot_index, lk.hdr.generation, lk.hdr.rt_epoch, 0, lr,
                    lk.raw_valid, q0, q1);
    if (lk.state == TEX_ST_CORRUPT) {
        if (nvn_strict_mode()) {
            nvn_diag_dump_for_addr(fn_name, guest, lk.hdr.slot_index, lk.hdr.generation, lr);
            nvn_trace_dump(fn_name);
            FATAL("[texture-life] corrupt NVNtexture in %s: guest=%llx raw=[%016llx %016llx] lr=%llx\n",
                  fn_name, (unsigned long long)guest, (unsigned long long)q0, (unsigned long long)q1,
                  (unsigned long long)lr);
        }
        uint64_t cnt = 0;
        if (nvn_diag_should_log(NVN_EV_LOOKUP_FAIL, (uint8_t)lk.state, lr, &cnt)) {
            LOG("[nvn-diag] TAINTED/MITIGATED corrupt %s (x%llu): guest=%llx raw=[%016llx %016llx] lr=%llx\n",
                fn_name, (unsigned long long)cnt, (unsigned long long)guest, (unsigned long long)q0,
                (unsigned long long)q1, (unsigned long long)lr);
            if (cnt == 1) {
                nvn_diag_dump_for_addr(fn_name, guest, lk.hdr.slot_index, lk.hdr.generation, lr);
                nvn_trace_dump(fn_name);
            }
        }
        return NULL;
    }
    // FINALIZED_INST / STALE_RESET / STALE_RECYCLED / BAD_ADDR (non-NULL)
    uint64_t cnt = 0;
    if (nvn_diag_should_log(NVN_EV_LOOKUP_FAIL, (uint8_t)lk.state, lr, &cnt)) {
        LOG("[nvn-diag] invalid %s (x%llu) state=%u mem=%s guest=%llx lr=%llx\n",
            fn_name, (unsigned long long)cnt, (unsigned)lk.state, guest_mem_status_name(lk.mem_status),
            (unsigned long long)guest, (unsigned long long)lr);
    }
    if (nvn_strict_mode()) {
        nvn_diag_dump_for_addr(fn_name, guest, lk.hdr.slot_index, lk.hdr.generation, lr);
        FATAL("[texture-life] invalid NVNtexture in %s: state=%u mem=%s guest=%llx lr=%llx\n",
              fn_name, (unsigned)lk.state, guest_mem_status_name(lk.mem_status), (unsigned long long)guest,
              (unsigned long long)lr);
    }
    return NULL;
}
#define TEX_GET(fn, expr) do { \
    const NvnTexture *t = texture_getter_lookup(A(0), (fn), c->x[30]); \
    RET(t ? (expr) : 0); \
} while (0)
NVN(nvnTextureGetFlags) { TEX_GET("nvnTextureGetFlags", t->flags); }
NVN(nvnTextureGetTarget) { TEX_GET("nvnTextureGetTarget", t->target); }
NVN(nvnTextureGetWidth) { TEX_GET("nvnTextureGetWidth", t->width); }
NVN(nvnTextureGetHeight) { TEX_GET("nvnTextureGetHeight", t->height); }
NVN(nvnTextureGetDepth) { TEX_GET("nvnTextureGetDepth", t->depth); }
NVN(nvnTextureGetLevels) { TEX_GET("nvnTextureGetLevels", t->levels); }
NVN(nvnTextureGetFormat) { TEX_GET("nvnTextureGetFormat", t->format); }
NVN(nvnTextureGetSamples) { TEX_GET("nvnTextureGetSamples", t->samples); }
NVN(nvnTextureGetSwizzle) {
    const NvnTexture *t = texture_getter_lookup(A(0), "nvnTextureGetSwizzle", c->x[30]);
    for (int i = 0; i < 4; i++) {
        if (guest_mem_validate_range(A(1 + i), sizeof(uint32_t), 4, GMEM_PERM_RW) == GMEM_OK)
            *(uint32_t *)AP(1 + i) = t ? t->swizzle[i] : 0;
    }
}
NVN(nvnTextureGetDepthStencilMode) { TEX_GET("nvnTextureGetDepthStencilMode", t->ds_mode); }
NVN(nvnTextureGetStride) { TEX_GET("nvnTextureGetStride", t->stride); }
NVN(nvnTextureGetTextureAddress) { TEX_GET("nvnTextureGetTextureAddress", t->addr); }
NVN(nvnTextureGetMemoryPool) { TEX_GET("nvnTextureGetMemoryPool", t->guest_pool); }
NVN(nvnTextureGetMemoryOffset) { TEX_GET("nvnTextureGetMemoryOffset", t->pool_offset); }
NVN(nvnTextureGetStorageSize) { TEX_GET("nvnTextureGetStorageSize", texture_storage_size(t)); }
NVN(nvnTextureGetDebugID) { TEX_GET("nvnTextureGetDebugID", t->debug_id); }
#undef TEX_GET
NVN(nvnTextureGetViewOffset) { RET(0); }
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(0); }  // [NVN_UNVERIFIED_HW]
NVN(nvnTextureCompare) {
    NvnTexLookup a = nvn_texture_lookup(A(0));
    NvnTexLookup b = nvn_texture_lookup(A(1));
    bool a_ok = (a.state == TEX_ST_LIVE_ORIG || a.state == TEX_ST_LIVE_COPY || a.state == TEX_ST_STALE_COPY);
    bool b_ok = (b.state == TEX_ST_LIVE_ORIG || b.state == TEX_ST_LIVE_COPY || b.state == TEX_ST_STALE_COPY);
    if (a_ok && b_ok) {
        if (a.tex == b.tex) { RET(1); return; }
        const NvnTexture *ta = a.tex, *tb = b.tex;
        RET(ta->addr == tb->addr && ta->target == tb->target && ta->format == tb->format &&
            ta->flags == tb->flags && ta->width == tb->width && ta->height == tb->height &&
            ta->depth == tb->depth && ta->levels == tb->levels && ta->samples == tb->samples &&
            ta->stride == tb->stride && ta->ds_mode == tb->ds_mode &&
            memcmp(ta->swizzle, tb->swizzle, 4) == 0);
        return;
    }
    RET(a.state == TEX_ST_ZEROED && b.state == TEX_ST_ZEROED && A(0) == A(1));
}
NVN(nvnTextureFlushTexels) { (void)c; }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views: compact 16-byte layout (NVN 55.0.0 ABI — verified in Engage 2.0.0 main)
NVN(nvnTextureViewSetDefaults) {
    if (guest_mem_validate_range(A(0), sizeof(NvnTextureView), 1, GMEM_PERM_RW) == GMEM_OK)
        memset(AP(0), 0, sizeof(NvnTextureView));
}
NVN(nvnTextureViewSetLevels) {
    NvnTextureView *v = AP(0);
    int32_t base = (int32_t)A(1);
    int32_t count = (int32_t)A(2);
    v->setmask |= 1;
    v->base_level = (uint8_t)(base < 0 ? 0 : (base > 255 ? 255 : base));
    v->num_levels = (uint8_t)(count < 0 ? 0 : (count > 255 ? 255 : count));
}
NVN(nvnTextureViewSetLayers) {
    NvnTextureView *v = AP(0);
    v->setmask |= 2;
    v->min_layer = (uint16_t)A(1);
    v->num_layers = (uint16_t)A(2);
}
NVN(nvnTextureViewSetFormat) {
    NvnTextureView *v = AP(0);
    v->setmask |= 4;
    v->format = (uint16_t)A(1);
}
NVN(nvnTextureViewSetSwizzle) {
    NvnTextureView *v = AP(0);
    v->setmask |= 8;
    v->swizzle[0] = (uint8_t)A(1); v->swizzle[1] = (uint8_t)A(2); v->swizzle[2] = (uint8_t)A(3); v->swizzle[3] = (uint8_t)A(4);
}
NVN(nvnTextureViewSetDepthStencilMode) { NvnTextureView *v = AP(0); v->setmask |= 16; v->ds_mode = (uint8_t)A(1); }
NVN(nvnTextureViewSetTarget) { NvnTextureView *v = AP(0); v->setmask |= 32; v->target = (uint8_t)A(1); }
NVN(nvnTextureViewGetLevels) {
    const NvnTextureView *v = AP(0);
    *(int32_t *)AP(1) = v->base_level; *(int32_t *)AP(2) = v->num_levels;
    RET(!!(v->setmask & 1));
}
NVN(nvnTextureViewGetLayers) {
    const NvnTextureView *v = AP(0);
    *(int32_t *)AP(1) = v->min_layer; *(int32_t *)AP(2) = v->num_layers;
    RET(!!(v->setmask & 2));
}
NVN(nvnTextureViewGetFormat) {
    const NvnTextureView *v = AP(0);
    *(uint32_t *)AP(1) = v->format;
    RET(!!(v->setmask & 4));
}
NVN(nvnTextureViewGetSwizzle) {
    const NvnTextureView *v = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = v->swizzle[i];
    RET(!!(v->setmask & 8));
}
NVN(nvnTextureViewGetDepthStencilMode) { const NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->ds_mode; RET(!!(v->setmask & 16)); }
NVN(nvnTextureViewGetTarget) { const NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->target; RET(!!(v->setmask & 32)); }
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), sizeof(NvnTextureView)) == 0); }

static void write_texels(Cpu *c, uint64_t row_pitch, uint64_t image_pitch) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    const NvnTextureView *view = (A(1) && guest_mem_validate_range(A(1), sizeof(NvnTextureView), 1, GMEM_PERM_R) == GMEM_OK)
                                     ? (const NvnTextureView *)AP(1) : NULL;
    if (!t || !t->addr || !t->pool_size ||
        guest_mem_validate_range(A(2), 6 * sizeof(int32_t), 4, GMEM_PERM_R) != GMEM_OK ||
        !A(3))
        return;
    const int32_t *r = AP(2);
    const uint8_t *pixels = AP(3);
    for (int axis = 0; axis < 3; ++axis)
        if (r[axis] < 0 || r[axis + 3] <= 0) return;

    uint32_t level = view && (view->setmask & 1) ? view->base_level : 0;
    if (level >= t->levels) return;
    LevelLayout l = tegra_level(&t->layout, level);
    const FormatInfo *fmt = t->layout.fmt;
    const bool volume = t->target == 2;
    const bool linear = (t->flags & 0x10) || t->target == 10;
    uint32_t layer = view && (view->setmask & 2) ? view->min_layer : 0;
    uint32_t x = r[0], y = r[1], z = r[2], width = r[3], height = r[4], slices = r[5];
    uint32_t layers = t->layout.layers;
    if (x >= l.w || width > l.w - x || y >= l.h || height > l.h - y ||
        x % fmt->bw || y % fmt->bh ||
        (width % fmt->bw && width != l.w - x) || (height % fmt->bh && height != l.h - y))
        return;
    if (volume ? (z >= l.d || slices > l.d - z)
               : (layer >= layers || z >= layers - layer || slices > layers - layer - z))
        return;

    uint32_t xbytes = (x / fmt->bw) * fmt->bpb, yblock = y / fmt->bh;
    uint32_t width_bytes = ((width + fmt->bw - 1) / fmt->bw) * fmt->bpb;
    uint32_t rows = (height + fmt->bh - 1) / fmt->bh;
    if (!row_pitch) row_pitch = width_bytes;
    if (row_pitch < width_bytes || row_pitch > UINT64_MAX / rows) return;
    uint64_t minimum_image_pitch = row_pitch * rows;
    if (!image_pitch) image_pitch = minimum_image_pitch;
    if (image_pitch < minimum_image_pitch || image_pitch > UINT64_MAX / slices) return;

    uint64_t layer_size = tegra_layer_size(&t->layout);
    uint64_t storage_size = linear ? t->stride * l.hb : tegra_total_size(&t->layout);
    if (t->pool_offset > t->pool_size || storage_size > t->pool_size - t->pool_offset)
        return;
    if (linear && (level || slices != 1 || z || layer || !t->stride ||
                   (uint64_t)xbytes + width_bytes > t->stride))
        return;
    uint64_t src_total = slices > 0 ? (uint64_t)(slices - 1) * image_pitch + (uint64_t)(rows - 1) * row_pitch + width_bytes : 0;
    if (guest_mem_validate_range(A(3), src_total, 1, GMEM_PERM_R) != GMEM_OK ||
        guest_mem_validate_range(t->addr, storage_size, 1, GMEM_PERM_RW) != GMEM_OK)
        return;

    uint8_t *storage = (uint8_t *)(uintptr_t)t->addr;
    for (uint32_t i = 0; i < slices; ++i) {
        const uint8_t *src = pixels + i * image_pitch;
        if (linear) {
            for (uint32_t row = 0; row < rows; ++row)
                memcpy(storage + (yblock + row) * t->stride + xbytes,
                       src + row * row_pitch, width_bytes);
        } else {
            uint8_t *dst = storage + l.offset + (volume ? 0 : (layer + z + i) * layer_size);
            tegra_swizzle_region(src, dst, &l, fmt->bpb, xbytes, yblock,
                                 volume ? z + i : 0, width_bytes, rows, row_pitch);
        }
    }
    backend_pool_written(t->addr, storage_size);
#ifndef __APPLE__
    {
        extern void backend_texture_written(NvnTexture *t) __attribute__((weak));
        if (backend_texture_written) backend_texture_written(t);
    }
#endif
}

NVN(nvnTextureWriteTexels) { write_texels(c, 0, 0); }
NVN(nvnTextureWriteTexelsStrided) { write_texels(c, A(4), A(5)); }
NVN(nvnTextureReadTexels) { LOG("[nvn] ReadTexels not implemented\n"); (void)c; }
NVN(nvnTextureReadTexelsStrided) { (void)c; }

// ================================================================== samplers
// SamplerBuilder: device@0 then an NvnSamplerDesc at 8
#define SD ((NvnSamplerDesc *)(AP(0) + 8))
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerDesc *d = SD;
    memset(d, 0, sizeof *d);
    d->min_filter = 0; d->mag_filter = 0;
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  // REPEAT
    d->lod_min = 0; d->lod_max = 1000.0f; d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1); SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { SD->wrap[0] = (uint32_t)A(1); SD->wrap[1] = (uint32_t)A(2); SD->wrap[2] = (uint32_t)A(3); }
NVN(nvnSamplerBuilderSetLodClamp) { SD->lod_min = AF(0); SD->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1); SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SD->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SD->reduction = (uint32_t)A(1); }
NVN(nvnSamplerBuilderSetLodSnap) { (void)c; }
NVN(nvnSamplerBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SD->min_filter; *(uint32_t *)AP(2) = SD->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SD->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SD->lod_min; *(float *)AP(2) = SD->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SD->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SD->compare_mode; *(uint32_t *)AP(2) = SD->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SD->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SD->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

// NVNsampler (0x60 bytes) stores canonical state inline (Phase 5A):
//   +0..4:  uint32_t magic (NVN_MAGIC_SMP_LIVE / NVN_MAGIC_SMP_FIN)
//   +4..8:  uint32_t debug_id
//   +8..80: NvnSamplerDesc (canonical state bytes; backend = NULL)
// Zero heap allocations -> value copies (memcpy) and double Finalize can never double-free.
typedef struct NvnSamplerHeader {
    uint32_t magic;
    uint32_t debug_id;
} NvnSamplerHeader;
_Static_assert(sizeof(NvnSamplerHeader) + sizeof(NvnSamplerDesc) <= 0x60, "NVNsampler inline layout exceeds 0x60");

NvnSamplerDesc *nvn_sampler_host(uint64_t guest) {
    if (guest_mem_validate_range(guest, sizeof(NvnSamplerHeader) + sizeof(NvnSamplerDesc), 8, GMEM_PERM_R) != GMEM_OK)
        return NULL;
    NvnSamplerHeader hdr;
    memcpy(&hdr, (const void *)(uintptr_t)guest, sizeof hdr);
    if (hdr.magic != NVN_MAGIC_SMP_LIVE) return NULL;
    return (NvnSamplerDesc *)((uint8_t *)(uintptr_t)guest + sizeof(NvnSamplerHeader));
}

NVN(nvnSamplerInitialize) {
    uint64_t guest = A(0), builder = A(1);
    if (guest_mem_validate_range(guest, sizeof(NvnSamplerHeader) + sizeof(NvnSamplerDesc), 8, GMEM_PERM_RW) != GMEM_OK ||
        guest_mem_validate_range(builder, 8 + sizeof(NvnSamplerDesc), 8, GMEM_PERM_R) != GMEM_OK) {
        RET(0);
        return;
    }
    NvnSamplerDesc desc;
    memcpy(&desc, (const uint8_t *)(uintptr_t)builder + 8, sizeof desc);
    desc.backend = NULL;
    NvnSamplerHeader hdr = {
        .magic = NVN_MAGIC_SMP_LIVE,
        .debug_id = (uint32_t)alloc_debug_id(),
    };
    memcpy((uint8_t *)(uintptr_t)guest, &hdr, sizeof hdr);
    memcpy((uint8_t *)(uintptr_t)guest + sizeof hdr, &desc, sizeof desc);
    RET(1);
}
NVN(nvnSamplerFinalize) {
    uint64_t guest = A(0);
    if (guest_mem_validate_range(guest, sizeof(NvnSamplerHeader), 8, GMEM_PERM_RW) != GMEM_OK) return;
    uint32_t fin = NVN_MAGIC_SMP_FIN;
    memcpy((uint8_t *)(uintptr_t)guest, &fin, sizeof fin);
}
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP HOST(NvnSamplerDesc, A(0))
NVN(nvnSamplerGetMinMagFilter) { NvnSamplerDesc *s = SMP; if (!s) return; *(uint32_t *)AP(1) = s->min_filter; *(uint32_t *)AP(2) = s->mag_filter; }
NVN(nvnSamplerGetWrapMode) { NvnSamplerDesc *s = SMP; if (!s) return; for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = s->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { NvnSamplerDesc *s = SMP; if (!s) return; *(float *)AP(1) = s->lod_min; *(float *)AP(2) = s->lod_max; }
NVN(nvnSamplerGetLodBias) { NvnSamplerDesc *s = SMP; RETF(s ? s->lod_bias : 0.0f); }
NVN(nvnSamplerGetCompare) { NvnSamplerDesc *s = SMP; if (!s) return; *(uint32_t *)AP(1) = s->compare_mode; *(uint32_t *)AP(2) = s->compare_func; }
NVN(nvnSamplerGetBorderColor) { NvnSamplerDesc *s = SMP; if (!s) return; memcpy(AP(1), s->border, 16); }
NVN(nvnSamplerGetBorderColori) { NvnSamplerDesc *s = SMP; if (!s) return; memcpy(AP(1), s->border, 16); }
NVN(nvnSamplerGetBorderColorui) { NvnSamplerDesc *s = SMP; if (!s) return; memcpy(AP(1), s->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { NvnSamplerDesc *s = SMP; RETF(s ? s->max_aniso : 1.0f); }
NVN(nvnSamplerGetReductionFilter) { NvnSamplerDesc *s = SMP; RET(s ? s->reduction : 0); }
NVN(nvnSamplerCompare) {
    NvnSamplerDesc *a = HOST(NvnSamplerDesc, A(0)), *b = HOST(NvnSamplerDesc, A(1));
    if (!a || !b) { RET(0); return; }
    RET(memcmp(a, b, offsetof(NvnSamplerDesc, backend)) == 0);
}
NVN(nvnSamplerGetDebugID) {
    if (guest_mem_validate_range(A(0), sizeof(NvnSamplerHeader), 8, GMEM_PERM_R) != GMEM_OK) { RET(0); return; }
    NvnSamplerHeader hdr;
    memcpy(&hdr, AP(0), sizeof hdr);
    RET(hdr.magic == NVN_MAGIC_SMP_LIVE ? (uint64_t)hdr.debug_id : 0);
}

// ================================================================== descriptor pools (Phase 5A generational & synchronized)
#define NVN_DPOOL_PAGE_SHIFT 6u
#define NVN_DPOOL_PAGE_SIZE  (1u << NVN_DPOOL_PAGE_SHIFT)
#define NVN_DPOOL_PAGE_MASK  (NVN_DPOOL_PAGE_SIZE - 1u)
#define NVN_DPOOL_MAX_PAGES  64u
#define NVN_DPOOL_MAX_SLOTS  (NVN_DPOOL_PAGE_SIZE * NVN_DPOOL_MAX_PAGES)

typedef struct NvnDescPoolSlot {
    _Atomic uint32_t generation;
    _Atomic uint32_t state;
    NvnDescPool pool;
} NvnDescPoolSlot;

typedef struct RetiredDescEntries {
    void *entries;
    struct RetiredDescEntries *next;
} RetiredDescEntries;

static _Atomic(NvnDescPoolSlot *) g_dpool_pages[NVN_DPOOL_MAX_PAGES];
static _Atomic uint16_t g_dpool_epoch = 1;
static pthread_mutex_t g_dpool_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_dpool_next_fresh = 0;
static RetiredDescEntries *g_dpool_retired = NULL;

static NvnDescPoolSlot *dpool_slot_at(uint32_t idx) {
    if (idx >= NVN_DPOOL_MAX_SLOTS) return NULL;
    NvnDescPoolSlot *page = atomic_load_explicit(&g_dpool_pages[idx >> NVN_DPOOL_PAGE_SHIFT], memory_order_acquire);
    return page ? &page[idx & NVN_DPOOL_PAGE_MASK] : NULL;
}

static bool dpool_read_handle(uint64_t guest, NvnGuestHandle *out) {
    if (guest_mem_read_checked(guest, out, sizeof *out, 8) != GMEM_OK) return false;
    if (out->magic != NVN_MAGIC_DPOOL_LIVE) return false;
    if (out->tag16 != nvn_handle_compute_tag(out, NVN_OBJ_DESC_POOL)) return false;
    if (out->rt_epoch != atomic_load_explicit(&g_dpool_epoch, memory_order_acquire)) return false;
    return true;
}

NvnDescPool *nvn_desc_pool_host(uint64_t guest) {
    NvnGuestHandle h;
    if (!dpool_read_handle(guest, &h)) return NULL;
    NvnDescPoolSlot *s = dpool_slot_at(h.slot_index);
    if (!s) return NULL;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != TEX_SLOT_ALIVE) return NULL;
    if (atomic_load_explicit(&s->generation, memory_order_acquire) != h.generation) return NULL;
    return &s->pool;
}

static void dpool_retire_entries_locked(void *entries) {
    if (!entries) return;
    RetiredDescEntries *node = malloc(sizeof *node);
    if (!node) return;
    node->entries = entries;
    node->next = g_dpool_retired;
    g_dpool_retired = node;
}

RT_ON_RESET(desc_pools_reset) {
    pthread_mutex_lock(&g_dpool_alloc_lock);
    uint16_t next_ep = (uint16_t)(atomic_load_explicit(&g_dpool_epoch, memory_order_relaxed) + 1u);
    if (!next_ep) next_ep = 1u;
    atomic_store_explicit(&g_dpool_epoch, next_ep, memory_order_release);
    for (uint32_t p = 0; p < NVN_DPOOL_MAX_PAGES; p++) {
        NvnDescPoolSlot *page = atomic_exchange_explicit(&g_dpool_pages[p], NULL, memory_order_acq_rel);
        if (page) {
            for (uint32_t i = 0; i < NVN_DPOOL_PAGE_SIZE; i++) {
                if (page[i].pool.entries) {
                    free(page[i].pool.entries);
                    pthread_mutex_destroy(&page[i].pool.write_lock);
                }
            }
            free(page);
        }
    }
    while (g_dpool_retired) {
        RetiredDescEntries *node = g_dpool_retired;
        g_dpool_retired = node->next;
        free(node->entries);
        free(node);
    }
    g_dpool_next_fresh = 0;
    pthread_mutex_unlock(&g_dpool_alloc_lock);
}

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, bool is_sampler) {
    if (count < 0 || guest_mem_validate_range(guest, sizeof(NvnGuestHandle), 8, GMEM_PERM_RW) != GMEM_OK) return NULL;
    NvnPool *mp = HOST(NvnPool, pool);
    if (!mp) return NULL;

    pthread_mutex_lock(&g_dpool_alloc_lock);
    if (g_dpool_next_fresh >= NVN_DPOOL_MAX_SLOTS) {
        pthread_mutex_unlock(&g_dpool_alloc_lock);
        FATAL("NVN descriptor pool slot table exhausted (%u slots)\n", NVN_DPOOL_MAX_SLOTS);
    }
    uint32_t idx = g_dpool_next_fresh++;
    uint32_t p = idx >> NVN_DPOOL_PAGE_SHIFT;
    NvnDescPoolSlot *page = atomic_load_explicit(&g_dpool_pages[p], memory_order_relaxed);
    if (!page) {
        page = calloc(NVN_DPOOL_PAGE_SIZE, sizeof(NvnDescPoolSlot));
        if (!page) {
            pthread_mutex_unlock(&g_dpool_alloc_lock);
            FATAL("NVN descriptor pool page allocation failed\n");
        }
        atomic_store_explicit(&g_dpool_pages[p], page, memory_order_release);
    }
    NvnDescPoolSlot *s = &page[idx & NVN_DPOOL_PAGE_MASK];
    uint32_t gen = atomic_load_explicit(&s->generation, memory_order_relaxed) + 1u;
    if (!gen) gen = 1u;
    atomic_store_explicit(&s->state, TEX_SLOT_FREE, memory_order_release);
    atomic_store_explicit(&s->generation, gen, memory_order_release);
    NvnDescPool *d = &s->pool;
    memset(d, 0, sizeof *d);
    pthread_mutex_init(&d->write_lock, NULL);
    d->addr = mp->storage + off;
    d->guest_pool = pool;
    d->pool_offset = off;
    d->count = count;
    d->is_sampler = is_sampler;
    d->entries = calloc(count ? (size_t)count : 1u, entry);
    if (!d->entries) {
        pthread_mutex_unlock(&g_dpool_alloc_lock);
        FATAL("NVN descriptor pool entries allocation failed\n");
    }
    atomic_store_explicit(&s->state, TEX_SLOT_ALIVE, memory_order_release);
    uint16_t ep = atomic_load_explicit(&g_dpool_epoch, memory_order_relaxed);
    pthread_mutex_unlock(&g_dpool_alloc_lock);

    NvnGuestHandle h = {
        .magic = NVN_MAGIC_DPOOL_LIVE,
        .slot_index = idx,
        .generation = gen,
        .rt_epoch = ep,
        .tag16 = 0,
    };
    h.tag16 = nvn_handle_compute_tag(&h, NVN_OBJ_DESC_POOL);
    guest_mem_write_checked(guest, &h, sizeof h, 8);
    return d;
}

static void desc_pool_finalize(uint64_t guest, uint64_t lr) {
    NvnGuestHandle h;
    if (!dpool_read_handle(guest, &h)) return;
    pthread_mutex_lock(&g_dpool_alloc_lock);
    NvnDescPoolSlot *s = dpool_slot_at(h.slot_index);
    if (s && atomic_load_explicit(&s->generation, memory_order_relaxed) == h.generation &&
        atomic_load_explicit(&s->state, memory_order_relaxed) == TEX_SLOT_ALIVE) {
        atomic_store_explicit(&s->state, TEX_SLOT_RETIRED, memory_order_release);
        // Keep backing entries memory alive until reset (in-flight command lists may still hold NvnDescPool*).
        dpool_retire_entries_locked(s->pool.entries);
        s->pool.entries = NULL;
        s->pool.count = 0;
        pthread_mutex_destroy(&s->pool.write_lock);
    }
    pthread_mutex_unlock(&g_dpool_alloc_lock);

    NvnGuestHandle fin = {
        .magic = NVN_MAGIC_DPOOL_FIN,
        .slot_index = h.slot_index,
        .generation = h.generation,
        .rt_epoch = h.rt_epoch,
        .tag16 = 0,
    };
    fin.tag16 = nvn_handle_compute_tag(&fin, NVN_OBJ_DESC_POOL);
    guest_mem_write_checked(guest, &fin, sizeof fin, 8);
    nvn_diag_record(NVN_EV_FINALIZE, NVN_OBJ_DESC_POOL, 0, guest, h.slot_index, h.generation, h.rt_epoch, 0, lr, false, 0, 0);
}

NVN(nvnTexturePoolInitialize) {
    RET(desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), false) != NULL);
}
NVN(nvnTexturePoolFinalize) { desc_pool_finalize(A(0), c->x[30]); }
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { NvnDescPool *d = HOST(NvnDescPool, A(0)); RET(d ? d->guest_pool : 0); }
NVN(nvnTexturePoolGetMemoryOffset) { NvnDescPool *d = HOST(NvnDescPool, A(0)); RET(d ? d->pool_offset : 0); }
NVN(nvnTexturePoolGetSize) { NvnDescPool *d = HOST(NvnDescPool, A(0)); RET(d ? (uint32_t)d->count : 0); }
static void register_texture(Cpu *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (!d || !d->entries || id < 0 || id >= d->count) return;
    NvnTexLookup lk = nvn_texture_lookup(A(2));
    NvnTexture *t = (lk.state == TEX_ST_LIVE_ORIG || lk.state == TEX_ST_LIVE_COPY || lk.state == TEX_ST_STALE_COPY)
                        ? lk.tex : NULL;
    NvnTextureView view = {0};
    bool has_view = false;
    if (A(3) && guest_mem_read_checked(A(3), &view, sizeof view, 4) == GMEM_OK) has_view = true;

    NvnTexEntryData tmp = {.tex = t, .view = view, .has_view = has_view};
    uint64_t w[4] = {0};
    memcpy(w, &tmp, sizeof tmp);

    pthread_mutex_lock(&d->write_lock);
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    uint32_t seq = atomic_load_explicit(&e->seq, memory_order_relaxed);
    atomic_store_explicit(&e->seq, seq + 1u, memory_order_release);
    for (int k = 0; k < 4; k++) atomic_store_explicit(&e->words[k], w[k], memory_order_relaxed);
    atomic_store_explicit(&e->seq, seq + 2u, memory_order_release);
    pthread_mutex_unlock(&d->write_lock);

    if (t) {
        nvn_diag_record(NVN_EV_DESC_REGISTER, NVN_OBJ_TEXTURE, (uint8_t)lk.state, A(2),
                        lk.hdr.slot_index, lk.hdr.generation, lk.hdr.rt_epoch, (uint16_t)id, c->x[30], false, 0, 0);
    }
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) {
    RET(desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerEntry), true) != NULL);
}
NVN(nvnSamplerPoolFinalize) { desc_pool_finalize(A(0), c->x[30]); }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { NvnDescPool *d = HOST(NvnDescPool, A(0)); RET(d ? d->guest_pool : 0); }
NVN(nvnSamplerPoolGetMemoryOffset) { NvnDescPool *d = HOST(NvnDescPool, A(0)); RET(d ? d->pool_offset : 0); }
NVN(nvnSamplerPoolGetSize) { NvnDescPool *d = HOST(NvnDescPool, A(0)); RET(d ? (uint32_t)d->count : 0); }
static void register_sampler_desc(NvnDescPool *d, int id, const NvnSamplerDesc *src) {
    if (!d || !d->entries || !src || id < 0 || id >= d->count) return;
    NvnSamplerDesc copy = *src;
    copy.backend = NULL;
    enum { NW = sizeof(NvnSamplerDesc) / 8 };
    uint64_t w[NW];
    memcpy(w, &copy, sizeof copy);
    pthread_mutex_lock(&d->write_lock);
    NvnSamplerEntry *e = &((NvnSamplerEntry *)d->entries)[id];
    uint32_t seq = atomic_load_explicit(&e->seq, memory_order_relaxed);
    atomic_store_explicit(&e->seq, seq + 1u, memory_order_release);
    for (int k = 0; k < NW; k++) atomic_store_explicit(&e->words[k], w[k], memory_order_relaxed);
    atomic_store_explicit(&e->seq, seq + 2u, memory_order_release);
    pthread_mutex_unlock(&d->write_lock);
}
NVN(nvnSamplerPoolRegisterSampler) {
    register_sampler_desc(HOST(NvnDescPool, A(0)), (int)A(1), HOST(NvnSamplerDesc, A(2)));
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    if (guest_mem_validate_range(A(2), 8 + sizeof(NvnSamplerDesc), 8, GMEM_PERM_R) != GMEM_OK) return;
    register_sampler_desc(HOST(NvnDescPool, A(0)), (int)A(1), (const NvnSamplerDesc *)((const uint8_t *)AP(2) + 8));
}

// ================================================================== programs (Phase 5A generational & immutable revisions)
// Recorded BindProgram commands keep these descriptors, not the guest object's
// storage. Uploads create immutable revisions, retained until the renderer stops.
#define NVN_PROG_PAGE_SHIFT 9u
#define NVN_PROG_PAGE_SIZE  (1u << NVN_PROG_PAGE_SHIFT)
#define NVN_PROG_PAGE_MASK  (NVN_PROG_PAGE_SIZE - 1u)
#define NVN_PROG_MAX_PAGES  64u
#define NVN_PROG_MAX_SLOTS  (NVN_PROG_PAGE_SIZE * NVN_PROG_MAX_PAGES)

typedef struct NvnProgSlot {
    _Atomic uint32_t generation;
    _Atomic uint32_t state;
    NvnProgram program;
} NvnProgSlot;

static _Atomic(NvnProgSlot *) g_prog_pages[NVN_PROG_MAX_PAGES];
static _Atomic uint16_t g_prog_epoch = 1;
static pthread_mutex_t g_prog_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_prog_next_fresh = 0;

static NvnProgSlot *prog_slot_at(uint32_t idx) {
    if (idx >= NVN_PROG_MAX_SLOTS) return NULL;
    NvnProgSlot *page = atomic_load_explicit(&g_prog_pages[idx >> NVN_PROG_PAGE_SHIFT], memory_order_acquire);
    return page ? &page[idx & NVN_PROG_PAGE_MASK] : NULL;
}

static NvnProgram *program_slot_alloc(NvnGuestHandle *out_hdr) {
    pthread_mutex_lock(&g_prog_alloc_lock);
    if (g_prog_next_fresh >= NVN_PROG_MAX_SLOTS) {
        pthread_mutex_unlock(&g_prog_alloc_lock);
        FATAL("NVN program slot table exhausted (%u slots)\n", NVN_PROG_MAX_SLOTS);
    }
    uint32_t idx = g_prog_next_fresh++;
    uint32_t p = idx >> NVN_PROG_PAGE_SHIFT;
    NvnProgSlot *page = atomic_load_explicit(&g_prog_pages[p], memory_order_relaxed);
    if (!page) {
        page = calloc(NVN_PROG_PAGE_SIZE, sizeof(NvnProgSlot));
        if (!page) {
            pthread_mutex_unlock(&g_prog_alloc_lock);
            FATAL("nvn: program descriptor allocation failed\n");
        }
        atomic_store_explicit(&g_prog_pages[p], page, memory_order_release);
    }
    NvnProgSlot *s = &page[idx & NVN_PROG_PAGE_MASK];
    uint32_t gen = atomic_load_explicit(&s->generation, memory_order_relaxed) + 1u;
    if (!gen) gen = 1u;
    atomic_store_explicit(&s->generation, gen, memory_order_release);
    memset(&s->program, 0, sizeof s->program);
    atomic_store_explicit(&s->state, TEX_SLOT_ALIVE, memory_order_release);
    uint16_t ep = atomic_load_explicit(&g_prog_epoch, memory_order_relaxed);
    pthread_mutex_unlock(&g_prog_alloc_lock);

    NvnGuestHandle h = {
        .magic = NVN_MAGIC_PROG_LIVE,
        .slot_index = idx,
        .generation = gen,
        .rt_epoch = ep,
        .tag16 = 0,
    };
    h.tag16 = nvn_handle_compute_tag(&h, NVN_OBJ_PROGRAM);
    *out_hdr = h;
    return &s->program;
}

NvnProgram *nvn_program_host(uint64_t guest) {
    NvnGuestHandle h;
    if (guest_mem_read_checked(guest, &h, sizeof h, 8) != GMEM_OK) return NULL;
    if (h.magic != NVN_MAGIC_PROG_LIVE) return NULL;
    if (h.tag16 != nvn_handle_compute_tag(&h, NVN_OBJ_PROGRAM)) return NULL;
    if (h.rt_epoch != atomic_load_explicit(&g_prog_epoch, memory_order_acquire)) return NULL;
    NvnProgSlot *s = prog_slot_at(h.slot_index);
    if (!s) return NULL;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != TEX_SLOT_ALIVE) return NULL;
    if (atomic_load_explicit(&s->generation, memory_order_acquire) != h.generation) return NULL;
    return &s->program;
}

RT_ON_RESET(program_identities_reset) {
    pthread_mutex_lock(&g_prog_alloc_lock);
    uint16_t next_ep = (uint16_t)(atomic_load_explicit(&g_prog_epoch, memory_order_relaxed) + 1u);
    if (!next_ep) next_ep = 1u;
    atomic_store_explicit(&g_prog_epoch, next_ep, memory_order_release);
    for (uint32_t p = 0; p < NVN_PROG_MAX_PAGES; p++) {
        NvnProgSlot *page = atomic_exchange_explicit(&g_prog_pages[p], NULL, memory_order_acq_rel);
        free(page);
    }
    g_prog_next_fresh = 0;
    pthread_mutex_unlock(&g_prog_alloc_lock);
}

NVN(nvnProgramInitialize) {
    if (guest_mem_validate_range(A(0), sizeof(NvnGuestHandle), 8, GMEM_PERM_RW) != GMEM_OK) { RET(0); return; }
    NvnGuestHandle h;
    program_slot_alloc(&h);
    guest_mem_write_checked(A(0), &h, sizeof h, 8);
    RET(1);
}
NVN(nvnProgramFinalize) {
    NvnGuestHandle h;
    if (guest_mem_read_checked(A(0), &h, sizeof h, 8) == GMEM_OK && h.magic == NVN_MAGIC_PROG_LIVE) {
        h.magic = NVN_MAGIC_PROG_FIN;
        h.tag16 = nvn_handle_compute_tag(&h, NVN_OBJ_PROGRAM);
        guest_mem_write_checked(A(0), &h, sizeof h, 8);
    }
}
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
static void set_shaders(Cpu *c, int stride) {
    static _Atomic uint64_t revisions;
    int n = (int)A(1);
    if (n < 0 || n > 6 || guest_mem_validate_range(A(0), sizeof(NvnGuestHandle), 8, GMEM_PERM_RW) != GMEM_OK ||
        (n > 0 && guest_mem_validate_range(A(2), (uint64_t)n * (uint64_t)stride, 8, GMEM_PERM_R) != GMEM_OK)) {
        RET(0);
        return;
    }
    uint64_t revision = atomic_fetch_add_explicit(&revisions, 1, memory_order_relaxed) + 1;
    NvnGuestHandle h;
    NvnProgram *p = program_slot_alloc(&h);
    const uint8_t *data = AP(2);  // NVNshaderData[n] = {u64 data (GPU addr), const void* control}
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(const uint64_t *)(data + i * stride);
        uint64_t ctrl_addr = *(const uint64_t *)(data + i * stride + 8);
        if (guest_mem_validate_range(code, 0x34, 4, GMEM_PERM_R) != GMEM_OK ||
            guest_mem_validate_range(ctrl_addr, 0x704, 4, GMEM_PERM_R) != GMEM_OK) {
            RET(0);
            return;
        }
        const uint8_t *control = (const uint8_t *)(uintptr_t)ctrl_addr;
        // stage from the Maxwell shader program header (after the 0x30-byte NVN header)
        uint32_t sph0 = *(const uint32_t *)(uintptr_t)(code + 0x30);
        uint32_t stage;
        switch ((sph0 >> 10) & 0xF) {
        case 1: stage = 0; break;  // vertex
        case 5: stage = 1; break;  // fragment
        case 4: stage = 2; break;  // geometry
        case 2: stage = 3; break;  // tess control
        case 3: stage = 4; break;  // tess eval
        default: stage = 5; break; // compute (no SPH)
        }
        if (*(const uint32_t *)(uintptr_t)code != 0x12345678) stage = 5;
        p->stages[stage].stage = stage;
        p->stages[stage].code_addr = code;
        p->stages[stage].revision = revision;
        p->stages[stage].control = control;
        uint32_t coff = *(const uint32_t *)(control + 0x700);
        uint32_t csz = *(const uint32_t *)(control + 0x6FC);
        p->stages[stage].const_offset = coff;
        p->stages[stage].const_size = csz;
        free(p->stages[stage].const_snap);
        p->stages[stage].const_snap = NULL;
        p->stages[stage].const_snap_size = 0;
        if (csz > 0 && csz <= 4096 &&
            guest_mem_validate_range(code + coff, csz, 1, GMEM_PERM_R) == GMEM_OK) {
            uint8_t *snap = malloc(csz);
            if (snap) {
                memcpy(snap, (const void *)(uintptr_t)(code + coff), csz);
                p->stages[stage].const_snap = snap;
                p->stages[stage].const_snap_size = csz;
            }
        }
        p->stage_mask |= 1u << stage;
    }
    guest_mem_write_checked(A(0), &h, sizeof h, 8);
    RET(1);
}

NVN(nvnProgramSetShaders) { set_shaders(c, 16); }    // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, 24); } // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? ((uint64_t *)(uintptr_t)G(uint64_t, A(0), 0x18))[i] : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    memcpy(w->textures, (void *)(uintptr_t)G(uint64_t, A(1), 0x18), w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { NvnWindow *w = WIN; RET(w ? w->num : 0); }
NVN(nvnWindowGetNumActiveTextures) { NvnWindow *w = WIN; RET(w ? w->active : 0); }
NVN(nvnWindowSetNumActiveTextures) { NvnWindow *w = WIN; if (w) w->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { NvnWindow *w = WIN; RET(w ? w->interval : 1); }
NVN(nvnWindowSetPresentInterval) { NvnWindow *w = WIN; if (w) w->interval = (int)A(1); }
NVN(nvnWindowSetCrop) {
    NvnWindow *w = WIN;
    if (!w) return;
    int *cr = w->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4);
}
NVN(nvnWindowGetCrop) {
    NvnWindow *w = WIN;
    if (w && guest_mem_validate_range(A(1), 16, 4, GMEM_PERM_RW) == GMEM_OK) memcpy(AP(1), w->crop, 16);
}
NVN(nvnWindowAcquireTexture) {
    NvnWindow *w = WIN;
    if (A(1)) {
        NvnSync *sync = nvn_sync_host(A(1));
        if (sync) {
            atomic_store_explicit(&sync->seq, 0, memory_order_release);
            atomic_store_explicit(&sync->signaled, 1, memory_order_release);
        }
    }
    if (!w) { RET(0); return; }
    if (guest_mem_validate_range(A(2), sizeof(int32_t), 4, GMEM_PERM_RW) == GMEM_OK)
        *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    if (!w) { RET(0); return; }
    if (guest_mem_validate_range(A(2), sizeof(int32_t), 4, GMEM_PERM_RW) == GMEM_OK)
        *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
NVN(nvnQueuePresentTexture) {
    if (guest_mem_validate_range(A(1), 8, 8, GMEM_PERM_R) != GMEM_OK) return;
    NvnWindow *w = HOST(NvnWindow, A(1));
    int i = (int)A(2);
    if (!w || i < 0 || i >= w->num) return;
    NvnTexLookup lk = nvn_texture_lookup(w->textures[i]);
    if (lk.state != TEX_ST_LIVE_ORIG && lk.state != TEX_ST_LIVE_COPY) {
        nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_TEXTURE, (uint8_t)lk.state, w->textures[i],
                        lk.hdr.slot_index, lk.hdr.generation, lk.hdr.rt_epoch, 0, c->x[30], false, 0, 0);
        uint64_t cnt = 0;
        if (nvn_diag_should_log(NVN_EV_LOOKUP_FAIL, (uint8_t)lk.state, c->x[30], &cnt)) {
            LOG("[nvn-diag] nvnQueuePresentTexture rejected invalid texture[%d] guest=%llx state=%u lr=%llx\n",
                i, (unsigned long long)w->textures[i], (unsigned)lk.state, (unsigned long long)c->x[30]);
        }
        return;
    }
    NvnTexture *t = lk.tex;
    memcpy(t->window_crop, w->crop, sizeof t->window_crop);
    extern void engage_present_pace(void);
    extern void engage_timing_apply(Cpu *);
    engage_timing_apply(c);
    engage_present_pace();
    backend_present(t);
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1);
    uint32_t m = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> ((int)A(1) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bit = (uint8_t)(1u << A(1));
    G(uint8_t, A(0), 0) = A(2) ? (m | bit) : (m & ~bit);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> A(1)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (d[4] & 0xF0) | (A(2) & 0xF);
    if (face & 2) d[4] = (d[4] & 0x0F) | (uint8_t)((A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int64_t *)AP(2) = v >> 13;
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
