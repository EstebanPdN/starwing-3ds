# Built-in updates

The application name is **Starwing 3DS**. Version numbers belong to release
metadata, not the game name or executable filename. Downloads use the fixed
names `Starwing-3DS.cia` and `Starwing-3DS.3dsx` in each versioned release.

## From the console

1. Connect the console to Wi-Fi and save current progress.
2. Open **Options → Update**. The selected channel is checked automatically.
3. Choose **Channel: Stable** or **Channel: Experimental**. If a newer release
   is available in that channel, choose **Install Update**.
4. Confirm the installation. No is selected initially.
5. Keep power connected and reopen Starwing 3DS after completion.

The update screen reuses Starwing's actual ROM font tables, panel, palette,
beveled buttons, highlight and touch controls. Installed and latest versions
appear above the actions. **A** activates the selected row; **Left/Right** on
the channel row switches channels. **L/R** pages the changelog on the upper
screen; the small Prev/Next areas on the lower screen provide touch paging.
The channel is remembered in `sdmc:/3ds/Starwing/update/channel.cfg`.
**B**, **START**, or the Cancel button cancels a check/download without closing
the panel. When idle, Back returns to Options. Confirmation defaults to No;
Yes downloads, verifies and installs before closing the game.

During gameplay, pause before opening Options. The existing menu pauses source
simulation; network and SD work run on a separate worker. Checking and
downloading can be cancelled. Installation is a separate critical section:
HOME, sleep, cancellation, and menu exits are blocked until it finishes.
A successful installation closes the running game.

CIA installation requires the console's homebrew/CFW installation services.
The downloaded CIA must match Title ID `0004000005f58b00` and have a higher
internal CIA title version. The updater uses AM's overwrite operation; it does
not delete the installed title first.

For Homebrew Launcher, the actual launch path must be known and start with
`sdmc:/`, ending in `.3dsx`. The existing executable is renamed to a backup
suffixed with its old marketing version before the verified download replaces
it. A failed replacement attempts rollback. An existing backup with the same
name stops replacement rather than overwriting it. Keep the backup until the
new version has been checked.

ROMs, derived companions, SRAM, checkpoints, settings, and dumps are outside
the executable replacement and are preserved. Temporary downloads live in
`sdmc:/3ds/Starwing/update/download.part` and are removed after the operation.

## Release contract

Stable reads the public latest normal release endpoint for
`EstebanPdN/starwing-3ds`. Experimental reads the latest 100 releases and
selects the numerically newest compatible prerelease with an `-E<number>`
suffix. Changing channels clears stale availability and automatically checks
again. An empty channel leaves installation disabled.

The selected release must satisfy all of the following:

- A published, non-draft release matching the selected channel.
- A numeric version tag, optionally prefixed with `v`, with up to three parts;
  experimental tags also require a positive `-E<number>` suffix.
- Exactly one matching fixed-name CIA or 3DSX asset for the launch route.
- A canonical asset URL under this repository and the same release tag.
- A positive asset size no larger than 64 MiB.
- GitHub's `sha256:` asset digest containing 64 lowercase hexadecimal digits.
- A numerically newer marketing version; `1.0` and `1.0.0` compare equal.

Future releases must retain these names and Title ID and increase
`platform/3ds/cia-version.txt`. Marketing and CIA title versions are separate.
The source version comes from `platform/3ds/version.txt`. A normal v1.1 release
can therefore update v1.0 without changing its installed identity or SD folder.
Stable excludes prereleases. `1.1-E10` sorts after `1.1-E2`, and normal `1.1`
sorts after both. A channel switch does not enable downgrades. Each installable
experimental package also needs a higher internal CIA version than its base.

HTTPS certificate and hostname validation remain enabled for metadata,
redirects, and downloads. The bundled public CA set is loaded from RomFS.
The full downloaded bytes must match the declared size and SHA-256 before
installation begins. CIA installation also checks package identity and space.
No credential or GitHub token is stored in the application.

## Errors and verification limits

A failed check or download leaves the installed executable in place. Errors
appear in the update panel. Check Wi-Fi, SD free space, system date, and the
current release's assets before retrying. Do not bypass certificate validation.
If a 3DSX backup blocks replacement, inspect it with the application closed
before moving or restoring it.

Host fixtures verify parsing, channel selection/persistence, native-font note
wrapping, rejection rules, and installer rollback/overwrite behavior. ARM compilation verifies the
adapter and install-service calls. Actual TLS behavior, AM installation, and
3DSX replacement on physical hardware remain separate acceptance checks; see
[Status](STATUS.md). The updater is an intentional new feature requested for
v1.0, separate from the behavior-preserving module extractions.

## 1.1 Experimental 1

Tag `v1.1-E1` is a prerelease with CIA title version 84. Select Experimental
in the built-in updater to find it. Public application assets keep the exact
names `Starwing-3DS.cia` and `Starwing-3DS.3dsx`. FPS LIMIT defaults to OFF and
can be set independently to 30 or 60 under Gameplay > Speed / Interpolation.
Save normally before installing. Settings, cartridge saves and model-specific
checkpoints retain their paths; do not delete the SD data folder.
