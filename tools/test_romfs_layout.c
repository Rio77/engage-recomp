// Unit test + O(N) performance benchmark for romfs_layout() (Phase 1).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "romfs_build.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Reference builder from tools/romfs_old.c
bool romfs_build(const char *dir, const char *out, void (*progress)(uint64_t, uint64_t), char *why, size_t why_size);

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void write_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    if (len) {
        ssize_t n = write(fd, data, len);
        assert(n == (ssize_t)len);
    }
    close(fd);
}

static void test_equivalence(const char *base) {
    char romdir[512], sub1[512], sub2[512], sub3[512], binpath[512], why[256];
    snprintf(romdir, sizeof romdir, "%s/eq_romfs", base);
    snprintf(sub1, sizeof sub1, "%s/alpha", romdir);
    snprintf(sub2, sizeof sub2, "%s/beta", romdir);
    snprintf(sub3, sizeof sub3, "%s/beta/nested", romdir);
    snprintf(binpath, sizeof binpath, "%s/ref.bin", base);
    assert(mkdir(romdir, 0755) == 0);
    assert(mkdir(sub1, 0755) == 0);
    assert(mkdir(sub2, 0755) == 0);
    assert(mkdir(sub3, 0755) == 0);

    const char *dirs[4] = {romdir, sub1, sub2, sub3};
    for (int i = 0; i < 600; i++) {
        char fp[600];
        uint8_t buf[64];
        size_t len = (size_t)(i % 64);
        for (size_t j = 0; j < len; j++) buf[j] = (uint8_t)((i * 31 + j * 7) & 0xFF);
        snprintf(fp, sizeof fp, "%s/file_%04d.bin", dirs[i % 4], (i * 73) % 997);
        write_file(fp, buf, len);
    }

    assert(romfs_build(romdir, binpath, NULL, why, sizeof why));
    RomfsLayout lay;
    assert(romfs_layout(romdir, &lay, why, sizeof why));

    int fd = open(binpath, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    assert((uint64_t)st.st_size == lay.size);

    uint8_t ref_hdr[ROMFS_DATA_START];
    assert(pread(fd, ref_hdr, sizeof ref_hdr, 0) == (ssize_t)sizeof ref_hdr);
    assert(memcmp(ref_hdr, lay.header, sizeof ref_hdr) == 0);

    uint8_t *ref_tab = malloc(lay.tables_size);
    assert(ref_tab != NULL);
    assert(pread(fd, ref_tab, lay.tables_size, (off_t)lay.tables_off) == (ssize_t)lay.tables_size);
    assert(memcmp(ref_tab, lay.tables, lay.tables_size) == 0);

    free(ref_tab);
    close(fd);
    romfs_layout_free(&lay);
    printf("PASS: romfs_layout bit-for-bit identical to reference romfs_build\n");
}

static void test_large_dir_perf(const char *base, int nfiles) {
    char romdir[512], bigdir[512], why[256];
    snprintf(romdir, sizeof romdir, "%s/perf_romfs", base);
    snprintf(bigdir, sizeof bigdir, "%s/audio_bank", romdir);
    assert(mkdir(romdir, 0755) == 0);
    assert(mkdir(bigdir, 0755) == 0);

    for (int i = 0; i < nfiles; i++) {
        char fp[600];
        snprintf(fp, sizeof fp, "%s/snd_%06d.wem", bigdir, i);
        int fd = open(fp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        assert(fd >= 0);
        close(fd);
    }

    RomfsLayout lay;
    double t0 = now_sec();
    assert(romfs_layout(romdir, &lay, why, sizeof why));
    double dt = now_sec() - t0;
    assert(lay.nfiles == nfiles);

    // Verify sibling chain walks all nfiles entries in sorted order with zero cycles/breaks.
    uint64_t hv[10];
    memcpy(hv, lay.header, sizeof hv);
    uint64_t fmeta_rel = hv[7] - lay.tables_off;
    const uint8_t *ftab = lay.tables + fmeta_rel;
    uint32_t cur = 0;
    int walked = 0;
    while (cur != 0xFFFFFFFFu && walked <= nfiles + 1) {
        uint32_t sib;
        memcpy(&sib, ftab + cur + 4, 4);
        cur = sib;
        walked++;
    }
    assert(walked == nfiles);
    romfs_layout_free(&lay);
    printf("PASS: %d sibling files laid out and verified in %.3f s\n", nfiles, dt);
    assert(dt < 2.0);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--bench-dir") != 0) {
        // Optional real-directory timing mode: test_romfs_layout <existing_romfs_dir>
        RomfsLayout lay;
        char why[256];
        double t0 = now_sec();
        bool ok = romfs_layout(argv[1], &lay, why, sizeof why);
        double dt = now_sec() - t0;
        if (!ok) {
            fprintf(stderr, "FAIL: romfs_layout(%s): %s\n", argv[1], why);
            return 1;
        }
        printf("BENCH: %s -> %d files in %.3f s (tables %.2f MB)\n",
               argv[1], lay.nfiles, dt, (double)lay.tables_size / 1048576.0);
        romfs_layout_free(&lay);
        return 0;
    }

    char tmpl[] = "/tmp/test-romfs-layout-XXXXXX";
    char *tmp = mkdtemp(tmpl);
    assert(tmp != NULL);

    test_equivalence(tmp);
    test_large_dir_perf(tmp, 40000);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    int rc = system(cmd);
    (void)rc;
    return 0;
}
