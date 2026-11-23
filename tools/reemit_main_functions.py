#!/usr/bin/env python3
"""Re-emit selected Engage main functions with their verified unwind extents."""

from bisect import bisect_right
import argparse
from pathlib import Path
import re
import struct
import sys


ROOT = Path(__file__).resolve().parent.parent
BASE = 0x7000000000
BUILD = "8c08b9719e085f91847b5e0f935d9488"
LARGE_EXTENTS = {
    0x7CE1C8: 0x7D28D4,
    0x9FC3FC: 0xA04580,
    0x1F8CD80: 0x1F92290,
    0x283BD40: 0x2842348,
    0x357C750: 0x35847EC,
}
KNOWN_EXTENTS = {0x42B424: 0x42B748, 0x66652C: 0x666734, 0x6701D0: 0x6706E0} | LARGE_EXTENTS
# This formatter has no unwind record. The matched stack prologue and
# epilogue and the next function entry establish its extent.
EXPLICIT_EXTENTS = {
    0x163BA80: 0x163CB10,
    0x17E57F8: 0x17E62D4,  # Leaf string dispatcher, through its final return.
}
sys.path.insert(0, str(ROOT / "source/tools"))

from analyze import read_encoded
from gen import Module


def unwind_records(module):
    nso = module.nso
    if nso.build_id != BUILD:
        raise ValueError(f"Unexpected main build ID {nso.build_id}")
    image, header = nso.image, nso.eh_start
    _, pointer_encoding, count_encoding, table_encoding = image[header:header + 4]
    cursor = header + 4 + read_encoded(image, header + 4, pointer_encoding, header)[1]
    count, width = read_encoded(image, cursor, count_encoding, header)
    cursor += width
    for _ in range(count):
        start, width = read_encoded(image, cursor, table_encoding, header)
        cursor += width
        frame, width = read_encoded(image, cursor, table_encoding, header)
        cursor += width
        cie = frame + 4 - nso.u32(frame + 4)
        encoding = image[cie + 8:cie + 29]
        is_zr = encoding.startswith(bytes.fromhex("017a5200017c1e011b"))
        is_zplr = (encoding.startswith(bytes.fromhex("017a504c5200017c1e0b9c"))
                   and encoding[19:21] == bytes.fromhex("1c1b"))
        if not is_zr and not is_zplr:
            continue
        displacement, length = struct.unpack_from("<iI", image, frame + 8)
        if frame + 8 + displacement == start and length and length % 4 == 0:
            yield start, start + length, frame, is_zplr


def unwind_extents(module):
    found = {start: end for start, end, _, _ in unwind_records(module)}
    for start, end in KNOWN_EXTENTS.items():
        if found.get(start) != end:
            raise ValueError(f"Unexpected unwind extent at {start:#x}")
    return found


def split_switches(module, generated):
    table = (generated / "main_table.c").read_text()
    entries = sorted(int(match, 16) for match in
                     re.findall(r"^void f_([0-9a-f]+)\(Cpu \*c\);$", table, re.MULTILINE))
    entry_set = set(entries)
    switches = {}
    extents = unwind_extents(module) | EXPLICIT_EXTENTS
    for start, stop in extents.items():
        begin, end = BASE + start, BASE + stop
        if begin not in entry_set or end > module.hi:
            continue
        next_index = bisect_right(entries, begin)
        if next_index == len(entries) or entries[next_index] >= end:
            continue
        if end - begin >= module.SPLIT_AT and start not in LARGE_EXTENTS:
            continue
        if any((module.word(pc) & 0xFFFFFC1F) == 0xD61F0000 and module.computed_target(pc, begin)
               for pc in range(begin, end, 4)):
            switches[begin] = end
    if not {BASE + start for start in KNOWN_EXTENTS} <= switches.keys():
        raise ValueError("Expected split switch was not discovered")
    return switches, entry_set


def route_missing_calls(code, missing):
    for addr in missing:
        # A fallthrough at the recorded end can point at alignment bytes.
        # Diagnose unexpected execution via the dispatcher.
        for index, line in enumerate(code):
            tail = f"__attribute__((musttail)) return f_{addr:x}(c);"
            if tail in line:
                code[index] = line.replace(tail, f"c->pc = {addr:#x}ull; "
                                           "__attribute__((musttail)) return rt_dispatch(c);")
            elif f"f_{addr:x}(c);" in line:
                code[index] = line.replace(f"f_{addr:x}(c);",
                                           f"c->pc = {addr:#x}ull; rt_dispatch(c);")


