"""Integration tests for installer shader preparation (Phase 4)."""
from pathlib import Path
from types import SimpleNamespace
import json
import os
import struct
import tempfile
import textwrap
import unittest
from unittest import mock

import app
from install_core import Installation


def _fake_vk_pcache(vendor=0x1002, device=0x163F, uuid_byte=0xAA):
    return struct.pack('<4I', 32, 1, vendor, device) + bytes([uuid_byte] * 16) + b'\x00' * 16


def _setup_fake_extracted(root: Path):
    extracted = root / 'extracted'
    (extracted / 'exefs').mkdir(parents=True, exist_ok=True)
    (extracted / 'romfs/Data/Managed/Metadata').mkdir(parents=True, exist_ok=True)
    (extracted / 'romfs/Data/StreamingAssets').mkdir(parents=True, exist_ok=True)
    (extracted / 'romfs/Data/data.unity3d').write_bytes(b'unity3d')
    (extracted / 'romfs/Data/Managed/Metadata/global-metadata.dat').write_bytes(b'meta')
    (extracted / 'engagegame.so').write_bytes(b'so')
    (extracted / 'engagegame.so.id').write_text(app.GAMELIB_ID + '\n')
    (extracted / 'mods').mkdir(exist_ok=True)
    (extracted / 'mods/test_mod.txt').write_text('keep-mod')
    (extracted / 'dlc/0100A6301214F001/romfs').mkdir(parents=True, exist_ok=True)
    (extracted / 'dlc/0100A6301214F001/romfs/d.bin').write_bytes(b'keep-dlc')
    (root / 'save').mkdir(exist_ok=True)
    (root / 'save/slot0').write_bytes(b'SAVE01')
    (root / 'larecomp.ini').write_text('fps=60\n')


def _write_fake_shader_prep_dir(base: Path):
    d = base / 'shader_prep'
    (d / 'data/0100A6301214E000').mkdir(parents=True)
    (d / 'data/0100A6301214E000/shader_map.tsv').write_text('')
    (d / 'engage_shader_extract.py').write_text(textwrap.dedent("""
        import argparse, pathlib
        ap = argparse.ArgumentParser()
        ap.add_argument('--exefs'); ap.add_argument('--romfs'); ap.add_argument('--data'); ap.add_argument('--out', required=True)
        a = ap.parse_args()
        pathlib.Path(a.out).write_bytes(b'EGSHSEED_SYNTH')
    """))
    return d


def _write_fake_engage_bin(bin_dir: Path, behavior='ok', uuid_byte=0xAA):
    bin_dir.mkdir(parents=True, exist_ok=True)
    exe = bin_dir / 'engage-recomp'
    exe.write_text(textwrap.dedent(f"""#!/usr/bin/env python3
import os, pathlib, signal, struct, sys, time
args = sys.argv[1:]
def arg(name):
    return args[args.index(name) + 1] if name in args else None
root = pathlib.Path(arg('--data-root'))
prog = pathlib.Path(arg('--progress-file'))
cache = root / 'cache'
(cache / 'xlat').mkdir(parents=True, exist_ok=True)
(cache / 'spirv').mkdir(parents=True, exist_ok=True)
behavior = {behavior!r}
uuid_byte = {uuid_byte}

stop = False
def on_sig(s, f):
    global stop
    stop = True
signal.signal(signal.SIGINT, on_sig)
signal.signal(signal.SIGTERM, on_sig)

if behavior == 'fail':
    print('[prepare] Vulkan headless initialization failed', file=sys.stderr)
    sys.exit(1)

if behavior == 'crash_once':
    marker = cache / '.crashed_once'
    if not marker.exists():
        marker.write_text('1')
        (cache / 'prepare-journal.bin').write_bytes(b'\\x01' * 64)
        sys.exit(134)
    (cache / 'prepare-journal.bin').unlink(missing_ok=True)

if behavior == 'slow_cancel':
    (cache / 'xlat/0000000000000001.bin').write_bytes(b'partial')
    prog.write_text('phase=shaders done=10 total=100 xlat_hits=0 xlat_new=10 xlat_fail=0 spv_hits=0 spv_new=5 spv_fail=0\\n')
    for _ in range(100):
        if stop:
            prog.write_text('phase=cancelled done=10 total=100 xlat_hits=0 xlat_new=10 xlat_fail=0 spv_hits=0 spv_new=5 spv_fail=0\\n')
            sys.exit(130)
        time.sleep(0.05)

(cache / 'xlat/1111111111111111.bin').write_bytes(b'xlat')
(cache / 'spirv/2222222222222222.spv').write_bytes(b'spv')
(cache / 'warm.bin').write_bytes(b'WARM')
(cache / 'vk_pipelines.bin').write_bytes(struct.pack('<4I', 32, 1, 0x1002, 0x163F) + bytes([uuid_byte]*16) + b'\\x00'*16)
prog.write_text('phase=done done=2 total=2 xlat_hits=0 xlat_new=2 xlat_fail=0 spv_hits=0 spv_new=1 spv_fail=0 pso_done=2 pso_total=2 pso_ok=2 pso_fail=0 pso_skip=0\\n')
"""))
    exe.chmod(0o755)
    return exe


