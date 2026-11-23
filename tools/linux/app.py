#!/usr/bin/env python3
"""Engage Recomp installation and launch frontend. SPDX-License-Identifier: MIT"""
from pathlib import Path
import argparse
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import textwrap
import time
import traceback
import uuid
from engage_build import build
from install_core import Installation, atomic_json, digest
from processes import stop_session

VERSION = '0.2.0'
BUNDLE = Path(__file__).resolve().parents[2] / 'bin'
ES = os.environ.get('LANG', '').startswith('es')
SHADER_CACHE_RESERVE = 512 * 1024**2
MAX_SHADER_CRASH_RETRIES = 3


def tr(es, en): return es if ES else en


def dialog(*args, title='Engage Recomp'):
    environment = dict(os.environ, QT_SCALE_FACTOR='1', QT_AUTO_SCREEN_SCALE_FACTOR='0')
    return subprocess.run(['kdialog', '--title', title, '--icon', str(BUNDLE / 'icon.png'),
        *map(str, args)], text=True, stdout=subprocess.PIPE, env=environment)


def choose(text, default):
    result = dialog('--getexistingdirectory', default, title=text)
    if result.returncode: raise KeyboardInterrupt
    return Path(result.stdout.strip()).resolve()


def choose_dlc(default):
    result = dialog('--yesnocancel', tr('¿Instalar DLC extraídos de Engage? Seleccioná una carpeta con subcarpetas ID/romfs. Los DLC existentes se conservan.',
        'Install extracted Engage DLC? Select a folder containing ID/romfs subfolders. Existing DLC is preserved.'),
        '--yes-label', tr('Instalar DLC', 'Install DLC'), '--no-label', tr('Sin DLC nuevos', 'No new DLC'))
    if result.returncode == 1: return None
    if result.returncode != 0: raise KeyboardInterrupt
    return choose(tr('Carpeta de DLC: ID/romfs', 'DLC folder: ID/romfs'), default)


def validate_dlc(source):
    if source.is_symlink() or not source.is_dir(): raise ValueError('Expected a DLC directory, without symbolic links')
    candidates = [source] if re.fullmatch(r'[0-9a-fA-F]{16}', source.name) else list(source.iterdir())
    imports = []
    seen = set()
    for folder in sorted(candidates):
        if folder.name.startswith('.'): continue
        if not re.fullmatch(r'[0-9a-fA-F]{16}', folder.name): raise ValueError('Expected DLC title ID folder: ' + str(folder))
        ident = int(folder.name, 16)
        if ident & ~0xfff != 0x0100A6301214F000 or not ident & 0xfff:
            raise ValueError('DLC does not belong to Engage: ' + folder.name)
        if ident in seen: raise ValueError('Duplicate DLC title ID: ' + folder.name)
        seen.add(ident)
        romfs = folder / 'romfs'
        # Some extraction tools place the RomFS payload directly under its title ID.
        if not romfs.exists() and not romfs.is_symlink(): romfs = folder
        if folder.is_symlink() or romfs.is_symlink() or not romfs.is_dir(): raise ValueError('Missing DLC romfs: ' + str(folder))
        content = list(romfs.rglob('*'))
        if any(p.is_symlink() or not (p.is_file() or p.is_dir()) for p in content):
            raise ValueError('DLC must contain regular files without symbolic links')
        if any(p.suffix.lower() in ('.nca', '.nsp', '.xci') or p.name == 'exefs' for p in content):
            raise ValueError('Select extracted DLC RomFS, not an encrypted container or ExeFS')
        # Entitlement-only DLC can be a zero-byte e0/h0 marker. A directory with
        # regular files is valid even when the total payload size is zero.
        if not any(p.is_file() for p in content): raise ValueError('Empty DLC romfs: ' + str(folder))
        imports.append((f'{ident:016X}', romfs))
    if not imports or len(imports) > 64: raise ValueError('Expected 1 to 64 extracted Engage DLC folders')
    return imports


