#!/usr/bin/env python3
"""Phase 0 PoC: Validate Maxwell shader binary extraction and NX Native table compatibility
against a local Fire Emblem Engage 2.0.0 RomFS/ExeFS dump.

Read-only on game files. Writes a compact binary seed for probe_translate.c.
"""
import argparse
import collections
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

import unityfs_reader as unityfs

CTL_MAGIC = bytes.fromhex('34127698')
GFX_MAGIC = bytes.fromhex('78563412')
GLSLC_MAGIC = b'\x91\x68\x86\x19'
NV_STAGES = ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']
STAGE_TO_ENGAGE = {'vs': 0, 'ps': 1, 'gs': 2, 'hs': 3, 'ds': 4, 'cs': 5}
STAGE_NAME_TO_CODE = {'vertex': 'vs', 'pixel': 'ps', 'geometry': 'gs', 'hull': 'hs', 'domain': 'ds', 'compute': 'cs'}


def skey(ctl: bytes, code: bytes) -> str:
    own = struct.unpack_from('<I', ctl, 0x20)[0]
    clen = own if 0x760 <= own < 0x880 else 0x880
    return hashlib.sha256(ctl[:clen] + code).hexdigest()[:16]


def decompress_nso(raw: bytes) -> bytes:
    if raw[:4] != b'NSO0':
        raise ValueError('Expected NSO0 binary for ExeFS main')
    flags = struct.unpack_from('<I', raw, 0x0C)[0]
    segs = []
    for i, off_hdr in enumerate((0x10, 0x20, 0x30)):
        foff, _moff, dsz = struct.unpack_from('<III', raw, off_hdr)
        csz = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        comp = bool(flags & (1 << i))
        chunk = raw[foff:foff + (csz if comp else dsz)]
        segs.append(unityfs.lz4_decompress_block(chunk, dsz) if comp else chunk)
    return b''.join(segs)


def extract_main_glslc(main_img: bytes):
    out = []
    pos = 0
    while True:
        h = main_img.find(GLSLC_MAGIC, pos)
        if h < 0:
            break
        pos = h + 4
        if h + 0x50 > len(main_img):
            continue
        size = struct.unpack_from('<I', main_img, h + 0x44)[0]
        nsec = struct.unpack_from('<I', main_img, h + 0x4C)[0]
        if size < 0x90 or h + size > len(main_img) or nsec == 0 or nsec > 16:
            continue
        for si in range(nsec):
            s = h + 0x90 + 0x90 * si
            if s + 0x40 > len(main_img):
                continue
            _ssz, soff, stype = struct.unpack_from('<3I', main_img, s)
            if stype != 0:
                continue
            co, ko, kn, cn = struct.unpack_from('<4I', main_img, s + 0x30)
            base = h + soff
            ctl = main_img[base + co:base + co + cn]
            code = main_img[base + ko:base + ko + kn]
            if len(ctl) < 0x760 or ctl[:4] != CTL_MAGIC:
                continue
            if struct.unpack_from('<I', ctl, 0x704)[0] != len(code):
                continue
            if code[:4] == GFX_MAGIC and len(code) >= 0x34:
                sph0 = struct.unpack_from('<I', code, 0x30)[0]
                st_map = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
                st = st_map.get((sph0 >> 10) & 0xF)
            else:
                st = 'cs'
            if st:
                out.append((st, ctl, code, f'main#GLSLC@{h:#x}'))
    return out


def extract_unity_file(path: Path, common: dict, trees: dict):
    d = path.read_bytes()
    sers = unityfs.bundle_files(d) if d[:8] == b'UnityFS\0' else [('', d)]
    out = []
    for node, x in sers:
        if not unityfs.is_serialized(x):
            continue
        for _cid, _pid, o in unityfs.serialized_objects(x, common, {unityfs.CLASS_SHADER}, trees):
            name = o['m_ParsedForm']['m_Name'] if 'm_ParsedForm' in o else o.get('m_Name', '?')
            for plat, k, sp in unityfs.shader_programs(o):
                code_blob = unityfs.subprogram(sp)[4]
                if len(code_blob) < 0x60:
                    continue
                h = struct.unpack_from('<24i', code_blob, 0)
                for i, st in enumerate(NV_STAGES):
                    off, nctl, ncode = h[6 + i], h[12 + i], h[18 + i]
                    if off == -1:
                        continue
                    a = 0x60 + off
                    ctl = code_blob[a:a + nctl]
                    cd = code_blob[a + nctl:a + nctl + ncode]
                    if ctl[:4] != CTL_MAGIC or struct.unpack_from('<I', ctl, 0x704)[0] != len(cd):
                        continue
                    out.append((st, ctl, cd, f'{path.name}:{node}#{name}#var{plat}.{k}'))
    return out


