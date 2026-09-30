#!/usr/bin/env python3
from __future__ import annotations

import contextlib
import csv
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("analyze-hardware-dump.py")
SPEC = importlib.util.spec_from_file_location("analyze_hardware_dump", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


FIELDS = [
    "session_s", "flow", "fps", "logic_hz", "loop_work_us",
    "tick_work_us", "renderer_work_us", "presentation_work_us", "bg2_plan_us",
    "bg2_plan_wait_us", "bg2_plan_worker_frames",
    "world_prepare_us", "world_raster_us", "world_composite_us",
    "native_model_us", "finalize_us",
    "tick_wipe_ms", "tick_dialogue_ms", "tick_palette_oam_ms",
    "tick_collision_ms", "tick_flow_ms",
    "bg2_plan_cache_hits", "bg2_plan_cache_misses", "world_cache_hits",
    "world_cache_misses", "frame_reuse_checks",
    "frame_reuse_hits",
]
SAMPLE = {
    "session_s": "12", "flow": "GAME OVER", "fps": "60.0",
    "logic_hz": "20.0", "loop_work_us": "5000",
    "tick_work_us": "1200",
    "renderer_work_us": "1000", "presentation_work_us": "300",
    "bg2_plan_us": "0", "bg2_plan_wait_us": "420",
    "bg2_plan_worker_frames": "2", "bg2_plan_cache_hits": "2",
    "world_prepare_us": "2100", "world_raster_us": "3100",
    "world_composite_us": "400", "native_model_us": "600",
    "finalize_us": "1800",
    "tick_wipe_ms": "0.10", "tick_dialogue_ms": "0.20",
    "tick_palette_oam_ms": "0.30", "tick_collision_ms": "0.40",
    "tick_flow_ms": "0.50",
    "bg2_plan_cache_misses": "1", "world_cache_hits": "0",
    "world_cache_misses": "17", "frame_reuse_checks": "3",
    "frame_reuse_hits": "2",
}


def make_capture(root: Path, name: str, row: dict[str, str] = SAMPLE) -> Path:
    capture = root / "dumps" / name
    capture.mkdir(parents=True)
    (capture / "manifest.json").write_text(json.dumps({
        "schema": 1, "kind": "full", "build": "candidate", "complete": True,
    }))
    (capture / "COMPLETE").write_text("complete\n")
    (capture / "runtime.json").write_text(json.dumps({
        "build": "candidate", "flow": "GAME OVER", "fps": 60.0,
        "new_3ds": True, "speedup_init_result": 0,
        "speedup_config_result": 0, "audio_core": 2,
        "cpu_quota_percent": 0, "timing_mode": "unlocked_20_hz",
    }))
    with (capture / "performance.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerow(row)
    names = ("runtime.json", "performance.csv")
    lines = []
    for filename in names:
        digest = hashlib.sha256((capture / filename).read_bytes()).hexdigest()
        lines.append(f"{digest}  {filename}")
    (capture / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n")
    return capture


class HardwareDumpAnalyzerTests(unittest.TestCase):
    def test_verifies_complete_structured_capture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            capture = make_capture(Path(temporary), "capture")
            manifest = json.loads((capture / "manifest.json").read_text())
            checked, total, errors = analyzer.verify_capture(
                capture, manifest, skip_hashes=False)
            self.assertEqual((checked, total, errors), (2, 2, []))

    def test_rejects_checksum_paths_outside_capture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            capture = make_capture(Path(temporary), "capture")
            sums = capture / "SHA256SUMS.txt"
            sums.write_text("0" * 64 + "  ../../outside.bin\n")
            manifest = json.loads((capture / "manifest.json").read_text())
            _, _, errors = analyzer.verify_capture(
                capture, manifest, skip_hashes=True)
            self.assertTrue(any("escapes capture folder" in error for error in errors))

    def test_deduplicates_rolling_trace_rows_across_captures(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_capture(root, "capture-a")
            make_capture(root, "capture-b")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = analyzer.summarize_structured_dumps(root, skip_hashes=False)
            self.assertTrue(result)
            text = output.getvalue()
            self.assertIn("structured dumps: 2 capture(s)", text)
            self.assertIn("unique trace intervals: 1 (deduplicated 1 repeated rows)", text)
            self.assertIn("GAME OVER: n=1", text)
            self.assertIn("tick_work_ms=1.20", text)
            self.assertIn("bg2_wait_ms=0.42", text)
            self.assertIn("world_prepare_ms=2.10", text)
            self.assertIn("world_raster_ms=3.10", text)
            self.assertIn("finalize_ms=1.80", text)
            self.assertIn("tick_wipe_ms=0.10", text)
            self.assertIn("tick_dialogue_ms=0.20", text)
            self.assertIn("tick_collision_ms=0.40", text)
            self.assertIn("bg2_worker_frames=2", text)
            self.assertIn("world_cache=0/17 (0.0% hit)", text)
            self.assertIn("bg2_cache=2/1 (66.7% hit)", text)
            self.assertIn("frame_reuse=2/3 (66.7% hit)", text)
            self.assertIn(
                "new3ds=true speedup_init=0 speedup_config=0 audio_core=2",
                text)


if __name__ == "__main__":
    unittest.main()
