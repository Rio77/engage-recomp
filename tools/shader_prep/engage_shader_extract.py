#!/usr/bin/env python3
"""Local Maxwell shader extractor for Fire Emblem Engage 2.0.0.

Reads the user's extracted ExeFS (main) and RomFS using NX Native's MIT
metadata tables (shader_map.tsv, shader_pairs.tsv, shader_facts.tsv,
pso_manifest.tsv) and writes a temporary EGSHSEED binary file for
`engage-recomp --prepare-shaders`.

No Nintendo binaries or extracted shaders are distributed with the repository.
SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time
from typing import Callable

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import unityfs_reader as unityfs  # noqa: E402

DEFAULT_DATA_DIR = SCRIPT_DIR / 'data' / '0100A6301214E000'

SEED_MAGIC = b'EGSHSEED'
SEED_VERSION = 1
SEED_FLAG_FLIP_Y = 1 << 0
SEED_FLAG_DEPTH_MINUS_ONE = 1 << 1

CTL_MAGIC = bytes.fromhex('34127698')
GFX_MAGIC = bytes.fromhex('78563412')
GLSLC_MAGIC = b'\x91\x68\x86\x19'

NV_STAGES = ('vs', 'ps', 'gs', 'hs', 'ds', 'cs')
STAGE_TO_ENGAGE = {'vs': 0, 'ps': 1, 'gs': 2, 'hs': 3, 'ds': 4, 'cs': 5}
STAGE_NAME_TO_CODE = {
    'vertex': 'vs',
    'pixel': 'ps',
    'geometry': 'gs',
    'hull': 'hs',
    'domain': 'ds',
    'compute': 'cs',
}

# Valid historical pso_skey byte lengths from nxnative/dm/gfx/nvn/nvn_pso_skey.h
VALID_PSO_SKEY_LENS = (432, 440, 504, 512)

HEADER_STRUCT = struct.Struct('<8s6I')
SHADER_STRUCT = struct.Struct('<8s8I')
PAIR_STRUCT = struct.Struct('<3I')
PSO_STRUCT = struct.Struct('<2I8I6I64B32I32I32I16I16I2B6x')

assert HEADER_STRUCT.size == 32
assert SHADER_STRUCT.size == 40
assert PAIR_STRUCT.size == 12
assert PSO_STRUCT.size == 648


@dataclass
class PsoRecord:
    vs_key: str
    fs_key: str
    cfmt: tuple[int, ...]
    dfmt: int
    sfmt: int
    samples: int
    blend_en: int
    chmask: int
    a2c: int
    blend: bytes          # 64 bytes: 8 targets * 8 bytes
    attr_fmt: list[int]   # 32 ints
    attr_off: list[int]   # 32 ints
    attr_bind: list[int]  # 32 ints
    stream_stride: list[int]   # 16 ints
    stream_divisor: list[int]  # 16 ints


@dataclass
class ExtractStats:
    elapsed_sec: float
    files_scanned: int
    missing_files: int
    extracted_total: int
    matched_map: int
    skipped_gs: int
    pairs_written: int
    psos_written: int
    psos_filtered: int
    compute_written: int
    seed_bytes: int


def dm_shader_key(ctl: bytes, code: bytes) -> str:
    """Compute NX Native's 16-hex-char SHA-256 shader key (control prefix + code)."""
    own = struct.unpack_from('<I', ctl, 0x20)[0]
    clen = own if 0x760 <= own < 0x880 else 0x880
    return hashlib.sha256(ctl[:clen] + code).hexdigest()[:16]


def decompress_nso(raw: bytes) -> bytes:
    """Decompress NSO0 .text/.rodata/.data segments into a contiguous image."""
    if len(raw) < 0x100 or raw[:4] != b'NSO0':
        raise ValueError('Expected NSO0 binary for ExeFS main')
    flags = struct.unpack_from('<I', raw, 0x0C)[0]
    segs = []
    for i, off_hdr in enumerate((0x10, 0x20, 0x30)):
        foff, _moff, dsz = struct.unpack_from('<III', raw, off_hdr)
        csz = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        comp = bool(flags & (1 << i))
        in_len = csz if comp else dsz
        if foff + in_len > len(raw):
            raise ValueError(f'Truncated NSO0 segment {i}')
        chunk = raw[foff:foff + in_len]
        segs.append(unityfs.lz4_decompress_block(chunk, dsz) if comp else chunk)
    return b''.join(segs)


