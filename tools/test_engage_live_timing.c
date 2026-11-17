#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "engage_timing.h"
#include "settings.h"

Settings g_settings = {.engage_fps = 60};
void rt_fatal_trace(void) { exit(99); }
static int calls;
void guest_call(Cpu *c, uint64_t address) {
    assert(address == 0x70032f92c0ull);
    assert(c->x[0] == (engage_timing_fps() == 60 ? 1 : 2));
    c->x[7] = 99;
    calls++;
}
static void check(int hz) {
    engage_timing_set_fps(hz);
    Cpu c = {0};
    float v = 4, result;
    c.x[7] = 42;
    engage_timing_apply(&c);
    memcpy(&c.v[0], &v, 4);
    engage_timing_site(&c, 0x2a5e870);
    memcpy(&result, &c.v[0], 4);
    assert(result == (hz == 60 ? 2 : 4));
    assert(c.x[7] == 42);
    engage_timing_site(&c, 0x32f92d4);
    assert(c.x[19] == (hz == 60 ? 1 : 2));
}
int main(void) {
    check(60); assert(calls == 0);
    check(30); assert(calls == 1);
    check(60); assert(calls == 2);
    engage_timing_set_fps(45);
    assert(engage_timing_fps() == 60);
    puts("live timing transitions PASS");
}
