# Engage Recomp

Experimental native Linux recompilation of **Fire Emblem Engage 2.0.0**, aimed
at Steam Deck. Version **0.1.0** is a release of an experimental port. It uses static
ARM64-to-C translation and a Vulkan runtime derived from
[FE3H Recomp](https://gitlab.com/onerag90/fe3h-recomp) and
[LASRecomp](https://github.com/peiikai/LASRecomp).

**You must supply your own extracted ExeFS and RomFS, with the 2.0.0 update
applied to the base game.** Update-only files are insufficient. No game files,
recompiled game library, saves or shader cache are distributed.

## Install on Steam Deck

1. Download and extract the installer kit from Releases in Desktop mode.
2. Run `Install.desktop` (or `Install.sh`). Select the installation folder and
   your ExeFS/RomFS folders, then optionally an extracted DLC folder, when asked. Spanish and English follow the desktop
   language. Default destination: `~/Games/Engage-Recomp`.
3. Wait for local generation and compilation. This first optimized build can
   take hours on the Deck. Progress details go to `installation.log`; cancellation
   leaves the current installation usable. Keep ample free space for your dump,
   generated sources, compiler objects and the recovery copy on upgrades.
4. Add `Engage-Recomp.sh` in the installation folder to Steam as a non-Steam
   game. It runs natively: **do not enable Proton**. Use the supplied PNG for
   Steam artwork if desired. Choose 30 or 60 FPS while playing in **F1 → Video → Frame rate**.

DLC imports accept `<title-id>/romfs/...` or `<title-id>/...` (direct extracted
RomFS payload). Select the parent folder containing the Engage DLC title IDs.
Encrypted NSP/XCI/NCA files are not accepted. Declining new DLC preserves any
already installed add-ons on reinstall. DLC gameplay remains experimental.

The AppImage contains the compiler, frozen generator and runtime dependencies.
No Podman, pip installation or SteamOS system modification is required to
install a game. Python 3.10+, KDialog and a Vulkan-capable x86-64-v3 Linux system
are required; SteamOS provides them. Launch scripts use AppImage extraction
mode, so FUSE is not required. Other Linux distributions are not yet validated.

## State and limitations

Battle-end progression and new saves have been tested using the packaged
runtime. Required shaders/pipelines finish before drawing, and known pipelines
are prepared on later launches. Unknown resources can still pause on first use.
60 FPS is a selectable target, **not a promise of stable 60 FPS**. See the
validation report for the measured scene and its limitations.

Known issues: intermittent monochrome opening movie; Additional Content can
freeze while music continues; DLC is not comprehensively validated. Initial
loads can be slow. See [VALIDATION.md](VALIDATION.md) for actual release checks.

## Data, updates and recovery

Saves and configuration live in the chosen installation root, separately from
`extracted`. The installer stages a replacement before switching it into place.
Previous extracted data remains in `.recovery`; failed builds remain under
`.installer-work` for diagnosis. Do not delete these while installing or playing.
Rerun the installer to recover an interrupted activation. Back up your saves
before changing versions. No automatic update or cloud synchronization is installed.

## Build and licenses

Maintainers can build the game-free AppImage with
`bash tools/linux/build_appimage.sh`. Podman is a maintainer build dependency.
The build does not require game data. The release includes corresponding source
archives and checksum manifests. See [THIRD_PARTY.md](THIRD_PARTY.md),
[UPSTREAM.md](UPSTREAM.md) and [LICENSE](LICENSE). Dependencies keep their own
licenses, including the generator's Unicorn GPLv2 component. Preserve all notices.

Release maintainers can export the tracked project with `git archive` and pass
that source-only TAR.GZ to `python3 tools/linux/release_kit.py` after building the
AppImage. It assembles the installer, corresponding-source bundle and checksums;
private game build directories must never be used as the source export.

The application icon was supplied by the maintainer; the MIT code license does
not grant rights to the depicted emblem. Independent, unofficial project, not
affiliated with Nintendo. Development used AI assistance.

The victory portrait can intermittently remain dark. This remains a known issue in 0.1.0; see [investigation notes](docs/KNOWN-PORTRAIT-ISSUE.md).
