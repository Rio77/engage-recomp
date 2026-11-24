#!/usr/bin/env python3
"""Bridge the SDK's context restore to active C frames on the observed path."""

import argparse
import re
import sys

from reemit_main_functions import (
    BASE,
    ROOT,
    Module,
    prepare_texture_dispatch_labels,
    read_encoded,
    restore_texture_dispatch,
    route_missing_calls,
    unwind_records,
)


FRAMES = {
    0x351F2B0: 0x351F500,
    0x2DBA740: 0x2DBACD8,
    0x2108F10: 0x210919C,
    0x443838: 0x443908,
    0x31997A0: 0x319AA1C,
    0x31980F0: 0x3198724,
    0x2ED3990: 0x2ED3C88,
    0x2ED3150: 0x2ED329C,
    0x2ED1D70: 0x2ED2BD0,
    0x2CA8960: 0x2CA9074,
    0x2CA64A0: 0x2CA6C98,
}

HEADER = """#pragma once
#include <setjmp.h>
#include "cpu.h"

typedef struct EngageEhFrame {
    struct EngageEhFrame *previous;
    jmp_buf landing;
    uint64_t begin, end, sp, fp;
} EngageEhFrame;

void engage_eh_enter(EngageEhFrame *frame, Cpu *c, uint64_t begin, uint64_t end);
void engage_eh_leave(EngageEhFrame *frame);

#define ENGAGE_EH_CALL(c, begin, end, call) do {                     \\
    EngageEhFrame frame;                                           \\
    engage_eh_enter(&frame, (c), (begin), (end));                    \\
    if (setjmp(frame.landing) == 0) {                               \\
        call;                                                      \\
        engage_eh_leave(&frame);                                   \\
    } else {                                                       \\
        engage_eh_leave(&frame);                                   \\
        goto L_dispatch;                                           \\
    }                                                              \\
} while (0)
"""

RUNTIME = """#include <stdlib.h>
#include "rt.h"
#include "engage_unwind.h"

static _Thread_local EngageEhFrame *eh_top;

void engage_eh_enter(EngageEhFrame *frame, Cpu *c, uint64_t begin, uint64_t end) {
    frame->previous = eh_top;
    frame->begin = begin;
    frame->end = end;
    frame->sp = c->sp;
    frame->fp = c->x[29];
    eh_top = frame;
}

void engage_eh_leave(EngageEhFrame *frame) {
    if (eh_top != frame) FATAL("Engage exception frame mismatch\\n");
    eh_top = frame->previous;
}

void engage_resume_exception(Cpu *c) {
    uint64_t pc = c->x[30];
    for (EngageEhFrame *frame = eh_top; frame; frame = frame->previous) {
        if (pc >= frame->begin && pc < frame->end && c->sp == frame->sp && c->x[29] == frame->fp) {
            c->pc = pc;
            eh_top = frame;
            if (getenv("ENGAGE_TRACE_EXCEPTIONS"))
                LOG("[engage-eh] resume %llx sp=%llx\\n", pc, c->sp);
            longjmp(frame->landing, 1);
        }
    }
    FATAL("Engage exception landing pad %llx sp=%llx fp=%llx has no native frame\\n",
          pc, c->sp, c->x[29]);
}
"""


