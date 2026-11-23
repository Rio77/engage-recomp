# Engage Recomp validation

## 0.2.0

- Steam Deck Desktop Mode, Engage 2.0.0. The general Maxwell correction kept
  the grass tone stable during a physical and a magic attack. The tester
  reported no recurrence of the graphical artifacts seen in the earlier
  candidate. The complete-frame run recorded zero skipped draws.
- Steam Deck Gaming Mode, launched through Steam under Gamescope. The tester
  confirmed the grass stayed correctly lit. Runtime metrics logged zero
  skipped draws in the test session. A subsequent test of the shutdown
  synchronization returned to Steam with exit status 0 after closing from
  the in-game menu.
- The packaged runtime played S01F.webm in Desktop Mode using the existing
  working test library. Capture 1550 showed the cinematic in color, with zero
  skipped draws or NVN diagnostics in that session.
- Native regression suite and ASan, UBSan and TSan targets passed in the
  Linux build container, including the deterministic Maxwell regression and
  the lifecycle stress test.
- The Maxwell regression test checks the conditional discard, output clear
  and exit pattern, preserving behavior for unmatched sequences. The launcher
  regression test checks normal startup without a progress window and
  opt-in complete-frame rendering with a progress window.
- Python suite: 26 passed in the build container. On SteamOS, 24 passed and
  two platform-dependent tests were skipped.

### Asynchronous launcher validation

- The 0.2.0 candidate enabled complete-frame waits at normal launch,
  bringing back the shader progress window and long waits during gameplay.
  The launcher now uses the previous background behavior by default.
- The revised AppImage started in Desktop Mode and passed the opening
  cinematic using both an exercised cache and a separate copy of the
  cache prepared offline. The tester confirmed in Gaming Mode that performance felt
  similar to the previous working version and the grass stayed correctly lit.
  The session closed with exit code 0.
- Across the two Gaming Mode sessions, the largest recorded shader wait
  total per reporting interval went from 1,830 ms to 12 ms in the sampled
  gameplay range. The second run reused a cache populated by the first.
  The runtime logged transient skipped draws and some remaining slow
  frames, including one during a large asset upload.

### Initial clean installation on Steam Deck

- The installer activated version 0.2.0 after a fresh build from Engage 2.0.0
  ExeFS and RomFS. It generated and built 1,408 units at -O1 with five workers.
  Generation, compilation and link took 42 min 23 s. Copying the input,
  building the library and preparing shaders took 48 min 32 s in total.
- The packaged installer then updated the same isolated installation to the
  final AppImage in 4 min 59 s. It reused the generated game library, whose
  SHA-256 was unchanged. Both installed version metadata and shader
  preparation metadata matched the final AppImage's SHA-256.
- The offline shader run completed in 213 s. It recorded 4,711 successful
  pipeline preparations, one pipeline failure, 134 translation failures
  and zero SPIR-V compilation failures. These counts describe the offline
  inventory. Runtime smoke tests cover the resources used by the scenes
  they exercise.

- The first fresh library failed at boot because the compiled local dispatch
  omitted a valid target from an inferred jump table. The generator now keeps
  the local fallback for inferred jump tables. A synthetic test covers the
  missing-target case, while exception-only dispatch still uses exact targets.

### Clean installation with the corrected generator

- The candidate with the corrected generator installed from a fresh directory
  in 47 min 23 s. Zig compiled 1,464 units at -O1 with five workers. Generation
  took 5 min 35 s, C compilation 36 min 13 s and linking 10 s. Offline shader
  preparation took 2 min 52 s. The installed app and shader-preparation
  records matched that candidate's SHA-256.
- The installed launcher reached and played S01.webm in Steam Deck Desktop
  Mode, past the original missing-branch failure. The smoke run reached frame
  2,400 with zero skipped draws or NVN diagnostics and exited with code 0.
- The same installed AppImage and freshly generated library started from
  Steam in Gaming Mode under Gamescope. The game passed the opening
  cinematic, rendered over 5,200 frames and exited from the window close
  with code 0. The log recorded zero skipped draws and NVN diagnostics.
  This run used copies of the test saves.

### Final kit upgrade and gameplay

- The final kit updated the isolated 0.2.0 installation in 3 min 52 s. It
  reused the game library, whose SHA-256 stayed identical. The activated
  AppImage and shader-preparation records both matched the release AppImage.
- The tester confirmed that Chapters 2 and 3 both started in Gaming Mode.
- After the upgrade, the tester created a manual save in a new slot in
  Gaming Mode and loaded it successfully. The runtime logged the save
  commit and exited with code 0. The test used a separate copy of the saves.

## 0.1.0 validation

This is an experimental Linux x86-64 / Steam Deck release. Tests used a Steam
Deck LCD, SteamOS, a 15 W power limit and a private Engage 2.0.0 dump. Test saves,
game data, generated C, game libraries, shader caches and graphics captures are
excluded from the repository and release assets.

## Installer and build

### Updated installer and live FPS

- Python frontend tests: 17 total, 15 passed and 2 platform/tool-dependent skips.
  Coverage includes optional DLC selection, cancellation before a transaction,
  DLC normalization, duplicate/wrong-game/link rejection and preservation on reinstall.
