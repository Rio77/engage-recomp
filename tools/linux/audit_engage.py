#!/usr/bin/env python3
"""Publication boundary audit. Does not open or require a private game dump."""
from pathlib import Path
import argparse
import re
import subprocess

def audit(root, appdir=False):
    forbidden = {'exefs', 'romfs', 'extracted', 'save', 'generated'}
    errors = []
    for file in root.rglob('*'):
        relative = file.relative_to(root)
        if relative.parts[0] in ('build', '.git', 'reports') and not appdir: continue
        if any(p in forbidden for p in relative.parts): errors.append(str(relative))
        if file.is_file() and (file.name in ('hosts.yml', 'id_ed25519', 'id_rsa') or
            re.search(r'(?:game|main|sdk|subsdk0)\.so(?:\.id)?$', file.name)):
            errors.append(str(relative))
        if file.is_file() and file.suffix in ('.py', '.sh', '.md', '.txt', '.c', '.h', '.json') and file.stat().st_size < 4_000_000:
            text = file.read_text(errors='replace')
            if re.search(r'-----BEGIN (?:OPENSSH|RSA|EC) PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{30,}', text):
                errors.append('credential pattern: ' + str(relative))
    if appdir:
        if (root / 'AppRun').read_bytes().split(b'\n', 1)[0] != b'#!/bin/sh':
            errors.append('AppRun must have a Unix shebang without CRLF')
        runtime = root / 'usr/bin/engage-recomp'
        symbols = subprocess.check_output(['nm', '--defined-only', str(runtime)], text=True)
        if re.search(r'\bf_70[0-9a-f]+\b', symbols): errors.append('guest function compiled into runtime')
        for name in ('engagegen/engagegen', 'zig/zig', 'gamelib-src/cpu.h', 'icon.png'):
            if not (root / 'usr/bin' / name).is_file(): errors.append('missing ' + name)
        if not (root / 'usr/share/licenses/engage-recomp/components.json').is_file(): errors.append('missing license inventory')
    if errors: raise SystemExit('\n'.join(errors))
    print('Publication boundary audit passed:', root)
if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--appdir', action='store_true')
    args = parser.parse_args()
    audit(args.root, args.appdir)
