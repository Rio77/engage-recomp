// Unit tests for circular NVN diagnostic ring buffer (Phase 3).
#include "nvn/diag.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    unsetenv("ENGAGE_NVN_DIAG");
    unsetenv("ENGAGE_TRACE_TEXTURES");
    nvn_diag_reset();
    assert(nvn_diag_total_events() == 0);

    // Record lifecycle sequence for a specific texture guest address
    uint64_t gaddr = 0x7100123400ull;
    nvn_diag_record(NVN_EV_INIT, NVN_OBJ_TEXTURE, 8, gaddr, 42, 7, 1, 0, 0x7000AA00ull, false, 0, 0);
    nvn_diag_record(NVN_EV_DESC_REGISTER, NVN_OBJ_TEXTURE, 8, gaddr, 42, 7, 1, 19, 0x7000BB00ull, false, 0, 0);
    nvn_diag_record(NVN_EV_FINALIZE, NVN_OBJ_TEXTURE, 8, gaddr, 42, 7, 1, 0, 0x7000CC00ull, false, 0, 0);
    nvn_diag_record(NVN_EV_LOOKUP_FAIL, NVN_OBJ_TEXTURE, 2, gaddr, 0, 0, 0, 0, 0x7000DD00ull,
                    true, 0x53ull, 0x0053467974696e55ull);

    assert(nvn_diag_total_events() == 4);
    NvnDiagEvent ev = {0};
    assert(nvn_diag_find_last(gaddr, NVN_EV_FINALIZE, &ev));
    assert(ev.slot_index == 42 && ev.generation == 7 && ev.caller_lr == 0x7000CC00ull);
    assert(nvn_diag_find_last(gaddr, NVN_EV_DESC_REGISTER, &ev));
    assert(ev.desc_index == 19);
    assert(nvn_diag_find_last(gaddr, NVN_EV_LOOKUP_FAIL, &ev));
    assert((ev.flags & NVN_DIAG_FLAG_RAW_VALID) && ev.raw_q1 == 0x0053467974696e55ull);

    // Verify ring buffer wrap-around stays bounded (6000 > 4096) without losing newest events
    for (uint64_t i = 0; i < 6000; i++) {
        nvn_diag_record(NVN_EV_INIT, NVN_OBJ_TEXTURE, 8, 0x7100000000ull + i * 128, (uint32_t)(i & 1023), 1, 1, 0, 0x70000010ull, false, 0, 0);
    }
    assert(nvn_diag_total_events() == 6004);
    // Old event #1 was overwritten cleanly; newest event is findable
    assert(!nvn_diag_find_last(gaddr, NVN_EV_INIT, &ev));
    assert(nvn_diag_find_last(0x7100000000ull + 5999ull * 128, NVN_EV_INIT, &ev));

    // Verify deduplication rate limiting: 1, 2, 3, 10 logged; 4..9 suppressed
    nvn_diag_reset();
    int logged = 0;
    for (int i = 1; i <= 25; i++) {
        uint64_t cnt = 0;
        if (nvn_diag_should_log(NVN_EV_LOOKUP_FAIL, 2, 0x70001234ull, &cnt)) logged++;
    }
    assert(logged == 4);  // counts 1, 2, 3, 10

    // Verify ENGAGE_NVN_DIAG=off silences recording
    setenv("ENGAGE_NVN_DIAG", "off", 1);
    nvn_diag_record(NVN_EV_INIT, NVN_OBJ_TEXTURE, 8, gaddr, 1, 1, 1, 0, 0, false, 0, 0);
    assert(nvn_diag_total_events() == 0);
    unsetenv("ENGAGE_NVN_DIAG");

    puts("PASS: nvn_diag circular ring buffer, wrap-around, history lookup and deduplication");
    return 0;
}