def extract_main_glslc(main_img: bytes) -> list[tuple[str, bytes, bytes, str]]:
    """Extract embedded GLSLC compiler outputs from ExeFS main."""
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
            if base + co + cn > len(main_img) or base + ko + kn > len(main_img):
                continue
            ctl = main_img[base + co:base + co + cn]
            code = main_img[base + ko:base + ko + kn]
            if len(ctl) < 0x760 or ctl[:4] != CTL_MAGIC:
                continue
            if struct.unpack_from('<I', ctl, 0x704)[0] != len(code):
                continue
            if len(code) >= 0x34 and code[:4] == GFX_MAGIC:
                sph0 = struct.unpack_from('<I', code, 0x30)[0]
                st = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}.get((sph0 >> 10) & 0xF)
            else:
                st = 'cs'
            if st:
                out.append((st, ctl, code, f'main#GLSLC@{h:#x}'))
    return out


def _decode_unity_program(code_blob: bytes, fallback_stage: str | None) -> list[tuple[str, bytes, bytes]]:
    if len(code_blob) >= 0x14 and code_blob[0x10:0x14] == CTL_MAGIC:
        if fallback_stage is None:
            return []
        nctl, ncode = struct.unpack_from('<2I', code_blob, 8)
        end = 0x10 + nctl + ncode
        if end > len(code_blob):
            return []
        ctl, cd = code_blob[0x10:0x10 + nctl], code_blob[0x10 + nctl:end]
        if len(ctl) >= 0x760 and ctl[:4] == CTL_MAGIC and struct.unpack_from('<I', ctl, 0x704)[0] == len(cd):
            return [(fallback_stage, ctl, cd)]
        return []
    if len(code_blob) < 0x60:
        return []
    h = struct.unpack_from('<24i', code_blob, 0)
    out = []
    for i, st in enumerate(NV_STAGES):
        off, nctl, ncode = h[6 + i], h[12 + i], h[18 + i]
        if off < 0:
            continue
        a = 0x60 + off
        if a + nctl + ncode > len(code_blob) or nctl < 0x760:
            continue
        ctl = code_blob[a:a + nctl]
        cd = code_blob[a + nctl:a + nctl + ncode]
        if ctl[:4] != CTL_MAGIC or struct.unpack_from('<I', ctl, 0x704)[0] != len(cd):
            continue
        if st != 'cs' and (len(cd) < 0x80 or cd[:4] != GFX_MAGIC):
            continue
        out.append((st, ctl, cd))
    return out


def _unity_stages(o: dict) -> dict[int, str]:
    m: dict[int, str] = {}
    for sub in (o.get('m_ParsedForm') or {}).get('m_SubShaders', []):
        for p in sub.get('m_Passes', []):
            for field, st in (('progVertex', 'vs'), ('progFragment', 'ps'),
                              ('progGeometry', 'gs'), ('progHull', 'hs'), ('progDomain', 'ds')):
                for sp in (p.get(field) or {}).get('m_SubPrograms', []):
                    m.setdefault(sp.get('m_BlobIndex'), st)
    return m


def extract_unity_file(path: Path, common: dict, trees: dict) -> list[tuple[str, bytes, bytes, str]]:
    """Extract all Switch Maxwell stage shaders from a UnityFS bundle or serialized file."""
    d = path.read_bytes()
    sers = unityfs.bundle_files(d) if d[:8] == b'UnityFS\0' else [('', d)]
    out = []
    for node, x in sers:
        if not unityfs.is_serialized(x):
            continue
        for _cid, _pid, o in unityfs.serialized_objects(x, common, {unityfs.CLASS_SHADER}, trees):
            name = o['m_ParsedForm']['m_Name'] if 'm_ParsedForm' in o else o.get('m_Name', '?')
            stages = _unity_stages(o)
            for plat, k, sp in unityfs.shader_programs(o):
                code_blob = unityfs.subprogram(sp)[4]
                for st, ctl, cd in _decode_unity_program(code_blob, stages.get(k)):
                    out.append((st, ctl, cd, f'{path.name}:{node}#{name}#var{plat}.{k}'))
    return out


