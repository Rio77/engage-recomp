// Centralized guest memory validation with lock-free seqlock region snapshot + TLS fast-path.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "guest_mem.h"
#include "rt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

#define GMEM_MAX_REGIONS 4096

enum {
    GMEM_MODE_PROBE = 0,         // Standalone unit tests (kernel.c not initialized)
    GMEM_MODE_KERNEL = 1,        // Production runtime (regions[] published by kernel.c)
    GMEM_MODE_TEST_REGIONS = 2,  // Unit tests explicitly testing kernel seqlock validation
};

static _Atomic int g_mode = GMEM_MODE_PROBE;
static _Atomic uint64_t g_seq = 0;  // even = stable, odd = writer updating
static GuestRegion g_regions[GMEM_MAX_REGIONS];
static int g_nregions = 0;
static uint64_t g_win_lo = GUEST_LO;
static uint64_t g_win_hi = GUEST_HI;
static pthread_mutex_t g_pub_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint64_t seq;
    uint64_t lo, hi;
    uint32_t perm;
    int mode;
} TlsCache;

static _Thread_local TlsCache g_tls_cache;

const char *guest_mem_status_name(GuestMemStatus st) {
    switch (st) {
    case GMEM_OK: return "OK";
    case GMEM_NULL: return "NULL";
    case GMEM_OUT_OF_WINDOW: return "OUT_OF_WINDOW";
    case GMEM_OVERFLOW: return "OVERFLOW";
    case GMEM_MISALIGNED: return "MISALIGNED";
    case GMEM_UNMAPPED: return "UNMAPPED";
    case GMEM_NO_PERM: return "NO_PERM";
    default: return "UNKNOWN";
    }
}

static void publish_internal(int new_mode, const GuestRegion *regions, int count, uint64_t win_lo, uint64_t win_hi) {
    if (count < 0) count = 0;
    if (count > GMEM_MAX_REGIONS) count = GMEM_MAX_REGIONS;
    pthread_mutex_lock(&g_pub_lock);
    uint64_t s = atomic_load_explicit(&g_seq, memory_order_relaxed);
    atomic_store_explicit(&g_seq, s + 1, memory_order_release);  // odd: writer in progress
    atomic_thread_fence(memory_order_seq_cst);
    if (count > 0 && regions) memcpy(g_regions, regions, (size_t)count * sizeof(GuestRegion));
    g_nregions = count;
    g_win_lo = win_lo;
    g_win_hi = win_hi;
    atomic_store_explicit(&g_mode, new_mode, memory_order_release);
    atomic_store_explicit(&g_seq, s + 2, memory_order_release);  // even: published
    pthread_mutex_unlock(&g_pub_lock);
}

void guest_mem_publish_regions(const GuestRegion *regions, int count) {
    publish_internal(GMEM_MODE_KERNEL, regions, count, GUEST_LO, GUEST_HI);
}

void guest_mem_kernel_reset(void) {
    publish_internal(GMEM_MODE_KERNEL, NULL, 0, GUEST_LO, GUEST_HI);
}

void guest_mem_test_set_regions(const GuestRegion *regions, int count, uint64_t win_lo, uint64_t win_hi) {
    publish_internal(GMEM_MODE_TEST_REGIONS, regions, count, win_lo, win_hi);
}

void guest_mem_test_clear(void) {
    publish_internal(GMEM_MODE_PROBE, NULL, 0, GUEST_LO, GUEST_HI);
}

