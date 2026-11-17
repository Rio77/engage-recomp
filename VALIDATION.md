# Engage Recomp 0.1.0 validation

This is an experimental Linux x86-64 / Steam Deck release. Tests used a Steam
Deck LCD, SteamOS, a 15 W power limit and a private Engage 2.0.0 dump. Test saves,
game data, generated C, game libraries, shader caches and graphics captures are
excluded from the repository and release assets.

## Installer and build

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
- Packaged Settings window opened and saved 60 FPS. Progress and settings windows
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