def parse_pso_skey(raw: bytes) -> PsoRecord | None:
    """Decode an NX Native pso_skey blob into a Vulkan-convertible PsoRecord.

    Returns None when the pipeline relies on Metal-only / unsupported features
    (merged passes, tile-specialized fragments, alpha-test / logic-op / border variants)
    or is not a standard VS+FS graphics pipeline.
    """
    if len(raw) not in VALID_PSO_SKEY_LENS:
        return None
    vs_raw = raw[0:24].split(b'\0', 1)[0]
    fs_raw = raw[24:48].split(b'\0', 1)[0]
    if not vs_raw or not fs_raw:
        return None
    vs_key = vs_raw.decode('ascii', errors='replace').removesuffix('_b')
    fs_key = fs_raw.decode('ascii', errors='replace').removesuffix('_b')
    if len(vs_key) != 16 or len(fs_key) != 16:
        return None
    vs_stage, fs_stage, merged, fz = struct.unpack_from('<4B', raw, 48)
    if vs_stage != 0 or fs_stage != 1 or merged != 0 or fz != 0:
        return None

    koff = 88
    if len(raw) >= koff + 352:
        atest, logic = struct.unpack_from('<2I', raw, koff + 344)
        if atest or logic:
            return None
    if len(raw) >= koff + 424:
        border, _ = struct.unpack_from('<2I', raw, koff + 416)
        if border:
            return None

    cfmt = struct.unpack_from('<8I', raw, koff + 16)
    dfmt, sfmt, samples, blend_en, chmask, a2c = struct.unpack_from('<6I', raw, koff + 48)

    blend_bytes = bytearray(64)
    for i in range(8):
        b = struct.unpack_from('<Q', raw, koff + 72 + i * 8)[0]
        blend_bytes[i * 8 + 0] = b & 0x7
        blend_bytes[i * 8 + 1] = (b >> 3) & 0x7F
        blend_bytes[i * 8 + 2] = (b >> 10) & 0x7F
        blend_bytes[i * 8 + 3] = (b >> 17) & 0x7F
        blend_bytes[i * 8 + 4] = (b >> 24) & 0x7F
        blend_bytes[i * 8 + 5] = (b >> 31) & 0x7
        blend_bytes[i * 8 + 6] = (b >> 34) & 0x7
        blend_bytes[i * 8 + 7] = (b >> 37) & 0x3F

    attr_fmt = [0] * 32
    attr_off = [0] * 32
    attr_bind = [0] * 32
    for i in range(16):
        va = struct.unpack_from('<I', raw, koff + 136 + i * 4)[0]
        attr_fmt[i] = va & 0xFF
        attr_off[i] = (va >> 8) & 0x7FFFF
        attr_bind[i] = (va >> 27) & 0x1F
    if len(raw) >= koff + 416:
        for i in range(16):
            va = struct.unpack_from('<I', raw, koff + 352 + i * 4)[0]
            attr_fmt[16 + i] = va & 0xFF
            attr_off[16 + i] = (va >> 8) & 0x7FFFF
            attr_bind[16 + i] = (va >> 27) & 0x1F

    stream_stride = [0] * 16
    stream_divisor = [0] * 16
    for s in range(16):
        stride, div = struct.unpack_from('<2I', raw, koff + 200 + s * 8)
        stream_stride[s] = stride
        stream_divisor[s] = div

    return PsoRecord(
        vs_key=vs_key,
        fs_key=fs_key,
        cfmt=cfmt,
        dfmt=dfmt,
        sfmt=sfmt,
        samples=samples or 1,
        blend_en=blend_en,
        chmask=chmask,
        a2c=1 if a2c else 0,
        blend=bytes(blend_bytes),
        attr_fmt=attr_fmt,
        attr_off=attr_off,
        attr_bind=attr_bind,
        stream_stride=stream_stride,
        stream_divisor=stream_divisor,
    )


