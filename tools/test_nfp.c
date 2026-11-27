// Unit tests for runtime/services/nfp.c (Amiibo .bin parser, scanner, and nfp:IUser state machine).
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)

#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#include "../runtime/kernel.h"
#include "../runtime/paths.h"
#include "../runtime/services/nfp.h"

// ---------------- Minimal host/kernel/IPC test harness ----------------

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KObject *g_handles[512];
static char g_test_root[512];
FILE *g_log = NULL;

void rt_log(const char *fmt, ...) {
    (void)fmt;
}

const char *rt_data_root(void) { return g_test_root; }

KObject *handle_get(uint32_t h) {
    if (h < 0x101 || h >= 0x100 + 512) return NULL;
    return g_handles[h - 0x100];
}

uint32_t kernel_new_handle(KObject *o) {
    pthread_mutex_lock(&g_klock);
    uint32_t out = 0;
    for (int i = 1; i < 512; i++) {
        if (!g_handles[i]) {
            g_handles[i] = o;
            o->refs++;
            out = (uint32_t)(0x100 + i);
            break;
        }
    }
    pthread_mutex_unlock(&g_klock);
    CHECK(out != 0);
    return out;
}

static void test_close_handle(uint32_t h) {
    pthread_mutex_lock(&g_klock);
    KObject *o = handle_get(h);
    if (o) {
        g_handles[h - 0x100] = NULL;
        o->refs--;
    }
    pthread_mutex_unlock(&g_klock);
}

void kernel_signal(KObject *o) { o->signaled = true; }

Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = name;
    s->handler = h;
    s->state = state;
    return s;
}

void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }

extern const ServiceDef __start_services[];
extern const ServiceDef __stop_services[];
extern const ResetFn __start_resets[];
extern const ResetFn __stop_resets[];

static Service *find_nfp_user_service(void) {
    for (const ServiceDef *d = __start_services; d < __stop_services; d++) {
        if (d->name && strcmp(d->name, "nfp:user") == 0)
            return service_new(d->name, d->handler, NULL);
    }
    return NULL;
}

static void run_ipc(Service *svc, uint32_t cmd, const void *in, uint32_t in_sz, IpcBuf *outb, IpcResp *resp) {
    IpcReq req;
    memset(&req, 0, sizeof req);
    memset(resp, 0, sizeof *resp);
    req.cmd = cmd;
    req.data = (const uint8_t *)in;
    req.data_size = in_sz;
    if (outb) {
        req.c[0] = *outb;
        req.nc = 1;
    }
    svc->handler(svc, &req, resp);
}

static void make_synthetic_bin(uint8_t *buf, size_t len, const uint8_t uid[7], uint8_t c0, uint8_t c1, uint8_t c2,
                               uint8_t type, uint16_t numbering_id, uint8_t series_id) {
    memset(buf, 0, len);
    buf[0x00] = uid[0];
    buf[0x01] = uid[1];
    buf[0x02] = uid[2];
    buf[0x03] = 0x88;  // BCC0 should be skipped by GetTagInfo
    buf[0x04] = uid[3];
    buf[0x05] = uid[4];
    buf[0x06] = uid[5];
    buf[0x07] = uid[6];
    buf[0x54] = c0;
    buf[0x55] = c1;
    buf[0x56] = c2;
    buf[0x57] = type;
    buf[0x58] = (uint8_t)(numbering_id >> 8);
    buf[0x59] = (uint8_t)(numbering_id & 0xFF);
    buf[0x5A] = series_id;
    buf[0x5B] = 0x02;
}

static void write_file_bytes(const char *path, const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(buf, 1, n, f) == n);
    fclose(f);
}

static bool is_signaled(uint32_t h) {
    pthread_mutex_lock(&g_klock);
    KObject *o = handle_get(h);
    bool s = o && o->signaled;
    pthread_mutex_unlock(&g_klock);
    return s;
}

static void clear_handles_table(void) {
    memset(g_handles, 0, sizeof g_handles);
}

// ---------------- Test cases ----------------

