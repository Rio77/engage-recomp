// Standalone Linux probe for Phase 5B: tests userfaultfd(UFFD_USER_MODE_ONLY) + UFFD_FEATURE_WP_ASYNC
// + /proc/self/pagemap PAGEMAP_SCAN write tracking on anonymous memory (the exact backing of guest memory).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef PAGEMAP_SCAN
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WPALLOWED (1ULL << 0)
#define PAGE_IS_WRITTEN   (1ULL << 1)
#define PAGE_IS_FILE      (1ULL << 2)
#define PAGE_IS_PRESENT   (1ULL << 3)
#define PAGE_IS_SWAPPED   (1ULL << 4)
#define PAGE_IS_PFNZERO   (1ULL << 5)
#define PAGE_IS_HUGE      (1ULL << 6)
#define PAGE_IS_SOFT_DIRTY (1ULL << 7)
#define PM_SCAN_WP_MATCHING   (1U << 0)
#define PM_SCAN_CHECK_WPASYNC (1U << 1)
struct page_region {
    uint64_t start;
    uint64_t end;
    uint64_t categories;
};
struct pm_scan_arg {
    uint64_t size;
    uint64_t flags;
    uint64_t start;
    uint64_t end;
    uint64_t walk_end;
    uint64_t vec;
    uint64_t vec_len;
    uint64_t max_pages;
    uint64_t category_inverted;
    uint64_t category_mask;
    uint64_t category_anyof_mask;
    uint64_t return_mask;
};
#endif

