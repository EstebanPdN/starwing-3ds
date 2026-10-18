# Nintendo 3DS adapter

The adapter keeps libctru, Citro3D/Citro2D, console input, CSND playback,
SD storage, diagnostics, and the built-in updater separate from engine rules.
The product name is **Starwing 3DS**; release version comes from `version.txt`.
`cia-version.txt` is the monotonic AM installation version.

Start with these maintained guides:

- [Build and private inputs](../../docs/BUILDING.md).
- [Runtime and ownership](../../docs/ARCHITECTURE.md).
- [SD data and configuration](../../docs/SD-DATA.md).
- [Built-in updates](../../docs/UPDATING.md).
- [Diagnostics](../../docs/DIAGNOSTICS.md).
- [Tests](../../docs/TESTING.md).
- [Current verification](../../docs/STATUS.md).

The module map is in the architecture guide. Keep comments brief and put
extended explanations there. Preserve Title ID `0004000005f58b00`, the fixed
SD directory, controls, assets, state schemas, and timing policy when moving
code. Track intentional functional changes separately.
