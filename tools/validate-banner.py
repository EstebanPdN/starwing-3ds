#!/usr/bin/env python3
"""Check the actual HOME Menu resources before packaging a Starwing CIA."""

import argparse
import importlib.util
import pathlib
import struct
import wave


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def decompress_lz11(data: bytes) -> tuple[bytes, int]:
    require(len(data) >= 4 and data[0] == 0x11, "CBMD model is not LZ11")
    expected = int.from_bytes(data[1:4], "little")
    pos = 4
    if expected == 0:
        require(len(data) >= 8, "Truncated LZ11 extended size")
        expected = u32(data, 4)
        pos = 8
    require(0 < expected <= 0x80000, "Decompressed CGFX exceeds the 0x80000 banner limit")
    output = bytearray()
    while len(output) < expected:
        require(pos < len(data), "Truncated LZ11 flags")
        flags = data[pos]
        pos += 1
        for bit in range(7, -1, -1):
            if len(output) == expected:
                break
            if not flags & (1 << bit):
                require(pos < len(data), "Truncated LZ11 literal")
                output.append(data[pos])
                pos += 1
                continue
            require(pos < len(data), "Truncated LZ11 back-reference")
            first = data[pos]
            mode = first >> 4
            if mode == 0:
                require(pos + 3 <= len(data), "Truncated LZ11 long back-reference")
                second, third = data[pos + 1:pos + 3]
                length = ((first & 15) << 4 | second >> 4) + 0x11
                distance = ((second & 15) << 8 | third) + 1
                pos += 3
            elif mode == 1:
                require(pos + 4 <= len(data), "Truncated LZ11 extra-long back-reference")
                second, third, fourth = data[pos + 1:pos + 4]
                length = ((first & 15) << 12 | second << 4 | third >> 4) + 0x111
                distance = ((third & 15) << 8 | fourth) + 1
                pos += 4
            else:
                require(pos + 2 <= len(data), "Truncated LZ11 short back-reference")
                second = data[pos + 1]
                length = mode + 1
                distance = ((first & 15) << 8 | second) + 1
                pos += 2
            require(distance <= len(output), "LZ11 back-reference precedes model data")
            require(len(output) + length <= expected, "LZ11 back-reference exceeds declared size")
            for _ in range(length):
                output.append(output[-distance])
    return bytes(output), pos


def validate(icon_path: pathlib.Path, banner_path: pathlib.Path,
             cgfx_path: pathlib.Path, wav_path: pathlib.Path) -> None:
    icon = icon_path.read_bytes()
    require(len(icon) == 0x36C0 and icon[:4] == b"SMDH", "Invalid HOME Menu icon")
    flags = u32(icon, 0x2028)
    require(flags & 0x425 == 0x425, "SMDH lacks visible/3D/extended/no-save flags")

    source = cgfx_path.read_bytes()
    require(len(source) >= 0x14 and source[:4] == b"CGFX", "Invalid source CGFX")
    require(len(source) <= 0x80000, "Source CGFX exceeds the 0x80000 banner limit")
    require(u32(source, 0x0C) == len(source), "CGFX declared size differs from file size")

    banner = banner_path.read_bytes()
    require(len(banner) >= 0x90 and banner[:4] == b"CBMD", "Invalid CBMD banner")
    model_at = u32(banner, 0x08)
    audio_at = u32(banner, 0x84)
    require(0x88 <= model_at < audio_at < len(banner), "Invalid CBMD model/audio offsets")
    model, consumed = decompress_lz11(banner[model_at:audio_at])
    require(model == source, "CBMD LZ11 model differs from the selected CGFX")
    require(not any(banner[model_at + consumed:audio_at]), "Nonzero bytes after compressed CGFX")
    dict_validator = pathlib.Path(__file__).resolve().parent.parent / "platform/3ds/tools/validate-cgfx-dicts.py"
    spec = importlib.util.spec_from_file_location("validate_cgfx_dicts", dict_validator)
    require(spec is not None and spec.loader is not None, "CGFX dictionary validator unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    dict_report = module.validate(model)
    require(not dict_report["failures"],
            "CGFX dictionary validation failed: " + "; ".join(dict_report["failures"]))

    require(banner[audio_at:audio_at + 4] == b"CWAV", "CBMD audio offset lacks CWAV")
    require(u16(banner, audio_at + 4) == 0xFEFF, "CWAV is not little endian")
    require(u32(banner, audio_at + 0x0C) == len(banner) - audio_at,
            "CWAV declared size differs from contained data")
    info_at = audio_at + u32(banner, audio_at + 0x18)
    data_at = audio_at + u32(banner, audio_at + 0x24)
    require(audio_at + 0x40 <= info_at < data_at < len(banner), "Invalid CWAV section offsets")
    require(banner[info_at:info_at + 4] == b"INFO" and
            banner[data_at:data_at + 4] == b"DATA", "Invalid CWAV INFO/DATA sections")
    require(info_at + u32(banner, info_at + 4) <= data_at, "CWAV INFO overlaps DATA")
    require(data_at + u32(banner, data_at + 4) <= len(banner), "CWAV DATA exceeds banner")
    channels = u32(banner, info_at + 0x1C)
    sample_rate = u32(banner, info_at + 0x0C)
    frames = u32(banner, info_at + 0x14)
    require(channels == 2 and sample_rate == 32000 and 0 < frames <= 3 * sample_rate,
            "CWAV must be stereo, 32000 Hz, and at most 3 seconds")

    with wave.open(str(wav_path), "rb") as sound:
        require(sound.getnchannels() == channels and sound.getframerate() == sample_rate and
                sound.getsampwidth() == 2 and sound.getnframes() == frames,
                "CWAV format or duration differs from normalized PCM16 audio")
    print(f"Banner validated: CGFX {len(model)} bytes; CWAV {channels} channels, "
          f"{sample_rate} Hz, {frames / sample_rate:.3f} s; SMDH flags 0x{flags:x}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("icon", type=pathlib.Path)
    parser.add_argument("banner", type=pathlib.Path)
    parser.add_argument("cgfx", type=pathlib.Path)
    parser.add_argument("wav", type=pathlib.Path)
    args = parser.parse_args()
    validate(args.icon, args.banner, args.cgfx, args.wav)


if __name__ == "__main__":
    main()