- Imported both original Engage DLC folders into an isolated location and verified
  file hashes. Empty entitlement marker files are accepted. This checks import,
  not complete DLC gameplay; the known Additional Content freeze is unresolved.
- Native fixed 30/60 and repeated live-transition tests passed. On the actual Deck,
  F1 → Video → Frame rate changed 30→60→30→60 during the opening movie without a
  fatal/dispatch error, and the final rate was saved in the INI. Battle animation
  speed under repeated live switching still needs manual acceptance testing.
- The packaged optional DLC dialog was displayed and interrupted before compilation;
  no install transaction or compiler was created. Existing installations and original
  saves retained their verified hashes throughout isolated testing.
- The complete fresh game-library build below was performed before these runtime
  and frontend updates. It was not repeated for this installer update; the generator
  and game-library ABI are unchanged.

### Original build validation

- Built runtime and frozen three-module generator in the Ubuntu 22.04 container.
- Completed a fresh end-to-end AppImage installation from the user's own dump:
  1,463 generated C units plus the entry unit; Zig O1, three compilation workers;
  2 h 26 min elapsed and approximately 8.3 GB peak service memory. No earlier
  private game object cache was reused. This is one measurement, not an ETA.
- Confirmed the final runtime-only updates do not require regenerating the game
  library; the frozen generator stayed identical to the fresh-install build.
- Synthetic installation/recovery tests passed, including spaces, save
  preservation, corrupted application rejection and four interruption boundaries.
- Process cancellation preserved an unrelated process and terminated the private
  worker's compiler descendants. The real graphical progress window also passed
  cancellation: no activation and no worker left running.
- Before this update, the packaged external Settings window opened and saved 60 FPS.
  It is superseded by the in-game Video menu in the updated installer. Progress and settings windows
  fitted the Deck desktop display. Full controller-only Gaming-mode interaction
  remains a manual acceptance check.
- The real folder chooser accepted the selected input directory; its 629 x 415
  window fitted the Deck desktop display.
- Synthetic dump validation, undefined guest-link rejection and isolated AppImage
  extraction/exit-code tests passed.

## Runtime

The focused native suite passed texture identity/copy/finalization, pending
uploads across view-cache hits, complete-frame success/failure/real 30-second
timeout, 30/60 timing and invalid-rate rejection, Vulkan cache headers, and
bindless TLD/TLD4 GLSL compilation to SPIR-V. The view-cache regression test also
failed on the previous implementation as expected, demonstrating the missing
pending-upload refresh.

Additional regressions passed for reuploading shaders at reused addresses and
combining shader stages without recovering an earlier revision. Command-array
arguments also remain intact when command-memory allocation calls back into the
guest and overwrites its temporary array. That test fails the original recorder
and passes the fixed recorder. Null binding arrays are zero-initialized.

The command-lifetime tests also passed independent recordings over the same
guest command-memory budget, snapshots surviving a new recording, copied lists
surviving source-control reuse, retirement while a replay lease remains active,
finalizing a recorder without invalidating its handles, and control-memory
callbacks. Shader descriptor revisions survive finalization and later uploads;
their metadata is reclaimed at the stopped-renderer reset boundary.

Real isolated save replays completed the chapter-end transition, reached the
victory screen and wrote a new manual save with both 30 and 60 FPS settings.
These settings are limits, not a guarantee of delivered FPS. The previously
reported chapter-end crash did not recur in these replays.

## Scope and limitations

Unknown graphics resources can still cause first-use waits. The opening may
intermittently start monochrome; Additional Content can freeze. Full campaign,
DLC, long sessions, subjective audio quality and controller-only Gaming-mode
acceptance are not certified. Some earlier diagnostic runs stalled during
startup or loading; normal replays subsequently progressed. A reliable cause
for those diagnostic stalls has not been established.

Historical measurements in PREVIOUS-VALIDATION.md belong to the earlier
candidate and must not be presented as measurements of this release. No total
power improvement or stable 60 FPS across the game is claimed.

Victory portrait lighting remains intermittent: final runtime replays completed chapter-end and saved at 30 and 60 FPS, but the 60 FPS replay reproduced the dark portrait. This is a known issue, explicitly deferred by the maintainer. See docs/KNOWN-PORTRAIT-ISSUE.md. No lighting fix is claimed.

A short profiled map sample (same scripted camera, 1280x720 output) is reported separately from loading pauses; it does not certify whole-game performance. A launch attempt exited before producing a native log; a retry and three help-command checks succeeded. That startup anomaly remains unisolated.

| Target | Map sample median | Approx. GPU time/frame | Largest frame in sample |
|---|---:|---:|---:|
| 60 FPS | 43.65 FPS | 9.02 ms | 27 ms |
| 30 FPS | 30.0 FPS | 8.38–9.08 ms | 190 ms |

These are three/four profiler windows of the same scripted scene, including a first-use hitch in the 30 FPS sample. GPU time is estimated from profiler totals divided by presented frames. Total power was not measured anew. No performance gain over the preceding candidate is claimed.
