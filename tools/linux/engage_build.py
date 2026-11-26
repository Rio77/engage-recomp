"""Local game-library build inside an extracted or mounted Engage AppImage."""
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import os
import shutil
import subprocess
import struct
import re
import time

GAMELIB_ID = 'engage-031-switch-v2'


def reusable_gamelib(path: Path, *, allow_symlink: bool = False) -> bool:
    if (not allow_symlink and path.is_symlink()) or not path.is_file():
        return False
    try:
        return path.with_suffix('.so.id').read_text(encoding='ascii').strip() == GAMELIB_ID
    except (OSError, UnicodeError):
        return False


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def _copy_if_different(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_file() and src.is_file() and dst.stat().st_size == src.stat().st_size:
        if dst.read_bytes() == src.read_bytes():
            return
    tmp = dst.with_name(dst.name + '.tmp')
    shutil.copy2(src, tmp)
    tmp.replace(dst)


def save_incremental_cache(src_workspace: Path, cache_dir: Path):
    """Persist object files and main chunk metadata into cache/gamelib-build/ (without bulky untouched C)."""
    src_workspace = src_workspace.resolve()
    cache_dir = cache_dir.resolve()
    if src_workspace == cache_dir:
        # Already built inside cache_dir; prune bulky untouched C files in place.
        for mod in ('sdk', 'subsdk0'):
            mod_dir = cache_dir / 'generated' / mod
            if mod_dir.is_dir():
                for cfile in mod_dir.glob('*.c'):
                    cfile.unlink(missing_ok=True)
        main_dir = cache_dir / 'generated/main'
        if main_dir.is_dir() and (main_dir / 'main_chunks.json').is_file():
            for cfile in main_dir.glob('main_*.c'):
                if cfile.name != 'main_table.c':
                    cfile.unlink(missing_ok=True)
        (cache_dir / 'engagegame.so').unlink(missing_ok=True)
        return
    if cache_dir.exists():
        shutil.rmtree(cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)
    for name in ('build-meta.json', 'compiler-probe.o'):
        p = src_workspace / name
        if p.is_file():
            shutil.copy2(p, cache_dir / name)
    if (src_workspace / 'source').is_dir():
        shutil.copytree(src_workspace / 'source', cache_dir / 'source')
    if (src_workspace / 'objects').is_dir():
        shutil.copytree(src_workspace / 'objects', cache_dir / 'objects')
    src_main = src_workspace / 'generated/main'
    dst_main = cache_dir / 'generated/main'
    if src_main.is_dir():
        dst_main.mkdir(parents=True, exist_ok=True)
        for keep in ('main_chunks.json', 'main_table.c', 'cheat_variants.txt'):
            if (src_main / keep).is_file():
                shutil.copy2(src_main / keep, dst_main / keep)
    if (src_workspace / 'zig-cache').is_dir():
        shutil.copytree(src_workspace / 'zig-cache', cache_dir / 'zig-cache')
    for so_name in ('engagegame.so.id', 'engagegame.so.cheats'):
        p = src_workspace / so_name
        if p.is_file():
            shutil.copy2(p, cache_dir / so_name)

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


def build(bundle: Path, workspace: Path, exefs: Path, cheats_path: Path | None = None,
          incremental: bool = False, existing_so: Path | None = None):
    bundle, workspace, exefs = bundle.resolve(), workspace.resolve(), exefs.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    source = workspace / 'source/runtime'
    source.mkdir(parents=True, exist_ok=True)
    headers = bundle / 'gamelib-src'
    for file in headers.iterdir():
        if file.is_file():
            _copy_if_different(file, source / file.name)
    resolved_cheats = str(cheats_path.resolve()) if (cheats_path and cheats_path.is_file()) else os.devnull
    environment = dict(os.environ, LARECOMP_CHEATS=resolved_cheats,
        ZIG_GLOBAL_CACHE_DIR=str(workspace / 'zig-cache/global'),
        ZIG_LOCAL_CACHE_DIR=str(workspace / 'zig-cache/local'))
    resume = os.environ.get('ENGAGE_GAMELIB_RESUME', '0').strip() not in ('', '0', 'false', 'no')
    compiler = [str(bundle / 'zig/zig'), 'cc', '-target', 'x86_64-linux-gnu',
        '-O1', '-g0', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label',
        '-fvisibility=hidden', '-mcpu=x86_64_v3', '-fPIC', '-I' + str(source)]
    t_start = time.monotonic()
    probe = workspace / 'compiler-probe.c'
    probe_obj = workspace / 'compiler-probe.o'
    if not (incremental and probe_obj.is_file()):
        probe.write_text('#include "cpu.h"\nint engage_compiler_probe(void) { return sizeof(Cpu)>0; }\n')
        subprocess.run(compiler + ['-c', str(probe), '-o', str(probe_obj)], env=environment, check=True)

    main_sha = _sha256_file(exefs / 'main')
    meta_path = workspace / 'build-meta.json'
    # Completed caches may have pruned sources and contain objects from an older generator.
    # The manual resume path applies only to a build interrupted before writing metadata.
    resume = resume and not meta_path.exists()
    chunks_meta_path = workspace / 'generated/main/main_chunks.json'
    variants_txt = workspace / 'generated/main/cheat_variants.txt'
    can_incremental = False
    if incremental and meta_path.is_file() and chunks_meta_path.is_file() and variants_txt.is_file():
        try:
            bmeta = json.loads(meta_path.read_text(encoding='utf-8'))
            cmeta = json.loads(chunks_meta_path.read_text(encoding='utf-8'))
            expected_main_objs = [
                workspace / 'objects/main' / (Path(c['file']).stem + '.o') for c in cmeta.get('chunks', [])
            ] + [workspace / 'objects/main/main_table.o']
            can_incremental = (
                bmeta.get('format') == 1
                and bmeta.get('gamelib_id') == GAMELIB_ID
                and bmeta.get('main_sha256') == main_sha
                and (workspace / 'generated/main/main_table.c').is_file()
                and (workspace / 'objects/sdk').is_dir()
                and any((workspace / 'objects/sdk').glob('*.o'))
                and (workspace / 'objects/subsdk0').is_dir()
                and any((workspace / 'objects/subsdk0').glob('*.o'))
                and (workspace / 'objects/entry.o').is_file()
                and all(p.is_file() and p.stat().st_size > 0 for p in expected_main_objs)
            )
        except (OSError, ValueError, KeyError):
            can_incremental = False

    gen_cmd = [str(bundle / 'engagegen/engagegen'), '--workspace', str(workspace),
               '--exefs', str(exefs), '--hle', str(source / 'hle_symbols.txt'),
               '--cheats', resolved_cheats]

    dirty_files: list[str] = []
    if can_incremental:
        dirty_marker = workspace / 'dirty_chunks.txt'
        dirty_marker.unlink(missing_ok=True)
        subprocess.run(gen_cmd + ['--update-cheats'], env=environment, check=True)
        if dirty_marker.is_file():
            dirty_files = [ln.strip() for ln in dirty_marker.read_text(encoding='utf-8').splitlines() if ln.strip()]
            dirty_marker.unlink(missing_ok=True)
        else:
            can_incremental = False

    if not can_incremental:
        if not (resume and (workspace / 'generated/main').exists() and any((workspace / 'generated/main').glob('*.c'))):
            subprocess.run(gen_cmd, env=environment, check=True)

    t_gen = time.monotonic()
    objects = []
    jobs = []
    entry = workspace / 'objects/entry.o'

    if can_incremental:
        cmeta = json.loads(chunks_meta_path.read_text(encoding='utf-8'))
        for c in cmeta.get('chunks', []):
            objects.append(workspace / 'objects/main' / (Path(c['file']).stem + '.o'))
        objects.append(workspace / 'objects/main/main_table.o')
        for mod in ('sdk', 'subsdk0'):
            objects.extend(sorted((workspace / 'objects' / mod).glob('*.o')))
        objects.append(entry)
        for chunk_name in dirty_files:
            cfile = workspace / 'generated/main' / chunk_name
            ofile = workspace / 'objects/main' / (Path(chunk_name).stem + '.o')
            ofile.unlink(missing_ok=True)
            jobs.append((cfile, ofile))
    else:
        for module in ('main', 'sdk', 'subsdk0'):
            for file in sorted((workspace / 'generated' / module).glob('*.c')):
                target = workspace / 'objects' / module / file.with_suffix('.o').name
                target.parent.mkdir(parents=True, exist_ok=True)
                objects.append(target)
                jobs.append((file, target))
        jobs.append((source / 'gamelib_entry.c', entry))
        objects.append(entry)

    # Longest Processing Time (LPT) scheduling: compile largest C units first to prevent tail stalls.
    jobs.sort(key=lambda pair: pair[0].stat().st_size, reverse=True)
    def pick_workers():
        cpus = os.cpu_count() or 4
        avail_mib = 8192
        try:
            with open('/proc/meminfo', 'r', encoding='ascii', errors='ignore') as mf:
                for line in mf:
                    if line.startswith('MemAvailable:'):
                        avail_mib = max(2048, int(line.split()[1]) // 1024)
                        break
        except OSError:
            pass
        # Each zig cc worker uses ~200-350 MB RSS; allow all logical threads when RAM permits.
        return max(1, min(16, cpus, max(4, avail_mib // 450)))
    if jobs:
        workers = min(len(jobs), pick_workers())
        print(f'Compiling {len(jobs)} units (-O1) with {workers} workers (gen {t_gen - t_start:.1f}s)', flush=True)
        def compile_one(pair):
            file, target = pair
            if not can_incremental and resume and target.exists() and target.stat().st_size > 0:
                return
            temporary = target.with_suffix('.o.tmp')
            flags = [f'-DGAMELIB_ID="{GAMELIB_ID}"'] if target == entry else []
            print('Compiling ' + file.name, flush=True)
            subprocess.run(compiler + flags + ['-c', str(file), '-o', str(temporary)], env=environment, check=True)
            temporary.replace(target)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(compile_one, pair) for pair in jobs]
            try:
                for future in as_completed(futures): future.result()
            except BaseException:
                for future in futures: future.cancel()
                raise
    else:
        print(f'No code-patching cheat changes detected (gen {t_gen - t_start:.1f}s)', flush=True)

    t_comp = time.monotonic()
    output = workspace / 'engagegame.so'
    if can_incremental and not jobs and existing_so and reusable_gamelib(existing_so):
        output = existing_so
    elif not (can_incremental and not jobs and output.is_file()):
        temporary = output.with_suffix('.so.tmp')
        response = workspace / 'objects.rsp'
        # Zig/Clang response files use quoted arguments, including paths with spaces.
        response.write_text('\n'.join('"' + str(p).replace('\\', '\\\\').replace('"', '\\"') + '"' for p in objects))
        subprocess.run(compiler + ['-shared', '@' + str(response), '-o', str(temporary)], env=environment, check=True)
        verify_library(temporary)
        temporary.replace(output)
    id_target = output.with_suffix('.so.id')
    if not (output == existing_so and id_target.is_file()):
        id_target.write_text(GAMELIB_ID + '\n')
    cheats_out = output.with_suffix('.so.cheats')
    if variants_txt.is_file():
        _copy_if_different(variants_txt, cheats_out)
    elif not cheats_out.is_file() or cheats_out.read_text() != '':
        cheats_out.write_text('')
    meta_path.write_text(json.dumps({
        'format': 1,
        'gamelib_id': GAMELIB_ID,
        'main_sha256': main_sha,
    }, indent=2), encoding='utf-8')
    t_end = time.monotonic()
    print(f'Built engagegame.so in {t_end - t_start:.1f}s (gen {t_gen - t_start:.1f}s, cc {t_comp - t_gen:.1f}s, link {t_end - t_comp:.1f}s)', flush=True)
    return output
