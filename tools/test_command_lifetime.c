#include <assert.h>
#include "../runtime/nvn/cmdbuf.c"

static unsigned callbacks;
static unsigned char callback_control[4096];
void rt_fatal_trace(void) { abort(); }
void guest_call(Cpu *c, uint64_t address) {
    assert(address == 1 && c->x[1] == 1);
    callbacks++;
    NvnCmdBuf *cb = cb_of(c->x[0]);
    cb->control = (uintptr_t)callback_control;
    cb->control_size = sizeof callback_control; cb->control_used = 0;
}
static NvnCmdBuf *init(Cpu *c, uint64_t *guest, void *budget, void *control) {
    c->x[0] = (uintptr_t)guest; hle_nvnCommandBufferInitialize(c);
    NvnCmdBuf *cb = cb_of((uintptr_t)guest);
    cb->mem = (uintptr_t)budget; cb->size = 4096;
    cb->control = (uintptr_t)control; cb->control_size = control ? 4096 : 0;
    cb->callback = 1;
    return cb;
}
static uint64_t record_fixture(Cpu *c, uint64_t *guest, uint64_t *handles) {
    c->x[0] = (uintptr_t)guest; hle_nvnCommandBufferBeginRecording(c);
    c->x[0] = (uintptr_t)guest; c->x[1] = 1; c->x[2] = 0; c->x[3] = 2; c->x[4] = (uintptr_t)handles;
    hle_nvnCommandBufferBindSeparateTextures(c);
    c->x[0] = (uintptr_t)guest; hle_nvnCommandBufferEndRecording(c);
    return c->x[0];
}
static uint64_t bound(const void *data, uint64_t size) {
    for (uint64_t offset = 0; offset < size;) {
        const CmdRec *r = (const CmdRec *)((const uint8_t *)data + offset);
        assert(r->size >= sizeof *r && r->size <= size - offset);
        assert(r->op != CMD_CALL && r->op != CMD_JUMP);
        if (r->op == CMD_BIND_SEPARATE_TEXTURES) { uint64_t value; memcpy(&value, r->extra, 8); return value; }
        offset += r->size;
    }
    abort();
}
int main(void) {
    Cpu cpu = {0}; uint64_t child[16] = {0}, parent[16] = {0};
    unsigned char shared_command_memory[4096], ctl1[4096], ctl2[4096];
    memset(shared_command_memory, 0xcc, sizeof shared_command_memory);
    init(&cpu, child, shared_command_memory, ctl1);
    uint64_t handles[2] = {11, 22};
    uint64_t first = record_fixture(&cpu, child, handles);
    uint64_t size; const void *lease = nvn_command_acquire(first, &size);
    assert(lease && bound(lease, size) == 11);
    handles[0] = 33;
    uint64_t second = record_fixture(&cpu, child, handles);
    assert(second != first && bound(lease, size) == 11);
    init(&cpu, parent, shared_command_memory, ctl2);
    cpu.x[0] = (uintptr_t)parent; hle_nvnCommandBufferBeginRecording(&cpu);
    uint64_t list[] = {first};
    cpu.x[0] = (uintptr_t)parent; cpu.x[1] = 1; cpu.x[2] = (uintptr_t)list;
    hle_nvnCommandBufferCopyCommands(&cpu);
    cpu.x[0] = (uintptr_t)parent; hle_nvnCommandBufferEndRecording(&cpu);
    uint64_t copied = cpu.x[0];
    cpu.x[0] = (uintptr_t)child; cpu.x[1] = (uintptr_t)ctl1; cpu.x[2] = sizeof ctl1;
    hle_nvnCommandBufferAddControlMemory(&cpu);
    assert(!nvn_command_acquire(first, NULL) && !nvn_command_acquire(second, NULL));
    assert(bound(lease, size) == 11); // retired handles stay alive while submitted replay owns them
    nvn_command_release(first);
    const void *copy = nvn_command_acquire(copied, &size);
    assert(copy && bound(copy, size) == 11);
    nvn_command_release(copied);
    for (unsigned i = 0; i < sizeof shared_command_memory; i++) assert(shared_command_memory[i] == 0xcc);
    cpu.x[0] = (uintptr_t)child; hle_nvnCommandBufferFinalize(&cpu);
    cpu.x[0] = (uintptr_t)parent; hle_nvnCommandBufferFinalize(&cpu);
    assert(nvn_command_acquire(copied, NULL)); nvn_command_release(copied);
    nvn_command_finalize(copied); assert(!nvn_command_acquire(copied, NULL));
    uint64_t automatic[16] = {0}; init(&cpu, automatic, shared_command_memory, NULL);
    uint64_t last = record_fixture(&cpu, automatic, handles);
    assert(last && callbacks == 1);
    cpu.x[0] = (uintptr_t)automatic; hle_nvnCommandBufferFinalize(&cpu);
    command_copies_reset(); assert(!command_copies);
    puts("PASS: independent recordings, command copies, control reuse, pinned replay and control callbacks");
}
