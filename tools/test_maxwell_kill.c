#include "shader.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool check(bool matched, bool clear, bool glsl, bool print_source) {
    uint64_t program[32] = {0};
    program[0x78 / 8] = 0xf;
    uint64_t *code = program + 0x80 / 8;
    code[0] = UINT64_C(0x001f8000fc0007e0);
    code[1] = UINT64_C(0xe33000000000000f);  // @P0 KIL
    code[2] = UINT64_C(0x5c9807800ff00000);  // @P0 MOV R0,RZ
    code[3] = UINT64_C(0x5c9807800ff00001);  // @P0 MOV R1,RZ
    code[4] = UINT64_C(0x001f8000fc0007e0);  // scheduling word between KIL and EXIT
    code[5] = UINT64_C(0xe30000000000000f) | (matched ? 0 : UINT64_C(0x10000));
    code[6] = UINT64_C(0xc0380007a0070000);  // implicit 2D sample
    code[7] = UINT64_C(0xe30000000007000f);  // unconditional exit
    code[8] = UINT64_C(0x001f8000fc0007e0);
    code[9] = UINT64_C(0xe2400fffff87000f);
    if (!clear) {
        code[2] = code[5];
        code[3] = code[6];
        code[5] = code[7];
        code[6] = code[9];
    }
    ShaderKey key = {.glsl = glsl};
    ShaderInfo *shader = maxwell_translate((const uint8_t *)program, sizeof program, STAGE_FRAGMENT, &key);
    const char *source = shader->msl;
    bool valid = source && shader->uses_discard;
    if (valid && glsl) {
        valid = strstr(source, "texture(t0,") && strstr(source, "    break;");
        if (matched && clear)
            valid = valid && strstr(source, "demote;") && !strstr(source, "helperInvocationEXT()") &&
                    !strstr(source, "discard;");
        else
            valid = valid && strstr(source, "discard;") && !strstr(source, "helperInvocationEXT()");
    } else if (valid) {
        valid = strstr(source, "discard_fragment();") && !strstr(source, "demote;");
    }
    if (!valid) fprintf(stderr, "incorrect KIL/EXIT translation (matching=%d, glsl=%d)\n",
                        matched, glsl);
    if (valid && print_source) fputs(source, stdout);
    maxwell_free(shader);
    return valid;
}

int main(int argc, char **argv) {
    bool valid = check(false, true, true, false);
    valid &= check(true, false, true, false);
    valid &= check(true, true, false, false);
    valid &= check(true, true, true, argc == 2 && strcmp(argv[1], "--glsl") == 0);
    return !valid;
}
