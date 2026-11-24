/* Offline shader and pipeline preparation (--prepare-shaders): translates
 * locally extracted Maxwell shaders into cache/xlat/, compiles unique GLSL
 * outputs to SPIR-V in cache/spirv/, and pre-builds Vulkan graphics/compute
 * pipelines into cache/vk_pipelines.bin, cache/warm.bin, and cache/warm_cs.bin
 * using the runtime's real GPU capabilities.
 */
#include <SDL3/SDL_cpuinfo.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../../tools/shader_prep/seed_format.h"
#include "../../platform.h"
#include "../../rt.h"
#include "prepare_shaders.h"
#include "shaders.h"
#include "vk.h"

static volatile sig_atomic_t g_stop_requested;

static void on_cancel_signal(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

/* ---------------- open-addressed 64-bit set & key->val map ---------------- */

typedef struct {
    uint64_t *tab;
    size_t cap;
    size_t count;
} U64Set;

static bool u64set_init(U64Set *s, size_t cap) {
    s->cap = cap;
    s->count = 0;
    s->tab = calloc(cap, sizeof(uint64_t));
    return s->tab != NULL;
}

static bool u64set_add(U64Set *s, uint64_t k) {
    if (!k) k = 1;
    if ((s->count + 1) * 2 >= s->cap) {
        size_t ncap = s->cap * 2;
        uint64_t *ntab = calloc(ncap, sizeof(uint64_t));
        if (!ntab) return false;
        size_t nmask = ncap - 1;
        for (size_t i = 0; i < s->cap; i++) {
            uint64_t v = s->tab[i];
            if (!v) continue;
            size_t j = (size_t)(v ^ (v >> 31)) & nmask;
            while (ntab[j]) j = (j + 1) & nmask;
            ntab[j] = v;
        }
        free(s->tab);
        s->tab = ntab;
        s->cap = ncap;
    }
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

static bool u64set_has(const U64Set *s, uint64_t k) {
    if (!s->tab || !s->cap) return false;
    if (!k) k = 1;
    size_t mask = s->cap - 1;
    size_t i = (size_t)(k ^ (k >> 31)) & mask;
    while (s->tab[i]) {
        if (s->tab[i] == k) return true;
        i = (i + 1) & mask;
    }
    return false;
}

typedef struct {
    uint64_t key;
    uint64_t val;
} U64KV;

typedef struct {
    U64KV *tab;
    size_t cap;
    size_t count;
} U64Map;

static bool u64map_init(U64Map *m, size_t cap) {
    m->cap = cap;
    m->count = 0;
    m->tab = calloc(cap, sizeof(U64KV));
    return m->tab != NULL;
}

static void u64map_put(U64Map *m, uint64_t k, uint64_t v) {
    if (!k) k = 1;
    if ((m->count + 1) * 2 >= m->cap) {
        size_t ncap = m->cap * 2;
        U64KV *ntab = calloc(ncap, sizeof(U64KV));
        if (!ntab) return;
        size_t nmask = ncap - 1;
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->tab[i].key) continue;
            size_t j = (size_t)(m->tab[i].key ^ (m->tab[i].key >> 31)) & nmask;
            while (ntab[j].key) j = (j + 1) & nmask;
            ntab[j] = m->tab[i];
        }
        free(m->tab);
        m->tab = ntab;
        m->cap = ncap;
    }
    size_t mask = m->cap - 1;
    size_t i = (size_t)(k ^ (k >> 31)) & mask;
    while (m->tab[i].key && m->tab[i].key != k) i = (i + 1) & mask;
    if (!m->tab[i].key) m->count++;
    m->tab[i].key = k;
    m->tab[i].val = v;
}

