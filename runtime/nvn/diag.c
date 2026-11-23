// Circular NVN diagnostic ring buffer + deduplicated reporting (Phase 3).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "diag.h"
#include "../rt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define NVN_DIAG_CAP 4096u
#define NVN_DIAG_MASK (NVN_DIAG_CAP - 1u)
#define NVN_DEDUP_CAP 64

enum {
    DIAG_MODE_RING = 0,    // default: record in ring buffer, dump on failure/dedup warnings
    DIAG_MODE_OFF = 1,     // ENGAGE_NVN_DIAG=off
    DIAG_MODE_SUMMARY = 2, // ENGAGE_NVN_DIAG=summary
    DIAG_MODE_TRACE = 3,   // ENGAGE_NVN_DIAG=trace or ENGAGE_TRACE_TEXTURES=1
};

static NvnDiagEvent g_ring[NVN_DIAG_CAP];
static _Atomic uint64_t g_seq = 0;

typedef struct {
    uint64_t caller_lr;
    uint64_t count;
    uint8_t event_kind;
    uint8_t state_code;
    bool used;
} DedupEnt;

static DedupEnt g_dedup[NVN_DEDUP_CAP];
static pthread_mutex_t g_dedup_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_lines_logged = 0;

static int diag_mode(void) {
    const char *e = getenv("ENGAGE_NVN_DIAG");
    if (e && *e) {
        if (!strcmp(e, "off")) return DIAG_MODE_OFF;
        if (!strcmp(e, "summary")) return DIAG_MODE_SUMMARY;
        if (!strcmp(e, "trace")) return DIAG_MODE_TRACE;
        return DIAG_MODE_RING;
    }
    const char *legacy = getenv("ENGAGE_TRACE_TEXTURES");
    if (legacy && *legacy && strcmp(legacy, "0") != 0) return DIAG_MODE_TRACE;
    return DIAG_MODE_RING;
}

static uint64_t diag_line_limit(void) {
    const char *e = getenv("ENGAGE_NVN_DIAG_LIMIT");
    if (e && *e) {
        long v = strtol(e, NULL, 10);
        if (v > 0) return (uint64_t)v;
    }
    return 500;
}

static uint32_t current_tid(void) {
#if defined(__linux__)
    return (uint32_t)syscall(SYS_gettid);
#else
    return (uint32_t)(uintptr_t)pthread_self();
#endif
}

static const char *ev_name(uint8_t k) {
    switch (k) {
    case NVN_EV_INIT: return "INIT";
    case NVN_EV_REINIT_OVER_LIVE: return "REINIT_OVER_LIVE";
    case NVN_EV_FINALIZE: return "FINALIZE";
    case NVN_EV_DOUBLE_FINALIZE: return "DOUBLE_FINALIZE";
    case NVN_EV_COPY_SEEN: return "COPY_SEEN";
    case NVN_EV_DESC_REGISTER: return "DESC_REGISTER";
    case NVN_EV_BACKEND_DESTROY: return "BACKEND_DESTROY";
    case NVN_EV_MOVIE_UPLOAD: return "MOVIE_UPLOAD";
    case NVN_EV_LOOKUP_FAIL: return "LOOKUP_FAIL";
    case NVN_EV_RESET: return "RESET";
    default: return "NONE";
    }
}

static const char *obj_name(uint8_t k) {
    switch (k) {
    case NVN_OBJ_TEXTURE: return "TEX";
    case NVN_OBJ_POOL: return "POOL";
    case NVN_OBJ_SYNC: return "SYNC";
    case NVN_OBJ_SAMPLER: return "SMP";
    case NVN_OBJ_DESC_POOL: return "DPOOL";
    case NVN_OBJ_CMDBUF: return "CMD";
    case NVN_OBJ_PROGRAM: return "PROG";
    default: return "?";
    }
}

bool nvn_diag_should_log(uint8_t event_kind, uint8_t state_code, uint64_t caller_lr, uint64_t *out_count) {
    if (diag_mode() == DIAG_MODE_OFF) return false;
    uint64_t count = 1;
    pthread_mutex_lock(&g_dedup_lock);
    int free_idx = -1, hit = -1;
    for (int i = 0; i < NVN_DEDUP_CAP; i++) {
        if (g_dedup[i].used && g_dedup[i].event_kind == event_kind &&
            g_dedup[i].state_code == state_code && g_dedup[i].caller_lr == caller_lr) {
            hit = i;
            break;
        }
        if (!g_dedup[i].used && free_idx < 0) free_idx = i;
    }
    if (hit >= 0) {
        count = ++g_dedup[hit].count;
    } else if (free_idx >= 0) {
        g_dedup[free_idx] = (DedupEnt){
            .caller_lr = caller_lr, .count = 1, .event_kind = event_kind, .state_code = state_code, .used = true};
        count = 1;
    }
    pthread_mutex_unlock(&g_dedup_lock);
    if (out_count) *out_count = count;

    bool power_of_10 = (count <= 3 || count == 10 || count == 100 || count == 1000 || (count % 10000) == 0);
    if (!power_of_10) return false;
    uint64_t lim = diag_line_limit();
    if (atomic_fetch_add_explicit(&g_lines_logged, 1, memory_order_relaxed) >= lim) return false;
    return true;
}

