# Victory portrait: intermittent dark composition

Status: deferred by the maintainer on 2026-11-17; not fixed in 0.1.0.

Reproduce: load the prepared pre-chapter-end manual save, end the turn, let the
battle conclude, and inspect Vander on the victory screen. Sometimes his image
remains dark until advancing. Both 30 and 60 FPS settings can reproduce it.

The model renders correctly into its intermediate image. A later composition
pass can sample a half-size bloom texture instead of the full portrait image.
Captured model textures were identical between a good and bad run. The observed
bindings arrive through individual BindSeparateTexture calls; the final probe
found no deferred source array to refresh at that boundary. The caller was
identified privately for further investigation. Its game disassembly, captures,
shader data and saves are not distributed.

Pending-upload refresh, shader revision identity, argument snapshots and command
recording lifetime have independent regression coverage, but none established a
complete fix for this symptom. Disabling the descriptor memo also did not fix it.
The diagnostic binding override has been removed. Do not raise brightness or
report the issue resolved based on a single successful replay.

Next investigation: trace the selected game texture/descriptor backwards from
the individual binding call and compare the first divergent assignment between
a lit and dark replay, including the game's resource/state cache.
