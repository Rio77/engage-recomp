"""Synthetic unit tests for Phase 1 shader extractor (no game data required)."""
from __future__ import annotations

import ctypes
import ctypes.util
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / 'shader_prep'))

import engage_shader_extract as ese  # noqa: E402
import unityfs_reader as unityfs  # noqa: E402


def _make_ctl(code_len: int, const_off: int = 0x100, const_size: int = 0, local=(4, 4, 4)) -> bytes:
    ctl = bytearray(0x880)
    ctl[0:4] = ese.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, 0x880)
    struct.pack_into('<3I', ctl, 0x6FC, const_size, const_off, code_len)
    struct.pack_into('<3I', ctl, 0x748, *local)
    return bytes(ctl)


def _make_gfx_code(stage_bits: int, body_len: int = 0x100) -> bytes:
    code = bytearray(body_len)
    code[0:4] = ese.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (stage_bits << 10) | 0x61)
    struct.pack_into('<Q', code, 0x88, 0xE300000000000000)  # EXIT-like word
    return bytes(code)


def _make_glslc(ctl: bytes, code: bytes) -> bytes:
    # Single-section GLSLC container: header 0x90 + section header 0x90 + gpu header 0x40 + ctl + code
    sec_off = 0x120
    gpu_off = 0x40
    co = gpu_off
    ko = gpu_off + len(ctl)
    total = sec_off + gpu_off + len(ctl) + len(code)
    buf = bytearray(total)
    buf[0:4] = ese.GLSLC_MAGIC
    struct.pack_into('<I', buf, 0x44, total)
    struct.pack_into('<I', buf, 0x4C, 1)  # nsec
    # section header at 0x90: size, offset, type=0
    struct.pack_into('<3I', buf, 0x90, gpu_off + len(ctl) + len(code), sec_off, 0)
    # gpu code header fields inside section header at 0x90 + 0x30: co, ko, kn, cn
    struct.pack_into('<4I', buf, 0x90 + 0x30, co, ko, len(code), len(ctl))
    buf[sec_off + co:sec_off + co + len(ctl)] = ctl
    buf[sec_off + ko:sec_off + ko + len(code)] = code
    return bytes(buf)


def _make_nso(payload: bytes) -> bytes:
    # Uncompressed NSO0 with 3 segments (text=payload, rodata=empty, data=empty)
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    struct.pack_into('<I', hdr, 0x0C, 0)  # uncompressed
    struct.pack_into('<3I', hdr, 0x10, 0x100, 0, len(payload))
    struct.pack_into('<3I', hdr, 0x20, 0x100 + len(payload), len(payload), 0)
    struct.pack_into('<3I', hdr, 0x30, 0x100 + len(payload), len(payload), 0)
    return bytes(hdr) + payload


def _lz4_compress_raw(data: bytes) -> bytes | None:
    lib_name = ctypes.util.find_library('lz4') or 'liblz4.so.1'
    lib = ctypes.CDLL(lib_name)
    if not hasattr(lib, 'LZ4_compress_default'):
        return None
    lib.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.LZ4_compress_default.restype = ctypes.c_int
    bound = len(data) + len(data) // 255 + 16
    out = ctypes.create_string_buffer(bound)
    n = lib.LZ4_compress_default(data, out, len(data), bound)
    return out.raw[:n] if n > 0 else None


def _pack_unity_string(s: str) -> bytes:
    b = s.encode('utf-8')
    pad = (-len(b)) & 3
    return struct.pack('<I', len(b)) + b + b'\0' * pad