void nvn_diag_record(NvnDiagEventKind ev, NvnDiagObjKind obj, uint8_t state_code,
                     uint64_t guest_addr, uint32_t slot_index, uint32_t generation,
                     uint16_t rt_epoch, uint16_t desc_index, uint64_t caller_lr,
                     bool raw_valid, uint64_t raw_q0, uint64_t raw_q1) {
    int mode = diag_mode();
    if (mode == DIAG_MODE_OFF) return;

    uint64_t seq = atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed) + 1;
    NvnDiagEvent *e = &g_ring[seq & NVN_DIAG_MASK];
    e->guest_addr = guest_addr;
    e->caller_lr = caller_lr;
    e->raw_q0 = raw_valid ? raw_q0 : 0;
    e->raw_q1 = raw_valid ? raw_q1 : 0;
    e->slot_index = slot_index;
    e->generation = generation;
    e->tid = current_tid();
    e->rt_epoch = rt_epoch;
    e->event_kind = (uint8_t)ev;
    e->obj_kind = (uint8_t)obj;
    e->state_code = state_code;
    e->flags = raw_valid ? NVN_DIAG_FLAG_RAW_VALID : 0;
    e->desc_index = desc_index;
    atomic_store_explicit((_Atomic uint64_t *)&e->seq, seq, memory_order_release);

    if (mode == DIAG_MODE_TRACE) {
        if (atomic_fetch_add_explicit(&g_lines_logged, 1, memory_order_relaxed) < diag_line_limit()) {
            LOG("[nvn-diag] #%llu %s.%s st=%u guest=%llx slot=%u gen=%u ep=%u tid=%u lr=%llx\n",
                (unsigned long long)seq, obj_name((uint8_t)obj), ev_name((uint8_t)ev), state_code,
                (unsigned long long)guest_addr, slot_index, generation, rt_epoch, e->tid,
                (unsigned long long)caller_lr);
        }
    }
}

void nvn_diag_dump_for_addr(const char *reason, uint64_t guest_addr, uint32_t slot_index,
                            uint32_t generation, uint64_t caller_lr) {
    if (diag_mode() == DIAG_MODE_OFF) return;
    uint64_t cur = atomic_load_explicit(&g_seq, memory_order_acquire);
    uint64_t oldest = cur > NVN_DIAG_CAP ? cur - NVN_DIAG_CAP + 1 : 1;

    NvnDiagEvent matches[16];
    int nmatch = 0;
    for (uint64_t s = cur; s >= oldest && nmatch < 16; s--) {
        NvnDiagEvent snap = g_ring[s & NVN_DIAG_MASK];
        uint64_t eseq = atomic_load_explicit((_Atomic uint64_t *)&g_ring[s & NVN_DIAG_MASK].seq, memory_order_acquire);
        if (eseq != s) continue;  // overwritten while reading
        bool addr_match = (guest_addr != 0 && snap.guest_addr == guest_addr);
        bool slot_match = (generation != 0 && snap.slot_index == slot_index && snap.generation == generation);
        if (addr_match || slot_match) matches[nmatch++] = snap;
        if (s == 1) break;
    }

    LOG("[nvn-diag] === dump (%s): guest=%llx slot=%u gen=%u lr=%llx (%d recent event(s)) ===\n",
        reason ? reason : "?", (unsigned long long)guest_addr, slot_index, generation,
        (unsigned long long)caller_lr, nmatch);
    for (int i = nmatch - 1; i >= 0; i--) {
        const NvnDiagEvent *e = &matches[i];
        LOG("[nvn-diag]   #%llu %s.%s st=%u guest=%llx slot=%u gen=%u ep=%u desc=%u tid=%u lr=%llx",
            (unsigned long long)e->seq, obj_name(e->obj_kind), ev_name(e->event_kind), e->state_code,
            (unsigned long long)e->guest_addr, e->slot_index, e->generation, e->rt_epoch,
            e->desc_index, e->tid, (unsigned long long)e->caller_lr);
        if (e->flags & NVN_DIAG_FLAG_RAW_VALID) {
            LOG(" raw=[%016llx %016llx]", (unsigned long long)e->raw_q0, (unsigned long long)e->raw_q1);
        }
        LOG("\n");
    }
}

