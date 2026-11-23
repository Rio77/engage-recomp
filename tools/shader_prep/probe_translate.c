// Phase 0 PoC harness: translates all extracted Fire Emblem Engage 2.0.0 shaders with
// Engage Recomp's real Maxwell translator (translate.c + scf*.c), compiles GLSL -> SPIR-V
// via libshaderc_shared.so.1, and compares generated xlat_hash / src_hash keys against a
// real runtime cache directory (cache/xlat and cache/spirv).
//
// Strictly read-only on any reference cache directory; writes nothing to game installs.
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../runtime/nvn/maxwell/shader.h"
#include "../../runtime/rt.h"

// Minimal ABI-stable shaderc C declarations (matching Khronos shaderc/shaderc.h)
typedef struct shaderc_compiler *shaderc_compiler_t;
typedef struct shaderc_compile_options *shaderc_compile_options_t;
typedef struct shaderc_compilation_result *shaderc_compilation_result_t;

typedef enum {
    shaderc_vertex_shader = 0,
    shaderc_fragment_shader = 1,
    shaderc_compute_shader = 2,
} shaderc_shader_kind;

typedef enum {
    shaderc_target_env_vulkan = 0,
} shaderc_target_env;

typedef enum {
    shaderc_env_version_vulkan_1_3 = ((1u << 22) | (3u << 12)),
} shaderc_env_version;

typedef enum {
    shaderc_optimization_level_zero = 0,
} shaderc_optimization_level;

typedef enum {
    shaderc_compilation_status_success = 0,
} shaderc_compilation_status;

extern shaderc_compiler_t shaderc_compiler_initialize(void);
extern void shaderc_compiler_release(shaderc_compiler_t);
extern shaderc_compile_options_t shaderc_compile_options_initialize(void);
extern void shaderc_compile_options_release(shaderc_compile_options_t);
extern void shaderc_compile_options_set_target_env(shaderc_compile_options_t, shaderc_target_env, uint32_t);
extern void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t, shaderc_optimization_level);
extern shaderc_compilation_result_t shaderc_compile_into_spv(
    const shaderc_compiler_t, const char *, size_t, shaderc_shader_kind,
    const char *, const char *, const shaderc_compile_options_t);
extern shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t);
extern size_t shaderc_result_get_length(const shaderc_compilation_result_t);
extern const char *shaderc_result_get_bytes(const shaderc_compilation_result_t);
extern const char *shaderc_result_get_error_message(const shaderc_compilation_result_t);
extern void shaderc_result_release(shaderc_compilation_result_t);

#ifndef TRANSLATOR_ID
#define TRANSLATOR_ID "dev"
#endif

// Exact copy of runtime/nvn/vulkan/exec.c:xlat_hash()
static uint64_t xlat_hash(const uint8_t *blob, size_t n, int stage, const ShaderKey *key) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = TRANSLATOR_ID; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    uint64_t stats = maxwell_stats_all ? 1 : 0;
    for (int i = 0; i < maxwell_stats_nonly; i++) stats = (stats ^ maxwell_stats_only[i]) * 0x100000001B3ull;
    uint64_t g[11] = {
        (uint64_t)stage | (uint64_t)maxwell_glsl_warp64 << 8,
        maxwell_glsl_denorm_preserve,
        maxwell_gmem_base,
        maxwell_gmem_size,
        getenv("FE3H_NATIVE_HALF") && (stage == STAGE_FRAGMENT || stage == STAGE_COMPUTE),
        getenv("FE3H_SPARSE_LOCAL") && stage != STAGE_VERTEX,
        maxwell_scf && stage == STAGE_COMPUTE,
        stats,
        getenv("FE3H_BRANCHLESS_HALF") != NULL,
        fe3h_lighting_scf && stage == STAGE_FRAGMENT,
        fe3h_ydir_fix,
    };
    for (size_t i = 0; i < sizeof g; i++) { h ^= ((const uint8_t *)g)[i]; h *= 0x100000001B3ull; }
    for (size_t i = 0; i < sizeof *key; i++) { h ^= ((const uint8_t *)key)[i]; h *= 0x100000001B3ull; }
    for (size_t i = 0; i < n; i++) { h ^= blob[i]; h *= 0x100000001B3ull; }
    return h;
}

