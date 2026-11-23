#include <assert.h>
#include "../runtime/nvn/objects.c"

static unsigned destroyed;
void backend_texture_destroyed(NvnTexture *texture) { assert(texture); destroyed++; }
void rt_fatal_trace(void) { abort(); }

int main(void) {
    uint64_t original[16] = {0}, copy[16] = {0};
    Cpu cpu = {0};
    cpu.x[0] = (uintptr_t)original;
    hle_nvnTextureGetLevels(&cpu);
    assert(cpu.x[0] == 0);
    original[0] = 0x53; original[1] = 0x0053467974696e55;
    assert(!nvn_texture_host((uintptr_t)original));
    assert(nvn_texture_lookup((uintptr_t)original).state == TEX_ST_CORRUPT);
    NvnTexture *texture = texture_identity_bind_test((uintptr_t)original, 4);
    assert(HOST(NvnTexture, (uintptr_t)original) == texture);
    memcpy(copy, original, sizeof copy);
    cpu.x[0] = (uintptr_t)original;
    hle_nvnTextureFinalize(&cpu);
    assert(!nvn_texture_host((uintptr_t)original));
    assert(nvn_texture_lookup((uintptr_t)original).state == TEX_ST_FINALIZED_INST);
    assert(destroyed == 1 && nvn_texture_host((uintptr_t)copy) == texture);
    assert(nvn_texture_lookup((uintptr_t)copy).state == TEX_ST_STALE_COPY);
    cpu.x[0] = (uintptr_t)copy;
    hle_nvnTextureGetLevels(&cpu);
    assert(cpu.x[0] == 4);
    copy[1]++;
    assert(!nvn_texture_host((uintptr_t)copy));
    texture_identities_reset();
    assert(!nvn_texture_host((uintptr_t)copy));
    puts("PASS: pending, malformed, copied, finalized and stale NVN texture identities");
}
