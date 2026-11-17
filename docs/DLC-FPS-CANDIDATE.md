# DLC import and live FPS — 0.1.0 update, 2026-11-17

Included in the updated 0.1.0 release. The port remains experimental; release status does not imply full campaign or DLC validation.

- Installation asks whether to import DLC after selecting base ExeFS/RomFS and before any copying or compilation. Cancel exits before creating a build transaction.
- Select a folder containing Engage title IDs, each with `romfs/` or a direct extracted RomFS payload. A single title-ID folder is also accepted. The files are normalized to `extracted/dlc/ID/romfs`. IDs from other games, duplicate IDs, empty content, links and encrypted container extensions are rejected.
- Reinstalling preserves existing DLC not replaced by a newly selected title. Optional import does not certify DLC gameplay; Additional Content's known freeze remains unresolved.
- F1 → **Video → Frame rate** selects 30/60 FPS while playing. The setting persists as `engage_fps` in `fe3hrecomp.ini`; an old `engage-fps` value is read only as the fallback for installations missing that INI key. New installs do not create an external FPS selector. An old settings launcher displays the new menu location.
- Unity vSyncCount and host presentation pacing are updated in the guest presentation entry. Timing scalar hooks use the same atomic effective rate. The guest setter uses a separate register context so the current NVN call's arguments are preserved.

Validation: 17 Python tests, 15 passed and 2 expected platform skips. Native timing tests pass at fixed 30/60 and repeated live transitions. Actual Deck movie test changed 30→60→30→60 without a fatal or dispatch error and persisted 60. Full battle speed/animation and DLC-content validation are still required; no new performance gain is claimed. Current installations and original saves were not modified by the isolated smoke test.
