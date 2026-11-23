/* Unit tests for SeedPsoRec -> PipeKey conversion (Phase 3). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/nvn/vulkan/pipe_key.h"

static VkFormat mock_vfmt_support(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return f;
    }
}

static SeedPsoRec base_pso(void) {
    SeedPsoRec r = {0};
    r.samples = 1;
    r.chmask = 0xffffffffu;
    r.topo_class = 2;
    r.poly_fill = 1;
    return r;
}

static void test_blend_and_chmask(void) {
    SeedPsoRec r = base_pso();
    r.mtl_cfmt[0] = 71; /* RGBA8Unorm_sRGB */
    r.mtl_dfmt = 260;
    r.mtl_sfmt = 260;
    r.blend_en = 0xff;
    r.chmask = 0xfffffff7u;
    r.blend[0][0] = 0;
    r.blend[0][1] = 2; r.blend[0][2] = 6; /* ONE / ONE_MINUS_SRC_ALPHA */
    r.blend[0][3] = 1; r.blend[0][4] = 6;
    r.blend[0][5] = 1; r.blend[0][6] = 1;
    r.blend[1][1] = 5; /* stale unused slot */

    PipeKey k;
    assert(vk_seed_to_pipe_key(&r, 0, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
    assert(k.ncolor == 1);
    assert(k.color[0] == VK_FORMAT_R8G8B8A8_SRGB);
    assert(k.depth == VK_FORMAT_D32_SFLOAT_S8_UINT);
    assert(k.stencil == VK_FORMAT_D32_SFLOAT_S8_UINT);
    assert(k.blend_on == 1);
    assert(memcmp(k.blend[0], r.blend[0], 8) == 0);
    for (int i = 1; i < 8; i++) {
        assert(k.write_mask[i] == 0);
        for (int j = 0; j < 8; j++) assert(k.blend[i][j] == 0);
    }
    assert(k.write_mask[0] == 7);

    /* FS does not write RT0 -> write_mask[0] must be 0. */
    assert(vk_seed_to_pipe_key(&r, 0, 0x0u, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
    assert(k.write_mask[0] == 0);

    /* D24S8 GPU test */
    assert(vk_seed_to_pipe_key(&r, 0, 0xFu, VK_FORMAT_D24_UNORM_S8_UINT, true, mock_vfmt_support, &k));
    assert(k.depth == VK_FORMAT_D24_UNORM_S8_UINT);
    assert(k.stencil == VK_FORMAT_D24_UNORM_S8_UINT);
}

static void test_depth_only_and_samples(void) {
    SeedPsoRec r = base_pso();
    r.mtl_dfmt = 250; /* Depth16Unorm */
    r.mtl_sfmt = 0;
    r.samples = 2;
    r.blend_en = 0xff;
    r.chmask = 0xfffffff0u;

    PipeKey k;
    assert(vk_seed_to_pipe_key(&r, 0, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
    assert(k.ncolor == 0);
    assert(k.depth == VK_FORMAT_D16_UNORM);
    assert(k.stencil == VK_FORMAT_UNDEFINED);
    assert(k.samples == 2);
    assert(k.blend_on == 0);
    assert(k.write_mask[0] == 0);
}

static void test_vertex_attribs_and_divisor(void) {
    SeedPsoRec r = base_pso();
    r.mtl_cfmt[0] = 70; /* RGBA8Unorm */
    /* Attr 0: bound to stream 0 */
    r.attr_fmt[0] = 0x22; /* R32G32B32_SFLOAT */
    r.attr_off[0] = 12;
    r.attr_bind[0] = 0;
    /* Attr 1: bound to stream 2 with unsupported 3-comp 16f */
    r.attr_fmt[1] = 0x1D; /* R16G16B16_SFLOAT -> mocked to R16G16B16A16_SFLOAT */
    r.attr_off[1] = 0;
    r.attr_bind[1] = 2;
    /* Attr 3: fmt == 0 (unbound -> ZERO_BINDING) */
    r.attr_fmt[3] = 0;
    r.attr_bind[3] = 1;
    /* Attr 5: stream >= 16 (unbound -> ZERO_BINDING) */
    r.attr_fmt[5] = 0x2E;
    r.attr_bind[5] = 16;
    /* Attr 7: valid, but masked out by vs_attr_mask */
    r.attr_fmt[7] = 0x2E;
    r.attr_bind[7] = 3;

    r.stream_stride[0] = 24;
    r.stream_divisor[0] = 0;
    r.stream_stride[2] = 16;
    r.stream_divisor[2] = 4;
    r.stream_stride[3] = 32;
    r.stream_divisor[3] = 2;

    uint32_t vs_mask = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 5);
    PipeKey k;
    assert(vk_seed_to_pipe_key(&r, vs_mask, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, false, mock_vfmt_support, &k));
    assert(k.attr_mask == vs_mask);
    assert(k.attr_fmt[0] == VK_FORMAT_R32G32B32_SFLOAT && k.attr_bind[0] == 0 && k.attr_off[0] == 12);
    assert(k.attr_fmt[1] == VK_FORMAT_R16G16B16A16_SFLOAT && k.attr_bind[1] == 2);
    assert(k.attr_fmt[3] == VK_FORMAT_R32G32B32A32_SFLOAT && k.attr_bind[3] == ZERO_BINDING);
    assert(k.attr_fmt[5] == VK_FORMAT_R32G32B32A32_SFLOAT && k.attr_bind[5] == ZERO_BINDING);
    assert(k.attr_fmt[7] == 0);
    assert(k.used_streams == ((1u << 0) | (1u << 2) | (1u << ZERO_BINDING)));
    assert(k.stride[0] == 24 && k.divisor[0] == 0);
    assert(k.stride[2] == 16 && k.divisor[2] == 1); /* clamped when !has_divisor */
    assert(k.stride[3] == 0); /* stream 3 unused */

    /* With divisor extension supported, divisor=4 is kept. */
    assert(vk_seed_to_pipe_key(&r, vs_mask, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
    assert(k.divisor[2] == 4);
}

static void test_int_rt_and_unknown_fmt(void) {
    SeedPsoRec r = base_pso();
    r.mtl_cfmt[0] = 74; /* RGBA8Sint -> rt_kind=1, blend disabled */
    r.blend_en = 0xff;
    PipeKey k;
    assert(vk_seed_to_pipe_key(&r, 0, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
    assert(k.color[0] == VK_FORMAT_R8G8B8A8_SINT);
    assert(k.blend_on == 0);

    r.mtl_cfmt[0] = 9999;
    assert(mtl_to_vk_color_format(9999) == VK_FORMAT_UNDEFINED);
    assert(!vk_seed_to_pipe_key(&r, 0, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
}

static void test_real_seed_if_provided(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    SeedHeader hdr;
    assert(fread(&hdr, sizeof hdr, 1, f) == 1);
    assert(memcmp(hdr.magic, SEED_MAGIC, 8) == 0 && hdr.version == SEED_VERSION);
    for (uint32_t i = 0; i < hdr.nshaders; i++) {
        SeedShaderRec sr;
        assert(fread(&sr, sizeof sr, 1, f) == 1);
        assert(fseek(f, (long)sr.code_len, SEEK_CUR) == 0);
    }
    assert(fseek(f, (long)((size_t)hdr.npairs * sizeof(SeedPairRec)), SEEK_CUR) == 0);
    uint32_t converted = 0;
    for (uint32_t i = 0; i < hdr.npsos; i++) {
        SeedPsoRec p;
        assert(fread(&p, sizeof p, 1, f) == 1);
        PipeKey k;
        assert(vk_seed_to_pipe_key(&p, 0xFFFFFFFFu, 0xFu, VK_FORMAT_D32_SFLOAT_S8_UINT, true, mock_vfmt_support, &k));
        converted++;
    }
    fclose(f);
    printf("test_pso_convert: converted %u/%u seed PSOs successfully\n", converted, hdr.npsos);
}

int main(int argc, char **argv) {
    test_blend_and_chmask();
    test_depth_only_and_samples();
    test_vertex_attribs_and_divisor();
    test_int_rt_and_unknown_fmt();
    if (argc > 1) test_real_seed_if_provided(argv[1]);
    printf("test_pso_convert: ALL PASSED\n");
    return 0;
}
