#!/usr/bin/env python3
"""Assemble game-free release assets from an audited AppImage and git export."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/linux/x86_64'
OUT = ROOT / 'build/dist'
VERSION = 'Engage-Recomp-0.3.3'

def sha(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): result.update(block)
    return result.hexdigest()

def checksums(folder):
    files = sorted(p for p in folder.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
    (folder / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.relative_to(folder).as_posix() + '\n' for p in files))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('git_export', type=Path)
    args = parser.parse_args()
    image = OUT / (VERSION + '-linux-x86_64.AppImage')
    with tempfile.TemporaryDirectory(prefix='engage-release-kit-') as temporary:
        temporary = Path(temporary)
        kit = temporary / VERSION
        kit.mkdir()
        shutil.copy2(image, kit / image.name)
        for name in ('Install.sh', 'Install.desktop', 'run_appimage.py'):
            shutil.copy2(ROOT / 'tools/linux' / name, kit / name)
        for name in ('README.md', 'LEEME.md', 'LICENSE', 'THIRD_PARTY.md', 'UPSTREAM.md',
                     'VALIDATION.md', 'PREVIOUS-VALIDATION.md', 'CHANGES.md'):
            shutil.copy2(ROOT / name, kit / name)
        shutil.copy2(ROOT / 'docs/icon/engage.png', kit / 'engage.png')
        (kit / 'docs').mkdir()
        shutil.copy2(ROOT / 'docs/KNOWN-PORTRAIT-ISSUE.md', kit / 'docs/KNOWN-PORTRAIT-ISSUE.md')
        shutil.copytree(BASE / 'licenses', kit / 'licenses')
        for name in ('Install.sh', 'Install.desktop', image.name): (kit / name).chmod(0o755)
        checksums(kit)
        with tarfile.open(OUT / (VERSION + '-installer-linux-x86_64.tar.gz'), 'w:gz') as archive:
            archive.add(kit, arcname=kit.name)
        sources = temporary / (VERSION + '-corresponding-sources')
        project = sources / 'project'
        project.mkdir(parents=True)
        with tarfile.open(args.git_export) as archive:
            for member in archive.getmembers():
                path = Path(member.name)
                if path.is_absolute() or '..' in path.parts or not (member.isfile() or member.isdir()):
                    raise ValueError('Unsafe or unsupported git export member: ' + member.name)
            archive.extractall(project)
        shutil.copytree(BASE / 'corresponding-sources', sources / 'upstream')
        shutil.copytree(BASE / 'licenses', sources / 'licenses')
        for name in ('helper-packages.json', 'helper-packages.txt'):
            shutil.copy2(BASE / name, sources / name)
        checksums(sources)
        with tarfile.open(OUT / (sources.name + '.tar.gz'), 'w:gz') as archive:
            archive.add(sources, arcname=sources.name)
    assets = [image, OUT / (VERSION + '-installer-linux-x86_64.tar.gz'), OUT / (VERSION + '-corresponding-sources.tar.gz')]
    (OUT / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.name + '\n' for p in assets))
    print((OUT / 'SHA256SUMS').read_text())

if __name__ == '__main__': main()