class ShaderPrepareInstallerTests(unittest.TestCase):
    def test_play_only_waits_for_complete_frames_when_requested(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _setup_fake_extracted(root)
            for override, expected in ((None, None), ('0', '0'), ('1', '1')):
                with self.subTest(override=override):
                    env = {'ENGAGE_COMPLETE_FRAMES': override} if override is not None else {}
                    with mock.patch.dict(os.environ, env, clear=True), \
                         mock.patch('sys.argv', ['app.py', '--play', '--root', str(root)]), \
                         mock.patch.object(app.subprocess, 'Popen') as popen:
                        popen.return_value.wait.return_value = 0
                        app.main()
                    launch_env = popen.call_args_list[0].kwargs['env']
                    self.assertEqual(launch_env.get('ENGAGE_COMPLETE_FRAMES'), expected)
                    self.assertEqual(popen.call_count, 2 if override == '1' else 1)

    def test_end_to_end_skip_and_invalidation(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            root = tmp / 'install_root'
            _setup_fake_extracted(root)
            prep_dir = _write_fake_shader_prep_dir(tmp)
            bundle = tmp / 'bundle/bin'
            _write_fake_engage_bin(bundle, behavior='ok', uuid_byte=0xAA)
            app_file = tmp / 'Engage.AppImage'
            app_file.write_bytes(b'app-v1')

            events = []
            st1 = app.prepare_shaders_step(
                root, app_path=app_file, interactive=False,
                bundle_dir=bundle, shader_prep_dir=prep_dir,
                on_progress=lambda m, p: events.append((m, p)),
            )
            self.assertEqual(st1, 'complete')
            self.assertTrue((root / 'cache/prepare-meta.json').is_file())
            self.assertTrue((root / 'cache/warm.bin').is_file())
            self.assertEqual(list((root / '.installer-work').glob('shader-prep-*')), [])
            self.assertTrue(any(p == 100 for _, p in events))

            # Second run with identical AppImage, binary, and GPU pipeline UUID skips immediately.
            st2 = app.prepare_shaders_step(
                root, app_path=app_file, interactive=False,
                bundle_dir=bundle, shader_prep_dir=prep_dir,
            )
            self.assertEqual(st2, 'skipped')

            # Changing AppImage / translator binary triggers re-run cleanly.
            app_file.write_bytes(b'app-v2')
            st3 = app.prepare_shaders_step(
                root, app_path=app_file, interactive=False,
                bundle_dir=bundle, shader_prep_dir=prep_dir,
            )
            self.assertEqual(st3, 'complete')

            # Changing GPU driver UUID in cache/vk_pipelines.bin triggers re-run cleanly.
            (root / 'cache/vk_pipelines.bin').write_bytes(_fake_vk_pcache(uuid_byte=0xBB))
            st4 = app.prepare_shaders_step(
                root, app_path=app_file, interactive=False,
                bundle_dir=bundle, shader_prep_dir=prep_dir,
            )
            self.assertEqual(st4, 'complete')

            # User saves, ini, mods, and DLC remain intact throughout.
            self.assertEqual((root / 'save/slot0').read_bytes(), b'SAVE01')
            self.assertEqual((root / 'larecomp.ini').read_text(), 'fps=60\n')
            self.assertEqual((root / 'extracted/mods/test_mod.txt').read_text(), 'keep-mod')
            self.assertEqual((root / 'extracted/dlc/0100A6301214F001/romfs/d.bin').read_bytes(), b'keep-dlc')

    def test_cancel_preserves_partial_cache_and_cleans_seed(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            root = tmp / 'install_root'
            _setup_fake_extracted(root)
            prep_dir = _write_fake_shader_prep_dir(tmp)
            bundle = tmp / 'bundle/bin'
            _write_fake_engage_bin(bundle, behavior='slow_cancel')

            polls = 0
            def cancel_soon():
                nonlocal polls
                polls += 1
                return polls >= 2 and (root / 'cache/xlat/0000000000000001.bin').is_file()

            st = app.prepare_shaders_step(
                root, interactive=False, bundle_dir=bundle,
                shader_prep_dir=prep_dir, cancel_check=cancel_soon,
            )
            self.assertEqual(st, 'cancelled')
            self.assertTrue((root / 'cache/xlat/0000000000000001.bin').is_file())
            self.assertFalse((root / 'cache/prepare-meta.json').exists())
            self.assertEqual(list((root / '.installer-work').glob('shader-prep-*')), [])

    def test_driver_crash_auto_retry_and_nonfatal_failure(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            root = tmp / 'install_root'
            _setup_fake_extracted(root)
            prep_dir = _write_fake_shader_prep_dir(tmp)
            bundle = tmp / 'bundle/bin'
            _write_fake_engage_bin(bundle, behavior='crash_once')

            st_retry = app.prepare_shaders_step(
                root, interactive=False, bundle_dir=bundle, shader_prep_dir=prep_dir,
            )
            self.assertEqual(st_retry, 'complete')

            # Persistent failure (e.g. headless CI without Vulkan) returns 'failed' without corrupting game files.
            _write_fake_engage_bin(bundle, behavior='fail')
            st_fail = app.prepare_shaders_step(
                root, interactive=False, force=True, bundle_dir=bundle, shader_prep_dir=prep_dir,
            )
            self.assertEqual(st_fail, 'failed')
            self.assertEqual((root / 'save/slot0').read_bytes(), b'SAVE01')
            self.assertEqual((root / 'extracted/engagegame.so').read_bytes(), b'so')

    def test_install_succeeds_even_when_shader_prep_fails_or_cancels(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            root = tmp / 'install_root'
            exefs, romfs = tmp / 'src/exefs', tmp / 'src/romfs'
            exefs.mkdir(parents=True)
            for name in ('main', 'sdk', 'subsdk0'):
                (exefs / name).write_bytes(b'x')
            romfs.mkdir(parents=True)
            (romfs / 'f').write_bytes(b'y')
            app_img = tmp / 'Engage.AppImage'
            app_img.write_bytes(b'fake-appimage')

            class FakeProc:
                returncode = 0
                pid = 999999
                def poll(self): return 0

            def fake_popen(cmd, **kw):
                ws = Path(cmd[cmd.index('--worker') + 1])
                staged = ws / 'data/extracted'
                (staged / 'exefs').mkdir(parents=True, exist_ok=True)
                (staged / 'romfs').mkdir(parents=True, exist_ok=True)
                (staged / 'engagegame.so').write_bytes(b'so')
                return FakeProc()

            args = SimpleNamespace(
                root=root, exefs=exefs, romfs=romfs, dlc=None,
                no_dlc=True, noninteractive=True, appimage=app_img,
                no_shaders=False, force=False,
            )
            with mock.patch.object(app, 'validate'), \
                 mock.patch.object(app, 'launchers'), \
                 mock.patch.object(app.shutil, 'disk_usage', return_value=SimpleNamespace(free=100 * 1024**3)), \
                 mock.patch.object(app.subprocess, 'Popen', side_effect=fake_popen), \
                 mock.patch.object(app, 'prepare_shaders_step', return_value='cancelled') as prep_mock:
                app.install(args)
            prep_mock.assert_called_once()
            self.assertTrue((root / 'extracted/engagegame.so').is_file())
            self.assertTrue((root / 'current.json').is_file())

    def test_mesa_cache_env_overrides_steam_isolation(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'install_root'
            steam_env = {
                'MESA_SHADER_CACHE_DIR': '/home/deck/.local/share/Steam/steamapps/shadercache/123/mesa_shader_cache_sf',
                'MESA_DISK_CACHE_SINGLE_FILE': '1',
            }
            out = app._mesa_cache_env(root, steam_env)
            self.assertEqual(out['MESA_SHADER_CACHE_DIR'], str(root / 'cache/mesa_cache'))
            self.assertEqual(out['MESA_DISK_CACHE_SINGLE_FILE'], '0')
            self.assertEqual(out['MESA_SHADER_CACHE_MAX_SIZE'], '2G')
            self.assertTrue((root / 'cache/mesa_cache').is_dir())

            # Respect explicit opt-out.
            opt_out = app._mesa_cache_env(root, dict(steam_env, ENGAGE_MESA_CACHE_OVERRIDE='0'))
            self.assertEqual(opt_out['MESA_SHADER_CACHE_DIR'], steam_env['MESA_SHADER_CACHE_DIR'])
            self.assertEqual(opt_out['MESA_DISK_CACHE_SINGLE_FILE'], '1')


if __name__ == '__main__':
    unittest.main()