// Exact copy of runtime/nvn/vulkan/shaders.c:src_hash()
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

typedef struct {
    char dm_key[17];
    uint32_t stage;
    uint32_t in_pso;
    uint32_t const_offset;
    uint32_t const_size;
    uint32_t code_len;
    uint32_t local[3];
    uint8_t *code;
} SeedShader;

typedef struct {
    uint32_t vs_idx;
    uint32_t fs_idx;
    uint32_t in_pso;
} SeedPair;

// Simple open-addressed 64-bit hash set
typedef struct {
    uint64_t *tab;
    size_t cap;
    size_t count;
} HashSet;

static void hs_init(HashSet *s, size_t cap) {
    s->cap = cap;
    s->count = 0;
    s->tab = calloc(cap, sizeof(uint64_t));
}

static bool hs_add(HashSet *s, uint64_t k) {
    if (!k) k = 1;
    size_t mask = s->cap - 1;
    size_t i = (size_t)(k ^ (k >> 31)) & mask;
    while (s->tab[i]) {
        if (s->tab[i] == k) return false;
        i = (i + 1) & mask;
    }
    s->tab[i] = k;
    s->count++;
    return true;
}

static bool hs_has(const HashSet *s, uint64_t k) {
    if (!k) k = 1;
    size_t mask = s->cap - 1;
    size_t i = (size_t)(k ^ (k >> 31)) & mask;
    while (s->tab[i]) {
        if (s->tab[i] == k) return true;
        i = (i + 1) & mask;
    }
    return false;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Worker queue for parallel GLSL -> SPIR-V compilation
typedef struct {
    char *glsl;
    int stage;
    int in_pso;
    uint64_t shash;
    char dm_key[17];
} SpvJob;

static SpvJob *g_jobs;
static size_t g_njobs;
static _Atomic size_t g_next_job;
static _Atomic int g_spv_ok, g_spv_fail, g_spv_pso_ok, g_spv_pso_fail;
static _Atomic int g_spv_err_logged;
static pthread_mutex_t g_spv_set_mu = PTHREAD_MUTEX_INITIALIZER;
static HashSet g_gen_spv;

static void *spv_worker(void *arg) {
    (void)arg;
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(opt, shaderc_optimization_level_zero);

    for (;;) {
        size_t idx = atomic_fetch_add(&g_next_job, 1);
        if (idx >= g_njobs) break;
        SpvJob *j = &g_jobs[idx];
        shaderc_shader_kind kind = j->stage == STAGE_VERTEX ? shaderc_vertex_shader
                                 : j->stage == STAGE_FRAGMENT ? shaderc_fragment_shader
                                 : shaderc_compute_shader;
        shaderc_compilation_result_t r = shaderc_compile_into_spv(
            comp, j->glsl, strlen(j->glsl), kind, j->dm_key, "main", opt);
        if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
            atomic_fetch_add(&g_spv_ok, 1);
            if (j->in_pso) atomic_fetch_add(&g_spv_pso_ok, 1);
            pthread_mutex_lock(&g_spv_set_mu);
            hs_add(&g_gen_spv, j->shash);
            pthread_mutex_unlock(&g_spv_set_mu);
        } else {
            atomic_fetch_add(&g_spv_fail, 1);
            if (j->in_pso) atomic_fetch_add(&g_spv_pso_fail, 1);
            if (atomic_fetch_add(&g_spv_err_logged, 1) < 10) {
                fprintf(stderr, "[spv-fail] %s stage=%d pso=%d: %s\n",
                        j->dm_key, j->stage, j->in_pso, shaderc_result_get_error_message(r));
            }
        }
        shaderc_result_release(r);
        free(j->glsl);
        j->glsl = NULL;
    }
    shaderc_compile_options_release(opt);
    shaderc_compiler_release(comp);
    return NULL;
}

