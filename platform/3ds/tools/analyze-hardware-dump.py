#!/usr/bin/env python3
"""Summarize private Starwing 3DS SD diagnostics without reading the ROM."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics

FLOW = (
    "pregame_menu", "title", "ex_pregame_menu", "intro",
    "controls_type", "controls_choice", "training", "planet_select",
    "planet_travel", "gameplay", "stage_results", "game_over",
    "continue_choice", "credits", "finished",
)
PAIRS = re.compile(r"([a-z0-9_]+)=([0-9]+)")
REQUIRED = ("game.bin", "audio.bin", "frame.i8", "vram.bin", "cgram.bin", "oam.bin")


def read_samples(path: Path) -> list[dict[str, int]]:
    samples = []
    if not path.is_file():
        return samples
    for line in path.read_text(errors="replace").splitlines():
        values = {key: int(value) for key, value in PAIRS.findall(line)}
        if values.get("ms", 0) > 0 and "presented" in values:
            samples.append(values)
    return samples


def fmt_flow(value: int) -> str:
    return FLOW[value] if 0 <= value < len(FLOW) else f"unknown({value})"


def summarize_perf(folder: Path) -> None:
    samples = read_samples(folder / "perf.log")
    if not samples:
        print("perf.log: missing or no valid samples")
        return
    grouped: dict[int, list[dict[str, int]]] = defaultdict(list)
    for sample in samples:
        grouped[sample.get("flow", -1)].append(sample)
    print(f"perf.log: {len(samples)} measured intervals")
    for flow, rows in sorted(grouped.items()):
        total_ms = sum(row["ms"] for row in rows)
        presented = sum(row["presented"] for row in rows)
        dropped = sum(row.get("dropped", 0) for row in rows)
        ticks = sum(row.get("ticks", 0) for row in rows)
        gpu = sum(row.get("gpu", 0) for row in rows)
        fps = 1000 * presented / total_ms
        tick_rate = 1000 * ticks / total_ms
        interval_fps = [1000 * row["presented"] / row["ms"] for row in rows]
        print(
            f"  {fmt_flow(flow):18s} {total_ms/1000:6.1f}s "
            f"present={fps:5.1f}/s median_interval={statistics.median(interval_fps):5.1f}/s "
            f"logic={tick_rate:5.1f}/s dropped={dropped} gpu_frames={gpu}"
        )
        for key in ("phase", "game", "render", "upload", "audio", "vblank"):
            duration = sum(row.get(key, 0) for row in rows)
            print(f"    {key:7s} {duration/presented if presented else 0:6.2f} ms / presented frame")
    print("  Note: diagnostic 0.1 included dump I/O in one interval; 0.2 excludes it.")


def summarize_dumps(folder: Path) -> None:
    dump_dir = folder / "dumps"
    reports = sorted(dump_dir.glob("dump-*-report.txt")) if dump_dir.is_dir() else []
    if not reports:
        print("dumps/: no completed capture reports")
        return
    print(f"dumps/: {len(reports)} capture reports")
    for report in reports:
        metadata = dict(line.split("=", 1) for line in report.read_text(errors="replace").splitlines() if "=" in line)
        stem = report.name.removesuffix("-report.txt")
        missing = [suffix for suffix in REQUIRED if not (dump_dir / f"{stem}-{suffix}").is_file()]
        frame = dump_dir / f"{stem}-frame.i8"
        expected = int(metadata.get("frame_width", "0")) * int(metadata.get("frame_height", "0"))
        if frame.is_file() and frame.stat().st_size != expected:
            missing.append(f"frame size {frame.stat().st_size}, expected {expected}")
        print(
            f"  {stem}: complete={metadata.get('complete', '?')} "
            f"experience={metadata.get('experience', '?')} "
            f"flow={fmt_flow(int(metadata.get('flow', '-1')))} "
            f"gpu_bg2={metadata.get('gpu_bg2', '?')} "
            f"missing={','.join(missing) if missing else 'none'}"
        )
    for error in sorted(dump_dir.glob("dump-*-error.txt")):
        print(f"  capture error: {error.name}: {error.read_text(errors='replace').strip()}")


def structured_capture_folders(folder: Path) -> list[Path]:
    if (folder / "manifest.json").is_file():
        return [folder]
    dump_dir = folder / "dumps"
    if not dump_dir.is_dir():
        return []
    return sorted(path.parent for path in dump_dir.glob("*/manifest.json"))


def verify_capture(folder: Path, manifest: dict, skip_hashes: bool) -> tuple[int, int, list[str]]:
    errors: list[str] = []
    if manifest.get("complete") is not True:
        errors.append("manifest does not mark the capture complete")
    if not (folder / "COMPLETE").is_file():
        errors.append("COMPLETE marker is missing")

    sums_path = folder / "SHA256SUMS.txt"
    if manifest.get("kind") == "full" and not sums_path.is_file():
        errors.append("full capture has no SHA256SUMS.txt")
        return 0, 0, errors
    if not sums_path.is_file():
        return 0, 0, errors

    entries: list[tuple[str, str]] = []
    for line_number, line in enumerate(
        sums_path.read_text(errors="replace").splitlines(), start=1
    ):
        parts = line.split(maxsplit=1)
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
            errors.append(f"malformed checksum entry at line {line_number}")
            continue
        entries.append((parts[0].lower(), parts[1].lstrip("*")))

    checked = 0
    for expected, relative_name in entries:
        relative = Path(relative_name)
        target = (folder / relative).resolve()
        if relative.is_absolute() or not target.is_relative_to(folder.resolve()):
            errors.append(f"checksum path escapes capture folder: {relative_name}")
            continue
        if not target.is_file():
            errors.append(f"checksummed file is missing: {relative_name}")
            continue
        if skip_hashes:
            checked += 1
            continue
        digest = hashlib.sha256()
        try:
            with target.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
        except OSError as error:
            errors.append(f"cannot read checksummed file {relative_name}: {error}")
            continue
        checked += 1
        if digest.hexdigest() != expected:
            errors.append(f"checksum mismatch: {relative_name}")
    return checked, len(entries), errors


def number(row: dict[str, str], key: str) -> float | None:
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return None


def summarize_structured_dumps(folder: Path, skip_hashes: bool) -> bool:
    captures = structured_capture_folders(folder)
    if not captures:
        return True

    print(f"structured dumps: {len(captures)} capture(s)")
    all_rows: dict[tuple[tuple[str, str], ...], dict[str, str]] = {}
    integrity_ok = True
    total_input_rows = 0
    for capture in captures:
        try:
            manifest = json.loads((capture / "manifest.json").read_text())
        except (OSError, json.JSONDecodeError) as error:
            print(f"  {capture.name}: invalid manifest ({error})")
            integrity_ok = False
            continue
        checked, total, errors = verify_capture(capture, manifest, skip_hashes)
        integrity_ok = integrity_ok and not errors
        runtime_path = capture / "runtime.json"
        try:
            runtime = json.loads(runtime_path.read_text()) if runtime_path.is_file() else {}
        except (OSError, json.JSONDecodeError):
            runtime = {}
            errors.append("runtime.json is missing or invalid")
            integrity_ok = False
        trace_path = capture / "performance.csv"
        rows: list[dict[str, str]] = []
        if trace_path.is_file():
            try:
                with trace_path.open(newline="") as stream:
                    rows = list(csv.DictReader(stream))
                total_input_rows += len(rows)
            except (OSError, csv.Error) as error:
                errors.append(f"cannot read performance.csv: {error}")
                integrity_ok = False
        else:
            errors.append("performance.csv is missing")
            integrity_ok = False
        for row in rows:
            if row.get("flow") and number(row, "fps") is not None:
                all_rows[tuple(sorted(row.items()))] = row
        hash_status = f"hashes {checked}/{total}" if total else "no hash list"
        if skip_hashes and total:
            hash_status += " (not computed)"
        runtime_fields = (
            ("new_3ds", "new3ds"),
            ("speedup_init_result", "speedup_init"),
            ("speedup_config_result", "speedup_config"),
            ("audio_core", "audio_core"),
            ("cpu_quota_percent", "cpu_quota"),
            ("timing_mode", "timing"),
        )
        runtime_status = " ".join(
            f"{label}={str(runtime[key]).lower() if isinstance(runtime[key], bool) else runtime[key]}"
            for key, label in runtime_fields if key in runtime
        )
        print(
            f"  {capture.name}: kind={manifest.get('kind', '?')} "
            f"build={manifest.get('build', runtime.get('build', '?'))} "
            f"complete={manifest.get('complete', False)} {hash_status} "
            f"trace_rows={len(rows)} {runtime_status}"
        )
        for error in errors:
            print(f"    integrity: {error}")

    if not all_rows:
        print("  performance.csv: no valid flow/fps rows")
        return integrity_ok

    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in all_rows.values():
        grouped[row["flow"]].append(row)
    print(
        f"  unique trace intervals: {len(all_rows)} "
        f"(deduplicated {max(0, total_input_rows - len(all_rows))} repeated rows)"
    )
    fields = (
        ("fps", "fps", 1.0),
        ("logic_hz", "logic", 1.0),
        ("phase_ms", "phase_ms", 1.0),
        ("game_ms", "game_ms", 1.0),
        ("tick_strategy_ms", "strategy_ms", 1.0),
        ("tick_draw_ms", "tick_draw_ms", 1.0),
        ("tick_wipe_ms", "tick_wipe_ms", 1.0),
        ("tick_dialogue_ms", "tick_dialogue_ms", 1.0),
        ("tick_palette_oam_ms", "tick_palette_oam_ms", 1.0),
        ("tick_collision_ms", "tick_collision_ms", 1.0),
        ("tick_flow_ms", "tick_flow_ms", 1.0),
        ("draw_ms", "draw_ms", 1.0),
        ("upload_ms", "upload_ms", 1.0),
        ("loop_work_us", "loop_ms", 1000.0),
        ("tick_work_us", "tick_work_ms", 1000.0),
        ("renderer_work_us", "render_ms", 1000.0),
        ("presentation_work_us", "present_ms", 1000.0),
        ("bottom_work_us", "bottom_ms", 1000.0),
        ("bg2_plan_us", "bg2_ms", 1000.0),
        ("bg2_plan_wait_us", "bg2_wait_ms", 1000.0),
        ("world_prepare_us", "world_prepare_ms", 1000.0),
        ("world_raster_us", "world_raster_ms", 1000.0),
        ("world_composite_us", "world_composite_ms", 1000.0),
        ("native_model_us", "native_model_ms", 1000.0),
        ("finalize_us", "finalize_ms", 1000.0),
        ("vblank_wait_us", "vblank_ms", 1000.0),
        ("coalesced_phases", "coalesced", 1.0),
    )
    for flow, rows in sorted(grouped.items()):
        parts = [f"{flow}: n={len(rows)}"]
        for column, label, divisor in fields:
            values = [value / divisor for row in rows
                if (value := number(row, column)) is not None]
            if values:
                parts.append(f"{label}={statistics.median(values):.2f}")
        plan_hits = sum(number(row, "bg2_plan_cache_hits") or 0.0 for row in rows)
        plan_misses = sum(number(row, "bg2_plan_cache_misses") or 0.0 for row in rows)
        if plan_hits + plan_misses:
            hit_rate = 100.0 * plan_hits / (plan_hits + plan_misses)
            parts.append(
                f"bg2_cache={plan_hits:.0f}/{plan_misses:.0f} "
                f"({hit_rate:.1f}% hit)"
            )
        for hit_column, other_column, label, denominator_is_misses in (
            ("world_cache_hits", "world_cache_misses", "world_cache", True),
            ("frame_reuse_hits", "frame_reuse_checks", "frame_reuse", False),
            ("display_reuse_hits", "display_reuse_checks", "display_reuse", False),
            ("bg2_plan_cache_hits", "bg2_plan_cache_misses", "bg2_plan_cache", True),
            ("ppu_bg_cache_hits", "ppu_bg_cache_misses", "ppu_bg_cache", True),
        ):
            hits = sum(number(row, hit_column) or 0.0 for row in rows)
            other = sum(number(row, other_column) or 0.0 for row in rows)
            if any(number(row, hit_column) is not None for row in rows):
                denominator = hits + other if denominator_is_misses else other
                ratio = 100.0 * hits / denominator if denominator else 0.0
                parts.append(f"{label}={hits:.0f}/{other:.0f} ({ratio:.1f}% hit)")
        for column, label in (
            ("continue_bottom_reused", "continue_bottom_reuse"),
            ("coalesced_phases", "coalesced"),
            ("bg2_plan_worker_frames", "bg2_worker_frames"),
        ):
            values = [number(row, column) for row in rows]
            if any(value is not None for value in values):
                parts.append(f"{label}={sum(value or 0.0 for value in values):.0f}")
        presented = sum(number(row, "presented") or 0.0 for row in rows)
        for column, label in (("bg2_pica", "bg2_pica"), ("bg2_fallback", "bg2_fallback")):
            if presented and any(number(row, column) is not None for row in rows):
                frames = sum(number(row, column) or 0.0 for row in rows)
                parts.append(f"{label}={100.0 * frames / presented:.1f}%")
        print("    " + " ".join(parts))
    return integrity_ok


def render_frames(folder: Path, output_dir: Path, brightness: int) -> None:
    from PIL import Image

    dump_dir = folder / "dumps"
    output_dir.mkdir(parents=True, exist_ok=True)
    for report in sorted(dump_dir.glob("dump-*-report.txt")):
        metadata = dict(line.split("=", 1) for line in report.read_text(errors="replace").splitlines() if "=" in line)
        stem = report.name.removesuffix("-report.txt")
        width = int(metadata.get("frame_width", "0"))
        height = int(metadata.get("frame_height", "0"))
        if not (0 < width <= 1024 and 0 < height <= 1024):
            print(f"  {stem}: skipped PNG (invalid dimensions)")
            continue
        frame_path = dump_dir / f"{stem}-frame.i8"
        cgram_path = dump_dir / f"{stem}-cgram.bin"
        if not frame_path.is_file() or not cgram_path.is_file():
            print(f"  {stem}: skipped PNG (frame or palette missing)")
            continue
        pixels = frame_path.read_bytes()
        cgram = cgram_path.read_bytes()
        if len(pixels) != width * height or len(cgram) != 512:
            print(f"  {stem}: skipped PNG (frame or palette size mismatch)")
            continue
        palette = []
        for index in range(256):
            word = int.from_bytes(cgram[index * 2:index * 2 + 2], "little")
            for shift in (0, 5, 10):
                five = (word >> shift) & 31
                expanded = (five << 3) | (five >> 2)
                palette.append(expanded * brightness // 15)
        image = Image.frombytes("P", (width, height), pixels)
        image.putpalette(palette)
        image = image.convert("RGB").resize((400, 240), Image.Resampling.NEAREST)
        suffix = "foreground" if metadata.get("gpu_bg2") == "1" else "frame"
        target = output_dir / f"{stem}-{suffix}.png"
        image.save(target)
        print(f"  PNG: {target}")
    print("  PNG colours use the selected brightness; GPU Mode 2 backgrounds are not in the foreground capture.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("starwing_sd_folder", type=Path, help="copy of sdmc:/3ds/Starwing")
    parser.add_argument("--png-dir", type=Path, help="write 400x240 PNG previews here")
    parser.add_argument("--brightness", type=int, choices=range(16), default=15,
                        help="SNES display brightness for PNG previews (0-15; default 15)")
    parser.add_argument("--skip-hashes", action="store_true",
                        help="check capture structure but do not hash dump files")
    args = parser.parse_args()
    folder = args.starwing_sd_folder
    if not folder.is_dir():
        parser.error(f"not a directory: {folder}")
    legacy_perf = (folder / "perf.log").is_file()
    legacy_dump_dir = folder / "dumps"
    legacy_reports = legacy_dump_dir.glob("dump-*-report.txt") if legacy_dump_dir.is_dir() else ()
    has_legacy_dumps = any(legacy_reports)
    if legacy_perf:
        summarize_perf(folder)
    if has_legacy_dumps:
        summarize_dumps(folder)
    structured_ok = summarize_structured_dumps(folder, args.skip_hashes)
    if not legacy_perf and not has_legacy_dumps and not structured_capture_folders(folder):
        print("no recognized performance trace or completed capture format")
    if args.png_dir is not None:
        render_frames(folder, args.png_dir, args.brightness)
    error = folder / "last-error.txt"
    if error.is_file():
        print(f"last-error.txt: {error.read_text(errors='replace').strip()}")
    if not structured_ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
