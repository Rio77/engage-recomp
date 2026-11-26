"""Unit tests for Atmosphere cheat detection, incremental chunk rebuild, and Quit/Rebuild flow."""
from pathlib import Path
import hashlib
import importlib
import json
import struct
import sys
import tempfile
import unittest
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parents[1]
LINUX_DIR = Path(__file__).resolve().parent
for p in (str(TOOLS_DIR), str(LINUX_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

import cheats
import engage_build
import app


def _dummy_elf64_so() -> bytes:
    # Minimal little-endian ELF64 header sufficient for verify_library (0 section headers).
    hdr = bytearray(64)
    hdr[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<HHI', hdr, 16, 3, 62, 1)  # ET_DYN, x86-64
    struct.pack_into('<Q', hdr, 40, 0)           # e_shoff = 0
    struct.pack_into('<HHH', hdr, 58, 64, 0, 0)  # e_shentsize, e_shnum, e_shstrndx
    return bytes(hdr)


class CheatsAndRebuildTests(unittest.TestCase):
    def test_cheats_zero_regs_and_register_write_tracking(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'cheats.txt'
            p.write_text(
                '[Exp 2x]\n'
                '040B0000 01A39F74 0B0107E0\n'
                '[Modified Reg]\n'
                '400B0000 00000000 00001000\n'
                '040B0000 01A39F80 12345678\n'
                '[Infinite Money]\n'
                '580F0000 05E5EC30\n'
                '580F1000 000000B8\n'
                '780F0000 00000058\n'
                '640F0000 00000000 0098967F\n',
                encoding='utf-8',
            )
            cv = cheats.code_variants(str(p), lambda off: 0 <= off < 0x3f47038)
            self.assertEqual(cv, {0x01A39F74: [0x0B0107E0]})

    def test_incremental_rebuild_only_recompiles_dirty_chunks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / 'bundle'
            ws = root / 'cache/gamelib-build'
            exefs = root / 'extracted/exefs'
            (bundle / 'zig').mkdir(parents=True)
            (bundle / 'engagegen').mkdir(parents=True)
            (bundle / 'gamelib-src').mkdir(parents=True)
            (bundle / 'zig/zig').write_text('#!/bin/sh\nexit 0\n')
            (bundle / 'engagegen/engagegen').write_text('#!/bin/sh\nexit 0\n')
            (bundle / 'gamelib-src/cpu.h').write_text('typedef struct Cpu { int x; } Cpu;\n')
            (bundle / 'gamelib-src/hle_symbols.txt').write_text('')
            exefs.mkdir(parents=True)
            (exefs / 'main').write_bytes(b'NSO0dummy-main-binary')
            cheats_file = root / 'cheats.txt'
            cheats_file.write_text('[Exp 2x]\n040B0000 01A39F74 0B0107E0\n', encoding='utf-8')

            # Populate cached build metadata & objects as after a clean install.
            (ws / 'source/runtime').mkdir(parents=True)
            (ws / 'source/runtime/cpu.h').write_text('typedef struct Cpu { int x; } Cpu;\n')
            (ws / 'source/runtime/hle_symbols.txt').write_text('')
            (ws / 'generated/main').mkdir(parents=True)
            (ws / 'generated/main/main_table.c').write_text('/* table */\n')
            (ws / 'generated/main/cheat_variants.txt').write_text('')
            (ws / 'generated/main/main_chunks.json').write_text(json.dumps({
                'format': 1,
                'build_id': 'test',
                'simd': 'c',
                'chunks': [{'file': 'main_000.c', 'items': []}, {'file': 'main_001.c', 'items': []}],
            }))
            main_sha = hashlib.sha256((exefs / 'main').read_bytes()).hexdigest()
            (ws / 'build-meta.json').write_text(json.dumps({
                'format': 1,
                'gamelib_id': engage_build.GAMELIB_ID,
                'main_sha256': main_sha,
            }))
            (ws / 'compiler-probe.o').write_bytes(b'OBJ')
            for sub in ('main/main_000.o', 'main/main_001.o', 'main/main_table.o',
                        'sdk/sdk_000.o', 'subsdk0/subsdk0_000.o', 'entry.o'):
                p = ws / 'objects' / sub
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'OBJ')

            compiled_inputs = []
            linked = []

            def fake_run(cmd, env=None, check=True):
                if '--update-cheats' in cmd:
                    (ws / 'generated/main/main_001.c').write_text('/* regenerated */\n')
                    (ws / 'generated/main/cheat_variants.txt').write_text('01a39f74 0b0107e0\n')
                    (ws / 'dirty_chunks.txt').write_text('main_001.c\n')
                elif '-c' in cmd:
                    c_path = Path(cmd[cmd.index('-c') + 1])
                    o_path = Path(cmd[cmd.index('-o') + 1])
                    compiled_inputs.append(c_path.name)
                    o_path.write_bytes(b'NEW_OBJ')
                elif '-shared' in cmd:
                    o_path = Path(cmd[cmd.index('-o') + 1])
                    linked.append(o_path.name)
                    o_path.write_bytes(_dummy_elf64_so())
                return mock.Mock(returncode=0)

            with mock.patch.object(engage_build.subprocess, 'run', side_effect=fake_run):
                out = engage_build.build(bundle, ws, exefs, cheats_path=cheats_file, incremental=True)

            self.assertEqual(compiled_inputs, ['main_001.c'])
            self.assertEqual(linked, ['engagegame.so.tmp'])
            self.assertEqual((ws / 'engagegame.so.cheats').read_text(), '01a39f74 0b0107e0\n')
            self.assertTrue(out.is_file())

    def test_resume_rebuilds_completed_legacy_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = root / 'bundle'
            headers = bundle / 'gamelib-src'
            headers.mkdir(parents=True)
            (headers / 'gamelib_entry.c').write_text('int new_entry;')
            exefs = root / 'exefs'
            exefs.mkdir()
            (exefs / 'main').write_bytes(b'NSO0')
            workspace = root / 'cache'
            generated = workspace / 'generated/main'
            generated.mkdir(parents=True)
            (generated / 'main_table.c').touch()
            (workspace / 'build-meta.json').write_text('{"format": 1, "gamelib_id": "engage-030-perf"}')
            entry = workspace / 'objects/entry.o'
            entry.parent.mkdir()
            entry.write_bytes(b'old')
            generated_new = generated / 'main_new.c'

            def run_tool(cmd, **kwargs):
                if '--workspace' in cmd:
                    generated_new.touch()
                elif '-c' in cmd:
                    Path(cmd[cmd.index('-o') + 1]).write_bytes(b'new')
                elif '-shared' in cmd:
                    Path(cmd[cmd.index('-o') + 1]).write_bytes(_dummy_elf64_so())

            with mock.patch.dict('os.environ', ENGAGE_GAMELIB_RESUME='1'):
                with mock.patch.object(engage_build.subprocess, 'run', side_effect=run_tool):
                    engage_build.build(bundle, workspace, exefs, incremental=True)

            self.assertTrue(generated_new.is_file())
            self.assertEqual(entry.read_bytes(), b'new')

    def test_install_worker_rebuilds_legacy_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stage = root / 'stage'
            (stage / 'data/extracted').mkdir(parents=True)
            exefs = root / 'exefs'
            romfs = root / 'romfs'
            exefs.mkdir()
            romfs.mkdir()
            old = root / 'old/engagegame.so'
            old.parent.mkdir()
            old.write_bytes(b'old library')
            old.with_suffix('.so.id').write_text('engage-030-perf\n')
            rebuilt = root / 'new/engagegame.so'
            rebuilt.parent.mkdir()
            rebuilt.write_bytes(b'new library')
            rebuilt.with_suffix('.so.id').write_text(engage_build.GAMELIB_ID + '\n')

            with mock.patch.object(app, 'build', return_value=rebuilt):
                with mock.patch('sys.argv', ['app.py', '--worker', str(stage),
                                             '--exefs', str(exefs), '--romfs', str(romfs),
                                             '--reuse-gamelib', str(old)]):
                    app.main()

            installed = stage / 'data/extracted/engagegame.so'
            self.assertEqual(installed.read_bytes(), b'new library')
            self.assertEqual(installed.with_suffix('.so.id').read_text().strip(), engage_build.GAMELIB_ID)

    def test_play_rebuilds_legacy_library_once_before_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'install'
            library = root / 'extracted/engagegame.so'
            library.parent.mkdir(parents=True)
            library.write_bytes(b'old')
            id_file = library.with_suffix('.so.id')
            id_file.write_text('engage-030-perf\n')
            events = []

            def fake_rebuild(install, *, interactive, launch_after):
                self.assertEqual(install.root, root)
                self.assertFalse(interactive)
                self.assertTrue(launch_after)
                events.append('rebuild')
                id_file.write_text(engage_build.GAMELIB_ID + '\n')

            def fake_popen(cmd, **kwargs):
                self.assertEqual(cmd[-1], '--play')
                self.assertTrue(engage_build.reusable_gamelib(library))
                events.append('play')
                proc = mock.Mock()
                proc.wait.return_value = 0
                return proc

            with mock.patch.object(app, 'rebuild_gamelib', side_effect=fake_rebuild), \
                 mock.patch.object(app.subprocess, 'Popen', side_effect=fake_popen), \
                 mock.patch('sys.argv', ['app.py', '--play', '--noninteractive', '--root', str(root)]):
                app.main()
                app.main()

            self.assertEqual(events, ['rebuild', 'play', 'play'])

    def test_play_does_not_launch_after_failed_migration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'install'
            library = root / 'extracted/engagegame.so'
            library.parent.mkdir(parents=True)
            library.write_bytes(b'old')
            library.with_suffix('.so.id').write_text('engage-030-perf\n')

            error = RuntimeError('fixture build failed')
            with mock.patch.object(app, 'rebuild_gamelib', side_effect=error), \
                 mock.patch.object(app.subprocess, 'Popen') as popen, \
                 mock.patch('sys.argv', ['app.py', '--play', '--noninteractive', '--root', str(root)]):
                with self.assertRaisesRegex(RuntimeError, 'fixture build failed'):
                    app.main()
            popen.assert_not_called()

    def test_play_keeps_compatible_symlinked_game_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            library = base / 'library.so'
            library.write_bytes(b'shared')
            root = base / 'install'
            path = root / 'extracted/engagegame.so'
            path.parent.mkdir(parents=True)
            path.symlink_to(library)
            path.with_suffix('.so.id').write_text(engage_build.GAMELIB_ID + '\n')
            with mock.patch.object(app, 'rebuild_gamelib') as rebuild, \
                 mock.patch.object(app.subprocess, 'Popen') as popen, \
                 mock.patch('sys.argv', ['app.py', '--play', '--noninteractive', '--root', str(root)]):
                popen.return_value.wait.return_value = 0
                app.main()
            rebuild.assert_not_called()
            self.assertEqual(popen.call_args.args[0][-1], '--play')
            self.assertTrue(path.is_symlink())

    def test_play_handles_exit_42_rebuild_and_stays_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'install'
            (root / 'extracted').mkdir(parents=True)
            (root / 'extracted/engagegame.so').write_bytes(b'SO')
            (root / 'extracted/engagegame.so.id').write_text(engage_build.GAMELIB_ID + '\n')
            (root / 'cache').mkdir(parents=True)

            popens = []

            def fake_popen(cmd, **kwargs):
                popens.append(cmd)
                proc = mock.Mock()
                proc.wait.return_value = 42
                return proc

            rebuilt = []

            def fake_rebuild(inst, interactive=True):
                rebuilt.append((inst.root, interactive))

            with mock.patch.object(app.subprocess, 'Popen', side_effect=fake_popen), \
                 mock.patch.object(app, 'rebuild_gamelib', side_effect=fake_rebuild), \
                 mock.patch('sys.argv', ['app.py', '--play', '--noninteractive', '--root', str(root)]):
                app.main()

            self.assertEqual(len(popens), 1)  # Must NOT relaunch game after rebuild.
            self.assertEqual(rebuilt, [(root.resolve(), False)])


if __name__ == '__main__':
    unittest.main()
