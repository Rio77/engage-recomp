#include "shader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    uint64_t program[32] = {0};
    program[0x78 / 8] = 0xf;
    uint64_t *code = program + 0x80 / 8;
    code[0] = UINT64_C(0x001f8000fc0007e0);
    code[1] = UINT64_C(0xc0380007a0070000);  // implicit 2D sample
    code[2] = UINT64_C(0xe30000000007000f);  // unconditional exit
    code[3] = UINT64_C(0xe2400fffff87000f);  // self-branch padding

    ShaderKey glsl = {.glsl = SHADER_KEY_GLSL_BIT};
    ShaderInfo *base = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &glsl);
    assert(base->msl && strstr(base->msl, "texture(t0,"));
    assert(!strstr(base->msl, "depth_eq_tex_") && !base->uses_discard);

    ShaderKey bilateral = {.glsl = SHADER_KEY_GLSL_BIT | SHADER_KEY_GLSL_DEPTH_EQ};
    ShaderInfo *depth_eq = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &bilateral);
    assert(depth_eq->msl && depth_eq->uses_discard);
    assert(depth_eq->ntex == base->ntex);
    assert(strstr(depth_eq->msl, "layout(set = 1, binding = 22) uniform sampler2D depth_eq_tex_;"));
    const char *guard = strstr(depth_eq->msl, "texelFetch(depth_eq_tex_,");
    const char *sample = strstr(depth_eq->msl, "texture(t0,");
    assert(guard && sample && guard < sample);
    assert(strstr(guard, "demote;"));
    assert(strstr(guard, "2.0 / 16777215.0"));  // two 24-bit steps: the hardware bias plus the shaders' difference
    assert(!strstr(guard, "discard;"));

    if (argc == 2 && !strcmp(argv[1], "--glsl")) fputs(depth_eq->msl, stdout);
    maxwell_free(depth_eq);
    maxwell_free(base);

    ShaderInfo *vs_base = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_VERTEX, &glsl);
    ShaderInfo *vs_bilateral = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_VERTEX, &bilateral);
    assert(vs_base->msl && vs_bilateral->msl && !strcmp(vs_base->msl, vs_bilateral->msl));
    maxwell_free(vs_bilateral);
    maxwell_free(vs_base);

    program[0x78 / 8] |= UINT64_C(2) << 32;  // fragment exports depth
    ShaderInfo *depth_writer = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &bilateral);
    assert(!depth_writer->msl && strstr(depth_writer->error, "depth"));
    maxwell_free(depth_writer);

    program[0x78 / 8] = 0xf;
    code[1] = UINT64_C(0xeed400000007ff00);  // LDG.32 R0, [RZ]
    ShaderInfo *heap_reader = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &bilateral);
    assert(heap_reader->msl && heap_reader->uses_gmem && !heap_reader->uses_gmem_store);
    maxwell_free(heap_reader);

    code[1] = UINT64_C(0xeedc00000007ff00);  // STG.32 [RZ], R0
    ShaderInfo *heap_writer = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &glsl);
    assert(heap_writer->msl && heap_writer->uses_gmem && heap_writer->uses_gmem_store);
    maxwell_free(heap_writer);

    heap_writer = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &bilateral);
    assert(!heap_writer->msl && strstr(heap_writer->error, "guest memory writes"));
    maxwell_free(heap_writer);
}
