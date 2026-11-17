#include <assert.h>
#include "../runtime/nvn/objects.c"

void rt_fatal_trace(void) { abort(); }
int main(void) {
    uint64_t guest[16] = {0};
    uint32_t vertex[256] = {0x12345678}, fragment[256] = {0x12345678};
    uint32_t control[512] = {0};
    vertex[0x30 / 4] = 1 << 10;
    fragment[0x30 / 4] = 5 << 10;
    control[0x700 / 4] = 0x100;
    uint64_t data[] = {(uintptr_t)vertex, (uintptr_t)control, (uintptr_t)fragment, (uintptr_t)control};
    Cpu cpu = {0};
    cpu.x[0] = (uintptr_t)guest;
    hle_nvnProgramInitialize(&cpu);
    cpu.x[0] = (uintptr_t)guest; cpu.x[1] = 2; cpu.x[2] = (uintptr_t)data;
    hle_nvnProgramSetShaders(&cpu);
    NvnProgram *program = HOST(NvnProgram, (uintptr_t)guest);
    uint64_t first = program->stages[1].revision;
    assert(first && program->stages[0].revision == first);
    fragment[32] = 123; // different code uploaded to exactly the same address
    cpu.x[0] = (uintptr_t)guest; cpu.x[1] = 2; cpu.x[2] = (uintptr_t)data;
    hle_nvnProgramSetShaders(&cpu);
    NvnProgram *updated = HOST(NvnProgram, (uintptr_t)guest);
    assert(updated != program && program->stages[1].revision == first);
    assert(updated->stages[1].code_addr == (uintptr_t)fragment);
    assert(updated->stages[1].revision != first);
    cpu.x[0] = (uintptr_t)guest;
    hle_nvnProgramFinalize(&cpu);
    assert(updated->stages[1].code_addr == (uintptr_t)fragment && program->stages[1].revision == first);
    program_identities_reset();
    puts("PASS: shader reuploads receive new revisions at reused addresses");
}
