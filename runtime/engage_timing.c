#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "engage_timing.h"

static pthread_once_t timing_once = PTHREAD_ONCE_INIT;
static int timing_hz;
static void timing_init(void) {
    const char *value = getenv("ENGAGE_FPS");
    if (value && strcmp(value, "30") && strcmp(value, "60"))
        FATAL("ENGAGE_FPS must be 30 or 60\n");
    timing_hz = value && !strcmp(value, "30") ? 30 : 60;
    LOG("[engage-timing] %d Hz, fixed for this session\n", timing_hz);
}
void engage_timing_site(Cpu *c, uint64_t offset) {
    pthread_once(&timing_once, timing_init);
    if (offset == 0x32f92d4) { c->x[19] = timing_hz == 60 ? 1 : 2; return; }
    if (timing_hz != 60) return;
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
    if (timing_hz != 30) return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t deadline;
    const uint64_t period = 1000000000ull / 30;
    pthread_mutex_lock(&lock);
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