def copy_dlc(imports, target):
    for ident, romfs in imports:
        shutil.copytree(romfs, target / ident / 'romfs')


def validate(exefs, romfs):
    for name, ident in [('main', '8c08b9719e085f91847b5e0f935d9488'),
                        ('sdk', '56ec24747ec9e7f19b3efb062483a19d1da091a8'),
                        ('subsdk0', 'bffed55ea46c1b48ddeabcacdfbf81bf418125ac')]:
        with (exefs / name).open('rb') as file: header = file.read(256)
        if len(header) < 256 or header[:4] != b'NSO0' or not header[64:96].hex().startswith(ident):
            raise ValueError('Engage 2.0.0 ExeFS required: ' + name)
    if not romfs.is_dir() or not any(romfs.iterdir()): raise ValueError('Empty RomFS')
    for name in ('Data/Managed', 'Data/StreamingAssets'):
        if not (romfs / name).is_dir(): raise ValueError('Expected complete Engage RomFS: ' + name)
    for name in ('Data/data.unity3d', 'Data/Managed/Metadata/global-metadata.dat'):
        if not (romfs / name).is_file(): raise ValueError('Missing base game data: ' + name)
    for root in (exefs, romfs):
        if any(p.is_symlink() for p in root.rglob('*')): raise ValueError('Game dump must not contain symbolic links')


def launchers(root, app):
    icon = root / 'engage.png'
    shutil.copy2(BUNDLE / 'icon.png', icon)
    wrapper = root / 'run_appimage.py'
    shutil.copy2(Path(__file__).with_name('run_appimage.py'), wrapper)
    for name, mode in [('Engage Recomp', '--play')]:
        # Shell arguments are quoted independently; paths with spaces remain intact.
        script = root / (name.replace(' ', '-') + '.sh')
        script.write_text('#!/bin/sh\nexport APPIMAGE_EXTRACT_AND_RUN=1\nexec ' +
            shlex.join(['python3', str(wrapper), str(app), '--root', str(root), mode]) + '\n')
        script.chmod(0o755)
        desktop = root / (name.replace(' ', '-') + '.desktop')
        def esc(value): return str(value).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
        desktop.write_text('[Desktop Entry]\nType=Application\nName=' + name + '\nExec="' + esc(script) +
            '"\nIcon=' + str(icon) + '\nTerminal=false\nCategories=Game;\n')
    # Wayland compositors resolve a window icon through its application ID;
    # SDL_SetWindowIcon alone is insufficient there. Match SDL_APP_ID below.
    applications = Path.home() / '.local/share/applications'
    applications.mkdir(parents=True, exist_ok=True)
    registered = applications / 'engage-recomp.desktop'
    registered.write_text((root / 'Engage-Recomp.desktop').read_text() + 'StartupWMClass=engage-recomp\n')


class _KDialogProgress:
    """Thin wrapper over KDialog --progressbar + qdbus for cancellable steps."""

    def __init__(self, initial_text, log_path=None):
        self.qdbus = shutil.which('qdbus6') or shutil.which('qdbus')
        self.log_path = log_path
        self.handle = dialog('--progressbar', textwrap.fill(initial_text, 72), '100').stdout.strip()
        if self.handle and self.qdbus:
            subprocess.run([self.qdbus, *shlex.split(self.handle), 'showCancelButton', 'true'], capture_output=True)

    def was_cancelled(self):
        if not (self.handle and self.qdbus):
            return False
        res = subprocess.run([self.qdbus, *shlex.split(self.handle), 'wasCancelled'], capture_output=True, text=True)
        return res.returncode != 0 or res.stdout.strip() == 'true'

    def update(self, label, percent=None):
        if not (self.handle and self.qdbus):
            return
        text = textwrap.fill(label[:240], 72)
        if self.log_path:
            text += '\n' + textwrap.fill(str(self.log_path), 72)
        subprocess.run([self.qdbus, *shlex.split(self.handle), 'setLabelText', text], capture_output=True)
        if percent is not None:
            val = max(0, min(100, int(percent)))
            subprocess.run([self.qdbus, *shlex.split(self.handle), 'Set', '', 'value', str(val)], capture_output=True)

    def close(self):
        if self.handle and self.qdbus:
            subprocess.run([self.qdbus, *shlex.split(self.handle), 'close'], capture_output=True)
        self.handle = ''


