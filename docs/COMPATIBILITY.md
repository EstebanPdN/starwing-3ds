# Compatibility

Version **1.0** is the initial public distribution of the current Nintendo 3DS
adaptation. It includes the latest local development changes and organization
work. A release number does not establish universal compatibility.

| Area | Scope |
| --- | --- |
| New 3DS | Primary development target; current physical acceptance remains pending. |
| Old 3DS | Code and fallback paths exist; independent qualification remains pending. |
| Original Star Fox / Starwing | Primary tested experience during development. |
| Star Fox EX | Selectable, experimental, and not fully supported. |
| Stereo | Experimental in eligible wide Mode 2 gameplay/training; other scenes remain flat. |
| Presentation | Native 400×240 world and separate 320×240 UI. |
| Performance | 20 Hz source / 60 Hz presentation targets; sustained physical 60 FPS is not established. |

Supported clean-ROM revisions are in [SD data](SD-DATA.md). Neither an emulator
profile nor a screenshot certifies hardware performance. HOME, sleep/resume,
long stage progression, stereo comfort, audio on each physical model, and
checkpoint recovery require their own recorded runs.

An earlier 0.80 device report identified `std::bad_alloc` when entering a main
level after working training. The current source includes the subsequent
ordinary/linear memory-pool adjustment and pre-unwind failure diagnostics.
Treat physical confirmation of this correction as pending until measured on
the affected device. See [Status](STATUS.md) for completed checks.

## Automatic hardware profiles (1.1-E1 candidate)

The frontend detects the model without a separate download. Old 3DS uses one
cartridge SPC700/S-DSP instance for music and effects on the application core,
a 30 Hz presentation target, and 2D rendering. New 3DS retains its split-stem
audio worker and 60 Hz presentation target. These are budgets, not measured
physical FPS guarantees. Unknown-model detection uses the conservative profile.

Options → Gameplay → Speed / Interpolation exposes Original and 20 Hz cadence,
with interpolation independently Off or On. Old 3DS defaults to Original/Off;
New 3DS retains 20 Hz/On. Original preserves the engine's source pacing model,
not a promise that an overloaded device can meet real-time deadlines. Old's
30 Hz display budget does not change the 60 Hz source phases or SPC clock.

The Old profile renders Options only after a visible change, uses a tiled
RGB565 top-screen copy, and avoids a second stereo world raster. The single-SPC
route uses the cartridge's shared sound voices rather than the desktop port's
independent music/effects mixes. Compare music, engine noise, shots, voices,
pause/resume and stage transitions on the physical console before acceptance.

This candidate has host correctness and ARM package checks; sustained Old/New
3DS performance, underrun counts and the reported physical crash still need
same-build device evidence. A full-memory capture is not required for a normal
performance report; a quick capture in the title and one in gameplay suffice.