def _pack_shader_2020_3(name: str, subprograms: list[bytes]) -> bytes:
    # Minimal SHADER_2020_3 serialized object matching unityfs.SHADER_2020_3 field order
    out = bytearray()
    out += _pack_unity_string(name)  # Shader.m_Name
    # SerializedShader m_ParsedForm:
    out += struct.pack('<I', 0)  # m_PropInfo.m_Props count = 0
    out += struct.pack('<I', 0)  # m_SubShaders count = 0
    out += _pack_unity_string(name)  # m_Name
    out += _pack_unity_string('')    # m_CustomEditorName
    out += _pack_unity_string('')    # m_FallbackName
    out += struct.pack('<I', 0)      # m_Dependencies count = 0
    out += struct.pack('<B3x', 0)    # bool! m_DisableNoSubshadersMessage
    # platforms: [21 (Switch)]
    out += struct.pack('<II', 1, 21)
    # Build segment 0 inside compressedBlob: count + 12-byte entries {off, len, seg=0} + subprograms
    seg0 = bytearray()
    seg0 += struct.pack('<I', len(subprograms))
    data_off = 4 + 12 * len(subprograms)
    bodies = bytearray()
    for sp in subprograms:
        seg0 += struct.pack('<3I', data_off + len(bodies), len(sp), 0)
        bodies += sp
    seg0 += bodies
    comp = _lz4_compress_raw(bytes(seg0))
    assert comp is not None, 'liblz4 LZ4_compress_default required for synthetic test'
    # offsets: vector<vector<u32>> (1 platform, 1 segment)
    out += struct.pack('<III', 1, 1, 0)
    # compressedLengths
    out += struct.pack('<III', 1, 1, len(comp))
    # decompressedLengths
    out += struct.pack('<III', 1, 1, len(seg0))
    # compressedBlob (unaligned Array, then Shader has no trailing align? Wait: UInt8 vector has aligned Array)
    out += struct.pack('<I', len(comp)) + comp + b'\0' * ((-len(comp)) & 3)
    # m_Dependencies, m_NonModifiableTextures, bool! m_ShaderIsBaked
    out += struct.pack('<II', 0, 0) + struct.pack('<B3x', 1)
    return bytes(out)


def _pack_subprogram(vs_ctl: bytes, vs_code: bytes, fs_ctl: bytes, fs_code: bytes) -> bytes:
    # subprogram header (version 201806140, ptype=1) + kw + lkw + program code (0x60 header + vs + fs)
    prog = bytearray(0x60)
    vs_off = 0
    fs_off = len(vs_ctl) + len(vs_code)
    offs = [vs_off, fs_off, -1, -1, -1, -1]
    ctls = [len(vs_ctl), len(fs_ctl), 0, 0, 0, 0]
    cds = [len(vs_code), len(fs_code), 0, 0, 0, 0]
    struct.pack_into('<24i', prog, 0, *([0] * 6 + offs + ctls + cds))
    prog += vs_ctl + vs_code + fs_ctl + fs_code

    sp = bytearray()
    sp += struct.pack('<2i', 201806140, 1) + bytes(16)
    sp += struct.pack('<2I', 0, 0)  # kw=0, lkw=0
    sp += struct.pack('<I', len(prog)) + prog + b'\0' * ((-len(prog)) & 3)
    return bytes(sp)


def _pack_serialized_file(obj_bytes: bytes) -> bytes:
    # Serialized file format 20 (little-endian), no embedded type tree (uses KNOWN_TREES SHADER_2020_3)
    shader_hash = bytes.fromhex('a30e3dcf6c6d25153d6f64e5e41caf4e')
    meta = bytearray()
    meta += _pack_unity_string('2020.3.18f1')[:-1]  # wait: serialized header version string is null-terminated!
    meta = bytearray(b'2020.3.18f1\0')
    meta += struct.pack('<iB', 5, 0)  # platform=5 (Switch?), enableTypeTree=False
    # type table: 1 type (CLASS_SHADER=48)
    meta += struct.pack('<I', 1)
    meta += struct.pack('<iBh', 48, 0, -1) + shader_hash
    # object table: 1 object
    meta += struct.pack('<I', 1)
    meta += b'\0' * ((-len(meta)) & 3)
    meta += struct.pack('<qIII', 1, 0, len(obj_bytes), 0)

    hdr_size = 20
    doff = (hdr_size + len(meta) + 15) & ~15
    pad = doff - (hdr_size + len(meta))
    fsz = doff + len(obj_bytes)
    hdr = struct.pack('>IIII', len(meta), fsz, 20, doff) + struct.pack('<B3x', 1)  # le=1 (d[16]==0 for little-endian!)
    # Wait: in unityfs.py line 408: le = d[16] == 0! So d[16] must be 0 for little-endian!
    hdr = struct.pack('>IIII', len(meta), fsz, 20, doff) + b'\x00\x00\x00\x00'
    return hdr + bytes(meta) + b'\0' * pad + obj_bytes


