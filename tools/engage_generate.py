#!/usr/bin/env python3
"""Frozen generator entry point: user-owned Engage modules and guarded patches."""
import argparse
import multiprocessing
import os
from pathlib import Path
import sys


def main():
    multiprocessing.freeze_support()
    parser = argparse.ArgumentParser()
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--hle', type=Path, required=True)
    parser.add_argument('--cheats', type=Path, default=None)
    parser.add_argument('--update-cheats', action='store_true')
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    exefs = args.exefs.resolve(strict=True)
    hle = args.hle.resolve(strict=True)
    cheats_path = str(args.cheats.resolve()) if args.cheats else os.devnull
    os.environ['LARECOMP_CHEATS'] = cheats_path
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    # Preflight every module before generating any output.
    from nso import Nso
    if not args.update_cheats:
        for name in ('main', 'sdk', 'subsdk0'):
            Nso(str(exefs / name))
    if Nso(str(exefs / 'main')).build_id != '8c08b9719e085f91847b5e0f935d9488':
        raise ValueError('Expected Fire Emblem Engage 2.0.0')
    import gen
    original = sys.argv
    try:
        if args.update_cheats:
            dirty = gen.update_main_chunks(str(exefs / 'main'), str(workspace / 'generated/main'), cheats_path, simd='c')
            if dirty is not None:
                (workspace / 'dirty_chunks.txt').write_text('\n'.join(dirty) + ('\n' if dirty else ''))
                if dirty:
                    dirty_set = set(dirty)
                    import reemit_main_functions as reemit
                    reemit.ROOT = workspace
                    reemit.CHUNKS_FILTER = dirty_set
                    sys.argv = ['reemit', str(exefs / 'main')]
                    reemit.main()
                    import enable_exception_landings as exceptions
                    exceptions.ROOT = workspace
                    exceptions.CHUNKS_FILTER = dirty_set
                    sys.argv = ['exceptions', str(exefs / 'main'), '--all']
                    exceptions.main()
                    import enable_engage_timing as timing
                    timing.ROOT = workspace
                    timing.INSTALL_RUNTIME_HOOK = False
                    timing.CHUNKS_FILTER = dirty_set
                    sys.argv = ['timing', str(exefs / 'main')]
                    timing.main()
                return
            for name in ('sdk', 'subsdk0'):
                Nso(str(exefs / name))

        for name, base in (('main','0x7000000000'),('sdk','0x7008000000'),('subsdk0','0x7010000000')):
            output = workspace / 'generated' / name
            output.mkdir(parents=True, exist_ok=True)
            sys.argv = ['gen', name, str(exefs / name), base, str(output), '--simd', 'c', '--hle', str(hle)]
            gen.main()
        # Re-populate gen.VARIANTS with main's cheats before running main post-passes
        # (sdk/subsdk0 gen.main() cleared gen.VARIANTS above).
        import cheats
        from gen import Module as _Mod
        _m = _Mod('main', str(exefs / 'main'), 0x7000000000, 'c')
        gen.VARIANTS.clear()
        gen.VARIANTS.update((a, ('rando_word', ws)) for a, ws in gen.RANDO_CODE.items())
        for off, ws in cheats.code_variants(cheats_path, lambda o: _m.in_text(0x7000000000 + o)).items():
            gen.VARIANTS[off] = ('cheat_word', ws)
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
