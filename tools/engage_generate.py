#!/usr/bin/env python3
"""Frozen generator entry point: user-owned Engage modules and guarded patches."""
import argparse
import os
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--hle', type=Path, required=True)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    exefs = args.exefs.resolve(strict=True)
    hle = args.hle.resolve(strict=True)
    os.environ['LARECOMP_CHEATS'] = os.devnull
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    # Preflight every module before generating any output.
    from nso import Nso
    for name in ('main', 'sdk', 'subsdk0'):
        Nso(str(exefs / name))
    if Nso(str(exefs / 'main')).build_id != '8c08b9719e085f91847b5e0f935d9488':
        raise ValueError('Expected Fire Emblem Engage 2.0.0')
    import gen
    original = sys.argv
    try:
        for name, base in (('main','0x7000000000'),('sdk','0x7008000000'),('subsdk0','0x7010000000')):
            output = workspace / 'generated' / name
            output.mkdir(parents=True, exist_ok=True)
            sys.argv = ['gen', name, str(exefs / name), base, str(output), '--simd', 'c', '--hle', str(hle)]
            gen.main()
        import patch_sdk_unwind
        sys.argv = ['patch-sdk', str(workspace / 'generated/sdk')]
        patch_sdk_unwind.main()
        import reemit_main_functions as reemit
        reemit.ROOT = workspace
        sys.argv = ['reemit', str(exefs / 'main')]
        reemit.main()
        import enable_exception_landings as exceptions
        exceptions.ROOT = workspace
        sys.argv = ['exceptions', str(exefs / 'main'), '--all']
        exceptions.main()
        import enable_engage_timing as timing
        timing.ROOT = workspace
        timing.INSTALL_RUNTIME_HOOK = False  # bundled runtime already contains this hook
        sys.argv = ['timing', str(exefs / 'main')]
        timing.main()
    finally:
        sys.argv = original


if __name__ == '__main__':
    main()