static size_t load_dir_hashes(const char *dirpath, const char *ext, uint64_t **out) {
    DIR *d = opendir(dirpath);
    if (!d) return 0;
    size_t cap = 1024, n = 0;
    uint64_t *arr = malloc(cap * sizeof(uint64_t));
    struct dirent *de;
    size_t extlen = strlen(ext);
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len != 16 + extlen || strcmp(de->d_name + 16, ext) != 0) continue;
        char *end = NULL;
        uint64_t h = strtoull(de->d_name, &end, 16);
        if (end != de->d_name + 16) continue;
        if (n == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(uint64_t)); }
        arr[n++] = h;
    }
    closedir(d);
    *out = arr;
    return n;
}

int main(int argc, char **argv) {
    const char *seed_path = NULL;
    const char *ref_cache = NULL;
    const char *dump_hashes_path = NULL;
    int nthreads = 4;
    bool flip_y = false;
    bool depth_minus_one = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed_path = argv[++i];
        else if (!strcmp(argv[i], "--ref-cache") && i + 1 < argc) ref_cache = argv[++i];
        else if (!strcmp(argv[i], "--dump-hashes") && i + 1 < argc) dump_hashes_path = argv[++i];
        else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flip-y")) flip_y = true;
        else if (!strcmp(argv[i], "--depth-minus-one")) depth_minus_one = true;
    }
    if (!seed_path) {
        fprintf(stderr, "Usage: %s --seed <phase0_seed.bin> [--ref-cache <dir>] [--jobs N]\n", argv[0]);
        return 1;
    }

    // Match Steam Deck RADV runtime globals (verified from fe3hrecomp.log)
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    maxwell_glsl_denorm_preserve = true;
    maxwell_glsl_warp64 = false;
    maxwell_scf = false;
    fe3h_lighting_scf = false;
    fe3h_ydir_fix = true;

    double t0 = now_sec();
    FILE *f = fopen(seed_path, "rb");
    if (!f) { perror("fopen seed"); return 1; }
    char magic[8];
    uint32_t hdr[4];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "P0SHSEED", 8) != 0 || fread(hdr, 4, 4, f) != 4) {
        fprintf(stderr, "Bad seed header\n");
        return 1;
    }
    uint32_t ver = hdr[0], nshaders = hdr[1], npairs = hdr[2], ncompute = hdr[3];
    if (ver != 1) { fprintf(stderr, "Unsupported seed version %u\n", ver); return 1; }

    SeedShader *shaders = calloc(nshaders, sizeof(SeedShader));
    for (uint32_t i = 0; i < nshaders; i++) {
        uint32_t meta[8];
        if (fread(shaders[i].dm_key, 1, 16, f) != 16 || fread(meta, 4, 8, f) != 8) {
            fprintf(stderr, "Truncated shader %u\n", i);
            return 1;
        }
        shaders[i].dm_key[16] = 0;
        shaders[i].stage = meta[0];
        shaders[i].in_pso = meta[1];
        shaders[i].const_offset = meta[2];
        shaders[i].const_size = meta[3];
        shaders[i].code_len = meta[4];
        shaders[i].local[0] = meta[5];
        shaders[i].local[1] = meta[6];
        shaders[i].local[2] = meta[7];
        shaders[i].code = malloc(shaders[i].code_len);
        if (fread(shaders[i].code, 1, shaders[i].code_len, f) != shaders[i].code_len) {
            fprintf(stderr, "Truncated code %u\n", i);
            return 1;
        }
    }
    SeedPair *pairs = calloc(npairs, sizeof(SeedPair));
    if (npairs && fread(pairs, sizeof(SeedPair), npairs, f) != npairs) {
        fprintf(stderr, "Truncated pairs\n");
        return 1;
    }
    uint32_t *computes = calloc(ncompute ? ncompute : 1, sizeof(uint32_t));
    if (ncompute && fread(computes, sizeof(uint32_t), ncompute, f) != ncompute) {
        fprintf(stderr, "Truncated compute\n");
        return 1;
    }
    fclose(f);

    // Cache per-VS translation (out_mask + status) so each unique VS translates once
    uint8_t *vs_done = calloc(nshaders, 1);
    uint32_t *vs_out_mask = calloc(nshaders, sizeof(uint32_t));
    uint64_t *vs_xhash = calloc(nshaders, sizeof(uint64_t));

    HashSet gen_xlat, seen_spv_job;
    hs_init(&gen_xlat, 65536);
    hs_init(&seen_spv_job, 65536);
    hs_init(&g_gen_spv, 65536);

    size_t jobs_cap = (size_t)nshaders * 2 + 64;
    g_jobs = malloc(jobs_cap * sizeof(SpvJob));
    g_njobs = 0;

    int vs_ok = 0, vs_fail = 0, vs_pso_ok = 0, vs_pso_fail = 0;
    int fs_ok = 0, fs_fail = 0, fs_pso_ok = 0, fs_pso_fail = 0;
    int cs_ok = 0, cs_fail = 0;
    int brx_zero = 0, xlat_err_logged = 0;

    FILE *dump_f = dump_hashes_path ? fopen(dump_hashes_path, "w") : NULL;

    double t_xlat0 = now_sec();

    // 1. Translate all pairs (VS first -> propagate vi->out_mask -> FS)
    for (uint32_t pi = 0; pi < npairs; pi++) {
        uint32_t vi = pairs[pi].vs_idx;
        uint32_t fi = pairs[pi].fs_idx;
        SeedShader *vs = &shaders[vi];
        SeedShader *fs = &shaders[fi];

        if (!vs_done[vi]) {
            ShaderKey vkey = {0};
            vkey.glsl = 1;
            vkey.flip_y = flip_y;
            vkey.depth_minus_one = depth_minus_one;
            size_t vmax = vs->const_offset ? vs->const_offset : vs->code_len;
            if (vmax > vs->code_len) vmax = vs->code_len;
            size_t vn = maxwell_input_size(vs->code, vmax, STAGE_VERTEX);
            if (!vn) brx_zero++;
            uint64_t vh = vn ? xlat_hash(vs->code, vn, STAGE_VERTEX, &vkey) : 0;
            ShaderInfo *si = maxwell_translate(vs->code, vmax, STAGE_VERTEX, &vkey);
            if (si->msl) {
                vs_done[vi] = 1;
                vs_out_mask[vi] = si->out_mask;
                vs_xhash[vi] = vh;
                vs_ok++;
                if (vs->in_pso) vs_pso_ok++;
                if (vh) hs_add(&gen_xlat, vh);
                uint64_t sh = src_hash(si->msl, STAGE_VERTEX);
                if (dump_f) fprintf(dump_f, "vs\t%s\t%016llx\t%016llx\t%u\t%u\n",
                                    vs->dm_key, (unsigned long long)vh, (unsigned long long)sh, vs->in_pso, si->out_mask);
                if (hs_add(&seen_spv_job, sh)) {
                    if (g_njobs == jobs_cap) { jobs_cap *= 2; g_jobs = realloc(g_jobs, jobs_cap * sizeof(SpvJob)); }
                    g_jobs[g_njobs++] = (SpvJob){strdup(si->msl), STAGE_VERTEX, (int)vs->in_pso, sh, {0}};
                    memcpy(g_jobs[g_njobs - 1].dm_key, vs->dm_key, 17);
                }
            } else {
                vs_done[vi] = 2;
                vs_fail++;
                if (vs->in_pso) vs_pso_fail++;
                if (xlat_err_logged++ < 15)
                    fprintf(stderr, "[xlat-fail] VS %s (pso=%u): %s\n", vs->dm_key, vs->in_pso, si->error);
            }
            maxwell_free(si);
        }

        if (vs_done[vi] != 1) continue;

        ShaderKey fkey = {0};
        fkey.glsl = 1;
        fkey.flip_y = flip_y;
        fkey.vs_out_mask = vs_out_mask[vi];
        size_t fmax = fs->const_offset ? fs->const_offset : fs->code_len;
        if (fmax > fs->code_len) fmax = fs->code_len;
        size_t fn = maxwell_input_size(fs->code, fmax, STAGE_FRAGMENT);
        if (!fn) brx_zero++;
        uint64_t fh = fn ? xlat_hash(fs->code, fn, STAGE_FRAGMENT, &fkey) : 0;
        if (fh && hs_has(&gen_xlat, fh)) {
            // Already translated this exact (fs, vs_out_mask) variant
            continue;
        }
        ShaderInfo *si = maxwell_translate(fs->code, fmax, STAGE_FRAGMENT, &fkey);
        if (si->msl) {
            fs_ok++;
            if (fs->in_pso) fs_pso_ok++;
            if (fh) hs_add(&gen_xlat, fh);
            uint64_t sh = src_hash(si->msl, STAGE_FRAGMENT);
            if (dump_f) fprintf(dump_f, "fs\t%s\t%016llx\t%016llx\t%u\t%u\n",
                                fs->dm_key, (unsigned long long)fh, (unsigned long long)sh, fs->in_pso, vs_out_mask[vi]);
            if (hs_add(&seen_spv_job, sh)) {
                if (g_njobs == jobs_cap) { jobs_cap *= 2; g_jobs = realloc(g_jobs, jobs_cap * sizeof(SpvJob)); }
                g_jobs[g_njobs++] = (SpvJob){strdup(si->msl), STAGE_FRAGMENT, (int)fs->in_pso, sh, {0}};
                memcpy(g_jobs[g_njobs - 1].dm_key, fs->dm_key, 17);
            }
        } else {
            fs_fail++;
            if (fs->in_pso) fs_pso_fail++;
            if (xlat_err_logged++ < 15)
                fprintf(stderr, "[xlat-fail] FS %s (pso=%u): %s\n", fs->dm_key, fs->in_pso, si->error);
        }
        maxwell_free(si);
    }

    // 2. Translate compute shaders
    for (uint32_t ci = 0; ci < ncompute; ci++) {
        SeedShader *cs = &shaders[computes[ci]];
        ShaderKey ckey = {0};
        ckey.glsl = 1;
        size_t cmax = cs->const_offset ? cs->const_offset : cs->code_len;
        if (cmax > cs->code_len) cmax = cs->code_len;
        size_t cn = maxwell_input_size(cs->code, cmax, STAGE_COMPUTE);
        uint64_t ch = cn ? xlat_hash(cs->code, cn, STAGE_COMPUTE, &ckey) : 0;
        ShaderInfo *si = maxwell_translate(cs->code, cmax, STAGE_COMPUTE, &ckey);
        if (si->msl) {
            cs_ok++;
            if (ch) hs_add(&gen_xlat, ch);
            uint64_t sh = src_hash(si->msl, STAGE_COMPUTE);
            if (dump_f) fprintf(dump_f, "cs\t%s\t%016llx\t%016llx\t%u\t%u\n",
                                cs->dm_key, (unsigned long long)ch, (unsigned long long)sh, cs->in_pso, 0u);
            if (hs_add(&seen_spv_job, sh)) {
                if (g_njobs == jobs_cap) { jobs_cap *= 2; g_jobs = realloc(g_jobs, jobs_cap * sizeof(SpvJob)); }
                g_jobs[g_njobs++] = (SpvJob){strdup(si->msl), STAGE_COMPUTE, (int)cs->in_pso, sh, {0}};
                memcpy(g_jobs[g_njobs - 1].dm_key, cs->dm_key, 17);
            }
        } else {
            cs_fail++;
            fprintf(stderr, "[xlat-fail] CS %s: %s\n", cs->dm_key, si->error);
        }
        maxwell_free(si);
    }
    if (dump_f) fclose(dump_f);
    double t_xlat1 = now_sec();

    // 3. Compile all unique GLSL outputs to SPIR-V in parallel
    double t_spv0 = now_sec();
    if (nthreads < 1) nthreads = 1;
    pthread_t *th = calloc((size_t)nthreads, sizeof(pthread_t));
    for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, spv_worker, NULL);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    free(th);
    double t_spv1 = now_sec();

    printf("=== Phase 0 Translation & SPIR-V Report ===\n");
    printf("Config: TRANSLATOR_ID=%s flip_y=%d depth_minus_one=%d denorm_preserve=%d warp64=%d ydir_fix=%d jobs=%d\n",
           TRANSLATOR_ID, flip_y, depth_minus_one, maxwell_glsl_denorm_preserve, maxwell_glsl_warp64, fe3h_ydir_fix, nthreads);
    printf("Maxwell -> GLSL time: %.2f s (1 thread)\n", t_xlat1 - t_xlat0);
    printf("  VS: %d ok / %d fail (%.2f%%) | in pso_manifest: %d ok / %d fail\n",
           vs_ok, vs_fail, 100.0 * vs_ok / (vs_ok + vs_fail ? vs_ok + vs_fail : 1), vs_pso_ok, vs_pso_fail);
    printf("  FS: %d ok / %d fail (%.2f%%) | in pso_manifest: %d ok / %d fail\n",
           fs_ok, fs_fail, 100.0 * fs_ok / (fs_ok + fs_fail ? fs_ok + fs_fail : 1), fs_pso_ok, fs_pso_fail);
    printf("  CS: %d ok / %d fail\n", cs_ok, cs_fail);
    printf("  BRX (maxwell_input_size()==0): %d\n", brx_zero);
    printf("  Unique xlat_hash generated: %zu\n", gen_xlat.count);
    int spv_ok = atomic_load(&g_spv_ok), spv_fail = atomic_load(&g_spv_fail);
    int spv_pso_ok = atomic_load(&g_spv_pso_ok), spv_pso_fail = atomic_load(&g_spv_pso_fail);
    printf("GLSL -> SPIR-V time: %.2f s (%d threads, %zu unique GLSL)\n", t_spv1 - t_spv0, nthreads, g_njobs);
    printf("  SPIR-V: %d ok / %d fail (%.2f%%) | in pso_manifest: %d ok / %d fail (%.2f%%)\n",
           spv_ok, spv_fail, 100.0 * spv_ok / (spv_ok + spv_fail ? spv_ok + spv_fail : 1),
           spv_pso_ok, spv_pso_fail, 100.0 * spv_pso_ok / (spv_pso_ok + spv_pso_fail ? spv_pso_ok + spv_pso_fail : 1));
    printf("Total wall time: %.2f s\n", now_sec() - t0);

    // 4. Compare against real gameplay cache (if provided)
    if (ref_cache) {
        char p_xlat[512], p_spv[512];
        snprintf(p_xlat, sizeof p_xlat, "%s/xlat", ref_cache);
        snprintf(p_spv, sizeof p_spv, "%s/spirv", ref_cache);
        uint64_t *rx = NULL, *rs = NULL;
        size_t nrx = load_dir_hashes(p_xlat, ".bin", &rx);
        size_t nrs = load_dir_hashes(p_spv, ".spv", &rs);
        size_t xhit = 0, shit = 0;
        for (size_t i = 0; i < nrx; i++) if (hs_has(&gen_xlat, rx[i])) xhit++;
        for (size_t i = 0; i < nrs; i++) if (hs_has(&g_gen_spv, rs[i])) shit++;
        printf("\n=== Real Runtime Cache Key Match (%s) ===\n", ref_cache);
        printf("cache/xlat/*.bin hits:  %zu / %zu (%.2f%%)\n", xhit, nrx, nrx ? 100.0 * xhit / nrx : 0.0);
        printf("cache/spirv/*.spv hits: %zu / %zu (%.2f%%)\n", shit, nrs, nrs ? 100.0 * shit / nrs : 0.0);
        // Print up to 10 missed xlat hashes for diagnosis
        int printed = 0;
        for (size_t i = 0; i < nrx && printed < 10; i++) {
            if (!hs_has(&gen_xlat, rx[i])) {
                if (printed == 0) printf("Sample missed xlat hashes:\n");
                printf("  %016llx\n", (unsigned long long)rx[i]);
                printed++;
            }
        }
        free(rx);
        free(rs);
    }

    for (uint32_t i = 0; i < nshaders; i++) free(shaders[i].code);
    free(shaders);
    free(pairs);
    free(computes);
    free(vs_done);
    free(vs_out_mask);
    free(vs_xhash);
    free(g_jobs);
    free(gen_xlat.tab);
    free(seen_spv_job.tab);
    free(g_gen_spv.tab);
    return 0;
}
