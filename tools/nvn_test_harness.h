// Shared isolated test harness for NVN ABI and lifecycle test suites (Phase 2).
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NVN_TEST_PHASE
#define NVN_TEST_PHASE 2
#endif

static int g_nvn_pass = 0;
static int g_nvn_xfail = 0;
static int g_nvn_xpass = 0;
static int g_nvn_fail = 0;

static void nvn_check_impl(int cond, int xfail_until_phase, const char *name, const char *expr, int line) {
    if (xfail_until_phase > NVN_TEST_PHASE) {
        if (!cond) {
            g_nvn_xfail++;
            printf("  [XFAIL phase %d] %s (%s) at line %d\n", xfail_until_phase, name, expr, line);
        } else {
            g_nvn_xpass++;
            printf("  [XPASS phase %d] %s (%s) at line %d\n", xfail_until_phase, name, expr, line);
        }
        return;
    }
    if (cond) {
        g_nvn_pass++;
    } else {
        g_nvn_fail++;
        fprintf(stderr, "  [FAIL] %s: %s (line %d)\n", name, expr, line);
    }
}

#define NVN_CHECK(cond, name) nvn_check_impl(!!(cond), 0, (name), #cond, __LINE__)
#define NVN_XFAIL_UNTIL(phase, cond, name) nvn_check_impl(!!(cond), (phase), (name), #cond, __LINE__)

static int nvn_summary(const char *suite_name) {
    printf("SUMMARY [%s, phase=%d]: pass=%d xfail=%d xpass=%d fail=%d\n",
           suite_name, NVN_TEST_PHASE, g_nvn_pass, g_nvn_xfail, g_nvn_xpass, g_nvn_fail);
    return g_nvn_fail == 0 ? 0 : 1;
}

// Execute fn(arg) in a forked child process so SIGSEGV / SIGABRT / FATAL (_exit(99))
// can be observed without killing the parent test runner.
// Returns: >=0 exit code (0 = normal exit, 99 = rt_fatal_trace), or -signum if killed by signal.
static int nvn_run_isolated(void (*fn)(void *), void *arg) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -999;
    }
    if (pid == 0) {
        // Prevent slow systemd-coredump writes for intentional SIGSEGV/SIGABRT probes.
        struct rlimit rl = {0, 0};
        setrlimit(RLIMIT_CORE, &rl);
        fn(arg);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -999;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -998;
}

static int nvn_all_bytes_eq(const uint8_t *buf, size_t n, uint8_t val) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != val) return 0;
    }
    return 1;
}
