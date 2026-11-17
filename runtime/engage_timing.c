#include <pthread.h>
#include <errno.h>
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
static void timing_init(void) {
    const char *value = getenv("ENGAGE_FPS");
    if (value && strcmp(value, "30") && strcmp(value, "60"))
        FATAL("ENGAGE_FPS must be 30 or 60\n");
    int hz = value ? (!strcmp(value, "30") ? 30 : 60) : (g_settings.engage_fps == 30 ? 30 : 60);
    atomic_store(&timing_hz, hz);
    atomic_store(&requested_hz, hz);
    LOG("[engage-timing] %d Hz (live switching enabled)\n", hz);
}
int engage_timing_fps(void) {
    pthread_once(&timing_once, timing_init);
    return atomic_load(&requested_hz);
}
void engage_timing_set_fps(int hz) {
    pthread_once(&timing_once, timing_init);
    if (hz == 30 || hz == 60) atomic_store(&requested_hz, hz);
}
void engage_timing_apply(Cpu *c) {
    pthread_once(&timing_once, timing_init);
    // Apply Unity's vSyncCount from guest presentation, never directly from the
    // host UI callback. Preserve all guest registers across the nested call.
    if (atomic_load(&requested_hz) != atomic_load(&timing_hz) &&
        !atomic_flag_test_and_set(&switching)) {
        int hz = atomic_load(&requested_hz);
        atomic_store(&timing_hz, hz);
        Cpu tmp = *c;
        tmp.x[0] = hz == 60 ? 1 : 2;
        guest_call(&tmp, MAIN_BASE + 0x32f92c0);
        LOG("[engage-timing] applied %d Hz from guest presentation\n", hz);
        atomic_flag_clear(&switching);
    }
}
void engage_timing_site(Cpu *c, uint64_t offset) {
    pthread_once(&timing_once, timing_init);
    int hz = atomic_load(&timing_hz);
    if (offset == 0x32f92d4) { c->x[19] = hz == 60 ? 1 : 2; return; }
    if (hz != 60) return;
    float value;
    memcpy(&value, &c->v[0], sizeof value);
    value *= offset == 0x2a5e8d0 || offset == 0x2a5e930 ? 0.25f : 0.5f;
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