static bool u64map_get(const U64Map *m, uint64_t k, uint64_t *out) {
    if (!m->tab || !m->cap) return false;
    if (!k) k = 1;
    size_t mask = m->cap - 1;
    size_t i = (size_t)(k ^ (k >> 31)) & mask;
    while (m->tab[i].key) {
        if (m->tab[i].key == k) {
            *out = m->tab[i].val;
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

/* Exact copy of shaders.c:src_hash for producer-side deduplication before queueing. */
static uint64_t glsl_src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t fnv_mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* ---------------- bounded SPIR-V queue ---------------- */

#define SPV_Q_CAP 64

typedef struct {
    char *glsl;
    int stage;
    char name[20];
} SpvTask;

typedef struct {
    SpvTask ring[SPV_Q_CAP];
    size_t head, tail, count;
    bool eof;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    _Atomic size_t hits;
    _Atomic size_t compiled;
    _Atomic size_t failed;
} SpvQueue;

static void spvq_init(SpvQueue *q) {
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void spvq_destroy(SpvQueue *q) {
    for (size_t i = 0; i < q->count; i++) {
        size_t idx = (q->head + i) % SPV_Q_CAP;
        free(q->ring[idx].glsl);
        q->ring[idx].glsl = NULL;
    }
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static bool spvq_push(SpvQueue *q, char *glsl, int stage, const uint8_t dm_key[8]) {
    pthread_mutex_lock(&q->mu);
    while (q->count == SPV_Q_CAP && !g_stop_requested) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&q->not_full, &q->mu, &ts);
    }
    if (g_stop_requested) {
        pthread_mutex_unlock(&q->mu);
        free(glsl);
        return false;
    }
    SpvTask *t = &q->ring[q->tail];
    t->glsl = glsl;
    t->stage = stage;
    snprintf(t->name, sizeof t->name, "%02x%02x%02x%02x%02x%02x%02x%02x",
             dm_key[0], dm_key[1], dm_key[2], dm_key[3],
             dm_key[4], dm_key[5], dm_key[6], dm_key[7]);
    q->tail = (q->tail + 1) % SPV_Q_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return true;
}

static void *spv_worker(void *arg) {
    SpvQueue *q = arg;
    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (q->count == 0 && !q->eof && !g_stop_requested) {
            pthread_cond_wait(&q->not_empty, &q->mu);
        }
        if (q->count == 0 || g_stop_requested) {
            /* Unblock producer if it is waiting on a full ring when cancelled. */
            if (g_stop_requested) pthread_cond_broadcast(&q->not_full);
            pthread_mutex_unlock(&q->mu);
            break;
        }
        SpvTask t = q->ring[q->head];
        q->ring[q->head].glsl = NULL;
        q->head = (q->head + 1) % SPV_Q_CAP;
        q->count--;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->mu);

        size_t nwords = 0;
        bool hit = false;
        uint32_t *spv = vk_glsl_to_spirv_ex(t.glsl, t.stage, t.name, &nwords, &hit);
        if (spv) {
            atomic_fetch_add(hit ? &q->hits : &q->compiled, 1);
            free(spv);
        } else {
            atomic_fetch_add(&q->failed, 1);
        }
        free(t.glsl);
    }
    return NULL;
}

/* ---------------- progress & atomic writes ---------------- */

typedef struct {
    size_t done;
    size_t total;
    uint64_t xlat_hits;
    uint64_t xlat_new;
    size_t xlat_fail;
    size_t spv_hits;
    size_t spv_new;
    size_t spv_fail;
    size_t pso_done;
    size_t pso_total;
    size_t pso_ok;
    size_t pso_fail;
    size_t pso_skip;
} PrepareStats;

static pthread_mutex_t g_prog_lock = PTHREAD_MUTEX_INITIALIZER;

static void write_progress(const char *path, const char *phase, const PrepareStats *st) {
    if (!path || !*path) return;
    pthread_mutex_lock(&g_prog_lock);
    char tmp[1088];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f,
                "phase=%s done=%zu total=%zu xlat_hits=%llu xlat_new=%llu xlat_fail=%zu "
                "spv_hits=%zu spv_new=%zu spv_fail=%zu pso_done=%zu pso_total=%zu "
                "pso_ok=%zu pso_fail=%zu pso_skip=%zu\n",
                phase, st->done, st->total,
                (unsigned long long)st->xlat_hits, (unsigned long long)st->xlat_new, st->xlat_fail,
                st->spv_hits, st->spv_new, st->spv_fail,
                st->pso_done, st->pso_total, st->pso_ok, st->pso_fail, st->pso_skip);
        if (fclose(f) == 0) {
            if (rename(tmp, path) != 0) remove(tmp);
        } else {
            remove(tmp);
        }
    }
    pthread_mutex_unlock(&g_prog_lock);
}

/* ---------------- crash journal & skip list ---------------- */

#define JR_SLOT_BYTES 64
#define JR_MAX_SLOTS 16
#define JR_PATH "cache/prepare-journal.bin"
#define SKIP_PATH "cache/prepare-skip.bin"
#define SKIP_MAGIC 0x504B5350u /* "PSKP" */

typedef struct {
    uint32_t vendor, device, driver, kind;
    uint64_t hash;
} SkipRec;

typedef struct {
    uint32_t magic, version, rec_size, count;
} SkipHdr;

typedef struct {
    uint64_t hash;
    uint32_t kind;
    uint8_t pad[JR_SLOT_BYTES - 12];
} JrSlot;

_Static_assert(sizeof(JrSlot) == JR_SLOT_BYTES, "JrSlot size mismatch");

static void journal_set(int jfd, int worker_id, uint64_t hash, uint32_t kind) {
    if (jfd < 0 || worker_id < 0 || worker_id >= JR_MAX_SLOTS) return;
    JrSlot slot = {0};
    slot.hash = hash;
    slot.kind = kind;
    off_t off = (off_t)worker_id * JR_SLOT_BYTES;
    (void)pwrite(jfd, &slot, sizeof slot, off);
}

static int prepare_journal_init(const VkPhysicalDeviceProperties *props, U64Set *skip_set) {
    SkipRec *all = NULL;
    size_t nall = 0, cap = 0;

    FILE *sf = fopen(SKIP_PATH, "rb");
    if (sf) {
        SkipHdr hdr = {0};
        if (fread(&hdr, sizeof hdr, 1, sf) == 1 && hdr.magic == SKIP_MAGIC &&
            hdr.version == 1 && hdr.rec_size == sizeof(SkipRec) && hdr.count <= (1u << 20)) {
            all = malloc((hdr.count ? hdr.count : 1) * sizeof(SkipRec));
            cap = hdr.count ? hdr.count : 1;
            if (all && fread(all, sizeof(SkipRec), hdr.count, sf) == hdr.count) {
                nall = hdr.count;
                for (size_t i = 0; i < nall; i++) {
                    if (all[i].vendor == props->vendorID && all[i].device == props->deviceID &&
                        all[i].driver == props->driverVersion) {
                        u64set_add(skip_set, all[i].hash);
                    }
                }
            } else {
                free(all);
                all = NULL;
                nall = 0;
                cap = 0;
            }
        }
        fclose(sf);
    }

    FILE *jf = fopen(JR_PATH, "rb");
    if (jf) {
        JrSlot slots[JR_MAX_SLOTS];
        size_t n = fread(slots, sizeof(JrSlot), JR_MAX_SLOTS, jf);
        fclose(jf);
        size_t added = 0;
        for (size_t i = 0; i < n; i++) {
            if (!slots[i].hash) continue;
            if (u64set_add(skip_set, slots[i].hash)) {
                if (nall == cap) {
                    size_t ncap = cap ? cap * 2 : 16;
                    SkipRec *grown = realloc(all, ncap * sizeof(SkipRec));
                    if (!grown) continue;
                    all = grown;
                    cap = ncap;
                }
                all[nall++] = (SkipRec){
                    .vendor = props->vendorID,
                    .device = props->deviceID,
                    .driver = props->driverVersion,
                    .kind = slots[i].kind,
                    .hash = slots[i].hash,
                };
                added++;
            }
        }
        if (added) {
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%s.tmp", SKIP_PATH);
            FILE *out = fopen(tmp, "wb");
            if (out) {
                SkipHdr hdr = {SKIP_MAGIC, 1, (uint32_t)sizeof(SkipRec), (uint32_t)nall};
                bool ok = fwrite(&hdr, sizeof hdr, 1, out) == 1 &&
                          fwrite(all, sizeof(SkipRec), nall, out) == nall;
                if (fclose(out) != 0) ok = false;
                if (ok) {
                    remove(SKIP_PATH);
                    rename(tmp, SKIP_PATH);
                } else {
                    remove(tmp);
                }
            }
            printf("[prepare] recovered from prior crash: skipped %zu pipeline(s) for this GPU/driver (%s)\n",
                   added, SKIP_PATH);
            fflush(stdout);
        }
        remove(JR_PATH);
    }
    free(all);

    int fd = open(JR_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        uint8_t zeros[JR_SLOT_BYTES * JR_MAX_SLOTS] = {0};
        if (pwrite(fd, zeros, sizeof zeros, 0) != (ssize_t)sizeof zeros) {
            close(fd);
            remove(JR_PATH);
            fd = -1;
        }
    }
    return fd;
}

