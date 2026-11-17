#include <assert.h>
#include "../runtime/nvn/cmdbuf.c"

static uint64_t handles[2] = {11, 22};
static _Alignas(16) unsigned char memory[4096];
static NvnCmdBuf buffer;
static unsigned callbacks;
void rt_fatal_trace(void) { abort(); }
void guest_call(Cpu *c, uint64_t address) {
    (void)c; assert(address == 1); callbacks++;
    handles[0] = 33; handles[1] = 44;
    buffer.mem = (uintptr_t)memory; buffer.size = sizeof memory; buffer.used = 0;
}
int main(void) {
    uint64_t guest[16] = {(uintptr_t)&buffer};
    buffer.guest = (uintptr_t)guest; buffer.callback = 1;
    memset(memory, 0xcc, sizeof memory);
    Cpu cpu = {0};
    cpu.x[0] = (uintptr_t)guest; cpu.x[1] = 1; cpu.x[2] = 0;
    cpu.x[3] = 2; cpu.x[4] = (uintptr_t)handles;
    hle_nvnCommandBufferBindSeparateTextures(&cpu);
    CmdRec *first = (CmdRec *)memory;
    assert(callbacks == 1 && first->x[0] == 1 && first->x[2] == 2);
    uint64_t copied[2]; memcpy(copied, first->extra, sizeof copied);
    assert(copied[0] == 11 && copied[1] == 22);
    unsigned next = buffer.used;
    cpu.x[0] = (uintptr_t)guest; cpu.x[1] = 1; cpu.x[2] = 0;
    cpu.x[3] = 2; cpu.x[4] = 0;
    hle_nvnCommandBufferBindTextures(&cpu);
    CmdRec *second = (CmdRec *)(memory + next);
    memcpy(copied, second->extra, sizeof copied);
    assert(callbacks == 1 && copied[0] == 0 && copied[1] == 0);
    puts("PASS: command callbacks preserve input arrays; null bindings are zeroed");
}
