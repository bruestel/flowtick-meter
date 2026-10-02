#!/usr/bin/env python3
"""
Same output as gen_ui_asset.py, callable without PlatformIO.

Used by the CMake fallback for a first configure, and handy for plain idf.py
builds. Usage: gen_ui_asset_standalone.py <index.html> <ui_asset.c>

Copyright 2026 Jonas Brüstel
SPDX-License-Identifier: Apache-2.0
"""

import gzip
import pathlib
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)

src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
packed = gzip.compress(src.read_bytes(), compresslevel=9, mtime=0)
body = ",".join(f"0x{b:02x}" for b in packed)
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(
    "/* Generated from www/index.html -- do not edit. */\n"
    f"const unsigned char ui_index_html_gz[] = {{{body}}};\n"
    f"const unsigned int ui_index_html_gz_len = {len(packed)};\n"
)
print(f"gen_ui_asset: {src.name} {len(src.read_bytes())} -> {len(packed)} bytes gzip")