static int open_uffd_wp_async(void) {
    int uffd = (int)syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (uffd < 0) {
        fprintf(stderr, "userfaultfd(USER_MODE_ONLY) failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }
    struct uffdio_api api = {0};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC;
    if (ioctl(uffd, UFFDIO_API, &api) != 0) {
        fprintf(stderr, "UFFDIO_API(WP_ASYNC) failed: %s (errno=%d)\n", strerror(errno), errno);
        close(uffd);
        return -1;
    }
    if (!(api.features & UFFD_FEATURE_WP_ASYNC)) {
        fprintf(stderr, "UFFD_FEATURE_WP_ASYNC not supported (features=%#llx)\n", (unsigned long long)api.features);
        close(uffd);
        return -1;
    }
    printf("OK: userfaultfd(USER_MODE_ONLY) + UFFD_FEATURE_WP_ASYNC supported (features=%#llx)\n",
           (unsigned long long)api.features);
    return uffd;
}

int main(void) {
    long pgsz = sysconf(_SC_PAGESIZE);
    size_t len = (size_t)pgsz * 64;
    uint8_t *buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 1; }
    memset(buf, 0, len);  // populate pages

    int uffd = open_uffd_wp_async();
    if (uffd < 0) return 2;

    struct uffdio_register reg = {0};
    reg.range.start = (uint64_t)(uintptr_t)buf;
    reg.range.len = len;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) != 0) {
        fprintf(stderr, "UFFDIO_REGISTER_MODE_WP failed: %s\n", strerror(errno));
        return 3;
    }
    struct uffdio_writeprotect wp = {0};
    wp.range.start = (uint64_t)(uintptr_t)buf;
    wp.range.len = len;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) != 0) {
        fprintf(stderr, "UFFDIO_WRITEPROTECT initial WP failed: %s\n", strerror(errno));
        return 4;
    }

    int pmfd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pmfd < 0) { perror("open /proc/self/pagemap"); return 5; }

    // Touch 3 specific pages: 3, 17, 63
    buf[3 * pgsz + 12] = 0xAA;
    buf[17 * pgsz + 0] = 0xBB;
    buf[63 * pgsz + pgsz - 1] = 0xCC;

    struct page_region regs[64];
    struct pm_scan_arg arg = {0};
    arg.size = sizeof arg;
    arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
    arg.start = (uint64_t)(uintptr_t)buf;
    arg.end = (uint64_t)(uintptr_t)(buf + len);
    arg.vec = (uint64_t)(uintptr_t)regs;
    arg.vec_len = 64;
    arg.category_mask = PAGE_IS_WRITTEN;
    arg.return_mask = PAGE_IS_WRITTEN;

    int n = ioctl(pmfd, PAGEMAP_SCAN, &arg);
    if (n < 0) {
        fprintf(stderr, "PAGEMAP_SCAN failed: %s (errno=%d)\n", strerror(errno), errno);
        return 6;
    }
    printf("PASS 1: PAGEMAP_SCAN returned %d regions (walk_end=%#llx)\n", n, (unsigned long long)arg.walk_end);
    int found_3 = 0, found_17 = 0, found_63 = 0, total_pages = 0;
    for (int i = 0; i < n; i++) {
        uint64_t p0 = (regs[i].start - (uint64_t)(uintptr_t)buf) / (uint64_t)pgsz;
        uint64_t p1 = (regs[i].end - (uint64_t)(uintptr_t)buf) / (uint64_t)pgsz;
        printf("  region[%d]: pages [%llu..%llu) cat=%#llx\n", i, (unsigned long long)p0, (unsigned long long)p1,
               (unsigned long long)regs[i].categories);
        for (uint64_t p = p0; p < p1; p++) {
            total_pages++;
            if (p == 3) found_3 = 1;
            if (p == 17) found_17 = 1;
            if (p == 63) found_63 = 1;
        }
    }
    if (total_pages != 3 || !found_3 || !found_17 || !found_63) {
        fprintf(stderr, "FAIL: expected exactly pages 3, 17, 63 (got total=%d)\n", total_pages);
        return 7;
    }

    // Second scan without writes must return 0 (PM_SCAN_WP_MATCHING re-armed WP atomically!)
    n = ioctl(pmfd, PAGEMAP_SCAN, &arg);
    if (n != 0) {
        fprintf(stderr, "FAIL: second scan expected 0, got %d\n", n);
        return 8;
    }

    // Write page 17 again -> must detect page 17 only
    buf[17 * pgsz + 5] = 0xDD;
    n = ioctl(pmfd, PAGEMAP_SCAN, &arg);
    if (n != 1 || regs[0].start != (uint64_t)(uintptr_t)(buf + 17 * pgsz) ||
        regs[0].end != (uint64_t)(uintptr_t)(buf + 18 * pgsz)) {
        fprintf(stderr, "FAIL: third scan expected page 17 only, got n=%d\n", n);
        return 9;
    }

    // Verify madvise(MADV_DONTNEED) + rewrite still tracks writes cleanly (simulates Unmap/MapPhysicalMemory)
    if (madvise(buf + 10 * pgsz, (size_t)pgsz * 4, MADV_DONTNEED) != 0) {
        perror("madvise");
        return 10;
    }
    buf[11 * pgsz] = 0xEE;
    n = ioctl(pmfd, PAGEMAP_SCAN, &arg);
    printf("PASS 2 (post-MADV_DONTNEED): PAGEMAP_SCAN returned %d region(s)\n", n);
    int found_11 = 0;
    for (int i = 0; i < n; i++) {
        uint64_t p0 = (regs[i].start - (uint64_t)(uintptr_t)buf) / (uint64_t)pgsz;
        uint64_t p1 = (regs[i].end - (uint64_t)(uintptr_t)buf) / (uint64_t)pgsz;
        printf("  post-dontneed region[%d]: pages [%llu..%llu) cat=%#llx\n", i, (unsigned long long)p0, (unsigned long long)p1,
               (unsigned long long)regs[i].categories);
        if (p0 <= 11 && 11 < p1) found_11 = 1;
    }
    if (!found_11) {
        fprintf(stderr, "FAIL: post-MADV_DONTNEED write to page 11 not tracked (n=%d)\n", n);
        return 11;
    }
    // After PM_SCAN_WP_MATCHING re-armed WP, next scan with no writes must be 0, and writing page 11 must detect page 11
    n = ioctl(pmfd, PAGEMAP_SCAN, &arg);
    if (n != 0) {
        fprintf(stderr, "FAIL: post-dontneed re-arm expected 0, got %d\n", n);
        return 12;
    }
    buf[11 * pgsz] = 0xEF;
    n = ioctl(pmfd, PAGEMAP_SCAN, &arg);
    if (n != 1 || regs[0].start != (uint64_t)(uintptr_t)(buf + 11 * pgsz) ||
        regs[0].end != (uint64_t)(uintptr_t)(buf + 12 * pgsz)) {
        fprintf(stderr, "FAIL: post-dontneed second write expected page 11 only, got n=%d\n", n);
        return 13;
    }

    printf("PASS: Linux UFFD_FEATURE_WP_ASYNC + PAGEMAP_SCAN write tracking works cleanly!\n");
    close(pmfd);
    close(uffd);
    munmap(buf, len);
    return 0;
}
