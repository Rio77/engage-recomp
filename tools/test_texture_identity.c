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
    NvnTexture *texture = calloc(1, sizeof *texture);
    texture->debug_id = 123; texture->levels = 4;
    texture_identity_add(texture);
    original[0] = (uintptr_t)texture; original[1] = 123;
    assert(HOST(NvnTexture, (uintptr_t)original) == texture);
    memcpy(copy, original, sizeof copy);
    cpu.x[0] = (uintptr_t)original;
    hle_nvnTextureFinalize(&cpu);
    assert(!nvn_texture_host((uintptr_t)original));
    assert(destroyed == 1 && nvn_texture_host((uintptr_t)copy) == texture);
    cpu.x[0] = (uintptr_t)copy;
    hle_nvnTextureGetLevels(&cpu);
    assert(cpu.x[0] == 4);
    copy[1]++;
    assert(!nvn_texture_host((uintptr_t)copy));
    texture_identities_reset();
    assert(!nvn_texture_host((uintptr_t)copy));
    puts("PASS: pending, malformed, copied, finalized and stale NVN texture identities");
}