static void test_parser(void) {
    uint8_t buf[572];
    const uint8_t uid[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    make_synthetic_bin(buf, sizeof buf, uid, 0x21, 0x08, 0x00, 0x00, 0x036F, 0x10);

    NfpAmiiboInfo info;
    CHECK(!nfp_parse_bin(NULL, 540, &info));
    CHECK(!nfp_parse_bin(buf, 531, &info));
    CHECK(!nfp_parse_bin(buf, 1025, &info));

    CHECK(nfp_parse_bin(buf, 540, &info));
    CHECK(memcmp(info.uid, uid, 7) == 0);
    CHECK(info.char_id[0] == 0x21 && info.char_id[1] == 0x08 && info.char_id[2] == 0x00);
    CHECK(info.numbering_id == 0x036F);
    CHECK(info.series_id == 0x10);
    CHECK(info.nfp_type == 0x00);

    // 532 and 572 byte dumps also valid
    CHECK(nfp_parse_bin(buf, 532, &info));
    CHECK(nfp_parse_bin(buf, 572, &info));

    // Wrong format byte at 0x5B rejected
    buf[0x5B] = 0x00;
    CHECK(!nfp_parse_bin(buf, 540, &info));
}

static void test_scanner_and_collisions(void) {
    char cmd[1024];
    snprintf(g_test_root, sizeof g_test_root, "/tmp/engage_nfp_test_%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/Amiibo/Fire Emblem' '%s/Amiibo/Super Smash Bros' '%s/Amiibo/d1/d2/d3'",
             g_test_root, g_test_root, g_test_root, g_test_root);
    CHECK(system(cmd) == 0);

    uint8_t marth[540], chrom_fe[540], chrom_ssb[540], bad[540];
    const uint8_t uid1[7] = {0x04, 1, 2, 3, 4, 5, 6};
    const uint8_t uid2[7] = {0x04, 2, 2, 3, 4, 5, 6};
    const uint8_t uid3[7] = {0x04, 3, 2, 3, 4, 5, 6};
    make_synthetic_bin(marth, sizeof marth, uid1, 0x21, 0x00, 0x00, 0, 0x0010, 0x00);
    make_synthetic_bin(chrom_fe, sizeof chrom_fe, uid2, 0x21, 0x08, 0x00, 0, 0x036F, 0x10);
    make_synthetic_bin(chrom_ssb, sizeof chrom_ssb, uid3, 0x21, 0x08, 0x00, 0, 0x0388, 0x00);
    memset(bad, 0xFF, sizeof bad);

    char p[768];
    snprintf(p, sizeof p, "%s/Amiibo/Marth.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Amiibo/Fire Emblem/Chrom.bin", g_test_root);
    write_file_bytes(p, chrom_fe, sizeof chrom_fe);
    snprintf(p, sizeof p, "%s/Amiibo/Super Smash Bros/Chrom.bin", g_test_root);
    write_file_bytes(p, chrom_ssb, sizeof chrom_ssb);
    // Ignored files: bad magic, dotfile, symlink, depth > 2, non-.bin
    snprintf(p, sizeof p, "%s/Amiibo/Bad.bin", g_test_root);
    write_file_bytes(p, bad, sizeof bad);
    snprintf(p, sizeof p, "%s/Amiibo/.Hidden.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Amiibo/readme.txt", g_test_root);
    write_file_bytes(p, marth, sizeof marth);
    snprintf(p, sizeof p, "%s/Amiibo/d1/d2/d3/TooDeep.bin", g_test_root);
    write_file_bytes(p, marth, sizeof marth);

    nfp_test_reset_all();
    nfp_rescan_amiibos();

    NfpAmiiboInfo entries[16];
    NfpUiState st;
    uint64_t gen = 0;
    nfp_ui_snapshot(entries, 16, &gen, &st);
    CHECK(st.count == 3);
    CHECK(st.selected_ui == 0);  // defaults to "None"
    CHECK(strcmp(entries[0].name, "Fire Emblem/Chrom") == 0);
    CHECK(entries[0].numbering_id == 0x036F);
    CHECK(strcmp(entries[1].name, "Marth") == 0);
    CHECK(strcmp(entries[2].name, "Super Smash Bros/Chrom") == 0);
    CHECK(entries[2].numbering_id == 0x0388);
}

static void test_state_machine_and_altar_flow(void) {
    clear_handles_table();
    unsetenv("ENGAGE_NFP_AMIIBO");

    Service *root = find_nfp_user_service();
    CHECK(root != NULL);

    IpcResp resp;
    run_ipc(root, 0, NULL, 0, NULL, &resp);
    CHECK(resp.result == 0 && resp.nobjects == 1 && resp.objects[0] != NULL);
    Service *iuser = resp.objects[0];

    // 1. Initialize + attach events + ListDevices + GetNpadId
    uint8_t init_in[16] = {0};
    run_ipc(iuser, 0, init_in, sizeof init_in, NULL, &resp);
    CHECK(resp.result == 0);

    uint64_t dev_list[4] = {0};
    IpcBuf dev_buf = {.addr = (uintptr_t)dev_list, .size = sizeof dev_list};
    run_ipc(iuser, 2, NULL, 0, &dev_buf, &resp);
    CHECK(resp.result == 0 && *(int32_t *)resp.data == 1 && dev_list[0] == 1ull);

    uint64_t dev = dev_list[0];
    run_ipc(iuser, 21, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && *(uint32_t *)resp.data == 0);  // NpadId::No1

    run_ipc(iuser, 23, NULL, 0, NULL, &resp);
    CHECK(resp.result == 0 && resp.ncopy == 1);
    uint32_t avail_h = resp.copy_handles[0];

    run_ipc(iuser, 17, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && resp.ncopy == 1);
    uint32_t act_h = resp.copy_handles[0];

    run_ipc(iuser, 18, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && resp.ncopy == 1);
    uint32_t deact_h = resp.copy_handles[0];

    // 2. No Amiibo ("None"): StartDetection stays SearchingForTag, activate_ev NOT signaled
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 1);  // SearchingForTag
    CHECK(!is_signaled(act_h));

    // Cancel at altar before any amiibo is read: StopDetection + Finalize cleanly
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    test_close_handle(act_h);
    test_close_handle(deact_h);
    test_close_handle(avail_h);
    run_ipc(iuser, 1, NULL, 0, NULL, &resp);
    CHECK(resp.result == 0);

    // 3. User pre-selects an Amiibo BEFORE returning to the altar: must stay selected (NOT auto-cleared!)
    nfp_ui_select(2);  // "Marth"
    NfpUiState st;
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 2);
    CHECK(!st.has_active_tag);

    // Cancel test with pre-selected Amiibo BEFORE a read finishes (user enters and exits without mounting/reading)
    run_ipc(iuser, 0, init_in, sizeof init_in, NULL, &resp);
    // Verify fresh handles are issued after previous svcCloseHandle!
    run_ipc(iuser, 23, NULL, 0, NULL, &resp);
    uint32_t avail_h2 = resp.copy_handles[0];
    run_ipc(iuser, 17, &dev, 8, NULL, &resp);
    uint32_t act_h2 = resp.copy_handles[0];
    run_ipc(iuser, 18, &dev, 8, NULL, &resp);
    uint32_t deact_h2 = resp.copy_handles[0];
    CHECK(handle_get(act_h2) != NULL && handle_get(deact_h2) != NULL && handle_get(avail_h2) != NULL);

    // Enter detection -> triggers TagFound, but exit BEFORE reading (no Mount/GetModelInfo)
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);  // user backed out before read
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 2);  // NOT cleared because no read took place!

    // 4. Full Altar Read Flow -> Mount(0, 1) -> GetModelInfo & GetTagInfo -> Unmount -> auto-resets to None!
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0 && is_signaled(act_h2));
    uint8_t mount_in[16] = {0};
    *(uint64_t *)(mount_in + 0) = dev;
    *(uint32_t *)(mount_in + 8) = 0;   // Amiibo
    *(uint32_t *)(mount_in + 12) = 1;  // Rom
    run_ipc(iuser, 5, mount_in, sizeof mount_in, NULL, &resp);
    CHECK(resp.result == 0);

    uint8_t tag_info[0x58], model_info[0x40];
    IpcBuf tb = {.addr = (uintptr_t)tag_info, .size = sizeof tag_info};
    IpcBuf mb = {.addr = (uintptr_t)model_info, .size = sizeof model_info};
    run_ipc(iuser, 13, &dev, 8, &tb, &resp);
    CHECK(resp.result == 0);
    CHECK(tag_info[0] == 0x04 && tag_info[3] == 0x03 && tag_info[0x0A] == 7);  // BCC0 skipped!
    CHECK(*(uint32_t *)(tag_info + 0x20) == 1 && *(uint32_t *)(tag_info + 0x24) == 2);

    run_ipc(iuser, 16, &dev, 8, &mb, &resp);
    CHECK(resp.result == 0);
    CHECK(model_info[0] == 0x21 && model_info[1] == 0x00 && model_info[2] == 0x00);

    // Before Unmount, selection stays during the active read session; Unmount completes read and resets to None!
    run_ipc(iuser, 6, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    CHECK(is_signaled(deact_h2));
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 0);  // auto-returned to "None" after real read!

    // 5. Live F1 selection while already waiting in SearchingForTag (StartDetection with None -> then pick Chrom FE)
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);  // StartDetection from TagRemoved works directly!
    CHECK(resp.result == 0 && !is_signaled(act_h2));
    nfp_ui_select(1);  // "Fire Emblem/Chrom" live from F1!
    CHECK(is_signaled(act_h2));
    run_ipc(iuser, 5, mount_in, sizeof mount_in, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 16, &dev, 8, &mb, &resp);
    CHECK(resp.result == 0);
    CHECK(model_info[0] == 0x21 && model_info[1] == 0x08);
    CHECK(*(uint16_t *)(model_info + 4) == 0x036F && model_info[3] == 0x10);
    run_ipc(iuser, 6, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.selected_ui == 0);

    // 6. A/B toggle ENGAGE_NFP_AMIIBO=0: StartDetection succeeds without signaling tag even when selected
    run_ipc(iuser, 4, &dev, 8, NULL, &resp);
    setenv("ENGAGE_NFP_AMIIBO", "0", 1);
    nfp_ui_select(2);
    run_ipc(iuser, 3, &dev, 8, NULL, &resp);
    CHECK(resp.result == 0);
    run_ipc(iuser, 20, &dev, 8, NULL, &resp);
    CHECK(*(uint32_t *)resp.data == 1);  // stays SearchingForTag
    CHECK(!is_signaled(act_h2));
    unsetenv("ENGAGE_NFP_AMIIBO");

    // 7. RT_ON_RESET hook clears session state but preserves Amiibo list
    for (const ResetFn *fn = __start_resets; fn < __stop_resets; fn++) (*fn)();
    nfp_ui_snapshot(NULL, 0, NULL, &st);
    CHECK(st.count == 3 && st.nfp_state == 0 && st.dev_state == 0);

    test_close_handle(act_h2);
    test_close_handle(deact_h2);
    test_close_handle(avail_h2);
    free(iuser);
    free(root);
}

