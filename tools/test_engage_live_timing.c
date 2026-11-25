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

    /* Dynamic dt scaling: simulate sustained 40 FPS (25 ms) then a 1.2s F1 pause jump. */
    Cpu dyn = {0};
    for (int i = 0; i < 40; ++i) engage_timing_apply_dt(&dyn, 1.0 / 40.0);
    float vmax = 4.0f, accel = 4.0f;
    memcpy(&dyn.v[0], &vmax, 4);
    engage_timing_site(&dyn, 0x2a5e870);
    float vmax_40;
    memcpy(&vmax_40, &dyn.v[0], 4);
    /* At 40 FPS (dt=1/40), k = 30/40 = 0.75 -> speed scales by 0.75x (4 * 0.75 = 3.0). */
    assert(vmax_40 > 2.95f && vmax_40 < 3.05f);
    memcpy(&dyn.v[0], &accel, 4);
    engage_timing_site(&dyn, 0x2a5e8d0);
    float accel_40;
    memcpy(&accel_40, &dyn.v[0], 4);
    /* Accel scales by k^2 = 0.5625 -> 4 * 0.5625 = 2.25. */
    assert(accel_40 > 2.20f && accel_40 < 2.30f);

    /* Pause / load spike (> 100 ms) must not overshoot above current speed. */
    engage_timing_apply_dt(&dyn, 1.200);
    memcpy(&dyn.v[0], &vmax, 4);
    engage_timing_site(&dyn, 0x2a5e870);
    float vmax_after_pause;
    memcpy(&vmax_after_pause, &dyn.v[0], 4);
    assert(vmax_after_pause <= vmax_40 && vmax_after_pause >= 2.0f);

    puts("live timing transitions PASS");
}