def has_brx(code: bytes, const_offset: int, stage_code: str) -> bool:
    cstart = 0 if stage_code == 'cs' else 0x80
    max_sz = const_offset if const_offset else len(code)
    if max_sz > len(code):
        max_sz = len(code)
    if max_sz <= cstart:
        return False
    n = min((max_sz - cstart) // 8, 65536)
    end = 0
    for i in range(n):
        if i % 4 == 0:
            continue
        w = struct.unpack_from('<Q', code, cstart + i * 8)[0]
        if (w & 0xFFF0000000000000) == 0xE240000000000000:
            imm = (w >> 20) & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            if imm == -8 and end:
                break
        if w == 0 and i > 4:
            break
        end = i + 1
    for i in range(end):
        if i % 4 == 0:
            continue
        w = struct.unpack_from('<Q', code, cstart + i * 8)[0]
        if (w & 0xFFF0000000000000) == 0xE250000000000000:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--nx-game', type=Path, required=True)
    ap.add_argument('--out-seed', type=Path, required=True)
    args = ap.parse_args()

    t0 = time.monotonic()

    # 1. Load NX Native metadata tables
    map_rows = {}
    source_files = set()
    for line in (args.nx_game / 'shader_map.tsv').read_text().splitlines():
        if not line or line.startswith('#') or line.startswith('package\t'):
            continue
        pkg, idx, st, key, src = line.split('\t')
        map_rows[key] = (STAGE_NAME_TO_CODE[st], src)
        fpath = src.split(':', 1)[0].split('#', 1)[0]
        if fpath != 'main':
            source_files.add(fpath)

    pairs = []
    for line in (args.nx_game / 'shader_pairs.tsv').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        vs_k, fs_k = line.split('\t')
        pairs.append((vs_k, fs_k))

    facts = {}
    for line in (args.nx_game / 'shader_facts.tsv').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        k, j = line.split('\t', 1)
        facts[k] = json.loads(j)

    pso_s_keys = set()
    pso_r_pairs = []
    for line in (args.nx_game / 'pso_manifest.tsv').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        f = line.split('\t')
        if f[0] == 'S':
            pso_s_keys.add(f[1].removesuffix('_b'))
        elif f[0] == 'R':
            raw = bytes.fromhex(f[1])
            vs = raw[0:24].split(b'\0', 1)[0].decode().removesuffix('_b')
            fs = raw[24:48].split(b'\0', 1)[0].decode().removesuffix('_b')
            pso_r_pairs.append((vs, fs))

    # 2. Extract main (common strings + 11 GLSLC shaders)
    main_img = decompress_nso((args.exefs / 'main').read_bytes())
    common = unityfs.common_strings(main_img)
    extracted = {}  # key -> (stage_code, ctl, code, src)
    for st, ctl, cd, src in extract_main_glslc(main_img):
        k = skey(ctl, cd)
        extracted.setdefault(k, (st, ctl, cd, src))

    # 3. Extract all 118 RomFS files listed in shader_map.tsv
    # Pre-seed KNOWN_TREES (SHADER_2020_3, COMPUTE_2020_3) so stripped files (data.unity3d, Resources) always decode
    trees = {k: unityfs.known_tree(*k) for k in unityfs.KNOWN_TREES}
    missing_files = []
    # Process StreamingAssets bundles first (they also contain live type trees), then engine files
    ordered_files = sorted(source_files, key=lambda p: (0 if '/StreamingAssets/' in p else 1, p))
    for rel in ordered_files:
        p = args.romfs / rel.lstrip('/')
        if not p.is_file():
            missing_files.append(rel)
            continue
        for st, ctl, cd, src in extract_unity_file(p, common, trees):
            k = skey(ctl, cd)
            extracted.setdefault(k, (st, ctl, cd, src))

    t1 = time.monotonic()

    # 4. Compatibility statistics
    matched_map = sum(1 for k in map_rows if k in extracted)
    missing_map = [k for k in map_rows if k not in extracted]
    extra_keys = len(extracted) - matched_map
    stage_counts = collections.Counter()
    brx_counts = collections.Counter()
    bad_gfx_magic = 0
    zero_const_off = 0
    total_code_bytes = 0

    for k, (st, ctl, cd, _src) in extracted.items():
        if k not in map_rows:
            continue
        stage_counts[st] += 1
        total_code_bytes += len(cd)
        const_off = struct.unpack_from('<I', ctl, 0x700)[0]
        if const_off == 0:
            zero_const_off += 1
        if st != 'cs' and cd[:4] != GFX_MAGIC:
            bad_gfx_magic += 1
        if has_brx(cd, const_off, st):
            brx_counts[st] += 1

    brx_in_pso = sum(
        1 for k in pso_s_keys
        if k in extracted and has_brx(extracted[k][2], struct.unpack_from('<I', extracted[k][1], 0x700)[0], extracted[k][0])
    )

    print(f'=== Phase 0 Extraction Compatibility Report ===')
    print(f'Elapsed: {t1 - t0:.2f} s ({len(ordered_files)} RomFS files + exefs/main)')
    print(f'Missing source files: {len(missing_files)}')
    print(f'shader_map.tsv matched: {matched_map} / {len(map_rows)} ({100.0 * matched_map / len(map_rows):.3f}%)')
    print(f'Missing keys: {len(missing_map)} {missing_map[:5]}')
    print(f'Extra extracted keys not in map: {extra_keys}')
    print(f'Stage breakdown (in map): {dict(stage_counts)}')
    print(f'Non-0x12345678 graphics shaders: {bad_gfx_magic}')
    print(f'Zero const_offset shaders: {zero_const_off}')
    print(f'Shaders containing BRX (maxwell_input_size()==0): {dict(brx_counts)} (total {sum(brx_counts.values())})')
    print(f'BRX shaders inside pso_manifest.tsv (798 S rows): {brx_in_pso} / {len(pso_s_keys)}')
    print(f'Total raw Maxwell code bytes: {total_code_bytes / (1024 * 1024):.2f} MiB')

    # 5. Write Phase 0 binary seed for probe_translate.c
    # Format:
    # Header: magic "P0SHSEED" (8), version u32=1, nshaders u32, npairs u32, ncompute u32
    # Shader records [nshaders]:
    #   dm_key_hex[16], stage u32, in_pso u32, const_offset u32, const_size u32, code_len u32, local[3] u32, code[code_len]
    # Pair records [npairs]:
    #   vs_idx u32, fs_idx u32, in_pso u32
    # Compute indices [ncompute]:
    #   cs_idx u32
    key_to_idx = {}
    shader_list = []
    for k in sorted(map_rows):
        if k not in extracted:
            continue
        st, ctl, cd, _src = extracted[k]
        if st not in ('vs', 'ps', 'cs'):
            continue
        key_to_idx[k] = len(shader_list)
        const_size, const_off = struct.unpack_from('<2I', ctl, 0x6FC)
        local = struct.unpack_from('<3I', ctl, 0x748) if st == 'cs' else (0, 0, 0)
        if st == 'cs' and k in facts and 'local' in facts[k]:
            local = tuple(facts[k]['local'])
        shader_list.append((k, STAGE_TO_ENGAGE[st], 1 if k in pso_s_keys else 0, const_off, const_size, local, cd))

    pso_pair_set = set(pso_r_pairs)
    pair_list = []
    for vs_k, fs_k in pairs:
        if vs_k in key_to_idx and fs_k in key_to_idx:
            pair_list.append((key_to_idx[vs_k], key_to_idx[fs_k], 1 if (vs_k, fs_k) in pso_pair_set else 0))
    # Also ensure any R pair in pso_manifest not in shader_pairs.tsv is included
    existing_pairs = {(vs_k, fs_k) for vs_k, fs_k in pairs}
    extra_pso_pairs = 0
    for vs_k, fs_k in pso_r_pairs:
        if (vs_k, fs_k) not in existing_pairs and vs_k in key_to_idx and fs_k in key_to_idx:
            existing_pairs.add((vs_k, fs_k))
            pair_list.append((key_to_idx[vs_k], key_to_idx[fs_k], 1))
            extra_pso_pairs += 1

    compute_list = [idx for idx, rec in enumerate(shader_list) if rec[1] == 5]

    args.out_seed.parent.mkdir(parents=True, exist_ok=True)
    tmp = args.out_seed.with_suffix('.tmp')
    with tmp.open('wb') as f:
        f.write(b'P0SHSEED')
        f.write(struct.pack('<4I', 1, len(shader_list), len(pair_list), len(compute_list)))
        for k, st_num, in_pso, const_off, const_size, local, cd in shader_list:
            f.write(k.encode('ascii'))
            f.write(struct.pack('<8I', st_num, in_pso, const_off, const_size, len(cd), local[0], local[1], local[2]))
            f.write(cd)
        for vi, fi, in_pso in pair_list:
            f.write(struct.pack('<3I', vi, fi, in_pso))
        for ci in compute_list:
            f.write(struct.pack('<I', ci))
    tmp.replace(args.out_seed)
    print(f'Wrote {args.out_seed} ({args.out_seed.stat().st_size / (1024 * 1024):.2f} MiB): '
          f'{len(shader_list)} shaders, {len(pair_list)} pairs (+{extra_pso_pairs} extra PSO pairs), {len(compute_list)} compute')


if __name__ == '__main__':
    main()
