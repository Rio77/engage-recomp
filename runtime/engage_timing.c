#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "rt.h"
#include "engage_timing.h"
#include "settings.h"

static pthread_once_t timing_once = PTHREAD_ONCE_INIT;
static atomic_int timing_hz, requested_hz;
static atomic_flag switching = ATOMIC_FLAG_INIT;
/* Bits of float k = smoothed_dt * 30.0f (0.5f at 60 FPS -> 1.0f at 30 FPS). */
static atomic_uint_least32_t frame_scale_bits = 0x3f000000u; /* 0.5f */
static uint64_t prev_present_ns;
static double smooth_dt = 1.0 / 60.0;

static bool dynamic_timing_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("ENGAGE_DYNAMIC_TIMING");
        on = (e && !strcmp(e, "0")) ? 0 : 1;
    }
    return on != 0;
}

static uint32_t float_to_bits(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

static float bits_to_float(uint32_t bits) {
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void timing_init(void) {
    const char *value = getenv("ENGAGE_FPS");
    if (value && strcmp(value, "30") && strcmp(value, "60"))
        FATAL("ENGAGE_FPS must be 30 or 60\n");
    int hz = value ? (!strcmp(value, "30") ? 30 : 60) : (g_settings.engage_fps == 30 ? 30 : 60);
    atomic_store(&timing_hz, hz);
    atomic_store(&requested_hz, hz);
    atomic_store(&frame_scale_bits, float_to_bits(hz == 60 ? 0.5f : 1.0f));
    LOG("[engage-timing] %d Hz (live switching enabled, dynamic dt=%s)\n",
        hz, dynamic_timing_on() ? "on" : "off");
}
int engage_timing_fps(void) {
    pthread_once(&timing_once, timing_init);
    return atomic_load(&requested_hz);
}
void engage_timing_set_fps(int hz) {
    pthread_once(&timing_once, timing_init);
    if (hz == 30 || hz == 60) atomic_store(&requested_hz, hz);
}
void engage_timing_apply_dt(Cpu *c, double dt_seconds) {
    pthread_once(&timing_once, timing_init);
    // Apply Unity's vSyncCount from guest presentation, never directly from the
    // host UI callback. Preserve all guest registers across the nested call.
    int cur = atomic_load(&timing_hz);
    int req = atomic_load(&requested_hz);
    if (req != cur && !atomic_flag_test_and_set(&switching)) {
        cur = atomic_load(&requested_hz);
        atomic_store(&timing_hz, cur);
        Cpu tmp = *c;
        tmp.x[0] = cur == 60 ? 1 : 2;
        guest_call(&tmp, MAIN_BASE + 0x32f92c0);
        smooth_dt = cur == 60 ? (1.0 / 60.0) : (1.0 / 30.0);
        atomic_store(&frame_scale_bits, float_to_bits(cur == 60 ? 0.5f : 1.0f));
        LOG("[engage-timing] applied %d Hz from guest presentation\n", cur);
        atomic_flag_clear(&switching);
        return;
    }
    if (cur != 60 || !dynamic_timing_on()) {
        atomic_store(&frame_scale_bits, float_to_bits(cur == 60 ? 0.5f : 1.0f));
        return;
    }
    // Ignore pause/menu/load jumps (> 100 ms) so unpausing doesn't overshoot.
    double clamped = (dt_seconds <= 0.0 || dt_seconds > 0.100) ? (1.0 / 60.0) : dt_seconds;
    smooth_dt += 0.35 * (clamped - smooth_dt);
    float k = (float)(smooth_dt * 30.0);
    if (k < 0.25f) k = 0.25f;
    if (k > 1.25f) k = 1.25f;
    atomic_store_explicit(&frame_scale_bits, float_to_bits(k), memory_order_relaxed);
}
void engage_timing_apply(Cpu *c) {
    uint64_t now = monotonic_ns();
    double dt = prev_present_ns ? (double)(now - prev_present_ns) / 1e9 : (1.0 / 60.0);
    prev_present_ns = now;
    engage_timing_apply_dt(c, dt);
}
void engage_timing_site(Cpu *c, uint64_t offset) {
    pthread_once(&timing_once, timing_init);
    int hz = atomic_load(&timing_hz);
    if (offset == 0x32f92d4) { c->x[19] = hz == 60 ? 1 : 2; return; }
    if (hz != 60) return;
    float k = dynamic_timing_on() ? bits_to_float(atomic_load_explicit(&frame_scale_bits, memory_order_relaxed)) : 0.5f;
    float value;
    memcpy(&value, &c->v[0], sizeof value);
    value *= (offset == 0x2a5e8d0 || offset == 0x2a5e930) ? (k * k) : k;
    memcpy(&c->v[0], &value, sizeof value);
}
// NVN window intervals were stored but not paced by this backend. The game's
// fixed 30 Hz tick must also be presented at 30 Hz; changing only vSyncCount
// leaves a 60 Hz producer and doubles the simulation speed. The existing VI
// clock already paces 60 Hz, so leave that proven path unchanged.
void engage_present_pace(void) {
    pthread_once(&timing_once, timing_init);
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t deadline;
    const uint64_t period = 1000000000ull / 30;
    pthread_mutex_lock(&lock);
    if (atomic_load(&timing_hz) != 30) {
        deadline = 0;
        pthread_mutex_unlock(&lock);
        return;
    }
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    uint64_t now = (uint64_t)time.tv_sec * 1000000000ull + time.tv_nsec;
    if (deadline && now < deadline) {
        struct timespec until = {.tv_sec = deadline / 1000000000ull,
                                 .tv_nsec = deadline % 1000000000ull};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {}
        clock_gettime(CLOCK_MONOTONIC, &time);
        now = (uint64_t)time.tv_sec * 1000000000ull + time.tv_nsec;
    }
    // Discard accumulated debt after a load/hitch instead of presenting a burst.
    deadline = !deadline || now > deadline + period ? now + period : deadline + period;
    pthread_mutex_unlock(&lock);
}
