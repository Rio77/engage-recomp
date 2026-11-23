#pragma once
/* Shared binary seed contract between tools/shader_prep/engage_shader_extract.py
 * and runtime/nvn/vulkan/prepare_shaders.c (offline local shader preparation).
 *
 * All integers are little-endian. No game binary data is stored in the repository;
 * this container is generated locally from the user's own RomFS/ExeFS and deleted
 * after preparation finishes.
 */
#include <stdint.h>

#define SEED_MAGIC "EGSHSEED"
#define SEED_VERSION 1u

enum {
    SEED_FLAG_FLIP_Y = 1u << 0,
    SEED_FLAG_DEPTH_MINUS_ONE = 1u << 1,
};

typedef struct {
    char magic[8];      /* "EGSHSEED" */
    uint32_t version;   /* SEED_VERSION */
    uint32_t flags;     /* SEED_FLAG_* */
    uint32_t nshaders;
    uint32_t npairs;
    uint32_t npsos;
    uint32_t ncompute;
} SeedHeader;

/* Followed by nshaders variable-length shader entries: SeedShaderRec + code_len bytes. */
typedef struct {
    uint8_t dm_key[8];      /* 64-bit NX Native control+code SHA-256 prefix */
    uint32_t stage;         /* 0=VS, 1=FS, 2=GS, 3=HS, 4=DS, 5=CS */
    uint32_t priority;      /* 0=pso_manifest, 1=shader_pairs, 2=other/compute */
    uint32_t const_offset;  /* control+0x700 */
    uint32_t const_size;    /* control+0x6FC */
    uint32_t code_len;      /* byte length of the following Maxwell code blob */
    uint32_t local[3];      /* compute thread group size (from shader_facts / control+0x748) */
} SeedShaderRec;

/* Followed by npairs SeedPairRec entries, ordered by priority (0 before 1). */
typedef struct {
    uint32_t vs_idx;
    uint32_t fs_idx;
    uint32_t priority;
} SeedPairRec;

/* Followed by npsos SeedPsoRec entries (preserving pso_manifest.tsv first-use order). */
typedef struct {
    uint32_t vs_idx;
    uint32_t fs_idx;
    uint32_t mtl_cfmt[8];       /* MTLPixelFormat values from NX Native pso_skey */
    uint32_t mtl_dfmt;
    uint32_t mtl_sfmt;
    uint32_t samples;
    uint32_t blend_en;
    uint32_t chmask;
    uint32_t a2c;
    uint8_t blend[8][8];        /* unpacked NVN blend_st: target, srcC, dstC, srcA, dstA, eqC, eqA, adv */
    uint32_t attr_fmt[32];      /* unpacked NVN va_st: fmt, offset, stream (attr 0..31) */
    uint32_t attr_off[32];
    uint32_t attr_bind[32];
    uint32_t stream_stride[16]; /* NVN vs_st */
    uint32_t stream_divisor[16];
    uint8_t topo_class;         /* 0=point, 1=line, 2=triangle (default 2) */
    uint8_t poly_fill;          /* 1=fill, 0=line (default 1) */
    uint8_t reserved[6];
} SeedPsoRec;

/* Followed by ncompute uint32_t indices into SeedShaderRec[]. */

_Static_assert(sizeof(SeedHeader) == 32, "SeedHeader size mismatch");
_Static_assert(sizeof(SeedShaderRec) == 40, "SeedShaderRec size mismatch");
_Static_assert(sizeof(SeedPairRec) == 12, "SeedPairRec size mismatch");
_Static_assert(sizeof(SeedPsoRec) == 648, "SeedPsoRec size mismatch");