def _pack_unityfs_bundle(node_name: str, ser_bytes: bytes) -> bytes:
    info = bytearray(16)  # 16-byte hash
    info += struct.pack('>iIIH', 1, len(ser_bytes), len(ser_bytes), 0)  # 1 uncompressed block
    info += struct.pack('>iqqI', 1, 0, len(ser_bytes), 4) + node_name.encode() + b'\0'
    hdr = bytearray(b'UnityFS\0')
    hdr += struct.pack('>I', 7) + b'5.x.x\x002020.3.18f1\x00'
    # placeholder for size, csz, usz, flags=0 (uncompressed block info)
    p_pos = len(hdr)
    hdr += struct.pack('>qIII', 0, len(info), len(info), 0)
    while len(hdr) & 15:
        hdr.append(0)
    total = len(hdr) + len(info) + len(ser_bytes)
    struct.pack_into('>qIII', hdr, p_pos, total, len(info), len(info), 0)
    return bytes(hdr) + bytes(info) + ser_bytes


def _make_pso_skey_432(vs_key: str, fs_key: str, *, merged=0, fz=0) -> bytes:
    buf = bytearray(432)
    buf[0:len(vs_key)] = vs_key.encode('ascii')
    buf[24:24 + len(fs_key)] = fs_key.encode('ascii')
    struct.pack_into('<4B', buf, 48, 0, 1, merged, fz)
    koff = 88
    struct.pack_into('<8I', buf, koff + 16, 71, 0, 0, 0, 0, 0, 0, 0)  # cfmt[0]=71 (RGBA8_SRGB)
    struct.pack_into('<6I', buf, koff + 48, 260, 260, 1, 0xFF, 0xFFFFFFFF, 0)
    # blend_st[0]: target=0, srcC=2(ONE), dstC=1(ZERO), srcA=5(SRC_ALPHA), dstA=6(1-SRC_ALPHA), eqC=1, eqA=3, adv=0
    b0 = 0 | (2 << 3) | (1 << 10) | (5 << 17) | (6 << 24) | (1 << 31) | (3 << 34)
    struct.pack_into('<Q', buf, koff + 72, b0)
    # va_st[0]: fmt=0x22, off=0, stream=0; va_st[1]: fmt=0x16, off=12, stream=1
    va0 = 0x22 | (0 << 8) | (0 << 27)
    va1 = 0x16 | (12 << 8) | (1 << 27)
    struct.pack_into('<2I', buf, koff + 136, va0, va1)
    # vs_st[0]: stride=20, div=0; vs_st[1]: stride=16, div=2
    struct.pack_into('<4I', buf, koff + 200, 20, 0, 16, 2)
    return bytes(buf)


