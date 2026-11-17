# Release 0.1.0 validation

This report distinguishes the release package from the preceding development
candidate. Historical gameplay and performance evidence is preserved in
[PREVIOUS-VALIDATION.md](PREVIOUS-VALIDATION.md); it is not evidence of a second
complete compilation of this AppImage.

## Package checks

- Runtime and frozen Engage generator built in the Ubuntu 22.04 build container.
- Frozen generator processed all three Engage 2.0.0 modules and applied SDK
  unwind, large-function, exception and guarded timing transformations.
- Synthetic installation tests passed: activation with spaces in the path,
  save preservation, corrupted application rejection and interruptions at
  each activation boundary.
- Source publication audit passed: no dump directories, game libraries or
  recognized private-key/token patterns in the publication tree.

The fresh end-to-end AppImage installation and runtime regression suite are
being validated before publication. Their final results will replace this
paragraph. No game data is sent to CI.

## Scope

The release is experimental. Full campaign/DLC compatibility, stable 60 FPS and
long sessions in Gaming mode are not certified. The intermittent monochrome
opening and Additional Content freeze remain known issues. Unknown graphics
resources may still cause first-use waits.
