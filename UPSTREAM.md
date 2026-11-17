# Source provenance

Engage Recomp is an independent, experimental Linux adaptation of the
FE3H Recomp / LASRecomp runtime and static ARM64-to-C translator. Upstream
copyright notices are retained in LICENSE. This is a selective source export,
not an unmodified checkout of any reference project.

- Runtime/translator foundation: https://gitlab.com/onerag90/fe3h-recomp and
  https://github.com/peiikai/LASRecomp, via the native FE3H opt4 branch.
- Linux distribution and recoverable installer adapted from
  https://github.com/Rio77/fe3h-deck-recomp (0.2.4 distribution).
- Engage ABI, service behavior and version metadata reference:
  https://github.com/seedds/nxnative/tree/main/dm/games/0100A6301214E000 .
- Selected timing sites adapted from engage-pc-recomp, MIT, commit
  `5e0ce98f120bd669cb79d3b365d0da1d6d54145b`,
  `recomp/patches-engage-v200.txt`:
  https://gitlab.com/engage-pc-recomp/engage-pc-recomp .
- uDPRE (https://github.com/u2-bot/uDPRE) and totk-pc-recomp
  (https://github.com/valentderah/totk-pc-recomp) were architectural references;
  they do not supply Engage game code to this distribution.

Local work covers Engage module layout and linking, SDK exception resumption,
large-function emission, exception landing pads, host services, GPU descriptor
analysis, texture identity/lifetime, required-pipeline waits, compatible cache
reuse, 30/60 timing and Linux packaging. See CHANGES.md and VALIDATION.md.

Development and documentation were assisted by AI tools. This does not replace
the upstream authors' credits. No Nintendo game code, assets, extracted shaders,
saves or private dump is part of the publication. Recompiled game functions are
generated only from the user's own files during installation.

The maintainer supplied the emblem used as the application icon. Its use here
is separate from the MIT code license; no rights to Nintendo marks or game
content are granted. This project is not affiliated with Nintendo.
