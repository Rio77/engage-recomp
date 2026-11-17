"""Local game-library build inside an extracted or mounted Engage AppImage."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import os
import shutil
import subprocess
import struct
import re

def verify_library(path):
    """Reject accidentally incomplete guest linkage without external binutils."""
    raw = path.read_bytes()
    if raw[:6] != b'\x7fELF\x02\x01': raise ValueError('Expected little-endian ELF64')
    shoff = struct.unpack_from('<Q', raw, 40)[0]
    entsize, count = struct.unpack_from('<HH', raw, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', raw, shoff + i * entsize) for i in range(count)]
    missing = []
    for section in sections:
        if section[1] != 11: continue  # SHT_DYNSYM
        strings = sections[section[6]]
        table = raw[strings[4]:strings[4]+strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', raw, offset)
            if index: continue
            symbol = table[name:table.find(b'\0', name)].decode('ascii', errors='replace')
            if re.fullmatch(r'f_[0-9a-f]+(?:_.*)?', symbol): missing.append(symbol)
    if missing: raise ValueError('Unresolved guest functions: ' + ', '.join(missing[:30]))


def build(bundle: Path, workspace: Path, exefs: Path):
    bundle, workspace, exefs = bundle.resolve(), workspace.resolve(), exefs.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    source = workspace / 'source/runtime'
    source.mkdir(parents=True, exist_ok=True)
    headers = bundle / 'gamelib-src'
    for file in headers.iterdir():
        if file.is_file(): shutil.copy2(file, source / file.name)
    environment = dict(os.environ, LARECOMP_CHEATS=os.devnull,
        ZIG_GLOBAL_CACHE_DIR=str(workspace / 'zig-cache/global'),
        ZIG_LOCAL_CACHE_DIR=str(workspace / 'zig-cache/local'))
    compiler = [str(bundle / 'zig/zig'), 'cc', '-target', 'x86_64-linux-gnu',
        '-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label',
        '-fvisibility=hidden', '-mcpu=x86_64_v3', '-fPIC', '-I' + str(source)]
    # Check the shipped compiler before spending time on the full translation.
    probe = workspace / 'compiler-probe.c'
    probe.write_text('#include "cpu.h"\nint engage_compiler_probe(void) { return sizeof(Cpu)>0; }\n')
    subprocess.run(compiler + ['-c', str(probe), '-o', str(workspace / 'compiler-probe.o')], env=environment, check=True)
    subprocess.run([str(bundle / 'engagegen/engagegen'), '--workspace', str(workspace),
        '--exefs', str(exefs), '--hle', str(source / 'hle_symbols.txt')], env=environment, check=True)
    objects = []
    jobs = []
    for module in ('main', 'sdk', 'subsdk0'):
        for file in sorted((workspace / 'generated' / module).glob('*.c')):
            target = workspace / 'objects' / module / file.with_suffix('.o').name
            target.parent.mkdir(parents=True, exist_ok=True)
            objects.append(target)
            jobs.append((file, target))
    entry = workspace / 'objects/entry.o'
    jobs.append((source / 'gamelib_entry.c', entry))
    objects.append(entry)
    def compile_one(pair):
        file, target = pair
        temporary = target.with_suffix('.o.tmp')
        flags = ['-DGAMELIB_ID="engage-200-bringup"'] if target == entry else []
        print('Compiling ' + file.name, flush=True)
        subprocess.run(compiler + flags + ['-c', str(file), '-o', str(temporary)], env=environment, check=True)
        temporary.replace(target)
    with ThreadPoolExecutor(max_workers=min(3, os.cpu_count() or 1)) as pool:
        futures = [pool.submit(compile_one, pair) for pair in jobs]
        try:
            for future in as_completed(futures): future.result()
        except BaseException:
            for future in futures: future.cancel()
            raise
    output = workspace / 'engagegame.so'
    temporary = output.with_suffix('.so.tmp')
    response = workspace / 'objects.rsp'
    # Zig/Clang response files use quoted arguments, including paths with spaces.
    response.write_text('\n'.join('"' + str(p).replace('\\', '\\\\').replace('"', '\\"') + '"' for p in objects))
    subprocess.run(compiler + ['-shared', '@' + str(response), '-o', str(temporary)], env=environment, check=True)
    verify_library(temporary)
    temporary.replace(output)
    output.with_suffix('.so.id').write_text('engage-200-bringup\n')
    return output
