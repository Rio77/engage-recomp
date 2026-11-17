# Optional DLC import and live Engage frame rate

User requests optional DLC selection before compilation and persistent 30/60 FPS selection in F1 while playing. Preserve current installations and saves. Do not restart the cancelled game-library build or publish over an existing release before validation.

## Design and implementation

- Validate a selected DLC container holding `<16-hex-title-id>/romfs`, restricted to Engage add-on IDs (base application ID + 0x1000, positive index). Reject links, missing/empty content, duplicate IDs and source/destination overlaps. No DLC selection is a supported path. Ask before starting any copy/build; cancellation exits without compiling. Copy into the transaction's `extracted/dlc`; include DLC in free-space estimates. Preserve previously installed DLC during reinstallation when the optional import is declined.
- Replace the external FPS launcher with an F1 Display choice, persist in settings, migrate the old `engage-fps` value only when no saved setting exists. Keep explicit developer environment override if supplied. Use an atomic runtime target; verify how guest vsync state is applied before claiming switching is safe. Reset presentation deadlines on transitions. Timing scale and presentation pacing must follow the same effective target.
- Add synthetic DLC selection/validation/copy tests and timing tests that switch 60→30→60 within one process, checking scalars and cadence. Run existing frontend and runtime tests.
- Rebuild runtime in the existing toolchain, reuse unchanged public generator/dependencies from the verified release, assemble a new candidate installer and test with isolated synthetic inputs plus existing compiled game library (never overwrite current installation). Deliver candidate and document any real-game checks still needed. Do not silently replace published assets.

## Checklist

- [x] Failing tests for DLC and live FPS
- [x] DLC import, CLI, cancellation and installer wiring
- [x] F1 persistence and live guest timing
- [x] Tests and native runtime build
- [x] Candidate package, integrity checks and handoff

## Validation and adjustment

The first candidate deferred application to a guest timing getter. In the real opening movie that getter did not run, so changes remained pending. Apply the change from the NVN guest presentation entry instead, calling Unity's existing vSyncCount setter through `guest_call` using a copied register context. Presentation pacing and scalar timing hooks use the same atomic effective target. The UI never invokes guest code directly.

Actual Deck smoke test: F1 → Video → Frame rate, 30→60→30→60 during the opening movie; each transition logged from guest presentation, no fatal/dispatch error, INI persisted 60. Comprehensive battle animation/speed testing remains a user acceptance check. The original DLC folder contains title IDs with direct RomFS payload (`e0`/`h0`), now accepted and normalized into `ID/romfs`. This validates the import format, not full DLC gameplay.
