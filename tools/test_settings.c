#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../runtime/nvn/output_scale.h"
#include "../runtime/settings.h"

#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                    \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)

FILE *g_log = NULL;
void rt_log(const char *fmt, ...) { (void)fmt; }
const char *const g_tunic_colours[N_TUNIC_COLOURS] = {[0 ... N_TUNIC_COLOURS - 1] = "Default"};
const char *const g_heart_colours[N_HEART_COLOURS] = {[0 ... N_HEART_COLOURS - 1] = "Default"};
bool bind_settings_load(const char *k, const char *v) { (void)k; (void)v; return false; }
bool cheatvm_settings_load(const char *k, const char *v) { (void)k; (void)v; return false; }
void bind_settings_save(FILE *f) { (void)f; }
void cheatvm_settings_save(FILE *f) { (void)f; }

int main(void) {
    char tmp[256], prev_cwd[512];
    CHECK(getcwd(prev_cwd, sizeof prev_cwd) != NULL);
    snprintf(tmp, sizeof tmp, "/tmp/engage_settings_test_%d", (int)getpid());
    char cmd[320];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", tmp, tmp);
    CHECK(system(cmd) == 0);
    CHECK(chdir(tmp) == 0);

    // Child process 1: save custom settings to fe3hrecomp.ini
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        settings_load();
        g_settings.pad_menu_combo = false;
        g_settings.upscaler = UPSCALE_INTEGER;
        g_settings.engage_fps = 30;
        settings_save();
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Parent process (fresh settings_load static guard): load and verify persistence
    g_settings.pad_menu_combo = true;
    g_settings.upscaler = UPSCALE_BILINEAR;
    g_settings.engage_fps = 60;
    settings_load();
    CHECK(g_settings.pad_menu_combo == false);
    CHECK(g_settings.upscaler == UPSCALE_INTEGER);
    CHECK(g_settings.engage_fps == 30);

    // Integer scaling viewport check: 640x360 into 1280x720 -> exact 2x centered
    float crop[4] = {0, 0, 1, 1};
    double vx = 0, vy = 0, vw = 1280, vh = 720;
    g_settings.res_mode = RES_Q50;
    CHECK(output_integer_viewport(640, 360, crop, &vx, &vy, &vw, &vh));
    CHECK(vx == 0 && vy == 0 && vw == 1280 && vh == 720);

    // Non-multiple: 800x450 into 1280x720 -> 1x centered with bars (240, 135, 800, 450)
    vx = 0; vy = 0; vw = 1280; vh = 720;
    float crop2[4] = {0, 0, 1, 1};
    CHECK(output_integer_viewport(800, 450, crop2, &vx, &vy, &vw, &vh));
    CHECK(vx == 240 && vy == 135 && vw == 800 && vh == 450);

    // Dynamic resolution disables integer snapping
    g_settings.res_mode = RES_DYNAMIC;
    CHECK(!output_integer_viewport(640, 360, crop, &vx, &vy, &vw, &vh));

    CHECK(chdir(prev_cwd) == 0);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    (void)system(cmd);
    printf("PASS test_settings\n");
    return 0;
}