def load_metadata(data_dir: Path):
    """Load NX Native Engage metadata TSV tables from data_dir."""
    map_rows: dict[str, tuple[str, str]] = {}
    source_files: set[str] = set()
    for line in (data_dir / 'shader_map.tsv').read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#') or line.startswith('package\t'):
            continue
        _pkg, _idx, st, key, src = line.split('\t')
        map_rows[key] = (STAGE_NAME_TO_CODE[st], src)
        fpath = src.split(':', 1)[0].split('#', 1)[0]
        if fpath != 'main':
            source_files.add(fpath)

    pairs: list[tuple[str, str]] = []
    for line in (data_dir / 'shader_pairs.tsv').read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        vs_k, fs_k = line.split('\t')
        pairs.append((vs_k, fs_k))

    facts: dict[str, dict] = {}
    facts_path = data_dir / 'shader_facts.tsv'
    if facts_path.is_file():
        for line in facts_path.read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            k, j = line.split('\t', 1)
            facts[k] = json.loads(j)

    pso_s_keys: list[str] = []
    pso_records: list[PsoRecord] = []
    pso_filtered = 0
    manifest_path = data_dir / 'pso_manifest.tsv'
    if manifest_path.is_file():
        for line in manifest_path.read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            f = line.split('\t')
            if f[0] == 'S' and len(f) >= 2:
                pso_s_keys.append(f[1].removesuffix('_b'))
            elif f[0] == 'R' and len(f) >= 2:
                rec = parse_pso_skey(bytes.fromhex(f[1]))
                if rec is None:
                    pso_filtered += 1
                else:
                    pso_records.append(rec)

    return map_rows, source_files, pairs, facts, pso_s_keys, pso_records, pso_filtered


def _discover_all_unity_files(romfs: Path) -> list[Path]:
    data_dir = romfs / 'Data'
    if not data_dir.is_dir():
        return []
    found = []
    for p in sorted(data_dir.rglob('*')):
        if not p.is_file() or p.is_symlink():
            continue
        if p.name in ('data.unity3d', 'unity default resources') or p.suffix == '.bundle':
            found.append(p)
    return found