/* ---------------- pipeline worker types ---------------- */

typedef struct {
    uint64_t vxh;
    uint64_t fxh;
    uint32_t vs_out_mask;
    VkOfflineProg *op;
} OfflineProgSlot;

enum { JOB_GFX = 1, JOB_CS = 2, JOB_LIB_VS = 3, JOB_LIB_FS = 4 };

typedef struct {
    uint32_t prog_idx;
    uint32_t cs_idx;
    uint8_t job_kind;
    uint8_t poly_fill;
    uint8_t a2c;
    uint32_t samples;
    uint64_t hash;
    PipeKey key;
    uint64_t cxh; /* compute xh OR vs/fs xh for GPL stage libs */
    uint32_t local[3];
} PipeJob;

typedef struct {
    int worker_id;
    int jfd;
    long fault_at;
    const PipeJob *jobs;
    size_t njobs;
    const OfflineProgSlot *progs;
    const U64Set *skip_set;
    _Atomic size_t *next_idx;
    _Atomic size_t *ok_cnt;
    _Atomic size_t *fail_cnt;
    _Atomic size_t *skip_cnt;
    _Atomic size_t *done_cnt;
    _Atomic uint64_t *last_prog_ns;
    const char *progress_path;
    const PrepareStats *base_stats;
} PipeWorkerCtx;

static void *pipe_worker(void *arg) {
    PipeWorkerCtx *ctx = arg;
    for (;;) {
        if (g_stop_requested) break;
        size_t idx = atomic_fetch_add(ctx->next_idx, 1);
        if (idx >= ctx->njobs) break;

        const PipeJob *job = &ctx->jobs[idx];
        uint32_t kind = job->job_kind;

        if (u64set_has(ctx->skip_set, job->hash)) {
            atomic_fetch_add(ctx->skip_cnt, 1);
        } else {
            journal_set(ctx->jfd, ctx->worker_id, job->hash, kind);
            if (ctx->fault_at >= 0 && (long)idx == ctx->fault_at) {
                /* Simulated crash while the driver compiles job[idx]; journal stays populated on disk. */
                _exit(134);
            }
            bool ok = false;
            if (job->job_kind == JOB_CS) {
                ok = vk_offline_build_compute(job->cxh, job->local);
            } else if (job->job_kind == JOB_LIB_VS) {
                ok = vk_offline_prebuild_vs_lib(job->cxh, job->poly_fill);
            } else if (job->job_kind == JOB_LIB_FS) {
                ok = vk_offline_prebuild_fs_lib(job->cxh, job->samples, job->a2c);
            } else {
                VkOfflineProg *op = ctx->progs[job->prog_idx].op;
                ok = vk_offline_build_graphics(op, &job->key);
            }
            journal_set(ctx->jfd, ctx->worker_id, 0, 0);
            if (ok) {
                size_t total_ok = atomic_fetch_add(ctx->ok_cnt, 1) + 1;
                if ((total_ok % 500) == 0) vk_pipeline_cache_save();
            } else {
                atomic_fetch_add(ctx->fail_cnt, 1);
            }
        }

        size_t done = atomic_fetch_add(ctx->done_cnt, 1) + 1;
        uint64_t now = plat_now_ns();
        uint64_t prev = atomic_load(ctx->last_prog_ns);
        if ((now - prev >= 250000000ull || done == ctx->njobs) &&
            atomic_compare_exchange_strong(ctx->last_prog_ns, &prev, now)) {
            PrepareStats st = *ctx->base_stats;
            st.done = done;
            st.total = ctx->njobs;
            st.pso_done = done;
            st.pso_total = ctx->njobs;
            st.pso_ok = atomic_load(ctx->ok_cnt);
            st.pso_fail = atomic_load(ctx->fail_cnt);
            st.pso_skip = atomic_load(ctx->skip_cnt);
            write_progress(ctx->progress_path, "pipelines", &st);
        }
    }
    return NULL;
}

/* ---------------- stage A helpers ---------------- */

typedef struct {
    SeedShaderRec meta;
    uint8_t *code;
} LoadedShader;

static void maybe_queue_spv(SpvQueue *q, U64Set *seen_spv, const char *msl, int stage, const uint8_t dm_key[8]) {
    uint64_t sh = glsl_src_hash(msl, stage);
    if (!u64set_add(seen_spv, sh)) return;
    char *copy = strdup(msl);
    if (!copy) return;
    spvq_push(q, copy, stage, dm_key);
}

static size_t shader_max_code(const LoadedShader *s) {
    return s->meta.const_offset && s->meta.const_offset <= s->meta.code_len
               ? s->meta.const_offset
               : s->meta.code_len;
}

static uint64_t pack_pair_sig(uint32_t fi, uint32_t vs_out_mask) {
    return ((uint64_t)fi << 32) | (uint64_t)vs_out_mask;
}

static bool has_nonzero_u8(const uint8_t *arr, size_t n) {
    for (size_t i = 0; i < n; i++) if (arr[i]) return true;
    return false;
}

