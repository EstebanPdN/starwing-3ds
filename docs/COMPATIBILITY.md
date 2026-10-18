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
