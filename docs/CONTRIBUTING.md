# Contributing

Start with [Architecture](ARCHITECTURE.md), [Building](BUILDING.md),
and [Testing](TESTING.md). Keep changes focused and explain their concrete
benefit. Preserve the port's title identity, approved visual resources, control
mapping, SD layout, save compatibility, and timing policy unless a change is
explicitly intended and documented.

## Changes

- Keep platform service calls inside the corresponding adapter.
- State ownership and failure policy at resource boundaries.
- Use concise comments for non-obvious invariants; put extended rationale in docs.
- Separate algorithm changes and defect corrections from mechanical extraction.
- Run relevant checks and report unavailable hardware/input-dependent checks.
- Preserve upstream authorship, component licenses, and the pinned reference.
- Keep generated game data, ROMs, saves, process memory, and credentials out of Git.

A useful contribution describes trigger, before/after behavior, affected paths,
compatibility, and executed validation. Do not claim physical FPS from host or
emulator measurements. Do not weaken comparisons merely to accept a changed
result. Keep public fixtures synthetic or independently distributable.

## Reports

Use [GitHub issues](https://github.com/EstebanPdN/starwing-3ds/issues) for a small
reproduction with version, console model, experience, ROM revision, scene, and
settings. Read [Diagnostics](DIAGNOSTICS.md) before sharing a capture.
Share full process memory only through a private maintainer route.

For a suspected security issue, avoid publishing user data or credentials in
an issue. Report the minimal affected path and coordinate privately with the
maintainer before public disclosure.

## Repository organization

Read the [repository map](REPOSITORY.md) before moving files. Keep new public text in English.
Group source, platform adapters, resources, documentation and generated output
by responsibility. Update build manifests, scripts and documentation links
together. Preserve existing version tags and released package bytes.

Run `python3 scripts/check-repository.py` from the repository root before
submitting a change. The check validates the declared root layout, current
documentation links and source/private-file boundaries; it does not replace
the game-specific build, host regressions or physical-console validation.

Use the issue templates for reproducible reports and feature requests. Review
diagnostic captures before sharing and never attach ROMs, extracted game data,
credentials or personal saves to a public report.