def uleb(image, offset, limit):
    value = 0
    for shift in range(0, 64, 7):
        if offset >= limit:
            break
        byte = image[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
    raise ValueError("Invalid LSDA ULEB128")


def protected_calls(module, frame, start, end):
    image = module.nso.image
    ro_base, ro_size = module.nso.rodata
    ro_end = ro_base + ro_size
    if image[frame + 16] != 8:
        raise ValueError(f"Unexpected LSDA pointer size at {start:#x}")
    lsda, _ = read_encoded(image, frame + 17, 0x1C, 0)
    if not ro_base <= lsda < ro_end - 3 or image[lsda] != 0xFF:
        raise ValueError(f"Unexpected LSDA address or landing base at {start:#x}")
    type_encoding = image[lsda + 1]
    pos = lsda + 2
    if type_encoding != 0xFF:
        _, pos = uleb(image, pos, ro_end)
    if pos >= ro_end or image[pos] != 1:
        raise ValueError(f"Unexpected LSDA call site format at {start:#x}")
    size, pos = uleb(image, pos + 1, ro_end)
    limit = pos + size
    if limit > ro_end:
        raise ValueError(f"Truncated LSDA call table at {start:#x}")
    while pos < limit:
        call, pos = uleb(image, pos, limit)
        length, pos = uleb(image, pos, limit)
        landing, pos = uleb(image, pos, limit)
        _, pos = uleb(image, pos, limit)  # The SDK handles the action/type table.
        if call + length > end - start or landing >= end - start:
            raise ValueError(f"Invalid LSDA call or landing at {start:#x}")
        if landing:
            yield BASE + start + call, BASE + start + call + length, BASE + start + landing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("main_nso")
    parser.add_argument("--all", action="store_true", help="cover all recorded exception handlers")
    args = parser.parse_args()
    module = Module("main", args.main_nso, BASE, "c")
    records = {start: (end, frame) for start, end, frame, has_lsda in unwind_records(module)
               if has_lsda}
    if any(records.get(start, (0, 0))[0] != end for start, end in FRAMES.items()):
        raise ValueError("Unexpected unwind records for exception landings")
    selected = records if args.all else {start: records[start] for start in FRAMES}

    generated = ROOT / "generated/main"
    entries = {int(addr, 16) for addr in re.findall(
        r"^void f_([0-9a-f]+)\(Cpu \*c\);$", (generated / "main_table.c").read_text(), re.MULTILINE)}
    definitions = re.compile(r"^void(?: __attribute__\(\(optnone\)\))? f_([0-9a-f]+)\(Cpu \*c\) \{", re.MULTILINE)
    remaining = {BASE + addr for addr in selected}
    updates = {}
    for chunk in sorted(generated.glob("main_*.c")):
        source = chunk.read_text()
        for begin in sorted(remaining & {int(addr, 16) for addr in definitions.findall(source)}):
            end_offset, frame = selected[begin - BASE]
            end = BASE + end_offset
            call_sites = list(protected_calls(module, frame, begin - BASE, end_offset))
            ranges = [(lo, hi) for lo, hi, _ in call_sites]
            landing_pads = {lp for _, _, lp in call_sites}
            labels, _, tables = module.scan(begin, end)
            prepare_texture_dispatch_labels(module, begin, labels, tables)
            original_split = module.SPLIT_AT
            module.SPLIT_AT = max(original_split, end - begin + 1)
            try:
                code = module.emit_function(begin, end, labels, tables, {}, dispatch_targets=landing_pads)
            finally:
                module.SPLIT_AT = original_split
            parts = list(getattr(module, "_parts", []))
            module._parts = []
            restore_texture_dispatch(module, begin, code, parts=parts)
            all_refs = set(module._refs)
            route_missing_calls(code, module._refs - entries)
            for p_lines, p_refs in parts:
                route_missing_calls(p_lines, p_refs - entries)
                all_refs |= p_refs
            calls = 0
            for blk in [code] + [p_lines for p_lines, _ in parts]:
                for i, line in enumerate(blk):
                    call = re.search(r"c->x\[30\] = (0x[0-9a-f]+)ull; .*?(?:f_[0-9a-f]+|rt_dispatch)\(c\);$", line)
                    if call and any(lo <= int(call[1], 16) - 4 < hi for lo, hi in ranges):
                        blk[i] = f"    ENGAGE_EH_CALL(c, {begin:#x}ull, {end:#x}ull, {line.strip()});"
                        calls += 1
            if not calls and begin - BASE in FRAMES:
                raise ValueError(f"No call sites in exception frame {begin:#x}")
            all_lines = list(code)
            for p_lines, _ in parts:
                all_lines.extend(p_lines)
            replacement = "\n".join(all_lines)
            pattern = re.compile(rf"^void(?: __attribute__\(\(optnone\)\))? f_{begin:x}\(Cpu \*c\) \{{\n.*?^\}}", re.MULTILINE | re.DOTALL)
            prototypes = "".join(f"void f_{addr:x}(Cpu *c);\n" for addr in sorted(all_refs & entries))
            source = re.sub(rf"^void f_{begin:x}_(?:p\d+|r)\(Cpu \*c\);\n", "", source, flags=re.MULTILINE)
            source = re.sub(rf"^void f_{begin:x}_(?:p\d+|r)\(Cpu \*c\) \{{\n.*?^\}}\n?", "", source, flags=re.MULTILINE | re.DOTALL)
            source = pattern.sub(lambda _: prototypes + replacement, source, count=1)
            if '#include "engage_unwind.h"' not in source:
                source = '#include "engage_unwind.h"\n' + source
            updates[chunk] = source
            remaining.remove(begin)
            print(f"{chunk.name}: {begin:#x}–{end:#x}, {calls} native landing checkpoints", flush=True)
        if not remaining:
            break
    if remaining:
        raise ValueError(f"Missing native frames {remaining!r}")
    runtime = ROOT / "source/runtime"
    (runtime / "engage_unwind.h").write_text(HEADER)
    (runtime / "engage_unwind.c").write_text(RUNTIME)
    for chunk, text in updates.items():
        if chunk.read_text() != text:
            chunk.write_text(text)


if __name__ == "__main__":
    main()
