#!/usr/bin/env python3
"""Engage Recomp installation and launch frontend. SPDX-License-Identifier: MIT"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import sys
import traceback
import shlex
import textwrap
from install_core import Installation, digest
from engage_build import build

VERSION = '0.1.0'
BUNDLE = Path(__file__).resolve().parents[2] / 'bin'
ES = os.environ.get('LANG', '').startswith('es')
def tr(es, en): return es if ES else en
def dialog(*args, title='Engage Recomp'):
    environment = dict(os.environ, QT_SCALE_FACTOR='1', QT_AUTO_SCREEN_SCALE_FACTOR='0')
    return subprocess.run(['kdialog', '--title', title, '--icon', str(BUNDLE / 'icon.png'),
        *map(str, args)], text=True, stdout=subprocess.PIPE, env=environment)
def choose(text, default):
    result = dialog('--getexistingdirectory', default, title=text)
    if result.returncode: raise KeyboardInterrupt
    return Path(result.stdout.strip()).resolve()
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
    for name, mode in [('Engage Recomp', '--play'), ('Engage Recomp Settings', '--settings')]:
        # Shell arguments are quoted independently; paths with spaces remain intact.
        import shlex
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
    if any(root.resolve().is_relative_to(folder.resolve()) for folder in (exefs, romfs)):
        raise ValueError('The installation folder must be outside your source dump')
    parent = next(path for path in (root, *root.parents) if path.exists())
    required = sum(p.stat().st_size for folder in (exefs, romfs) for p in folder.rglob('*') if p.is_file()) + 8 * 1024**3
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
                    import time
                    time.sleep(1)
                if process.returncode: raise RuntimeError('Compilation failed. See ' + str(log))
            finally:
                if process.poll() is None:
                    from processes import stop_session
                    stop_session(process)
                if progress and qdbus: subprocess.run([qdbus, *shlex.split(progress), 'close'], capture_output=True)
        stored = installation.add_app(app, digest(app))
        installation.commit(token, digest(stored), VERSION)
        launchers(installation.root, stored)
        installation.cleanup_workspace(token)
    if not args.noninteractive:
        dialog('--msgbox', tr('Instalación completa. Añadí Engage-Recomp.sh a Steam como juego externo. ',
            'Installation complete. Add Engage-Recomp.sh to Steam as a non-Steam game. ') + str(installation.root))
def main():
    parser = argparse.ArgumentParser()
    for flag in ('install', 'play', 'settings', 'noninteractive'): parser.add_argument('--' + flag, action='store_true')
    for flag in ('root', 'exefs', 'romfs', 'worker', 'appimage'): parser.add_argument('--' + flag, type=Path)
    args = parser.parse_args()
    if args.worker:
        staged = args.worker / 'data/extracted'
        print('Copying ExeFS', flush=True)
        shutil.copytree(args.exefs, staged / 'exefs')
        print('Copying RomFS', flush=True)
        shutil.copytree(args.romfs, staged / 'romfs')
        print('Generating and compiling game library', flush=True)
        output = build(BUNDLE, args.worker / 'compile', staged / 'exefs')
        shutil.copy2(output, staged / output.name)
        shutil.copy2(output.with_suffix('.so.id'), staged / 'engagegame.so.id')
        return
    if args.install or not (args.play or args.settings): return install(args)
    installation = Installation(args.root or Path.home() / 'Games/Engage-Recomp')
    with installation.lock() as lock_fd:
        installation.recover()
        fps_file = installation.root / 'engage-fps'
        if args.settings:
            choices = ['60', '60 FPS', '30', '30 FPS']
            if fps_file.exists() and fps_file.read_text().strip() == '30': choices = choices[2:] + choices[:2]
            result = dialog('--menu', tr('Límite de FPS', 'FPS limit'), *choices)
            if result.returncode == 0 and result.stdout.strip() in ('30', '60'):
                temporary = fps_file.with_suffix('.tmp')
                temporary.write_text(result.stdout.strip() + '\n')
                temporary.replace(fps_file)
            return
        extracted = installation.root / 'extracted'
        if not (extracted / 'engagegame.so').is_file():
            raise ValueError(tr('Primero ejecutá el instalador para esta carpeta.', 'Run the installer for this folder first.'))
        fps = fps_file.read_text().strip() if fps_file.exists() else '60'
        if fps not in ('30', '60'): fps = '60'
        env = dict(os.environ, LARECOMP_DATA_ROOT=str(installation.root),
            LARECOMP_GAME_LIB=str(extracted / 'engagegame.so'), ENGAGE_FPS=fps,
            ENGAGE_COMPLETE_FRAMES='1', LARECOMP_PACE='0', SDL_APP_ID='engage-recomp')
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
