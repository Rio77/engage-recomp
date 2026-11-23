// Low-cost circular diagnostic ring buffer for NVN lifecycle & memory events (Phase 3).
// Fixed 256 KB footprint (4096 x 64-byte events), zero unbounded log growth, deduplicated alerts.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NvnDiagEventKind {
    NVN_EV_NONE = 0,
    NVN_EV_INIT,
    NVN_EV_REINIT_OVER_LIVE,
    NVN_EV_FINALIZE,
    NVN_EV_DOUBLE_FINALIZE,
    NVN_EV_COPY_SEEN,
    NVN_EV_DESC_REGISTER,
    NVN_EV_BACKEND_DESTROY,
    NVN_EV_MOVIE_UPLOAD,
    NVN_EV_LOOKUP_FAIL,
    NVN_EV_RESET,
} NvnDiagEventKind;

typedef enum NvnDiagObjKind {
    NVN_OBJ_NONE = 0,
    NVN_OBJ_TEXTURE = 1,
    NVN_OBJ_POOL = 2,
    NVN_OBJ_SYNC = 3,
    NVN_OBJ_SAMPLER = 4,
    NVN_OBJ_DESC_POOL = 5,
    NVN_OBJ_CMDBUF = 6,
    NVN_OBJ_PROGRAM = 7,
} NvnDiagObjKind;

typedef struct NvnDiagEvent {
    uint64_t seq;
    uint64_t guest_addr;
    uint64_t caller_lr;
    uint64_t raw_q0;
    uint64_t raw_q1;
    uint32_t slot_index;
    uint32_t generation;
    uint32_t tid;
    uint16_t rt_epoch;
    uint8_t event_kind;
    uint8_t obj_kind;
    uint8_t state_code;
    uint8_t flags;       // bit 0: raw_q0/q1 valid
    uint16_t desc_index; // optional descriptor pool index
} NvnDiagEvent;

_Static_assert(sizeof(NvnDiagEvent) == 64, "NvnDiagEvent must be 64 bytes");

#define NVN_DIAG_FLAG_RAW_VALID 1u

void nvn_diag_record(NvnDiagEventKind ev, NvnDiagObjKind obj, uint8_t state_code,
                     uint64_t guest_addr, uint32_t slot_index, uint32_t generation,
                     uint16_t rt_epoch, uint16_t desc_index, uint64_t caller_lr,
                     bool raw_valid, uint64_t raw_q0, uint64_t raw_q1);

// Scans the ring buffer backwards and prints chronological history for guest_addr or (slot_index, generation).
void nvn_diag_dump_for_addr(const char *reason, uint64_t guest_addr, uint32_t slot_index,
                            uint32_t generation, uint64_t caller_lr);

// Rate-limited deduplicated warning/error emission (returns true if emitted).
bool nvn_diag_should_log(uint8_t event_kind, uint8_t state_code, uint64_t caller_lr, uint64_t *out_count);

// Introspection helpers for unit tests (tools/test_nvn_diag.c).
uint64_t nvn_diag_total_events(void);
bool nvn_diag_find_last(uint64_t guest_addr, NvnDiagEventKind kind, NvnDiagEvent *out);
void nvn_diag_reset(void);

// Lightweight 64-entry pre-failure telemetry ring (Zero I/O in hot path; dumped only on first TEX_ST_CORRUPT).
typedef enum NvnTraceKind {
    NVN_TR_NONE = 0,
    NVN_TR_TEX_FLAGS = 1,
    NVN_TR_TEX_TARGET = 2,
    NVN_TR_TEX_STORAGE_SIZE = 3,
    NVN_TR_TEX_STORAGE_ALIGN = 4,
    NVN_TR_POOL_INIT_FAIL = 5,
    NVN_TR_SVC_PHYS_MAP = 6,
} NvnTraceKind;

void nvn_trace_record(NvnTraceKind kind, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t lr);
void nvn_trace_dump(const char *reason);

#ifdef __cplusplus
}
#endif
