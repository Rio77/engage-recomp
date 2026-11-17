#include <assert.h>
#include "../runtime/nvn/vulkan/exec.c"

Settings g_settings;
void rt_fatal_trace(void) { abort(); }
int main(void) {
    NvnProgram vertex = {.stage_mask = 1}, fragment = {.stage_mask = 2};
    vertex.stages[0].code_addr = 0x10000; vertex.stages[0].revision = 1;
    fragment.stages[1].code_addr = 0x20000; fragment.stages[1].revision = 2;
    bind_program(&vertex, 1); bind_program(&fragment, 2);
    NvnProgram *first = st.program;
    ShaderKey vk = {0}, fk = {0};
    uint64_t key = program_key(first, &vk, &fk);
    fragment.stages[1].revision = 3;
    bind_program(&fragment, 2);
    assert(st.program != first);
    assert(st.program->stages[1].revision == 3);
    assert(program_key(st.program, &vk, &fk) != key);
    NvnProgram *second = st.program;
    bind_program(&fragment, 2);
    assert(st.program == second);
    puts("PASS: merged stages and graphics cache distinguish shader reuploads");
}