static void check_downloaded_set_if_present(void) {
    const char *dir = "/home/deck/Engage-Development/somniel-test/run-amiibo-test";
    struct stat st;
    char ami[512];
    snprintf(ami, sizeof ami, "%s/Amiibo/Marth.bin", dir);
    if (stat(ami, &st) != 0) {
        printf("  (skipping live 13-bin dataset check: %s not populated yet)\n", ami);
        return;
    }

    snprintf(g_test_root, sizeof g_test_root, "%s", dir);
    nfp_test_reset_all();
    nfp_rescan_amiibos();

    NfpAmiiboInfo entries[64];
    NfpUiState uis;
    uint64_t gen = 0;
    nfp_ui_snapshot(entries, 64, &gen, &uis);
    CHECK(uis.count >= 13);

    // Check every expected Fire Emblem Engage character ID (0x2100..0x2109, 0x210B) + Chrom/Corrin distinction
    uint32_t seen_mask = 0;
    bool saw_chrom_fe = false, saw_chrom_ssb = false, saw_corrin_m = false, saw_corrin_f = false;
    for (int i = 0; i < uis.count; i++) {
        uint16_t ampid = (uint16_t)((entries[i].char_id[0] << 8) | entries[i].char_id[1]);
        CHECK((ampid >= 0x2100 && ampid <= 0x2109) || ampid == 0x210B);
        seen_mask |= 1u << (ampid - 0x2100);
        if (entries[i].numbering_id == 0x036F) saw_chrom_fe = true;
        if (entries[i].numbering_id == 0x0388) saw_chrom_ssb = true;
        if (entries[i].numbering_id == 0x025A) saw_corrin_m = true;
        if (entries[i].numbering_id == 0x0363) saw_corrin_f = true;
    }
    CHECK((seen_mask & 0xBFFu) == 0xBFFu);  // bits 0..9 and bit 11 (0x210A doesn't exist in FE)
    CHECK(saw_chrom_fe && saw_chrom_ssb && saw_corrin_m && saw_corrin_f);
    printf("  verified all %d downloaded FE Amiibo .bin files (Character IDs + Chrom/Corrin variants)\n", uis.count);
}

int main(void) {
    test_parser();
    test_scanner_and_collisions();
    test_state_machine_and_altar_flow();
    check_downloaded_set_if_present();

    // Cleanup temp test folder
    if (strncmp(g_test_root, "/tmp/engage_nfp_test_", 21) == 0) {
        char rm_cmd[600];
        snprintf(rm_cmd, sizeof rm_cmd, "rm -rf '%s'", g_test_root);
        (void)system(rm_cmd);
    }
    printf("PASS test_nfp\n");
    return 0;
}
