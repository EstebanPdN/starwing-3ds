# Compatibility

> ⚠️ **Experimental release — known HOME Menu freeze**
>
> Pressing HOME during a game can freeze the console: the bottom screen turns black and the top screen remains frozen. A forced shutdown by holding POWER may be required. I am aware of this problem and am working on a fix. HOME/sleep/resume reliability has not been established; use this build only if you accept this known issue.

Version **1.2-E2** enables gameplay on New Nintendo 3DS / New 2DS XL and,
experimentally, on Old Nintendo 3DS / 2DS.
A release number does not establish universal compatibility.

| Area | Scope |
| --- | --- |
| New 3DS | Gameplay reported working, but HOME can freeze the console. Release acceptance failed. |
| Old 3DS | Experimental gameplay; busy scenes present fewer frames than New models. Physical results pending. |
| Original Star Fox / Starwing | Primary tested experience during development. |
| Star Fox EX | Selectable, experimental, and not fully supported. On Old 3DS it runs below full speed with audio gaps. |
| Stereo | Experimental in eligible wide Mode 2 gameplay/training on New 3DS; other scenes remain flat. |
| Presentation | Native 400×240 world and separate 320×240 UI. |
| Performance | Original speed, interpolation ON, 60 FPS cap by default; sustained physical 60 FPS is not established. |

Supported clean-ROM revisions are in [SD data](SD-DATA.md). Neither an emulator
profile nor a screenshot certifies hardware performance. HOME, sleep/resume,
long stage progression, stereo comfort, audio on each physical model, and
checkpoint recovery require their own recorded runs.

An earlier 0.80 device report identified `std::bad_alloc` when entering a main
level after working training. The current source includes the subsequent
ordinary/linear memory-pool adjustment and pre-unwind failure diagnostics.
Treat physical confirmation of this correction as pending until measured on
the affected device. See [Status](STATUS.md) for completed checks.
