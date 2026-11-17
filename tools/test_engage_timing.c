#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engage_timing.h"
#include "settings.h"
Settings g_settings = {.engage_fps = 60};
void guest_call(Cpu *c, uint64_t address) { (void)c; (void)address; }

void rt_fatal_trace(void) { exit(99); }

int main(int argc, char **argv) {
    assert(argc == 2);
    int hz = atoi(argv[1]);
    Cpu c = {0};
    c.x[19] = 7;
    engage_timing_site(&c, 0x32f92d4);
    assert(c.x[19] == (hz == 60 ? 1 : 2));
    const uint64_t sites[] = {0x2a5e870, 0x2a5e8d0, 0x2a5e930, 0x2a5e990};
    for (int i = 0; i < 4; ++i) {
        float initial = 4.0f, result;
        c.x[0] = 0x1234;
        memcpy(&c.v[0], &initial, sizeof initial);
        engage_timing_site(&c, sites[i]);
        memcpy(&result, &c.v[0], sizeof result);
        float scale = hz == 30 ? 1.0f : (i == 1 || i == 2 ? 0.25f : 0.5f);
        assert(fabsf(result - initial * scale) < 0.000001f);
        assert(c.x[0] == 0x1234);
        float power = i == 1 || i == 2 ? hz * hz : hz;
        assert(fabsf(result * power - initial * (i == 1 || i == 2 ? 900 : 30)) < 0.001f);
    }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < 10; ++i) engage_present_pace();
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    if (hz == 30) assert(elapsed >= 0.29);
    return 0;
}
