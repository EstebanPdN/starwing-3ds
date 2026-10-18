#!/usr/bin/env python3
"""Compare pre-change/candidate audio on the host. Never establishes 3DS FPS.

The before directory must contain audio_3ds.cpp/.hpp and spc700_audio.cpp.
The existing host build supplies unchanged simulation and SNES SPC libraries.
ROMs and generated SPC transcripts are private local inputs, never uploaded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def run(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout.strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("before", "host-build", "original-rom", "original-symbols",
                     "ex-rom", "ex-symbols", "output"):
        parser.add_argument("--" + argument, required=True, type=Path)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--keep-transcripts", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    before, host = args.before.resolve(), args.host_build.resolve()
    core = [args.cxx, "-std=c++20", "-O3", "-DNDEBUG",
            "-I" + str(source / "include"),
            "-I" + str(host / "generated/snes_spc")]
    libraries = [host / name for name in ("libstarfox_core.a", "libretro_cpu_65816.a",
                                         "libretro_cpu_core.a", "libsnes_spc_core.a")]
    report = []
    for version in ("baseline", "candidate"):
        wrapper = before if version == "baseline" else source / "platform/3ds/source"
        spc = before / "spc700_audio.cpp" if version == "baseline" else source / "src/audio/spc700_audio.cpp"
        run([args.cxx, "-std=c++20", "-O3", "-I" + str(source / "tests/audio_3ds_host"),
             "-I" + str(wrapper), source / "tests/audio_ring_test.cpp",
             wrapper / "audio_3ds.cpp", "-o", output / ("ring-" + version)])
        defines = ["-DSTARFOX_AUDIO_BASELINE"] if version == "baseline" else []
        run(core + defines + [source / "tests/audio_buffer_reuse_test.cpp", spc]
            + libraries + ["-o", output / ("spc-" + version)])
    for case in ("ring", "original", "ex"):
        pair = []
        for version in ("baseline", "candidate"):
            transcript = output / (case + "-" + version + ".bin")
            if case == "ring":
                command = [output / ("ring-" + version), transcript]
            else:
                rom = getattr(args, case + "_rom").resolve()
                symbols = getattr(args, case + "_symbols").resolve()
                command = [output / ("spc-" + version), rom, symbols, transcript]
            message = run(command)
            record = dict(case=case, version=version, result=message,
                          bytes=transcript.stat().st_size, sha256=digest(transcript))
            pair.append(record)
            print(case, version, message, flush=True)
        if pair[0]["sha256"] != pair[1]["sha256"]:
            raise AssertionError(case + " audio transcript differs; binaries retained")
        report += pair
        if not args.keep_transcripts:
            for version in ("baseline", "candidate"):
                (output / (case + "-" + version + ".bin")).unlink()
    run(core + [source / "tests/direct_audio_tests.cpp", source / "src/audio/spc700_audio.cpp"]
        + libraries + ["-o", output / "direct-audio"])
    for case in ("original", "ex"):
        run([output / "direct-audio", getattr(args, case + "_rom").resolve(),
             getattr(args, case + "_symbols").resolve()])
        report.append(dict(case="direct-" + case, result="PASS"))
    (output / "audio-differential.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PCM, stem, port, complete-state and ring differentials PASS; direct audio PASS")


if __name__ == "__main__":
    main()
