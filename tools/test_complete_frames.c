#include <assert.h>
#include <unistd.h>
#include "../runtime/nvn/vulkan/exec.c"

Settings g_settings;
void rt_fatal_trace(void) { _exit(99); }
static _Atomic int resource;
static void *finish_resource(void *arg) {
    if (*(int *)arg == 0) { SDL_Delay(31000); return NULL; }
    SDL_Delay(30);
    set_state(&resource, *(int *)arg);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("ENGAGE_COMPLETE_FRAMES", "1", 1);
    uint64_t waited = 0;
    atomic_store(&resource, 1);
    assert(wait_built(&resource, &waited));
    atomic_store(&resource, 0);
    int outcome = !strcmp(argv[1], "timeout") ? 0 : !strcmp(argv[1], "fail") ? 2 : 1;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_resource, &outcome) == 0);
    assert(wait_built(&resource, &waited));
    pthread_join(worker, NULL);
    assert(atomic_load(&stat_build_wait_ns) > 0);
    return 0;
}
