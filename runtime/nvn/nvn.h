// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "../guest_mem.h"
#include "../hle.h"
#include "tegra.h"

#define NVN_MAGIC_TEX_LIVE   0x4E565458u  // "NVTX"
#define NVN_MAGIC_TEX_FIN    0x4E565446u  // "NVTF"
#define NVN_MAGIC_POOL_LIVE  0x4E564D50u  // "NVMP"
#define NVN_MAGIC_POOL_FIN   0x4E564D46u  // "NVMF"
#define NVN_MAGIC_SYNC_LIVE  0x4E565359u  // "NVSY"
#define NVN_MAGIC_SMP_LIVE   0x4E56534Du  // "NVSM"
#define NVN_MAGIC_SMP_FIN    0x4E565346u  // "NVSF"
#define NVN_MAGIC_DPOOL_LIVE 0x4E564450u  // "NVDP"
#define NVN_MAGIC_DPOOL_FIN  0x4E564446u  // "NVDF"
#define NVN_MAGIC_PROG_LIVE  0x4E565052u  // "NVPR"
#define NVN_MAGIC_PROG_FIN   0x4E565046u  // "NVPF"

// 16-byte generational handle stored in guest NVN object memory (no raw host pointers).
// Survives Unity value copies (memcpy) and detects corruption, resets, and slot reuse.
typedef struct NvnGuestHandle {
    uint32_t magic;
    uint32_t slot_index;
    uint32_t generation;
    uint16_t rt_epoch;
    uint16_t tag16;
} NvnGuestHandle;
_Static_assert(sizeof(NvnGuestHandle) == 16, "NvnGuestHandle must be 16 bytes");

typedef enum NvnTexState {
    TEX_ST_BAD_ADDR = 0,
    TEX_ST_ZEROED,
    TEX_ST_CORRUPT,
    TEX_ST_STALE_RESET,
    TEX_ST_STALE_RECYCLED,
    TEX_ST_FINALIZED_INST,
    TEX_ST_STALE_COPY,
    TEX_ST_LIVE_COPY,
    TEX_ST_LIVE_ORIG,
} NvnTexState;

typedef struct NvnPool NvnPool;
typedef struct NvnTexture NvnTexture;
typedef struct NvnSamplerDesc NvnSamplerDesc;
typedef struct NvnDescPool NvnDescPool;
typedef struct NvnProgram NvnProgram;
typedef struct NvnSync NvnSync;

typedef struct NvnTexLookup {
    NvnTexState state;
    GuestMemStatus mem_status;
    NvnTexture *tex;
    NvnGuestHandle hdr;
    bool raw_valid;
} NvnTexLookup;

NvnTexLookup nvn_texture_lookup(uint64_t guest);
NvnTexture *nvn_texture_host(uint64_t guest);
NvnPool *nvn_pool_host(uint64_t guest);
NvnSync *nvn_sync_host(uint64_t guest);
NvnSamplerDesc *nvn_sampler_host(uint64_t guest);
NvnDescPool *nvn_desc_pool_host(uint64_t guest);
NvnProgram *nvn_program_host(uint64_t guest);
void nvn_sync_pack_handle(uint64_t guest, uint64_t *out_token, uint64_t *out_epoch);
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
void nvn_sync_set_seq_token(uint64_t guest, uint64_t token, uint64_t epoch, uint64_t seq);

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
#define HOST(T, guest) ((T *)_Generic((T *)0, \
    NvnTexture *: nvn_texture_host, \
    NvnPool *: nvn_pool_host, \
    NvnSync *: nvn_sync_host, \
    NvnSamplerDesc *: nvn_sampler_host, \
    NvnDescPool *: nvn_desc_pool_host, \
    NvnProgram *: nvn_program_host, \
    default: nvn_plain_host)(guest))
static inline void *nvn_plain_host(uint64_t guest) { return (void *)(uintptr_t)*(uint64_t *)(uintptr_t)guest; }
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
};

struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    NvnPool *pool;  // always NULL (Phase 5A: generational pool_slot/pool_gen replaces raw NvnPool*)
    uint32_t pool_slot, pool_gen;
    uint16_t pool_epoch;
    uint64_t pool_size;  // snapshot; pool object can be finalized independently
    uint64_t guest_pool;  // NVNmemoryPool address in guest memory
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    _Atomic bool finalized;  // Phase 7: prevents renderer_upload_rgba from recreating TexBackend after finalize
    uint64_t debug_id;
    char label[64];
    int window_crop[4];  // NVN presentation rectangle in guest texels (zero size: full image)
};

struct NvnSamplerDesc {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
};

#define NVN_FITS(T, N) _Static_assert(sizeof(T) <= (N), #T " exceeds NVN ABI size " #N)