int vk_prepare_shaders(const PrepareOpts *opts) {
    if (!opts || !opts->seed_path || !*opts->seed_path) {
        fprintf(stderr, "[prepare] missing --prepare-shaders seed path\n");
        return 1;
    }

    uint64_t t0 = plat_now_ns();
    FILE *f = fopen(opts->seed_path, "rb");
    if (!f) {
        fprintf(stderr, "[prepare] cannot open %s: %s\n", opts->seed_path, strerror(errno));
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long fsize_l = ftell(f);
    if (fsize_l <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[prepare] empty or unreadable seed %s\n", opts->seed_path);
        fclose(f);
        return 1;
    }
    size_t fsize = (size_t)fsize_l;

    SeedHeader hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1 || memcmp(hdr.magic, SEED_MAGIC, 8) != 0 || hdr.version != SEED_VERSION) {
        fprintf(stderr, "[prepare] invalid seed header in %s\n", opts->seed_path);
        fclose(f);
        return 1;
    }
    if (hdr.nshaders > (1u << 20) || hdr.npairs > (1u << 20) ||
        hdr.npsos > (1u << 20) || hdr.ncompute > (1u << 20)) {
        fprintf(stderr, "[prepare] seed counts out of bounds\n");
        fclose(f);
        return 1;
    }

    LoadedShader *shaders = calloc(hdr.nshaders ? hdr.nshaders : 1, sizeof *shaders);
    SeedPairRec *pairs = calloc(hdr.npairs ? hdr.npairs : 1, sizeof *pairs);
    SeedPsoRec *psos = calloc(hdr.npsos ? hdr.npsos : 1, sizeof *psos);
    uint32_t *computes = calloc(hdr.ncompute ? hdr.ncompute : 1, sizeof *computes);
    if (!shaders || !pairs || !psos || !computes) {
        free(shaders); free(pairs); free(psos); free(computes);
        fclose(f);
        return 1;
    }

    bool read_ok = true;
    for (uint32_t i = 0; i < hdr.nshaders && read_ok; i++) {
        if (fread(&shaders[i].meta, sizeof(SeedShaderRec), 1, f) != 1) { read_ok = false; break; }
        uint32_t clen = shaders[i].meta.code_len;
        long pos = ftell(f);
        if (clen == 0 || clen > (16u << 20) || pos < 0 || (size_t)pos + clen > fsize) {
            read_ok = false;
            break;
        }
        shaders[i].code = malloc(clen);
        if (!shaders[i].code || fread(shaders[i].code, 1, clen, f) != clen) read_ok = false;
    }
    if (read_ok && hdr.npairs && fread(pairs, sizeof(SeedPairRec), hdr.npairs, f) != hdr.npairs) read_ok = false;
    if (read_ok && hdr.npsos && fread(psos, sizeof(SeedPsoRec), hdr.npsos, f) != hdr.npsos) read_ok = false;
    if (read_ok && hdr.ncompute && fread(computes, sizeof(uint32_t), hdr.ncompute, f) != hdr.ncompute) read_ok = false;
    fclose(f);

    if (read_ok) {
        for (uint32_t i = 0; i < hdr.npairs; i++)
            if (pairs[i].vs_idx >= hdr.nshaders || pairs[i].fs_idx >= hdr.nshaders) { read_ok = false; break; }
        for (uint32_t i = 0; i < hdr.npsos && read_ok; i++)
            if (psos[i].vs_idx >= hdr.nshaders || psos[i].fs_idx >= hdr.nshaders) read_ok = false;
        for (uint32_t i = 0; i < hdr.ncompute && read_ok; i++)
            if (computes[i] >= hdr.nshaders) read_ok = false;
    }
    if (!read_ok) {
        fprintf(stderr, "[prepare] truncated or corrupt seed file: %s\n", opts->seed_path);
        for (uint32_t i = 0; i < hdr.nshaders; i++) free(shaders[i].code);
        free(shaders); free(pairs); free(psos); free(computes);
        return 1;
    }

    if (!vk_init_headless()) {
        fprintf(stderr, "[prepare] Vulkan headless initialization failed\n");
        for (uint32_t i = 0; i < hdr.nshaders; i++) free(shaders[i].code);
        free(shaders); free(pairs); free(psos); free(computes);
        return 1;
    }

    mkdir("cache", 0755);
    mkdir("cache/xlat", 0755);
    mkdir("cache/spirv", 0755);

    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;

    struct sigaction sa = {0}, old_int = {0}, old_term = {0};
    sa.sa_handler = on_cancel_signal;
    sigemptyset(&sa.sa_mask);
    g_stop_requested = 0;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    int nworkers = opts->jobs;
    if (nworkers <= 0) {
        int ncpu = SDL_GetNumLogicalCPUCores();
        nworkers = ncpu > 2 ? ncpu - 2 : 1;
        if (nworkers < 1) nworkers = 1;
        if (nworkers > 6) nworkers = 6;
    }
    if (nworkers > JR_MAX_SLOTS) nworkers = JR_MAX_SLOTS;

    uint8_t *vs_state = calloc(hdr.nshaders ? hdr.nshaders : 1, 1);
    uint32_t *vs_out_mask = calloc(hdr.nshaders ? hdr.nshaders : 1, sizeof(uint32_t));
    uint64_t *vs_xlat = calloc(hdr.nshaders ? hdr.nshaders : 1, sizeof(uint64_t));
    uint64_t *cs_xlat = calloc(hdr.ncompute ? hdr.ncompute : 1, sizeof(uint64_t));
    size_t cap_fx = (size_t)hdr.npairs + (size_t)hdr.npsos + 16;
    uint64_t *fx_list = calloc(cap_fx, sizeof(uint64_t));
    uint8_t *fx_alpha = calloc(cap_fx, 1);
    size_t n_fx = 0;
    typedef struct { uint64_t xh[2]; uint32_t vs_out_mask, pad; } WarmPairDiskRec;
    size_t cap_wp = (size_t)hdr.npairs + (size_t)hdr.npsos + 16;
    WarmPairDiskRec *wp_list = calloc(cap_wp, sizeof *wp_list);
    size_t n_wp = 0;
    U64Set seen_fs = {0}, seen_spv = {0}, seen_fx = {0}, seen_wp = {0};
    U64Map fs_xlat = {0};
    u64set_init(&seen_fs, 16384);
    u64set_init(&seen_spv, 16384);
    u64set_init(&seen_fx, 16384);
    u64set_init(&seen_wp, 16384);
    u64map_init(&fs_xlat, 16384);

    SpvQueue spvq;
    spvq_init(&spvq);
    pthread_t *threads = calloc((size_t)nworkers, sizeof(pthread_t));
    for (int i = 0; i < nworkers; i++) pthread_create(&threads[i], NULL, spv_worker, &spvq);

    const bool flip_y = (hdr.flags & SEED_FLAG_FLIP_Y) != 0;
    const bool depth_minus_one = (hdr.flags & SEED_FLAG_DEPTH_MINUS_ONE) != 0;
    const size_t total_units = (size_t)hdr.npairs + (size_t)hdr.ncompute;
    size_t xlat_fail = 0, done_units = 0;
    uint64_t xhits0 = 0, xmisses0 = 0;
    vk_xlat_stats(&xhits0, &xmisses0);
    uint64_t last_prog_ns = plat_now_ns();

    PrepareStats stats = {0};
    stats.total = total_units;
    stats.pso_total = opts->no_pipelines ? 0 : ((size_t)hdr.npsos + (size_t)hdr.ncompute);
    write_progress(opts->progress_path, "shaders", &stats);

    printf("[prepare] starting offline preparation: %u shaders, %u pairs, %u psos, %u compute (%d workers)\n",
           hdr.nshaders, hdr.npairs, hdr.npsos, hdr.ncompute, nworkers);
    fflush(stdout);

    for (uint32_t pi = 0; pi < hdr.npairs && !g_stop_requested; pi++) {
        uint32_t vi = pairs[pi].vs_idx;
        uint32_t fi = pairs[pi].fs_idx;
        const LoadedShader *vs = &shaders[vi];
        const LoadedShader *fs = &shaders[fi];

        if (vs_state[vi] == 0) {
            ShaderKey vkey = {0};
            vkey.glsl = 1;
            vkey.flip_y = flip_y;
            vkey.depth_minus_one = depth_minus_one;
            uint64_t vh = 0;
            ShaderInfo *si = vk_translate_cached(vs->code, shader_max_code(vs), STAGE_VERTEX, &vkey, &vh);
            if (si && si->msl) {
                vs_state[vi] = 1;
                vs_out_mask[vi] = si->out_mask;
                vs_xlat[vi] = vh;
                maybe_queue_spv(&spvq, &seen_spv, si->msl, STAGE_VERTEX, vs->meta.dm_key);
            } else {
                vs_state[vi] = 2;
                xlat_fail++;
            }
            maxwell_free(si);
        }

        if (vs_state[vi] == 1 && !g_stop_requested) {
            uint64_t pair_sig = pack_pair_sig(fi, vs_out_mask[vi]);
            uint64_t fh = 0;
            if (u64set_add(&seen_fs, pair_sig)) {
                ShaderKey fkey = {0};
                fkey.glsl = 1;
                fkey.flip_y = flip_y;
                fkey.vs_out_mask = vs_out_mask[vi];
                ShaderInfo *si = vk_translate_cached(fs->code, shader_max_code(fs), STAGE_FRAGMENT, &fkey, &fh);
                if (si && si->msl) {
                    u64map_put(&fs_xlat, pair_sig, fh);
                    if (n_fx < cap_fx && u64set_add(&seen_fx, fh)) {
                        fx_alpha[n_fx] = (strstr(si->msl, "discard") != NULL || strstr(si->msl, "demote") != NULL) ? 1 : 0;
                        fx_list[n_fx++] = fh;
                    }
                    maybe_queue_spv(&spvq, &seen_spv, si->msl, STAGE_FRAGMENT, fs->meta.dm_key);
                } else {
                    xlat_fail++;
                }
                maxwell_free(si);
            } else {
                u64map_get(&fs_xlat, pair_sig, &fh);
            }
            if (fh && n_wp < cap_wp) {
                uint64_t wpk = (vs_xlat[vi] * 0x9E3779B97F4A7C15ull) ^ fh;
                wpk = fnv_mix(wpk, &vs_out_mask[vi], sizeof(uint32_t));
                if (u64set_add(&seen_wp, wpk)) {
                    wp_list[n_wp++] = (WarmPairDiskRec){.xh = {vs_xlat[vi], fh}, .vs_out_mask = vs_out_mask[vi], .pad = 0};
                }
            }
        }

        done_units++;
        uint64_t now = plat_now_ns();
        if (now - last_prog_ns >= 250000000ull || done_units == total_units) {
            uint64_t xh = 0, xm = 0;
            vk_xlat_stats(&xh, &xm);
            stats.done = done_units;
            stats.xlat_hits = xh - xhits0;
            stats.xlat_new = xm - xmisses0;
            stats.xlat_fail = xlat_fail;
            stats.spv_hits = atomic_load(&spvq.hits);
            stats.spv_new = atomic_load(&spvq.compiled);
            stats.spv_fail = atomic_load(&spvq.failed);
            write_progress(opts->progress_path, "shaders", &stats);
            last_prog_ns = now;
        }
    }

    for (uint32_t ci = 0; ci < hdr.ncompute && !g_stop_requested; ci++) {
        const LoadedShader *cs = &shaders[computes[ci]];
        ShaderKey ckey = {0};
        ckey.glsl = 1;
        uint64_t ch = 0;
        ShaderInfo *si = vk_translate_cached(cs->code, shader_max_code(cs), STAGE_COMPUTE, &ckey, &ch);
        if (si && si->msl) {
            cs_xlat[ci] = ch;
            maybe_queue_spv(&spvq, &seen_spv, si->msl, STAGE_COMPUTE, cs->meta.dm_key);
        } else {
            xlat_fail++;
        }
        maxwell_free(si);
        done_units++;
    }

    pthread_mutex_lock(&spvq.mu);
    spvq.eof = true;
    pthread_cond_broadcast(&spvq.not_empty);
    pthread_cond_broadcast(&spvq.not_full);
    pthread_mutex_unlock(&spvq.mu);

    for (int i = 0; i < nworkers; i++) pthread_join(threads[i], NULL);

    uint64_t xhits1 = 0, xmisses1 = 0;
    vk_xlat_stats(&xhits1, &xmisses1);
    stats.done = done_units;
    stats.xlat_hits = xhits1 - xhits0;
    stats.xlat_new = xmisses1 - xmisses0;
    stats.xlat_fail = xlat_fail;
    stats.spv_hits = atomic_load(&spvq.hits);
    stats.spv_new = atomic_load(&spvq.compiled);
    stats.spv_fail = atomic_load(&spvq.failed);

    spvq_destroy(&spvq);

    /* ---------------- Phase 3: offline Vulkan pipeline prewarming ---------------- */
    size_t pso_ok = 0, pso_fail = 0, pso_skip = 0, pso_done = 0, pso_total = 0;

    if (!opts->no_pipelines && !g_stop_requested && (hdr.npsos > 0 || hdr.ncompute > 0 || vk_gpl_supported())) {
        U64Set skip_set = {0};
        u64set_init(&skip_set, 256);
        int jfd = prepare_journal_init(&g_vk.props, &skip_set);

        OfflineProgSlot *prog_slots = calloc(hdr.npsos ? hdr.npsos : 1, sizeof *prog_slots);
        U64Map prog_map = {0};
        u64map_init(&prog_map, 2048);
        size_t nprogs = 0;

        const bool use_gpl = vk_gpl_supported();
        size_t max_jobs = (size_t)hdr.npsos * 3 + (size_t)hdr.ncompute +
                          (use_gpl ? ((size_t)hdr.nshaders + cap_fx * 2 + 16) : 0);
        PipeJob *jobs = calloc(max_jobs ? max_jobs : 1, sizeof *jobs);
        U64Set seen_jobs = {0};
        u64set_init(&seen_jobs, 8192);
        size_t njobs = 0;

        VkFormat d24s8_vk = vk_depth24_stencil8_format();
        bool has_div = vk_divisor_supported();

        /* Resolve programs & translations needed by each SeedPsoRec. */
        uint32_t *pso_prog_idx = calloc(hdr.npsos ? hdr.npsos : 1, sizeof(uint32_t));
        for (uint32_t i = 0; i < hdr.npsos; i++) pso_prog_idx[i] = UINT32_MAX;

        for (uint32_t pi = 0; pi < hdr.npsos && !g_stop_requested; pi++) {
            const SeedPsoRec *rec = &psos[pi];
            uint32_t vi = rec->vs_idx, fi = rec->fs_idx;

            ShaderKey vkey = {0};
            vkey.glsl = 1;
            vkey.flip_y = flip_y;
            vkey.depth_minus_one = depth_minus_one;
            for (int a = 0; a < 32; a++) vkey.attr_kind[a] = (uint8_t)vk_nvn_attr_kind(rec->attr_fmt[a]);

            uint64_t vxh = 0;
            uint32_t omask = 0;
            bool need_vs = has_nonzero_u8(vkey.attr_kind, 32) || vs_xlat[vi] == 0;
            if (!need_vs) {
                vxh = vs_xlat[vi];
                omask = vs_out_mask[vi];
            } else {
                const LoadedShader *vs = &shaders[vi];
                ShaderInfo *si = vk_translate_cached(vs->code, shader_max_code(vs), STAGE_VERTEX, &vkey, &vxh);
                if (si && si->msl) {
                    omask = si->out_mask;
                    size_t nw = 0;
                    uint32_t *spv = vk_glsl_to_spirv_ex(si->msl, STAGE_VERTEX, "vs", &nw, NULL);
                    free(spv);
                } else {
                    vxh = 0;
                }
                maxwell_free(si);
            }
            if (!vxh) { pso_fail++; continue; }

            uint8_t rtk[8] = {0};
            for (int c = 0; c < 8; c++) rtk[c] = (uint8_t)mtl_rt_kind(rec->mtl_cfmt[c]);

            uint64_t fxh = 0;
            uint64_t pair_sig = pack_pair_sig(fi, omask);
            bool need_fs = has_nonzero_u8(rtk, 8) || !u64map_get(&fs_xlat, pair_sig, &fxh);
            if (need_fs) {
                const LoadedShader *fs = &shaders[fi];
                ShaderKey fkey = {0};
                fkey.glsl = 1;
                fkey.flip_y = flip_y;
                fkey.vs_out_mask = omask;
                memcpy(fkey.rt_kind, rtk, 8);
                ShaderInfo *si = vk_translate_cached(fs->code, shader_max_code(fs), STAGE_FRAGMENT, &fkey, &fxh);
                if (si && si->msl) {
                    size_t nw = 0;
                    uint32_t *spv = vk_glsl_to_spirv_ex(si->msl, STAGE_FRAGMENT, "fs", &nw, NULL);
                    free(spv);
                    if (n_fx < cap_fx && u64set_add(&seen_fx, fxh)) {
                        fx_alpha[n_fx] = (strstr(si->msl, "discard") != NULL || strstr(si->msl, "demote") != NULL) ? 1 : 0;
                        fx_list[n_fx++] = fxh;
                    }
                } else {
                    fxh = 0;
                }
                maxwell_free(si);
            }
            if (!fxh) { pso_fail++; continue; }

            uint64_t pk = ((vxh * 0x9E3779B97F4A7C15ull) ^ fxh);
            pk = fnv_mix(pk, &omask, sizeof omask);
            if (!pk) pk = 1;
            uint64_t p_idx = 0;
            if (!u64map_get(&prog_map, pk, &p_idx)) {
                p_idx = nprogs++;
                prog_slots[p_idx] = (OfflineProgSlot){.vxh = vxh, .fxh = fxh, .vs_out_mask = omask, .op = NULL};
                u64map_put(&prog_map, pk, p_idx);
            }
            if (n_wp < cap_wp && u64set_add(&seen_wp, pk)) {
                wp_list[n_wp++] = (WarmPairDiskRec){.xh = {vxh, fxh}, .vs_out_mask = omask, .pad = 0};
            }
            pso_prog_idx[pi] = (uint32_t)p_idx;
        }

        /* Instantiate unique VkShaderModules & VkPipelineLayouts once per unique (VS, FS) pair. */
        for (size_t u = 0; u < nprogs && !g_stop_requested; u++) {
            prog_slots[u].op = vk_offline_prog_create(prog_slots[u].vxh, prog_slots[u].fxh, prog_slots[u].vs_out_mask);
        }

        /* Build deduplicated PipeKeys in manifest order. */
        for (uint32_t pi = 0; pi < hdr.npsos && !g_stop_requested; pi++) {
            uint32_t pidx = pso_prog_idx[pi];
            if (pidx == UINT32_MAX) continue;
            VkOfflineProg *op = prog_slots[pidx].op;
            if (!op) { pso_fail++; continue; }

            PipeKey k;
            if (!vk_seed_to_pipe_key(&psos[pi],
                                     vk_offline_prog_attr_mask(op),
                                     vk_offline_prog_out_mask(op),
                                     d24s8_vk, has_div, vk_vertex_format_supported, &k)) {
                pso_fail++;
                continue;
            }
            if (g_vk.dyn_vi) pipe_key_strip_vi(&k);
            uint64_t h = fnv_mix(0xcbf29ce484222325ull, &prog_slots[pidx].vxh, sizeof(uint64_t));
            h = fnv_mix(h, &prog_slots[pidx].fxh, sizeof(uint64_t));
            h = fnv_mix(h, &k, sizeof k);
            if (!h) h = 1;
            if (!u64set_add(&seen_jobs, h)) continue;

            jobs[njobs++] = (PipeJob){
                .prog_idx = pidx,
                .job_kind = JOB_GFX,
                .hash = h,
                .key = k,
            };
        }
        free(pso_prog_idx);

        for (uint32_t ci = 0; ci < hdr.ncompute && !g_stop_requested; ci++) {
            uint64_t cxh = cs_xlat[ci];
            if (!cxh) { pso_fail++; continue; }
            const uint32_t *loc = shaders[computes[ci]].meta.local;
            uint64_t h = fnv_mix(0xcbf29ce484222325ull, &cxh, sizeof cxh);
            h = fnv_mix(h, loc, 3 * sizeof(uint32_t));
            if (!h) h = 1;
            if (!u64set_add(&seen_jobs, h)) continue;

            PipeJob job = {.cs_idx = ci, .job_kind = JOB_CS, .hash = h, .cxh = cxh};
            memcpy(job.local, loc, sizeof job.local);
            jobs[njobs++] = job;
        }

        if (use_gpl && !g_stop_requested) {
            /* Prebuild every unique VS pre-rast library and FS library so unseen state combos fast-link at <1ms. */
            for (uint32_t vi = 0; vi < hdr.nshaders && njobs < max_jobs; vi++) {
                uint64_t vxh = vs_xlat[vi];
                if (!vxh) continue;
                uint64_t tag = ((uint64_t)JOB_LIB_VS << 56) | 1ull;
                uint64_t h = fnv_mix(fnv_mix(0xcbf29ce484222325ull, &tag, sizeof tag), &vxh, sizeof vxh);
                if (!h) h = 1;
                if (!u64set_add(&seen_jobs, h)) continue;
                jobs[njobs++] = (PipeJob){.job_kind = JOB_LIB_VS, .poly_fill = 1, .hash = h, .cxh = vxh};
            }
            for (size_t fi = 0; fi < n_fx && njobs < max_jobs; fi++) {
                uint64_t fxh = fx_list[fi];
                if (!fxh) continue;
                uint64_t tag = ((uint64_t)JOB_LIB_FS << 56) | (1ull << 8);
                uint64_t h = fnv_mix(fnv_mix(0xcbf29ce484222325ull, &tag, sizeof tag), &fxh, sizeof fxh);
                if (!h) h = 1;
                if (u64set_add(&seen_jobs, h))
                    jobs[njobs++] = (PipeJob){.job_kind = JOB_LIB_FS, .samples = 1, .a2c = 0, .hash = h, .cxh = fxh};
                if (fx_alpha[fi] && njobs < max_jobs) {
                    uint64_t tag_a2c = ((uint64_t)JOB_LIB_FS << 56) | (1ull << 8) | 1ull;
                    uint64_t ha = fnv_mix(fnv_mix(0xcbf29ce484222325ull, &tag_a2c, sizeof tag_a2c), &fxh, sizeof fxh);
                    if (!ha) ha = 1;
                    if (u64set_add(&seen_jobs, ha))
                        jobs[njobs++] = (PipeJob){.job_kind = JOB_LIB_FS, .samples = 1, .a2c = 1, .hash = ha, .cxh = fxh};
                }
            }
            for (size_t gi = 0, ng = njobs; gi < ng && njobs + 2 <= max_jobs; gi++) {
                if (jobs[gi].job_kind != JOB_GFX) continue;
                const PipeKey *pk = &jobs[gi].key;
                uint64_t vxh = prog_slots[jobs[gi].prog_idx].vxh;
                uint64_t fxh = prog_slots[jobs[gi].prog_idx].fxh;
                if (pk->poly_fill != 1) {
                    uint64_t tag = ((uint64_t)JOB_LIB_VS << 56) | (uint64_t)pk->poly_fill;
                    uint64_t h = fnv_mix(fnv_mix(0xcbf29ce484222325ull, &tag, sizeof tag), &vxh, sizeof vxh);
                    if (!h) h = 1;
                    if (u64set_add(&seen_jobs, h))
                        jobs[njobs++] = (PipeJob){.job_kind = JOB_LIB_VS, .poly_fill = pk->poly_fill, .hash = h, .cxh = vxh};
                }
                if (pk->samples != 1 || pk->a2c != 0) {
                    uint64_t tag = ((uint64_t)JOB_LIB_FS << 56) | ((uint64_t)pk->samples << 8) | (uint64_t)pk->a2c;
                    uint64_t h = fnv_mix(fnv_mix(0xcbf29ce484222325ull, &tag, sizeof tag), &fxh, sizeof fxh);
                    if (!h) h = 1;
                    if (u64set_add(&seen_jobs, h))
                        jobs[njobs++] = (PipeJob){.job_kind = JOB_LIB_FS, .samples = pk->samples, .a2c = pk->a2c, .hash = h, .cxh = fxh};
                }
            }
        }

        pso_total = njobs + pso_fail;

        if (njobs > 0 && !g_stop_requested) {
            vk_warm_seed_begin();

            stats.done = 0;
            stats.total = pso_total;
            stats.pso_done = pso_fail;
            stats.pso_total = pso_total;
            stats.pso_fail = pso_fail;
            write_progress(opts->progress_path, "pipelines", &stats);

            printf("[prepare] prewarming %zu pipelines + stage libraries (%zu programs, gpl=%s)...\n",
                   njobs, nprogs, use_gpl ? "on" : "off");
            fflush(stdout);

            const char *fault_env = getenv("ENGAGE_PREPARE_FAULT_AT");
            long fault_at = (fault_env && *fault_env) ? strtol(fault_env, NULL, 10) : -1;

            _Atomic size_t next_idx = 0, a_ok = 0, a_fail = pso_fail, a_skip = 0, a_done = pso_fail;
            _Atomic uint64_t a_last_prog = plat_now_ns();

            PipeWorkerCtx *ctxs = calloc((size_t)nworkers, sizeof *ctxs);
            for (int i = 0; i < nworkers; i++) {
                ctxs[i] = (PipeWorkerCtx){
                    .worker_id = i,
                    .jfd = jfd,
                    .fault_at = fault_at,
                    .jobs = jobs,
                    .njobs = njobs,
                    .progs = prog_slots,
                    .skip_set = &skip_set,
                    .next_idx = &next_idx,
                    .ok_cnt = &a_ok,
                    .fail_cnt = &a_fail,
                    .skip_cnt = &a_skip,
                    .done_cnt = &a_done,
                    .last_prog_ns = &a_last_prog,
                    .progress_path = opts->progress_path,
                    .base_stats = &stats,
                };
                pthread_create(&threads[i], NULL, pipe_worker, &ctxs[i]);
            }
            for (int i = 0; i < nworkers; i++) pthread_join(threads[i], NULL);
            free(ctxs);

            pso_ok = atomic_load(&a_ok);
            pso_fail = atomic_load(&a_fail);
            pso_skip = atomic_load(&a_skip);
            pso_done = atomic_load(&a_done);

            vk_pipeline_cache_save();
            vk_warm_seed_end();
        }

        for (size_t u = 0; u < nprogs; u++) vk_offline_prog_destroy(prog_slots[u].op);

        if (jfd >= 0) close(jfd);
        remove(JR_PATH);

        free(seen_jobs.tab);
        free(jobs);
        free(prog_map.tab);
        free(prog_slots);
        free(skip_set.tab);
    }

    if (!g_stop_requested && n_wp > 0) {
        mkdir("cache", 0755);
        FILE *wf = fopen("cache/warm_pairs.bin.tmp", "wb");
        if (wf) {
            uint32_t whdr[4] = {0x53525057u, 1u, (uint32_t)sizeof(WarmPairDiskRec), (uint32_t)n_wp};
            bool ok = fwrite(whdr, sizeof whdr, 1, wf) == 1 &&
                      fwrite(wp_list, sizeof *wp_list, n_wp, wf) == n_wp;
            if (fclose(wf) != 0) ok = false;
            if (ok) { remove("cache/warm_pairs.bin"); if (rename("cache/warm_pairs.bin.tmp", "cache/warm_pairs.bin") != 0) remove("cache/warm_pairs.bin.tmp"); }
            else remove("cache/warm_pairs.bin.tmp");
        }
    }

    free(threads);

    bool cancelled = g_stop_requested != 0;
    stats.done = opts->no_pipelines ? done_units : pso_done;
    stats.total = opts->no_pipelines ? total_units : pso_total;
    stats.pso_done = pso_done;
    stats.pso_total = pso_total;
    stats.pso_ok = pso_ok;
    stats.pso_fail = pso_fail;
    stats.pso_skip = pso_skip;
    write_progress(opts->progress_path, cancelled ? "cancelled" : "done", &stats);

    double elapsed = (double)(plat_now_ns() - t0) / 1e9;
    printf("[prepare] %s in %.2f s: units=%zu/%zu xlat(hits=%llu new=%llu fail=%zu) spv(hits=%zu new=%zu fail=%zu) "
           "pso(ok=%zu fail=%zu skip=%zu total=%zu)\n",
           cancelled ? "cancelled" : "done", elapsed, done_units, total_units,
           (unsigned long long)stats.xlat_hits, (unsigned long long)stats.xlat_new, stats.xlat_fail,
           stats.spv_hits, stats.spv_new, stats.spv_fail,
           pso_ok, pso_fail, pso_skip, pso_total);
    fflush(stdout);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);

    free(seen_fs.tab);
    free(seen_spv.tab);
    free(seen_fx.tab);
    free(seen_wp.tab);
    free(fs_xlat.tab);
    free(vs_state);
    free(vs_out_mask);
    free(vs_xlat);
    free(cs_xlat);
    free(fx_list);
    free(fx_alpha);
    free(wp_list);
    for (uint32_t i = 0; i < hdr.nshaders; i++) free(shaders[i].code);
    free(shaders);
    free(pairs);
    free(psos);
    free(computes);

    vk_shutdown_headless();
    return cancelled ? 130 : 0;
}