def extract_seed(
    romfs: Path,
    exefs: Path,
    out_seed: Path,
    data_dir: Path = DEFAULT_DATA_DIR,
    scan_all: bool = False,
    flip_y: bool = False,
    depth_minus_one: bool = False,
    progress_cb: Callable[[int, int, str], None] | None = None,
) -> ExtractStats:
    """Extract Maxwell shaders locally and write EGSHSEED binary atomically."""
    t0 = time.monotonic()
    map_rows, source_files, pairs, facts, pso_s_keys, pso_records, pso_filtered = load_metadata(data_dir)

    main_path = exefs / 'main'
    if not main_path.is_file():
        raise FileNotFoundError(f'Missing ExeFS main: {main_path}')
    main_img = decompress_nso(main_path.read_bytes())
    common = unityfs.common_strings(main_img)

    extracted: dict[str, tuple[str, bytes, bytes, str]] = {}
    for st, ctl, cd, src in extract_main_glslc(main_img):
        k = dm_shader_key(ctl, cd)
        extracted.setdefault(k, (st, ctl, cd, src))

    trees = {k: unityfs.known_tree(*k) for k in unityfs.KNOWN_TREES}
    missing_files = 0
    if scan_all:
        file_list = _discover_all_unity_files(romfs)
    else:
        ordered_rels = sorted(source_files, key=lambda p: (0 if '/StreamingAssets/' in p else 1, p))
        file_list = [romfs / rel.lstrip('/') for rel in ordered_rels]

    total_files = len(file_list)
    for idx, p in enumerate(file_list, 1):
        if progress_cb and (idx == 1 or idx % 10 == 0 or idx == total_files):
            progress_cb(idx, total_files, p.name)
        if not p.is_file():
            missing_files += 1
            continue
        for st, ctl, cd, src in extract_unity_file(p, common, trees):
            k = dm_shader_key(ctl, cd)
            extracted.setdefault(k, (st, ctl, cd, src))

    pso_key_set = set(pso_s_keys)
    for r in pso_records:
        pso_key_set.add(r.vs_key)
        pso_key_set.add(r.fs_key)

    pair_key_set = set()
    for vs_k, fs_k in pairs:
        pair_key_set.add(vs_k)
        pair_key_set.add(fs_k)

    # Assign priorities and build ordered SeedShaderRec list
    # Priority: 0 = pso_manifest, 1 = shader_pairs, 2 = other/compute
    def _prio(k: str, st: str) -> int:
        if k in pso_key_set:
            return 0
        if k in pair_key_set and st in ('vs', 'ps'):
            return 1
        return 2

    # Order shaders deterministically: pso_s_keys order first, then sorted remaining keys
    ordered_keys: list[str] = []
    seen_k: set[str] = set()
    for k in pso_s_keys:
        if k in extracted and k not in seen_k:
            seen_k.add(k)
            ordered_keys.append(k)
    for r in pso_records:
        for k in (r.vs_key, r.fs_key):
            if k in extracted and k not in seen_k:
                seen_k.add(k)
                ordered_keys.append(k)
    for k in sorted(extracted):
        if k not in seen_k:
            seen_k.add(k)
            ordered_keys.append(k)

    skipped_gs = 0
    key_to_idx: dict[str, int] = {}
    shader_entries: list[tuple[bytes, int, int, int, int, tuple[int, int, int], bytes]] = []
    for k in ordered_keys:
        st, ctl, cd, _src = extracted[k]
        if st == 'gs':
            skipped_gs += 1
            continue
        if st not in ('vs', 'ps', 'cs'):
            continue
        key_to_idx[k] = len(shader_entries)
        const_size, const_off = struct.unpack_from('<2I', ctl, 0x6FC)
        local = struct.unpack_from('<3I', ctl, 0x748) if st == 'cs' else (0, 0, 0)
        if st == 'cs' and k in facts and 'local' in facts[k]:
            local = tuple(int(v) or 1 for v in facts[k]['local'][:3])
        shader_entries.append((
            bytes.fromhex(k),
            STAGE_TO_ENGAGE[st],
            _prio(k, st),
            const_off,
            const_size,
            local,
            cd,
        ))

    # Build ordered SeedPairRec list (manifest pairs first, then remaining shader_pairs.tsv)
    pair_entries: list[tuple[int, int, int]] = []
    seen_pairs: set[tuple[str, str]] = set()
    for r in pso_records:
        pair = (r.vs_key, r.fs_key)
        if pair in seen_pairs or r.vs_key not in key_to_idx or r.fs_key not in key_to_idx:
            continue
        seen_pairs.add(pair)
        pair_entries.append((key_to_idx[r.vs_key], key_to_idx[r.fs_key], 0))
    for vs_k, fs_k in pairs:
        pair = (vs_k, fs_k)
        if pair in seen_pairs or vs_k not in key_to_idx or fs_k not in key_to_idx:
            continue
        seen_pairs.add(pair)
        pair_entries.append((key_to_idx[vs_k], key_to_idx[fs_k], 1))

    # Build SeedPsoRec list preserving pso_manifest.tsv order (dedup identical payloads)
    pso_blobs: list[bytes] = []
    seen_pso_blobs: set[bytes] = set()
    for r in pso_records:
        if r.vs_key not in key_to_idx or r.fs_key not in key_to_idx:
            continue
        blob = PSO_STRUCT.pack(
            key_to_idx[r.vs_key],
            key_to_idx[r.fs_key],
            *r.cfmt,
            r.dfmt,
            r.sfmt,
            r.samples,
            r.blend_en,
            r.chmask,
            r.a2c,
            *r.blend,
            *r.attr_fmt,
            *r.attr_off,
            *r.attr_bind,
            *r.stream_stride,
            *r.stream_divisor,
            2,  # topo_class = TRIANGLE_LIST
            1,  # poly_fill = FILL
        )
        if blob not in seen_pso_blobs:
            seen_pso_blobs.add(blob)
            pso_blobs.append(blob)

    compute_indices = [idx for idx, entry in enumerate(shader_entries) if entry[1] == STAGE_TO_ENGAGE['cs']]

    flags = (SEED_FLAG_FLIP_Y if flip_y else 0) | (SEED_FLAG_DEPTH_MINUS_ONE if depth_minus_one else 0)

    out_seed.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_seed.with_name(out_seed.name + '.tmp')
    try:
        with tmp.open('wb') as f:
            f.write(HEADER_STRUCT.pack(
                SEED_MAGIC,
                SEED_VERSION,
                flags,
                len(shader_entries),
                len(pair_entries),
                len(pso_blobs),
                len(compute_indices),
            ))
            for dm_bytes, st_num, prio, const_off, const_size, local, cd in shader_entries:
                f.write(SHADER_STRUCT.pack(
                    dm_bytes,
                    st_num,
                    prio,
                    const_off,
                    const_size,
                    len(cd),
                    local[0],
                    local[1],
                    local[2],
                ))
                f.write(cd)
            for vi, fi, prio in pair_entries:
                f.write(PAIR_STRUCT.pack(vi, fi, prio))
            for blob in pso_blobs:
                f.write(blob)
            for ci in compute_indices:
                f.write(struct.pack('<I', ci))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, out_seed)
    finally:
        tmp.unlink(missing_ok=True)

    matched_map = sum(1 for k in map_rows if k in extracted)
    return ExtractStats(
        elapsed_sec=time.monotonic() - t0,
        files_scanned=total_files,
        missing_files=missing_files,
        extracted_total=len(extracted),
        matched_map=matched_map,
        skipped_gs=skipped_gs,
        pairs_written=len(pair_entries),
        psos_written=len(pso_blobs),
        psos_filtered=pso_filtered,
        compute_written=len(compute_indices),
        seed_bytes=out_seed.stat().st_size,
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description='Extract Engage Maxwell shaders into a local EGSHSEED file.')
    ap.add_argument('--romfs', type=Path, required=True, help='Path to extracted RomFS directory')
    ap.add_argument('--exefs', type=Path, required=True, help='Path to extracted ExeFS directory')
    ap.add_argument('--out', type=Path, required=True, help='Output shader-seed.bin path')
    ap.add_argument('--data', type=Path, default=DEFAULT_DATA_DIR, help='Metadata TSV directory')
    ap.add_argument('--scan-all', action='store_true', help='Scan all Unity bundles/assets instead of shader_map paths')
    ap.add_argument('--flip-y', action='store_true', help='Set SEED_FLAG_FLIP_Y in header')
    ap.add_argument('--depth-minus-one', action='store_true', help='Set SEED_FLAG_DEPTH_MINUS_ONE in header')
    ap.add_argument('--quiet', action='store_true', help='Suppress summary output')
    args = ap.parse_args(argv)

    stats = extract_seed(
        romfs=args.romfs,
        exefs=args.exefs,
        out_seed=args.out,
        data_dir=args.data,
        scan_all=args.scan_all,
        flip_y=args.flip_y,
        depth_minus_one=args.depth_minus_one,
    )
    if not args.quiet:
        print(
            f'[shader-extract] {stats.extracted_total} shaders extracted '
            f'({stats.matched_map} map matches, {stats.skipped_gs} gs skipped), '
            f'{stats.pairs_written} pairs, {stats.psos_written} psos '
            f'({stats.psos_filtered} filtered), {stats.compute_written} compute '
            f'-> {args.out} ({stats.seed_bytes / (1024 * 1024):.2f} MiB) in {stats.elapsed_sec:.2f} s',
            flush=True,
        )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