def find_shader_prep_dir():
    """Locate offline shader extraction scripts and NX Native metadata TSVs."""
    candidates = [
        Path(__file__).with_name('shader_prep'),
        Path(__file__).resolve().parents[1] / 'shader_prep',
    ]
    for cand in candidates:
        if (cand / 'engage_shader_extract.py').is_file() and (cand / 'data/0100A6301214E000/shader_map.tsv').is_file():
            return cand
    return None


def read_vk_pipeline_uuid(cache_dir: Path) -> str:
    """Read vendor:device:pipelineCacheUUID from cache/vk_pipelines.bin header."""
    p = cache_dir / 'vk_pipelines.bin'
    if not p.is_file():
        return ''
    try:
        hdr = p.read_bytes()[:32]
        if len(hdr) < 32:
            return ''
        hsize, hver, vendor, device = struct.unpack_from('<4I', hdr, 0)
        if hsize < 32 or hver != 1:
            return ''
        return f'{vendor:08x}:{device:08x}:{hdr[16:32].hex()}'
    except OSError:
        return ''


def parse_progress_line(text: str) -> dict[str, str]:
    out = {}
    for tok in text.strip().split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = v
    return out


def is_shader_prep_current(root: Path, app_sha256: str, bin_sha256: str) -> bool:
    """Return True only if prepared caches & driver pipeline UUID match current binaries."""
    cache = root / 'cache'
    meta_path = cache / 'prepare-meta.json'
    if not meta_path.is_file():
        return False
    if not ((cache / 'warm.bin').is_file() and (cache / 'vk_pipelines.bin').is_file()
            and (cache / 'xlat').is_dir() and (cache / 'spirv').is_dir()):
        return False
    try:
        meta = json.loads(meta_path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError):
        return False
    if meta.get('format') != 1 or meta.get('status') != 'complete':
        return False
    if meta.get('bin_sha256') != bin_sha256:
        return False
    if app_sha256 and meta.get('app_sha256') != app_sha256:
        return False
    vk_uuid = read_vk_pipeline_uuid(cache)
    return bool(vk_uuid) and meta.get('vk_pipeline_uuid') == vk_uuid


def _mesa_cache_env(root: Path, base_env: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ if base_env is None else base_env)
    if env.get('ENGAGE_MESA_CACHE_OVERRIDE') == '0':
        return env
    if env.get('MESA_SHADER_CACHE_DISABLE') in ('1', 'true'):
        return env
    mesa_dir = root / 'cache/mesa_cache'
    mesa_dir.mkdir(parents=True, exist_ok=True)
    env['MESA_SHADER_CACHE_DIR'] = str(mesa_dir)
    env['MESA_DISK_CACHE_SINGLE_FILE'] = '0'
    env.setdefault('MESA_SHADER_CACHE_MAX_SIZE', '2G')
    return env


def _append_log(log_path: Path | None, line: str):
    if not log_path:
        return
    with log_path.open('a', encoding='utf-8') as f:
        f.write(line.rstrip('\n') + '\n')


def _format_shader_progress(prog: dict[str, str]):
    phase = prog.get('phase', 'shaders')
    if phase == 'pipelines':
        done = int(prog.get('pso_done', prog.get('done', '0')))
        total = max(1, int(prog.get('pso_total', prog.get('total', '1'))))
        pct = 75 + (25 * done) // total
        return tr(f'Precalentando pipelines ({done}/{total})…', f'Prewarming pipelines ({done}/{total})…'), pct
    done = int(prog.get('done', '0'))
    total = max(1, int(prog.get('total', '1')))
    pct = (75 * done) // total
    return tr(f'Preparando shaders ({done}/{total})…', f'Preparing shaders ({done}/{total})…'), pct


