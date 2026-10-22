# Verification status

Current development version: **1.2-E2**; CIA internal title version: **90**.

> ⚠️ **Experimental release — known HOME Menu freeze**
>
> Pressing HOME during a game can freeze the console: the bottom screen turns black and the top screen remains frozen. A forced shutdown by holding POWER may be required. I am aware of this problem and am working on a fix. HOME/sleep/resume reliability has not been established; use this build only if you accept this known issue.
Version 1.2 enables gameplay on Old 3DS / 2DS and removes the launch notice.
It includes the 1.2-E1 crash correction, black-screen recovery, HOME/sleep
audio handling and Old 3DS performance work described below.
The application and HOME Menu title remain **Starwing 3DS**.

## Behavior

Both model profiles select ordered PICA spans for compatible panoramic and 4:3 2D Mode 2
world scenes. Geometry, exact special-span sampling and foreground layers
remain shared with the reference. Stereo, Mode 1 and unsupported scenes retain
the reference world renderer; stereo can still use GPU BG2 composition.
Bounded capacity or GPU failures request a complete reference frame.
An abandoned GPU keeps all in-flight resources and switches to reserved CPU
scanout buffers. It cannot be reused until a new process starts.

Old/unknown models select a single cartridge SPC on a core 1 worker with an
80, 70 or 50 percent time limit, or synchronous audio if no worker starts.
New retains split stems, its worker policy and stereo resources. Exact native
SPC blocks and ARM saturation apply to both audio routes. Configuration and
checkpoints stay isolated by model; cartridge SRAM paths are unchanged.

Options > Gameplay > Speed / Interpolation has independent GAME SPEED,
INTERPOLATION and FPS LIMIT controls. Defaults are Original speed,
interpolation ON and a 60 FPS limit. Existing saved choices are preserved. It gates drawing without altering source/audio clocks. OFF
removes the explicit software limiter; source scheduling and the physical
60 Hz display still bound visible output. Option buttons are 3 px shorter
and the group is 3 px higher; touch bounds follow the rendered controls.

## 1.2-E1 crash correction

A user-reported New 2DS XL data abort matches the 1.1 ELF: PC 0x002b43bc,
LR 0x00150c3c, R0=1, R1=0, and fault address 0x0000000c. The 96 captured
instruction bytes match the published build. The caller passes a null texture
to Citro3D 1.7.1 unit 1, whose type check dereferences it.

Remove the null binds for units 1/2. Owned palette/checker textures remain live;
Citro2D resets its combiner modes before drawing and does not sample these units.
No geometry, palette, timing, shader, or gameplay changes are included.

The ARM handoff regression executes the linked instructions with page zero
unmapped: 1.1 faults at address 12; the candidate reaches C2D_Prepare without
an invalid access. This CPU replay does not simulate GPU output or establish
physical New 2DS XL acceptance. User confirmation on hardware is still pending.

## Current renderer investigation

### Physical HOME failure remains unresolved

The owner reproduced a full console freeze after entering gameplay and pressing
HOME, including after both attempted HOME corrections. The bottom screen turns
black, the top screen retains the game image, and controls stop responding.
A forced shutdown is required. This is a failed physical acceptance case;
**1.2-E2 does not fix the HOME Menu freeze**.

The current candidate stops audio and drains pending GPU work in ordinary
application context before a pending HOME transition. Its APT audio hook only
updates memory flags. The host regression verifies this limited contract under
both model profiles; it does not establish successful APT/GSP/system-menu
handoff on hardware. Earlier removal of a CSND completion wait did not resolve
the reported physical freeze. HOME/sleep/resume acceptance remains open.

The owner requested publication as an explicitly experimental prerelease,
with the known failure disclosed at the start of the release notes. E2 replaces
the E1 prerelease; the stable 1.1 release remains available separately.

### Renderer evidence

The [1.2 renderer investigation](PERFORMANCE-1.2.md) compares the actual MKW-3DS
and Epic MK64 shader, geometry, state/cache, queue and quality implementations
with this port. It records the adaptations and validation limits.

Current checks include the callback/swap race, a callback already entered when
recovery starts, system-tick wait units, a bounded suspend timeout, and late
GPU completion/writes that must not alter the independent recovery scanout.
The bootstrap console's single-buffer mode is explicitly reset before GPU use.
Recovery reserves 375 KiB of linear memory for the process lifetime.

Azahar Software is used only as an emulator diagnostic; the release keeps PICA.
The final sixteen-case ROM-free composition/recovery test matches all 1,536,000
pixels per backend under both Software and Vulkan, including checker scales
1 through 4. Integrated Software 4:3 captures match 288,000 reference pixels;
New-profile Wide stereo final targets match 384,000 pixels across two paired
states. The post-benchmark Old Wide target matches 96,000 pixels. Vulkan's
direct cached framebuffer-memory readback showed stripes while the explicit
render-target readback and its visible window were correct; that raw memory
dump is not treated as evidence of a visible rendering defect.

An injected presentation failure switches to the reference renderer and then
independent CPU scanout; two later recovery frames match all 192,000 pixels.
The run continues to its scripted end. Host tests additionally check an actual
concurrent callback and a simulated late write to a former GPU destination.

The retained Wide Mode comparison under Azahar's Old profile measures median
presentation FPS of 23.715 for 1.2-E1 and 31.000 for the candidate. Source logic
is about 15 Hz in both runs. This is one sequential pair of emulator runs, not
physical Old 3DS FPS or proof of full source speed. All further performance
tests use Wide Mode; 4:3 remains in visual regression coverage only. Exact
windows, hashes and costs are in the investigation linked above.

The final Release CIA/3DSX are built with autopilot disabled. The final presenter
test includes the last failure-only initialization cleanup; integrated game
captures and timings precede that cleanup. These checks do not establish
physical-console black-screen or HOME acceptance.

## Inherited 1.1 checks

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