def prepare_texture_dispatch_labels(module, begin, labels, tables):
    """Ensure the 69-entry texture-format jump table targets are present in labels/tables."""
    if begin != BASE + 0x7CE1C8:
        return
    site, first, table, count = 0x7CE480, 0x7CE484, 0x508EC5C, 69
    if module.word(BASE + site) != 0xD61F0100:  # br x8
        raise ValueError("Unexpected texture dispatch instruction")
    targets = sorted({BASE + first + 4 * struct.unpack_from("<H", module.nso.image, table + 2*i)[0]
                      for i in range(count)})
    labels.add(BASE + site)
    labels.update(targets)
    tables[BASE + site] = targets


def restore_texture_dispatch(module, begin, code):
    """Preserve the local 69-entry texture-format jump table in a clean build."""
    if begin != BASE + 0x7CE1C8:
        return
    site, first, table, count = 0x7CE480, 0x7CE484, 0x508EC5C, 69
    if module.word(BASE + site) != 0xD61F0100:  # br x8
        raise ValueError("Unexpected texture dispatch instruction")
    targets = sorted({BASE + first + 4 * struct.unpack_from("<H", module.nso.image, table + 2*i)[0]
                      for i in range(count)})
    text = "\n".join(code)
    if len(targets) != 68 or any(f"L_{target:x}:;" not in text for target in targets):
        raise ValueError("Texture dispatch table does not match the generated labels")
    marker = f"L_{BASE + site:x}:;"
    index = code.index(marker)
    if code[index + 1].lstrip().startswith("switch (c->x[8])"):
        found = sorted(int(value, 16) for value in re.findall(r"case (0x[0-9a-f]+)ull:", code[index + 1]))
        if found != targets:
            raise ValueError("Unexpected generated texture dispatch cases")
        return
    cases = " ".join(f"case {target:#x}ull: goto L_{target:x};" for target in targets)
    code.insert(index + 1, "    switch (c->x[8]) { " + cases + " default: break; }")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("main_nso")
    parser.add_argument("--only", type=lambda value: int(value, 0), action="append",
                        help="re-emit only this module-relative function")
    args = parser.parse_args()
    module = Module("main", args.main_nso, BASE, "c")
    generated = ROOT / "generated/main"
    switches, entries = split_switches(module, generated)
    remaining = set(switches)
    if args.only:
        remaining = {BASE + offset for offset in args.only}
        if not remaining <= switches.keys():
            raise ValueError("Requested address is not a verified split function")
    print(f"Discovered {len(remaining)} split functions with computed branches", flush=True)
    definition = re.compile(r"^void(?: __attribute__\(\(optnone\)\))? f_([0-9a-f]+)\(Cpu \*c\) \{", re.MULTILINE)
    for chunk in sorted(generated.glob("main_*.c")):
        source = chunk.read_text()
        updated = source
        present = {int(match, 16) for match in definition.findall(source)}
        for begin in sorted(remaining & present):
            end = switches[begin]
            pattern = re.compile(rf"^void(?: __attribute__\(\(optnone\)\))? f_{begin:x}\(Cpu \*c\) \{{\n.*?^\}}",
                                 re.MULTILINE | re.DOTALL)
            match = pattern.search(updated)
            if not match:
                continue
            labels, _, tables = module.scan(begin, end)
            prepare_texture_dispatch_labels(module, begin, labels, tables)
            if not tables:
                raise ValueError(f"No indirect jump table found at {begin:#x}")
            original_split = module.SPLIT_AT
            if begin - BASE in LARGE_EXTENTS:
                module.SPLIT_AT = end - begin + 1
            try:
                code = module.emit_function(begin, end, labels, tables, {})
            finally:
                module.SPLIT_AT = original_split
            if module._parts:
                raise ValueError(f"Unexpected split function at {begin:#x}")
            restore_texture_dispatch(module, begin, code)
            route_missing_calls(code, module._refs - entries)
            prototypes = "".join(f"void f_{addr:x}(Cpu *c);\n"
                                 for addr in sorted(module._refs & entries))
            replacement = "\n".join(code)
            if match.group() != replacement:
                updated = updated[:match.start()] + prototypes + replacement + updated[match.end():]
            remaining.remove(begin)
            print(f"{chunk.name}: {begin:#x}–{end:#x}, {len(tables)} table(s)", flush=True)
        if updated != source:
            chunk.write_text(updated)
        if not remaining:
            return
    raise ValueError(f"Missing generated functions: {remaining!r}")


if __name__ == "__main__":
    main()
