/* Check the Engage opcodes, their descriptor pair and the generated Vulkan shader. */
#include "shader.h"
#include <assert.h>
#include <math.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <string.h>

static void test_op(shaderc_compiler_t compiler, uint64_t op, unsigned handle, const char *lookup)
{
    uint8_t blob[0xc0] = {0};
    uint64_t code[] = {
        0, 0x4c98078805a70000ull | handle,
        0x4c47020815a70000ull | (handle << 8) | handle,
        op, 0, 0xe30000000007000full
    };
    memcpy(blob + 0x80, code, sizeof code);
    ShaderKey key = {.glsl = 1};
    ShaderInfo *shader = maxwell_translate(blob, 0x80 + sizeof code, STAGE_FRAGMENT, &key);
    if (shader && !shader->msl) fprintf(stderr, "%s\n", shader->error);
    assert(shader && shader->msl);
    assert(shader->ntex == 1);
    assert(shader->tex[0].handle_offset == 0x168 && shader->tex[0].bindless_offset == 0x568);
    assert(shader->tex[0].type == ((op >> 50) & 1 ? TEXTYPE_2D_MS : TEXTYPE_2D));
    assert(strstr(shader->msl, lookup));
    shaderc_compilation_result_t result = shaderc_compile_into_spv(
        compiler, shader->msl, strlen(shader->msl), shaderc_fragment_shader, "engage", "main", NULL);
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
        fputs(shaderc_result_get_error_message(result), stderr);
    assert(shaderc_result_get_compilation_status(result) == shaderc_compilation_status_success);
    shaderc_result_release(result);
    maxwell_free(shader);
}

int main(void)
{
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    assert(compiler);
    test_op(compiler, 0xdd3a0007a0c70600ull, 12, "texelFetch(t0");
    test_op(compiler, 0xdefa0007a0570208ull, 5, "textureGather(t0");
    test_op(compiler, 0xdd3e0000a0670403ull, 6, "sampler2DMS");
    /* Both branches define the same packed handle before the fetch. */
    uint64_t words[16] = {0};
    words[1] = 0x4c98078805a7000aull;
    words[2] = 0x4c47020815a70a0aull;
    words[3] = 0xe24000000280000full;
    words[5] = 0x5c98078000a7000cull;
    words[6] = 0xe24000000307000full;
    words[7] = 0x50b0000000070f00ull;
    words[9] = 0x5c98078000a7000cull;
    words[10] = 0xe24000000107000full;
    /* Low bits encode predicate outputs, not an overwrite of r12. */
    words[11] = 0x4bb4839401a7020cull;
    words[13] = 0xdd3a0007a0c70600ull;
    words[14] = 0xe30000000007000full;
    uint8_t blob[0x100] = {0};
    memcpy(blob + 0x80, words, sizeof words);
    ShaderKey key = {.glsl = 1};
    ShaderInfo *diamond = maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &key);
    if (diamond && !diamond->msl) fprintf(stderr, "%s\n", diamond->error);
    assert(diamond && diamond->msl);
    assert(diamond->ntex == 1 && diamond->tex[0].handle_offset == 0x168);
    shaderc_compilation_result_t merged = shaderc_compile_into_spv(
        compiler, diamond->msl, strlen(diamond->msl), shaderc_fragment_shader, "diamond", "main", NULL);
    if (shaderc_result_get_compilation_status(merged) != shaderc_compilation_status_success)
        fputs(shaderc_result_get_error_message(merged), stderr);
    assert(shaderc_result_get_compilation_status(merged) == shaderc_compilation_status_success);
    shaderc_result_release(merged);
    maxwell_free(diamond);
    /* The taken branch now supplies a different descriptor. Reject the merge. */
    words[9] = 0x4c98078807a7000cull;
    memcpy(blob + 0x80, words, sizeof words);
    diamond = maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &key);
    assert(diamond && !diamond->msl);
    maxwell_free(diamond);
    /* A path without a descriptor must not silently inherit another path's handle. */
    words[9] = 0x5c9807800ff7000cull;
    memcpy(blob + 0x80, words, sizeof words);
    diamond = maxwell_translate(blob, sizeof blob, STAGE_FRAGMENT, &key);
    assert(diamond && !diamond->msl);
    maxwell_free(diamond);

    /* MapCloud (@P0 MOV + @P0 LOP.OR + @P0 TEX.B) must resolve when P0 is preserved. */
    uint64_t mapcloud[8] = {0};
    mapcloud[1] = 0x4c98078000000020ull | (2ull << 34) | ((0x190ull / 4ull) << 20);
    mapcloud[2] = 0x4c47020000002020ull | (2ull << 34) | ((0x590ull / 4ull) << 20);
    mapcloud[3] = 0xdeb8000000000000ull | (0xFull << 31) | (1ull << 29) | (32ull << 20) | (14ull << 8) | 14ull;
    mapcloud[5] = 0xe30000000007000full;
    uint8_t mc_blob[0x100] = {0};
    mc_blob[0x30 + 0x48] = 0x7; /* omap_target: color0 */
    memcpy(mc_blob + 0x80, mapcloud, sizeof mapcloud);
    ShaderInfo *mc = maxwell_translate(mc_blob, sizeof mc_blob, STAGE_FRAGMENT, &key);
    if (mc && !mc->msl) fprintf(stderr, "mapcloud err: %s\n", mc->error);
    assert(mc && mc->msl);
    assert(mc->ntex == 1);
    assert(mc->tex[0].handle_offset == 0x190 && mc->tex[0].bindless_offset == 0x590);
    shaderc_compilation_result_t mc_spv = shaderc_compile_into_spv(
        compiler, mc->msl, strlen(mc->msl), shaderc_fragment_shader, "mapcloud", "main", NULL);
    if (shaderc_result_get_compilation_status(mc_spv) != shaderc_compilation_status_success)
        fputs(shaderc_result_get_error_message(mc_spv), stderr);
    assert(shaderc_result_get_compilation_status(mc_spv) == shaderc_compilation_status_success);
    shaderc_result_release(mc_spv);
    maxwell_free(mc);

    /* If P0 is overwritten (e.g. ISETP P0) between @P0 MOV and @P0 TEX.B, reject it. */
    mapcloud[5] = mapcloud[3];
    mapcloud[6] = 0xe30000000007000full;
    mapcloud[3] = 0x5b6003800000ff00ull; /* ISETP P0 */
    memcpy(mc_blob + 0x80, mapcloud, sizeof mapcloud);
    ShaderInfo *mc_clobber = maxwell_translate(mc_blob, sizeof mc_blob, STAGE_FRAGMENT, &key);
    assert(mc_clobber && !mc_clobber->msl);
    maxwell_free(mc_clobber);

    shaderc_compiler_release(compiler);
    puts("PASS: TLD.B and TLD4.B translated and compiled with the expected descriptor pair");
}