uint64_t nvn_diag_total_events(void) {
    return atomic_load_explicit(&g_seq, memory_order_acquire);
}

bool nvn_diag_find_last(uint64_t guest_addr, NvnDiagEventKind kind, NvnDiagEvent *out) {
    uint64_t cur = atomic_load_explicit(&g_seq, memory_order_acquire);
    if (!cur) return false;
    uint64_t oldest = cur > NVN_DIAG_CAP ? cur - NVN_DIAG_CAP + 1 : 1;
    for (uint64_t s = cur; s >= oldest; s--) {
        NvnDiagEvent snap = g_ring[s & NVN_DIAG_MASK];
        if (snap.seq != s) continue;
        if ((guest_addr == 0 || snap.guest_addr == guest_addr) &&
            (kind == NVN_EV_NONE || snap.event_kind == (uint8_t)kind)) {
            if (out) *out = snap;
            return true;
        }
        if (s == 1) break;
    }
    return false;
}

void nvn_diag_reset(void) {
    pthread_mutex_lock(&g_dedup_lock);
    memset(g_ring, 0, sizeof g_ring);
    memset(g_dedup, 0, sizeof g_dedup);
    atomic_store_explicit(&g_seq, 0, memory_order_release);
    atomic_store_explicit(&g_lines_logged, 0, memory_order_release);
    pthread_mutex_unlock(&g_dedup_lock);
}

#define NVN_TRACE_CAP 64u
#define NVN_TRACE_MASK (NVN_TRACE_CAP - 1u)

typedef struct {
    _Atomic uint64_t seq;
    uint64_t a0, a1, a2, a3, lr;
    uint32_t tid;
    uint8_t kind;
} NvnTraceEvent;

static NvnTraceEvent g_trace[NVN_TRACE_CAP];
static _Atomic uint64_t g_trace_seq = 0;

static const char *trace_name(uint8_t k) {
    switch (k) {
    case NVN_TR_TEX_FLAGS: return "TEX.SET_FLAGS";
    case NVN_TR_TEX_TARGET: return "TEX.SET_TARGET";
    case NVN_TR_TEX_STORAGE_SIZE: return "TEX.GET_SIZE";
    case NVN_TR_TEX_STORAGE_ALIGN: return "TEX.GET_ALIGN";
    case NVN_TR_POOL_INIT_FAIL: return "POOL.INIT_FAIL";
    case NVN_TR_SVC_PHYS_MAP: return "SVC.PHYS_MAP";
    default: return "?";
    }
}

void nvn_trace_record(NvnTraceKind kind, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t lr) {
    if (diag_mode() == DIAG_MODE_OFF) return;
    uint64_t seq = atomic_fetch_add_explicit(&g_trace_seq, 1, memory_order_relaxed) + 1;
    NvnTraceEvent *e = &g_trace[seq & NVN_TRACE_MASK];
    e->a0 = a0;
    e->a1 = a1;
    e->a2 = a2;
    e->a3 = a3;
    e->lr = lr;
    e->tid = current_tid();
    e->kind = (uint8_t)kind;
    atomic_store_explicit(&e->seq, seq, memory_order_release);
}

void nvn_trace_dump(const char *reason) {
    if (diag_mode() == DIAG_MODE_OFF) return;
    uint64_t cur = atomic_load_explicit(&g_trace_seq, memory_order_acquire);
    if (!cur) return;
    uint64_t oldest = cur > NVN_TRACE_CAP ? cur - NVN_TRACE_CAP + 1 : 1;
    LOG("[nvn-trace] === last %llu RT/pool trace events (%s) ===\n",
        (unsigned long long)(cur - oldest + 1), reason ? reason : "?");
    for (uint64_t s = oldest; s <= cur; s++) {
        const NvnTraceEvent *slot = &g_trace[s & NVN_TRACE_MASK];
        NvnTraceEvent snap = *slot;
        if (atomic_load_explicit(&slot->seq, memory_order_acquire) != s) continue;
        LOG("[nvn-trace]   #%llu %-14s tid=%u lr=%llx a0=%llx a1=%llx a2=%llx a3=%llx\n",
            (unsigned long long)s, trace_name(snap.kind), snap.tid, (unsigned long long)snap.lr,
            (unsigned long long)snap.a0, (unsigned long long)snap.a1,
            (unsigned long long)snap.a2, (unsigned long long)snap.a3);
    }
}
