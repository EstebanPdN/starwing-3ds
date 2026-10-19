# Verification status

Current development version: **1.1**; CIA internal title version: **87**.
Stable 1.1 includes the renderer/audio implementation tested in development.
Old 3DS / 2DS now displays a Python-rasterized, game-style compatibility notice
before ROMFS, SD, audio, or gameplay initialization. New models continue normally.
The application and HOME Menu title remain **Starwing 3DS**.

## Behavior

Both model profiles select ordered PICA spans for compatible wide 2D Mode 2
world scenes. Geometry, exact special-span sampling and foreground layers
remain shared with the reference. Stereo, Mode 1 and unsupported scenes retain
the reference renderer; bounded capacity or GPU failures request a complete
reference frame. This is a new backend, not a complete engine rewrite.

Old/unknown models select a single cartridge SPC with no extra worker/quota.
New retains split stems, its worker policy and stereo resources. Exact native
SPC blocks and ARM saturation apply to both audio routes. Configuration and
checkpoints stay isolated by model; cartridge SRAM paths are unchanged.

Options > Gameplay > Speed / Interpolation has independent GAME SPEED,
INTERPOLATION and FPS LIMIT controls. Defaults are Original speed,
interpolation ON and a 60 FPS limit. Existing saved choices are preserved. It gates drawing without altering source/audio clocks. OFF
removes the explicit software limiter; source scheduling and the physical
60 Hz display still bound visible output. Option buttons are 3 px shorter
and the group is 3 px higher; touch bounds follow the rendered controls.

## Current 1.1 checks

- All 12 console language identifiers, every unknown byte value, and failed
  language reads: supported selection or English fallback as appropriate.
- All six localized screen pairs: text bounds, diacritics and lossless RLE
  round-trip of 1,036,800 pixels verified by the Python artwork generator.
- ARM build checks the libctru language values and all twelve image sizes.
- Azahar system CFG set to Portuguese selects Portuguese; Dutch selects English.
  Captures are emulator evidence, not physical-console validation.

## Earlier 1.1 checks

- ARM Release build; fresh-install Original/ON/60 defaults and saved-choice persistence.
- Timing/menu navigation and updater state regression checks.
- Old-profile Azahar launch without a ROM: notice and START exit.
- New-profile Azahar launch reaches the game intro with the same executable.
- Native pixel artwork uses the Options palette, borders and game glyph grid.

## Earlier development checks (not rerun for this package)

- All 75 discovered level/scene entries: 27 Original and 48 EX, each with Old
  and New audio/render policies. The 150-case matrix compares 187,200 candidate
  frames, including 128,500 native and 18,450 stereo reference frames.
- All active image/coverage comparisons and all 150 SHA-256 audio/full-state
  transcript pairs agree. Replays alternate steering/firing, interpolation
  alpha 0 / 0.5 / 1, and periodic stereo on New.
- Entries cover training, intro, title, continue, credits, all exported normal
  routes, ending, special and black-hole maps. Each case runs 1,200 or 1,800
  direct updates; this is bounded entry/replay coverage, not full playthroughs.
- Actual PICA shader/TEV replay in Azahar: 62 private world snapshots and
  5,952,000 RGBA8 readback pixels, zero differences or failed cases. Two more
  maps expose native frames only before the snapshot point and remain covered
  by the host matrix. This isolated GPU test excludes backgrounds/HUD/audio.
- Prior integrated game capture: all 96,000 RGB565 screen pixels agree with
  the CPU reconstruction of that same state. That capture belongs to its own
  recorded prototype fingerprint, not to the final public binary.
- Portable scene oracle: 294,912 pixels, including textures, masks, dither,
  painter order, index-zero coverage, bounds and capacity fallback.
- Native SPC: 12,288 exact PCM/full-core comparisons, including short deadlines,
  wrapping, flags, I/O, ROM writes and self-modification rejection.
- Original/EX controlled audio, restoration and corrupt-state rejection; the
  existing split audio/ring regression contracts are retained.
- ARMv6K SSAT: 231,075 values agree under ARM1176 instruction emulation,
  including int32 limits. No sample-rate/voice/echo reduction is introduced.
- OFF/30/60 pacing, model defaults, separate persistence, touch/D-pad actions,
  all Options pages and eight updater states/confirmation/cancel checks.
- ROM-free CI covers pacing, transfer, presentation/history, SHA, notes,
  canonical update metadata and install identity/space/cancel/rollback rules.
- ARMv6K Release O3/LTO builds the actual core, shader and frontend. Package
  receipts separately check title identity/version, content digest, stack and
  original icon/banner. Source/imported fingerprints identify each binary.

## Memory and performance interpretation

Vertices use 8 bytes instead of the prototype's 36 (786,432 versus 3,538,944
reserved VBO bytes). Old no longer reserves right-eye staging/textures/target.
Unused Mode 1 development buffers allocate lazily, avoiding 192,000 idle bytes.
Mode 1 PICA is not selected because one host preparation experiment regressed.

Instruction shares and host/emulator milliseconds do not prove physical FPS.
Parallel matrix execution is a correctness exercise, not a performance benchmark.
Five quick captures were used in the earlier investigation; expanded coverage
uses direct replays and GPU readback, with no additional process/quick dumps.

## Remaining acceptance

Physical New sustained FPS, sound continuity, all-stage playthroughs,
HOME/sleep, stereo comfort, updater installation and the intermittent New 3DS
system shutdown require device evidence. No claim of complete coverage or
elimination of that shutdown is made.

ROMs, extracted game assets, scene fixtures, saves and private captures are
excluded from source and release assets. See [Native engine](NATIVE-ENGINE.md),
[Performance research](PERFORMANCE-RESEARCH.md), [Testing](TESTING.md),
[Updating](UPDATING.md) and [Diagnostics](DIAGNOSTICS.md).