def prepare_shaders_step(root: Path, app_path: Path | None = None, interactive: bool = True,
                         force: bool = False, log_path: Path | None = None, jobs: int | None = None,
                         bundle_dir: Path | None = None, shader_prep_dir: Path | None = None,
                         on_progress=None, cancel_check=None) -> str:
    """Extract local game shaders and run headless Vulkan pre-compilation.

    Returns 'skipped', 'complete', 'cancelled', or 'failed'.
    Never deletes already-generated cache/ artifacts on cancel or failure.
    """
    bundle = bundle_dir or BUNDLE
    engage_bin = bundle / 'engage-recomp'
    prep_dir = shader_prep_dir or find_shader_prep_dir()
    extracted = root / 'extracted'
    cache_dir = root / 'cache'
    cache_dir.mkdir(parents=True, exist_ok=True)
    log_target = log_path or (cache_dir / 'shader-prepare.log')

    if not engage_bin.is_file() or prep_dir is None or not (extracted / 'exefs').is_dir() or not (extracted / 'romfs').is_dir():
        _append_log(log_target, '[prepare] missing binary, shader_prep data, or extracted game files')
        return 'failed'

    app_sha256 = digest(app_path) if (app_path and app_path.is_file()) else ''
    bin_sha256 = digest(engage_bin)
    if not force and is_shader_prep_current(root, app_sha256, bin_sha256):
        _append_log(log_target, '[prepare] existing shader cache is up to date; skipping')
        return 'skipped'

    work_parent = root / '.installer-work'
    work_parent.mkdir(parents=True, exist_ok=True)
    prep_work = work_parent / ('shader-prep-' + uuid.uuid4().hex[:12])
    prep_work.mkdir()
    seed_path = prep_work / 'shader-seed.bin'
    prog_path = prep_work / 'shader-progress.txt'

    ui = _KDialogProgress(tr('Extrayendo shaders del juego…', 'Extracting game shaders…'), log_target) if interactive else None

    def cancelled():
        return (cancel_check() if cancel_check else False) or (ui.was_cancelled() if ui else False)

    def notify(msg, pct):
        if ui:
            ui.update(msg, pct)
        if on_progress:
            on_progress(msg, pct)

    prep_env = _mesa_cache_env(root)

    def run_monitored(cmd, poll_cb):
        with log_target.open('a', encoding='utf-8') as stream:
            proc = subprocess.Popen(cmd, cwd=root, env=prep_env, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                while proc.poll() is None:
                    if cancelled():
                        stop_session(proc)
                        return None
                    poll_cb()
                    time.sleep(0.25)
                poll_cb()
                return proc.returncode
            finally:
                if proc.poll() is None:
                    stop_session(proc)

    try:
        notify(tr('Extrayendo shaders del juego…', 'Extracting game shaders…'), 0)
        extract_cmd = [
            sys.executable,
            str(prep_dir / 'engage_shader_extract.py'),
            '--exefs', str(extracted / 'exefs'),
            '--romfs', str(extracted / 'romfs'),
            '--data', str(prep_dir / 'data/0100A6301214E000'),
            '--out', str(seed_path),
        ]
        rc = run_monitored(extract_cmd, lambda: None)
        if rc is None:
            return 'cancelled'
        if rc != 0 or not seed_path.is_file():
            _append_log(log_target, f'[prepare] shader extraction exited with {rc}')
            return 'failed'

        prep_cmd = [
            str(engage_bin),
            '--data-root', str(root),
            '--prepare-shaders', str(seed_path),
            '--progress-file', str(prog_path),
        ]
        if jobs:
            prep_cmd += ['--jobs', str(jobs)]

        final_prog = {}

        def poll_prepare():
            nonlocal final_prog
            if not prog_path.is_file():
                return
            try:
                parsed = parse_progress_line(prog_path.read_text(encoding='utf-8'))
            except OSError:
                return
            if parsed:
                final_prog = parsed
                msg, pct = _format_shader_progress(parsed)
                notify(msg, pct)

        for attempt in range(MAX_SHADER_CRASH_RETRIES + 1):
            rc = run_monitored(prep_cmd, poll_prepare)
            if rc is None:
                return 'cancelled'
            if rc == 0:
                break
            journal_left = (cache_dir / 'prepare-journal.bin').is_file()
            _append_log(log_target, f'[prepare] attempt {attempt + 1} failed (rc={rc}, journal={journal_left})')
            if not journal_left or attempt >= MAX_SHADER_CRASH_RETRIES:
                return 'failed'

        vk_uuid = read_vk_pipeline_uuid(cache_dir)
        meta = {
            'format': 1,
            'status': 'complete',
            'app_sha256': app_sha256,
            'bin_sha256': bin_sha256,
            'vk_pipeline_uuid': vk_uuid,
            'summary': final_prog,
            'completed_at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        }
        atomic_json(cache_dir / 'prepare-meta.json', meta)
        notify(tr('Preparación de shaders completada.', 'Shader preparation complete.'), 100)
        return 'complete'
    finally:
        if ui:
            ui.close()
        shutil.rmtree(prep_work, ignore_errors=True)


def prepare_shaders_cli(args):
    installation = Installation(args.root or Path.home() / 'Games/Engage-Recomp')
    app_arg = os.environ.get('APPIMAGE') or args.appimage
    app_path = Path(app_arg).resolve() if app_arg else None
    with installation.lock():
        installation.recover()
        extracted = installation.root / 'extracted'
        if not (extracted / 'exefs').is_dir() or not (extracted / 'romfs').is_dir():
            raise ValueError(tr('Primero ejecutá el instalador para esta carpeta.', 'Run the installer for this folder first.'))
        current = installation.root / 'current.json'
        if app_path is None and current.is_file():
            info = json.loads(current.read_text(encoding='utf-8'))
            cand = installation.root / 'apps' / (info.get('app_sha256', '') + '.AppImage')
            if cand.is_file():
                app_path = cand
        status = prepare_shaders_step(
            installation.root,
            app_path=app_path,
            interactive=not args.noninteractive,
            force=args.force,
            log_path=installation.root / 'installation.log',
        )
        if status == 'cancelled':
            raise KeyboardInterrupt
        if status == 'failed':
            raise RuntimeError('Shader preparation failed. See ' + str(installation.root / 'installation.log'))


def install(args):
    root = args.root or Path.home() / 'Games/Engage-Recomp'
    if not args.exefs:
        result = dialog('--inputbox', tr('Carpeta de instalación', 'Installation folder'), root)
        if result.returncode: return
        root = Path(result.stdout.strip()).expanduser()
    if root.exists() and any(root.iterdir()) and not (root / 'current.json').is_file() and not (root / '.installer-work').is_dir():
        raise ValueError('Choose an empty dedicated installation folder')
    exefs = args.exefs or choose('ExeFS — Engage 2.0.0', Path.home())
    romfs = args.romfs or choose('RomFS — Engage 2.0.0', exefs.parent)
    validate(exefs, romfs)
    dlc = args.dlc
    if not dlc and not args.no_dlc and not args.noninteractive: dlc = choose_dlc(exefs.parent)
    imports = validate_dlc(dlc) if dlc else []
    sources = [exefs, romfs] + [folder for _, folder in imports]
    if any(root.resolve().is_relative_to(folder.resolve()) or folder.resolve().is_relative_to(root.resolve()) for folder in sources):
        raise ValueError('The installation folder must be outside your source dump')
    parent = next(path for path in (root, *root.parents) if path.exists())
    required = sum(p.stat().st_size for folder in sources for p in folder.rglob('*') if p.is_file()) + 8 * 1024**3 + SHADER_CACHE_RESERVE
    if shutil.disk_usage(parent).free < required:
        raise ValueError(tr('Espacio libre insuficiente para copiar y compilar: ',
            'Insufficient free space to copy and compile: ') + str(round(required / 1024**3, 1)) + ' GiB')
    app = Path(os.environ.get('APPIMAGE') or args.appimage or '').resolve()
    if not app.is_file(): raise ValueError('Run the packaged AppImage to install')
    installation = Installation(root)
    with installation.lock():
        installation.recover()
        token, workspace = installation.workspace()
        staged = workspace / 'data/extracted'
        staged.mkdir()
        log = installation.root / 'installation.log'
        # Builder runs in an independent session, so cancellation can reap its compiler children.
        command = [sys.executable, str(Path(__file__).resolve()), '--worker', str(workspace),
                   '--exefs', str(exefs), '--romfs', str(romfs)]
        if dlc: command += ['--dlc', str(dlc)]
        existing_lib = installation.root / 'extracted/engagegame.so'
        existing_id = installation.root / 'extracted/engagegame.so.id'
        if (not args.force and existing_lib.is_file() and not existing_lib.is_symlink()
                and existing_id.is_file() and existing_id.read_text(encoding='ascii', errors='ignore').strip() == 'engage-200-bringup'):
            command += ['--reuse-gamelib', str(existing_lib)]
        with log.open('w') as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            progress = None
            qdbus = shutil.which('qdbus6') or shutil.which('qdbus')
            if not args.noninteractive:
                message = tr('Preparando y compilando. Puede tardar bastante. Registro: ',
                    'Preparing and compiling. This can take a long time. Log: ') + str(log)
                progress = dialog('--progressbar', textwrap.fill(message, 72), '0').stdout.strip()
                if progress and qdbus:
                    subprocess.run([qdbus, *shlex.split(progress), 'showCancelButton', 'true'], capture_output=True)
            try:
                while process.poll() is None:
                    if progress and qdbus:
                        cancelled = subprocess.run([qdbus, *shlex.split(progress), 'wasCancelled'], capture_output=True, text=True)
                        if cancelled.returncode or cancelled.stdout.strip() == 'true': raise KeyboardInterrupt
                        with log.open('rb') as tail:
                            tail.seek(max(0, log.stat().st_size - 1024))
                            lines = tail.read().decode(errors='replace').splitlines()
                        label = (lines[-1][:240] if lines else tr('Copiando archivos…', 'Copying files…'))
                        subprocess.run([qdbus, *shlex.split(progress), 'setLabelText',
                            textwrap.fill(label, 72) + '\n' + textwrap.fill(str(log), 72)], capture_output=True)
                    time.sleep(1)
                if process.returncode: raise RuntimeError('Compilation failed. See ' + str(log))
            finally:
                if process.poll() is None:
                    stop_session(process)
                if progress and qdbus: subprocess.run([qdbus, *shlex.split(progress), 'close'], capture_output=True)
        stored = installation.add_app(app, digest(app))
        installation.commit(token, digest(stored), VERSION)
        launchers(installation.root, stored)
        installation.cleanup_workspace(token)
        shader_status = 'skipped'
        if not args.no_shaders:
            shader_status = prepare_shaders_step(
                installation.root,
                app_path=stored,
                interactive=not args.noninteractive,
                log_path=log,
            )
    if not args.noninteractive:
        if shader_status in ('cancelled', 'failed'):
            dialog('--msgbox', tr(
                'Instalación completa. La preparación anticipada de shaders se omitió o canceló (se compilarán al jugar). Añadí Engage-Recomp.sh a Steam como juego externo. ',
                'Installation complete. Offline shader preparation was skipped or cancelled (shaders will compile on demand). Add Engage-Recomp.sh to Steam as a non-Steam game. ') + str(installation.root))
        else:
            dialog('--msgbox', tr('Instalación completa. Añadí Engage-Recomp.sh a Steam como juego externo. ',
                'Installation complete. Add Engage-Recomp.sh to Steam as a non-Steam game. ') + str(installation.root))


def main():
    parser = argparse.ArgumentParser()
    for flag in ('install', 'play', 'settings', 'prepare-shaders', 'no-shaders', 'force', 'noninteractive', 'no-dlc'):
        parser.add_argument('--' + flag, action='store_true')
    for flag in ('root', 'exefs', 'romfs', 'dlc', 'worker', 'appimage', 'reuse-gamelib'):
        parser.add_argument('--' + flag, type=Path)
    args = parser.parse_args()
    if args.dlc and args.no_dlc: parser.error('--dlc and --no-dlc are mutually exclusive')
    if args.worker:
        staged = args.worker / 'data/extracted'
        print('Copying ExeFS', flush=True)
        shutil.copytree(args.exefs, staged / 'exefs')
        print('Copying RomFS', flush=True)
        shutil.copytree(args.romfs, staged / 'romfs')
        if args.dlc:
            print('Copying DLC', flush=True)
            copy_dlc(validate_dlc(args.dlc), staged / 'dlc')
        reuse = getattr(args, 'reuse_gamelib', None)
        if reuse and reuse.is_file() and not reuse.is_symlink():
            print('Reusing verified game library', flush=True)
            shutil.copy2(reuse, staged / 'engagegame.so')
            (staged / 'engagegame.so.id').write_text('engage-200-bringup\n')
        else:
            print('Generating and compiling game library', flush=True)
            output = build(BUNDLE, args.worker / 'compile', staged / 'exefs')
            shutil.copy2(output, staged / output.name)
            shutil.copy2(output.with_suffix('.so.id'), staged / 'engagegame.so.id')
        return
    if args.prepare_shaders: return prepare_shaders_cli(args)
    if args.install or not (args.play or args.settings): return install(args)
    installation = Installation(args.root or Path.home() / 'Games/Engage-Recomp')
    with installation.lock() as lock_fd:
        installation.recover()
        if args.settings:
            dialog('--msgbox', tr('Los FPS se eligen dentro del juego: F1 → Video → Frame rate.',
                'Choose FPS in game: F1 → Video → Frame rate.'))
            return
        extracted = installation.root / 'extracted'
        if not (extracted / 'engagegame.so').is_file():
            raise ValueError(tr('Primero ejecutá el instalador para esta carpeta.', 'Run the installer for this folder first.'))
        env = _mesa_cache_env(installation.root)
        env.update(LARECOMP_DATA_ROOT=str(installation.root),
            LARECOMP_GAME_LIB=str(extracted / 'engagegame.so'),
            LARECOMP_PACE='0', SDL_APP_ID='engage-recomp')
        env['ENGAGE_COMPLETE_FRAMES'] = env.get('ENGAGE_COMPLETE_FRAMES') or '1'
        (installation.root / 'cache/preparation.txt').unlink(missing_ok=True)
        process = subprocess.Popen([str(BUNDLE / 'engage-recomp'), str(extracted), '--play'],
            cwd=installation.root, env=env, pass_fds=(lock_fd,))
        progress = subprocess.Popen([sys.executable, str(Path(__file__).with_name('preparation_window.py')),
            str(installation.root), str(process.pid), str(BUNDLE / 'icon.png')],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            status = process.wait()
            if status: raise subprocess.CalledProcessError(status, 'engage-recomp')
        finally:
            if progress.poll() is None: progress.terminate()
            progress.wait()


if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: sys.exit(130)
    except Exception as error:
        traceback.print_exc()
        if '--noninteractive' not in sys.argv and '--worker' not in sys.argv: dialog('--error', str(error))
        sys.exit(1)
