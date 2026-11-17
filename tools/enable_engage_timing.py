#!/usr/bin/env python3
"""Emit guarded Engage timing hooks for the verified 2.0.0 module.

Sites and scale factors follow engage-pc-recomp commit
5e0ce98f120bd669cb79d3b365d0da1d6d54145b, recomp/patches-engage-v200.txt (MIT).
"""
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
INSTALL_RUNTIME_HOOK = True
sys.path.insert(0, str(ROOT / "source/tools"))
from nso import Nso
import gen
from gen import Module
from reemit_main_functions import route_missing_calls

SITES = {
    0x32f92d4: 0x2a0003f3,
    0x2a5e870: 0xbd402000,
    0x2a5e8d0: 0xbd402400,
    0x2a5e930: 0xbd402800,
    0x2a5e990: 0xbd402c00,
}


def main():
    nso = Nso(sys.argv[1])
    if nso.build_id != "8c08b9719e085f91847b5e0f935d9488":
        raise ValueError("Expected Engage main 2.0.0")
    for offset, instruction in SITES.items():
        if struct.unpack_from("<I", nso.image, offset)[0] != instruction:
            raise ValueError(f"Unexpected instruction at {offset:#x}")
    module = Module("main", sys.argv[1], 0x7000000000, "c")
    table = (ROOT / "generated/main/main_table.c").read_text()
    entries = sorted({int(a, 16) for a in re.findall(r"^void f_([0-9a-f]+)\(Cpu \*c\);$", table, re.M)})
    entry_set = set(entries)
    def timing_instruction(generator, address):
        instruction = generator.lift(address)
        if instruction.kind != "op":
            raise ValueError(f"Timing site is not a scalar operation: {address:x}")
        return instruction.code + f" engage_timing_site(c, {address - module.base:#x}ull);"
    owners = {}
    for offset in SITES:
        address = module.base + offset
        begin = max(entry for entry in entries if entry <= address)
        end = next(entry for entry in entries if entry > address)
        owners[begin] = end
        gen.PATCHES[address] = timing_instruction
    remaining = set(owners)
    updates = {}
    for path in (ROOT / "generated/main").glob("main_*.c"):
        text = path.read_text()
        updated = text
        for begin in remaining.copy():
            pattern = re.compile(rf"^void f_{begin:x}\(Cpu \*c\) \{{\n.*?^\}}", re.M | re.S)
            match = pattern.search(updated)
            if match is None:
                continue
            if "ENGAGE_EH_CALL" in match[0] or "engage_timing_site" in match[0]:
                raise ValueError("Timing frame already patched or contains exception checkpoints")
            end = owners[begin]
            labels, _, tables = module.scan(begin, end)
            code = module.emit_function(begin, end, labels, tables, {})
            if module._parts:
                raise ValueError("Unexpected split timing function")
            route_missing_calls(code, module._refs - entry_set)
            prototypes = "".join(f"void f_{a:x}(Cpu *c);\n" for a in sorted(module._refs & entry_set))
            replacement = prototypes + "\n".join(code)
            updated = updated[:match.start()] + replacement + updated[match.end():]
            remaining.remove(begin)
        if updated != text:
            updates[path] = '#include "engage_timing.h"\n' + updated
    if remaining:
        raise ValueError(f"Missing timing sites: {remaining}")
    runtime = ROOT / "source/runtime"
    (runtime / "engage_timing.h").write_text(
        '#pragma once\n#include "cpu.h"\nvoid engage_timing_site(Cpu *, uint64_t);\nvoid engage_present_pace(void);\n')
    (runtime / "engage_timing.c").write_text(SOURCE)
    objects = runtime / "nvn/objects.c"
    text = objects.read_text() if INSTALL_RUNTIME_HOOK else 'engage_present_pace();'
    if "engage_present_pace();" not in text:
        marker = "        backend_present(t);"
        if text.count(marker) != 1:
            raise ValueError("Expected one NVN window presentation site")
        objects.write_text(text.replace(marker,
            "        extern void engage_present_pace(void);\n        engage_present_pace();\n" + marker))
    for path, text in updates.items():
        path.write_text(text)
        print(path)


SOURCE = r'''#include <pthread.h>
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
'''


if __name__ == "__main__":
    main()