typedef struct {
    uint8_t setmask;    // bit 0: levels, 1: layers, 2: format, 3: swizzle, 4: ds_mode, 5: target
    uint8_t base_level;
    uint8_t num_levels;
    uint8_t target;
    uint16_t min_layer;
    uint16_t num_layers;
    uint16_t format;
    uint8_t ds_mode;
    uint8_t _pad;
    uint8_t swizzle[4];
} NvnTextureView;  // Compact 16-byte layout matching NVN 55.0.0 ABI (Engage 2.0.0)
NVN_FITS(NvnTextureView, 0x10);
NVN_FITS(NvnGuestHandle, 0x10);
NVN_FITS(NvnSamplerDesc, 0x58);

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
} NvnTexEntryData;
_Static_assert(sizeof(NvnTexEntryData) <= 32, "NvnTexEntryData must fit in 4 x u64");

typedef struct {
    _Atomic uint32_t seq;  // per-entry seqlock (odd = writer active)
    union {
        struct {
            NvnTexture *tex;
            NvnTextureView view;
            bool has_view;
        };
        _Atomic uint64_t words[4];
    };
} NvnTexEntry;

_Static_assert(sizeof(NvnSamplerDesc) % 8 == 0, "NvnSamplerDesc must be 8-byte multiple");
typedef struct {
    _Atomic uint32_t seq;  // per-entry seqlock (odd = writer active)
    union {
        NvnSamplerDesc desc;
        _Atomic uint64_t words[sizeof(NvnSamplerDesc) / 8];
    };
} NvnSamplerEntry;

struct NvnDescPool {
    uint64_t addr;
    uint64_t guest_pool;
    uint64_t pool_offset;
    int count;
    bool is_sampler;
    pthread_mutex_t write_lock;
    void *entries;  // NvnTexEntry[] or NvnSamplerEntry[]
};

static inline bool nvn_tex_pool_read(const NvnDescPool *d, uint32_t id, NvnTexEntry *out) {
    if (!d || !d->entries || id >= (uint32_t)d->count) return false;
    const NvnTexEntry *src = &((const NvnTexEntry *)d->entries)[id];
    for (int spins = 0; spins < 100000; spins++) {
        uint32_t s1 = atomic_load_explicit(&src->seq, memory_order_acquire);
        if (s1 & 1u) continue;
        uint64_t w[4];
        for (int k = 0; k < 4; k++) w[k] = atomic_load_explicit(&src->words[k], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&src->seq, memory_order_acquire);
        if (s1 == s2) {
            NvnTexEntryData tmp;
            memcpy(&tmp, w, sizeof tmp);
            out->tex = tmp.tex;
            out->view = tmp.view;
            out->has_view = tmp.has_view;
            return out->tex != NULL;
        }
    }
    return false;
}

static inline bool nvn_sampler_pool_read(const NvnDescPool *d, uint32_t id, NvnSamplerDesc *out) {
    if (!d || !d->entries || id >= (uint32_t)d->count) return false;
    const NvnSamplerEntry *src = &((const NvnSamplerEntry *)d->entries)[id];
    enum { NW = sizeof(NvnSamplerDesc) / 8 };
    for (int spins = 0; spins < 100000; spins++) {
        uint32_t s1 = atomic_load_explicit(&src->seq, memory_order_acquire);
        if (s1 & 1u) continue;
        uint64_t w[NW];
        for (int k = 0; k < NW; k++) w[k] = atomic_load_explicit(&src->words[k], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&src->seq, memory_order_acquire);
        if (s1 == s2) {
            memcpy(out, w, sizeof *out);
            return true;
        }
    }
    return false;
}

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    uint64_t revision;        // new shader upload, even when its address is reused
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    uint8_t *const_snap;      // host snapshot of compiler constants (c1) taken at SetShaders time
    uint32_t const_snap_size;
    void *backend;
} NvnShaderStage;

struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
};

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

struct NvnSync {
    _Atomic int signaled;
    _Atomic uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
};

// ---- command recording (host storage; guest memory supplies allocation budgets)
typedef struct {
    uint64_t mem;    // current guest command-memory budget (never written)
    uint64_t size;   // chunk size
    uint64_t used;   // bytes used in chunk
    uint64_t callback, callback_data;
    bool recording;
    uint64_t guest;  // the guest NVNcommandBuffer*
    uint8_t *records;
    uint64_t records_used, records_cap;
    uint64_t control, control_size, control_used;
    uint64_t copy_row_stride, copy_image_stride;
} NvnCmdBuf;

const void *nvn_command_acquire(uint64_t handle, uint64_t *size);
void nvn_command_release(uint64_t handle);
void nvn_command_finalize(uint64_t handle);

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    CMD_BIND_SEPARATE_TEXTURES, CMD_BIND_SEPARATE_SAMPLERS,
    CMD_BIND_TEXTURES_DEFERRED, CMD_BIND_IMAGES_DEFERRED,
    CMD_BIND_SEPARATE_TEXTURES_DEFERRED, CMD_BIND_SEPARATE_SAMPLERS_DEFERRED,
    CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
