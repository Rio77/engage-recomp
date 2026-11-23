// Centralized guest memory validation (Phase 3).
// Distinguishes:
//   1. Reserved virtual window [GUEST_LO, GUEST_HI)
//   2. Logically mapped regions in kernel.c (via lock-free seqlock snapshot)
//   3. Accessibility & permissions (R/W/X) without any unsafe dereference
//   4. Alignment and arithmetic overflow
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GuestMemStatus {
    GMEM_OK = 0,
    GMEM_NULL,
    GMEM_OUT_OF_WINDOW,
    GMEM_OVERFLOW,
    GMEM_MISALIGNED,
    GMEM_UNMAPPED,  // Inside window, but not mapped in kernel.c regions[]
    GMEM_NO_PERM,   // Mapped in kernel.c, but lacks requested R/W/X permission
} GuestMemStatus;

enum {
    GMEM_PERM_R = 1u,
    GMEM_PERM_W = 2u,
    GMEM_PERM_X = 4u,
    GMEM_PERM_RW = 3u,
};

typedef struct GuestRegion {
    uint64_t addr, size;
    uint32_t type, perm;
} GuestRegion;

const char *guest_mem_status_name(GuestMemStatus st);

// Published by runtime/kernel.c under region_lock whenever regions[] changes.
void guest_mem_publish_regions(const GuestRegion *regions, int count);
void guest_mem_kernel_reset(void);

// Explicit region table mode for unit tests (tests exact same seqlock + TLS path as kernel.c).
void guest_mem_test_set_regions(const GuestRegion *regions, int count, uint64_t win_lo, uint64_t win_hi);
void guest_mem_test_clear(void);

// Validates [addr, addr + size) against levels 1-3 without dereferencing addr.
GuestMemStatus guest_mem_validate_range(uint64_t addr, size_t size, size_t align, uint32_t req_perm);

// Safe checked read/write wrappers (never dereference addr unless validation returns GMEM_OK).
GuestMemStatus guest_mem_read_checked(uint64_t addr, void *dst, size_t size, size_t align);
GuestMemStatus guest_mem_write_checked(uint64_t addr, const void *src, size_t size, size_t align);

#ifdef __cplusplus
}
#endif
