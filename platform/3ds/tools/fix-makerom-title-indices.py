#!/usr/bin/env python3
"""Repair makerom's tool-version macro/title-version enum name collision.

Use on a local Project_CTR/makerom source tree, then run `make deps; make`.
Keep the original executable; point MAKEROM to the repaired local build.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1]) / "src"
header = root / "user_settings.h"
text = header.read_text()
if "TITLE_VER_MAJOR" not in text:
    for old, new in [("\tVER_MAJOR,", "\tTITLE_VER_MAJOR,"),
                     ("\tVER_MINOR,", "\tTITLE_VER_MINOR,"),
                     ("\tVER_MICRO\n", "\tTITLE_VER_MICRO\n")]:
        if old not in text:
            raise SystemExit("Unsupported makerom enum layout; no header changes written")
        text = text.replace(old, new)
    header.write_text(text)
for name in ["user_settings.c", "cia.c"]:
    path = root / name
    text = path.read_text()
    for component in ["MAJOR", "MINOR", "MICRO"]:
        text = text.replace("[VER_" + component + "]", "[TITLE_VER_" + component + "]")
    path.write_text(text)
print("Title-version indices renamed; makerom's displayed tool version is unchanged")