class ShaderExtractTests(unittest.TestCase):
    def test_pso_skey_decode_and_filter(self):
        vs_k = '0123456789abcdef'
        fs_k = 'fedcba9876543210'
        raw432 = _make_pso_skey_432(vs_k, fs_k)
        rec = ese.parse_pso_skey(raw432)
        self.assertIsNotNone(rec)
        self.assertEqual(rec.vs_key, vs_k)
        self.assertEqual(rec.fs_key, fs_k)
        self.assertEqual(rec.cfmt[0], 71)
        self.assertEqual(rec.dfmt, 260)
        self.assertEqual(rec.blend_en, 0xFF)
        self.assertEqual(list(rec.blend[:8]), [0, 2, 1, 5, 6, 1, 3, 0])
        self.assertEqual(rec.attr_fmt[0], 0x22)
        self.assertEqual(rec.attr_off[1], 12)
        self.assertEqual(rec.attr_bind[1], 1)
        self.assertEqual(rec.stream_stride[0], 20)
        self.assertEqual(rec.stream_divisor[1], 2)

        # Filtered cases
        self.assertIsNone(ese.parse_pso_skey(_make_pso_skey_432(vs_k, fs_k, merged=1)))
        self.assertIsNone(ese.parse_pso_skey(_make_pso_skey_432(vs_k, fs_k, fz=1)))
        self.assertIsNone(ese.parse_pso_skey(raw432 + b'\0' * 4))  # bad length
        raw512 = bytearray(raw432 + b'\0' * (512 - 432))
        struct.pack_into('<I', raw512, 88 + 416, 1)  # border=1
        self.assertIsNone(ese.parse_pso_skey(bytes(raw512)))

        # 512-byte with attr_hi[0] (slot 16)
        raw512_hi = bytearray(raw432 + b'\0' * (512 - 432))
        struct.pack_into('<I', raw512_hi, 88 + 352, 0x2E | (24 << 8) | (3 << 27))
        rec_hi = ese.parse_pso_skey(bytes(raw512_hi))
        self.assertIsNotNone(rec_hi)
        self.assertEqual(rec_hi.attr_fmt[16], 0x2E)
        self.assertEqual(rec_hi.attr_off[16], 24)
        self.assertEqual(rec_hi.attr_bind[16], 3)

    def test_synthetic_end_to_end_seed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exefs = root / 'exefs'
            romfs = root / 'romfs'
            data_dir = root / 'meta'
            exefs.mkdir()
            (romfs / 'Data' / 'StreamingAssets').mkdir(parents=True)
            data_dir.mkdir()

            vs_ctl = _make_ctl(0x100)
            vs_code = _make_gfx_code(1, 0x100)
            fs_ctl = _make_ctl(0x120)
            fs_code = _make_gfx_code(5, 0x120)
            cs_ctl = _make_ctl(0x80, const_off=0x80, local=(4, 4, 4))
            cs_code = b'\xF2\x07\xE0\xF7' + b'\x00' * 0x7C  # compute raw SASS

            vs_key = ese.dm_shader_key(vs_ctl, vs_code)
            fs_key = ese.dm_shader_key(fs_ctl, fs_code)
            cs_key = ese.dm_shader_key(cs_ctl, cs_code)

            # Build ExeFS main with common strings + compute GLSLC
            common_blob = b'AABB\0AnimationClip\0AnimationCurve\0Shader\0\0'
            glslc_blob = _make_glslc(cs_ctl, cs_code)
            (exefs / 'main').write_bytes(_make_nso(common_blob + b'\0' * 16 + glslc_blob))

            # Build UnityFS bundle with VS+FS shader
            sp = _pack_subprogram(vs_ctl, vs_code, fs_ctl, fs_code)
            sh_obj = _pack_shader_2020_3('Synthetic/TestShader', [sp])
            ser = _pack_serialized_file(sh_obj)
            bundle = _pack_unityfs_bundle('CAB-test', ser)
            bundle_rel = '/Data/StreamingAssets/test.shader.bundle'
            (romfs / bundle_rel.lstrip('/')).write_bytes(bundle)

            # Build metadata TSVs
            (data_dir / 'shader_map.tsv').write_text(
                'package\tindex\tstage\tdm_ctl_sha16\tsource\n'
                f'vs_assets.bin\t0\tvertex\t{vs_key}\t{bundle_rel}:CAB-test#Synthetic/TestShader#var0.0\n'
                f'ps_assets.bin\t0\tpixel\t{fs_key}\t{bundle_rel}:CAB-test#Synthetic/TestShader#var0.0\n'
                f'cs_main.bin\t0\tcompute\t{cs_key}\tmain#GLSLC@0x100\n',
                encoding='utf-8',
            )
            (data_dir / 'shader_pairs.tsv').write_text(f'{vs_key}\t{fs_key}\n', encoding='utf-8')
            (data_dir / 'shader_facts.tsv').write_text(f'{cs_key}\t{{"local":[8,8,1],"shared":0}}\n', encoding='utf-8')
            pso_hex = _make_pso_skey_432(vs_key, fs_key).hex()
            (data_dir / 'pso_manifest.tsv').write_text(
                f'S\t{vs_key}_b\t0\nS\t{fs_key}_b\t1\nR\t{pso_hex}\n',
                encoding='utf-8',
            )

            out_seed = root / 'out' / 'shader-seed.bin'
            stats = ese.extract_seed(romfs=romfs, exefs=exefs, out_seed=out_seed, data_dir=data_dir)

            self.assertEqual(stats.extracted_total, 3)
            self.assertEqual(stats.matched_map, 3)
            self.assertEqual(stats.pairs_written, 1)
            self.assertEqual(stats.psos_written, 1)
            self.assertEqual(stats.psos_filtered, 0)
            self.assertEqual(stats.compute_written, 1)
            self.assertFalse(out_seed.with_name(out_seed.name + '.tmp').exists())

            # Verify binary layout byte by byte
            raw = out_seed.read_bytes()
            magic, ver, flags, nsh, npairs, npsos, ncs = ese.HEADER_STRUCT.unpack_from(raw, 0)
            self.assertEqual(magic, b'EGSHSEED')
            self.assertEqual((ver, flags, nsh, npairs, npsos, ncs), (1, 0, 3, 1, 1, 1))

            pos = ese.HEADER_STRUCT.size
            recs = []
            for _ in range(nsh):
                dm_b, st, prio, coff, csz, clen, lx, ly, lz = ese.SHADER_STRUCT.unpack_from(raw, pos)
                pos += ese.SHADER_STRUCT.size
                cd = raw[pos:pos + clen]
                pos += clen
                recs.append((dm_b.hex(), st, prio, coff, csz, (lx, ly, lz), cd))

            self.assertEqual(recs[0][0], vs_key)
            self.assertEqual(recs[0][2], 0)  # priority 0 (in manifest)
            self.assertEqual(recs[1][0], fs_key)
            self.assertEqual(recs[2][0], cs_key)
            self.assertEqual(recs[2][5], (8, 8, 1))  # local size from shader_facts.tsv

            vi, fi, pprio = ese.PAIR_STRUCT.unpack_from(raw, pos)
            pos += ese.PAIR_STRUCT.size
            self.assertEqual((vi, fi, pprio), (0, 1, 0))

            pso_unpacked = ese.PSO_STRUCT.unpack_from(raw, pos)
            pos += ese.PSO_STRUCT.size
            self.assertEqual(pso_unpacked[0:2], (0, 1))
            self.assertEqual(pso_unpacked[2], 71)  # cfmt[0]
            self.assertEqual(pso_unpacked[10], 260)  # dfmt
            self.assertEqual(pso_unpacked[-2:], (2, 1))  # topo_class, poly_fill

            (ci,) = struct.unpack_from('<I', raw, pos)
            pos += 4
            self.assertEqual(ci, 2)
            self.assertEqual(pos, len(raw))

    def test_scan_all_missing_and_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exefs = root / 'exefs'
            romfs = root / 'romfs'
            data_dir = root / 'meta'
            exefs.mkdir()
            (romfs / 'Data' / 'StreamingAssets').mkdir(parents=True)
            data_dir.mkdir()

            (exefs / 'main').write_bytes(_make_nso(b'AABB\0AnimationClip\0AnimationCurve\0Shader\0\0'))
            vs_ctl = _make_ctl(0x100)
            vs_code = _make_gfx_code(1, 0x100)
            fs_ctl = _make_ctl(0x120)
            fs_code = _make_gfx_code(5, 0x120)
            sp = _pack_subprogram(vs_ctl, vs_code, fs_ctl, fs_code)
            bundle = _pack_unityfs_bundle('CAB-mod', _pack_serialized_file(_pack_shader_2020_3('Mod/Shader', [sp])))
            (romfs / 'Data' / 'StreamingAssets' / 'mod.shader.bundle').write_bytes(bundle)

            # Map points only to a non-existent bundle
            (data_dir / 'shader_map.tsv').write_text(
                'package\tindex\tstage\tdm_ctl_sha16\tsource\n'
                'vs_assets.bin\t0\tvertex\t0000000000000000\t/Data/StreamingAssets/missing.shader.bundle:x\n',
                encoding='utf-8',
            )
            (data_dir / 'shader_pairs.tsv').write_text('', encoding='utf-8')

            out_seed = root / 'seed.bin'
            stats_map = ese.extract_seed(romfs=romfs, exefs=exefs, out_seed=out_seed, data_dir=data_dir, scan_all=False)
            self.assertEqual(stats_map.missing_files, 1)
            self.assertEqual(stats_map.extracted_total, 0)

            stats_all = ese.extract_seed(
                romfs=romfs, exefs=exefs, out_seed=out_seed, data_dir=data_dir,
                scan_all=True, flip_y=True, depth_minus_one=True,
            )
            self.assertEqual(stats_all.extracted_total, 2)
            _m, _v, flags, nsh, *_ = ese.HEADER_STRUCT.unpack_from(out_seed.read_bytes(), 0)
            self.assertEqual(flags, ese.SEED_FLAG_FLIP_Y | ese.SEED_FLAG_DEPTH_MINUS_ONE)
            self.assertEqual(nsh, 2)


if __name__ == '__main__':
    unittest.main()
