# SD data and configuration

The fixed application directory is `sdmc:/3ds/Starwing/`. Installation keeps
Title ID `0004000005f58b00`. Upgrading the application must preserve this
folder and its user-owned ROMs, companion bundles, saves, and checkpoints.

## Supported input

Supported retail revisions: Star Fox Japan 1.0/1.1, USA 1.0/1.1/1.2,
Starwing Europe 1.0/1.1, and Germany 1.0. An unheadered ROM is 1,048,576 bytes;
a 512-byte copier header is accepted. The probe uses the revision CRC table in
`platform/3ds/source/rom_probe.c`. Renaming a modified ROM does not make it a
supported revision. Filenames need `.sfc` or `.smc` for selection.

## Files and ownership

| File | Purpose and update behavior |
| --- | --- |
| User `.sfc` / `.smc` | Required source ROM; never distributed by this project. |
| `Starfox-Assets-<CRC32>.BIN` | Derived companion for the selected ROM; validated against embedded resource manifest and checksums. |
| `Starfox-Assets.BIN` | Legacy companion path, retained by existing loading logic. |
| `last-rom.cfg` | Selected basename and experience on separate lines; validated against available ROMs. |
| `options.cfg` | Persistent display, gameplay, and developer settings. |
| `starfox-ex-<CRC32>.srm` | Per-ROM 65,536-byte EX SRAM; legacy path is `starfox-ex.srm`. |
| `autosave-original-<CRC32>.bin` / `autosave-ex-<CRC32>.bin` | Optional per-ROM checkpoint; legacy names omit the CRC suffix. |
| `last-error.txt` | Most recent startup/runtime error; a successful new launch clears the previous record. |
| `dumps/` | Numbered diagnostics; full captures contain private process memory. |

SRAM and checkpoints use temporary-file replacement. A failed or incomplete
write must not be interpreted as a newly completed save. Do not edit a save or
replace a companion while the application is running.

## Settings

Settings use one `name=unsigned_integer` per line. The file is read when the
menu/session is constructed. Menu changes persist through the existing save
path; there is no live external-file watcher.

| Key | Default | Interpretation |
| --- | --- | --- |
| `wide` | `1` | Nonzero enables wide presentation. |
| `top_hud` | `1` | Nonzero shows the top-screen HUD. |
| `volume` | `100` | Clamped to 100; zero mutes. |
| `auto_save` | `0` | Nonzero enables automatic checkpoints/restoration. |
| `show_fps` | `0` | Nonzero enables the FPS display. |
| `overlay` | `0` | Nonzero enables SELECT access to the diagnostic overlay. |

Unrecognized or unparsable lines are ignored by the existing loader. Repeated
recognized keys apply in file order, so the last recognized value wins. Numeric
parsing follows the existing unsigned scan behavior; use the documented
nonnegative values instead of relying on negative or overflowing input.

```text
wide=1
top_hud=1
volume=100
auto_save=0
show_fps=0
overlay=0
```

To restore defaults, close the application and rename `options.cfg` as a backup.
Keep all other SD files. Do not use the diagnostic-clear shortcut to reset
settings; it owns only capture folders.

## Checkpoints and recovery

The checkpoint container schema remains `0x41555301`. Its existing container
validation ties state to the selected ROM CRC. Game and SPC state are restored
together, then APU output ports are synchronized. Missing, invalid, truncated,
or incompatible checkpoint data is rejected by the current restore path.
No checkpoint format migration is introduced by v1.0's module extraction.

If startup reports `std::bad_alloc`, preserve `last-error.txt` before relaunching.
Record the version, console model, selected experience, and transition. v1.0
includes the memory-pool adjustment and failure context; current physical
validation is described in [Status](STATUS.md).

For damaged or stale derived companions, first back up the SD folder and retain
the source ROM. The loader validates resources before reuse and can reconstruct
a stale companion from a valid supported ROM. Do not obtain replacement ROMs
or bundles from a bug-report thread.

The updater remembers its selected channel in `update/channel.cfg` (`0` for
Stable, `1` for Experimental). `update/download.part` is temporary executable
data and is removed after an install attempt.