// Binary search for the first region with r.addr + r.size > addr (sorted by addr).
static GuestMemStatus validate_against_snapshot(uint64_t addr, size_t size, uint32_t req_perm, int mode) {
    uint64_t end = addr + (uint64_t)size;
    for (;;) {
        uint64_t s1 = atomic_load_explicit(&g_seq, memory_order_acquire);
        if (s1 & 1u) continue;  // writer active; retry

        // Fast-path: TLS single-region cache hit
        if (g_tls_cache.seq == s1 && g_tls_cache.mode == mode &&
            addr >= g_tls_cache.lo && end <= g_tls_cache.hi &&
            (g_tls_cache.perm & req_perm) == req_perm) {
            return GMEM_OK;
        }

        uint64_t wlo = g_win_lo, whi = g_win_hi;
        int n = g_nregions;

        GuestMemStatus res = GMEM_OK;
        uint64_t hit_lo = 0, hit_hi = 0;
        uint32_t hit_perm = 0;

        if (addr < wlo || end > whi) {
            res = GMEM_OUT_OF_WINDOW;
        } else if (n <= 0) {
            res = GMEM_UNMAPPED;
        } else {
            int lo = 0, hi = n - 1, idx = n;
            while (lo <= hi) {
                int mid = lo + ((hi - lo) >> 1);
                uint64_t r_end = g_regions[mid].addr + g_regions[mid].size;
                if (r_end > addr) {
                    idx = mid;
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }

            uint64_t pos = addr;
            res = GMEM_UNMAPPED;
            for (int i = idx; i < n && pos < end; i++) {
                uint64_t ra = g_regions[i].addr;
                uint64_t re = ra + g_regions[i].size;
                uint32_t rp = g_regions[i].perm;
                if (ra > pos) {
                    res = GMEM_UNMAPPED;
                    break;
                }
                if ((rp & req_perm) != req_perm) {
                    res = GMEM_NO_PERM;
                    break;
                }
                if (pos == addr && end <= re) {
                    hit_lo = ra;
                    hit_hi = re;
                    hit_perm = rp;
                }
                pos = re;
                if (pos >= end) {
                    res = GMEM_OK;
                    break;
                }
            }
        }

        atomic_thread_fence(memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&g_seq, memory_order_acquire);
        if (s1 == s2) {
            if (res == GMEM_OK && hit_hi > hit_lo) {
                g_tls_cache.seq = s1;
                g_tls_cache.mode = mode;
                g_tls_cache.lo = hit_lo;
                g_tls_cache.hi = hit_hi;
                g_tls_cache.perm = hit_perm;
            }
            return res;
        }
    }
}

#ifndef _WIN32
// Safe non-faulting page readability probe used ONLY in standalone unit tests
// when kernel.c is not linked/initialized (never runs in production gameplay).
static int g_probe_pipe[2] = {-1, -1};
static pthread_mutex_t g_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;

static void probe_init(void) {
    if (pipe(g_probe_pipe) == 0) {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(g_probe_pipe[i], F_GETFL, 0);
            if (fl >= 0) fcntl(g_probe_pipe[i], F_SETFL, fl | O_NONBLOCK);
            fcntl(g_probe_pipe[i], F_SETFD, FD_CLOEXEC);
        }
    }
}

static GuestMemStatus probe_readable_pages(uint64_t addr, size_t size) {
    if (addr < 0x10000ull || addr >= (1ull << 48)) return GMEM_OUT_OF_WINDOW;
    pthread_once(&g_probe_once, probe_init);
    if (g_probe_pipe[1] < 0) return GMEM_OK;

    uint64_t end = addr + (uint64_t)size;
    pthread_mutex_lock(&g_probe_lock);
    for (uint64_t cur = addr; cur < end; cur = (cur & ~4095ull) + 4096ull) {
#ifdef __linux__
        ssize_t r = (ssize_t)syscall(SYS_write, g_probe_pipe[1], (const void *)(uintptr_t)cur, (size_t)1);
#else
        ssize_t r = write(g_probe_pipe[1], (const void *)(uintptr_t)cur, 1);
#endif
        if (r == 1) {
            uint8_t sink;
            ssize_t rr = read(g_probe_pipe[0], &sink, 1);
            (void)rr;
        } else if (errno == EFAULT) {
            pthread_mutex_unlock(&g_probe_lock);
            return GMEM_UNMAPPED;
        }
    }
    pthread_mutex_unlock(&g_probe_lock);
    return GMEM_OK;
}
#else
static GuestMemStatus probe_readable_pages(uint64_t addr, size_t size) {
    (void)size;
    if (addr < 0x10000ull || addr >= (1ull << 48)) return GMEM_OUT_OF_WINDOW;
    return GMEM_OK;
}
#endif

GuestMemStatus guest_mem_validate_range(uint64_t addr, size_t size, size_t align, uint32_t req_perm) {
    if (!addr) return GMEM_NULL;
    if (!size || addr > UINT64_MAX - (uint64_t)size) return GMEM_OVERFLOW;
    if (align > 1 && (addr & ((uint64_t)align - 1u)) != 0) return GMEM_MISALIGNED;

    int mode = atomic_load_explicit(&g_mode, memory_order_acquire);
    if (mode != GMEM_MODE_PROBE) {
        return validate_against_snapshot(addr, size, req_perm, mode);
    }
    return probe_readable_pages(addr, size);
}

GuestMemStatus guest_mem_read_checked(uint64_t addr, void *dst, size_t size, size_t align) {
    GuestMemStatus st = guest_mem_validate_range(addr, size, align, GMEM_PERM_R);
    if (st != GMEM_OK) return st;
    memcpy(dst, (const void *)(uintptr_t)addr, size);
    return GMEM_OK;
}

GuestMemStatus guest_mem_write_checked(uint64_t addr, const void *src, size_t size, size_t align) {
    GuestMemStatus st = guest_mem_validate_range(addr, size, align, GMEM_PERM_RW);
    if (st != GMEM_OK) return st;
    memcpy((void *)(uintptr_t)addr, src, size);
    return GMEM_OK;
}
