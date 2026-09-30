# Verification status

Current development version: **0.67**; CIA internal title version: **66**.
No stable release is declared. No GitHub release or binary is published by this
repository's workflow.

## What the current project contains

Original / EX launch selection, clean-ROM validation and on-device asset
reconstruction, native SPC audio, per-ROM EX SRAM and optional checkpoints,
400x240 / 320x240 presentation, touch options, separate presentation/logic
telemetry, quick/full diagnostics, and an experimental stereo path are
implemented in the existing code.

The current local 0.67 delivery has CIA and complete 3DSX files and recorded
host tests. Its package metadata marks physical validation as pending. The
available physical dump records identify build **0.66**, not 0.67. They must
not be used as proof of the new build's FPS or stereo behavior.

## Validation boundaries

- Public host checks: frame pacing, launch-selection persistence and rejected
  filenames, screen transfer/brightness, and diagnostic-analyzer fixtures.
- Existing local engine tests: synthetic rendering/raster, BG2, stereo,
  simulation, and data-dependent differential tests. Run only the tests for
  which the required permitted inputs are available.
- ARM11 compilation and package checks establish build output and container
  consistency; they do not establish installation or gameplay on a console.
- Full-game physical testing, sustained 60 FPS, stereo comfort, and Old 3DS
  qualification remain pending.

## Physical checks still required for 0.67

1. Install on the intended New 3DS using the existing Title ID; retain SD data.
2. Check startup, Original / EX selection, controls, both screens, audio, and
   settings persistence.
3. Check SRAM and autosave behavior, HOME, sleep, close, and restart.
4. Compare the same gameplay/boss encounter with the 3D slider at zero and on.
5. Capture `L+R+A` during movement and inspect FPS, logic Hz, frame phases, BG2,
   audio, and GPU timings together with the screenshots.
6. Test Old 3DS separately before adding it to any compatibility claim.

The full dump contains private process memory. Keep it local; share only the
reviewed evidence needed for a bug report.

## Repository preparation checks — September 30, 2026

- The selected public source compiled successfully for ARM11 with the existing
  local private inputs supplied through the documented variables.
- The complete `Starwing-3DS-Game.3dsx` and
  `Starwing-3DS-Gameplay-0.67.cia` were produced locally. Banner/icon validation
  passed and ticket/TMD title version 66 was verified. These binaries were not
  uploaded to GitHub.
- Existing frame-pacing, launch-selection, and screen-transfer host tests passed;
  all three synthetic diagnostic-analyzer tests passed.
- The tracked publication selection passed excluded-data, possible-secret,
  version, pinned-submodule, local-link, and whitespace checks. Inherited shader
  and vendor formatting was preserved.

These are local source/build checks. They do not change the physical hardware
validation status stated above.
