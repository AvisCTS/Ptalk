#!/usr/bin/env python3
"""
Convert PNG (225x225) to C++ header with uint8_t[] binary array
All files (.py, .png, .hpp) stay in the SAME folder.

Usage:
    python convert_logo.py logo1.png
"""

import sys
import os
from PIL import Image

EXPECTED_SIZE = (225, 225)

def die(msg):
    print("ERROR:", msg)
    sys.exit(1)

def main():
    if len(sys.argv) != 2:
        print("Usage: python convert_logo.py logo.png")
        sys.exit(1)

    png_file = sys.argv[1]

    if not os.path.exists(png_file):
        die(f"File not found: {png_file}")

    # Output .hpp name from input png
    base = os.path.splitext(os.path.basename(png_file))[0]
    hpp_file = base + ".hpp"

    # ---------- Validate PNG ----------
    img = Image.open(png_file)

    if img.format != "PNG":
        die("Input image must be PNG")

    if img.size != EXPECTED_SIZE:
        die(f"Image must be {EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}, got {img.size}")

    # ---------- Read raw PNG bytes ----------
    with open(png_file, "rb") as f:
        png_bytes = f.read()

    # ---------- Symbol names ----------
    symbol = base.upper()
    array_name = f"{symbol}_PNG"
    len_name   = f"{symbol}_PNG_LEN"

    # ---------- Write header ----------
    with open(hpp_file, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stddef.h>\n\n")

        f.write(f"// Auto-generated from {png_file}\n")
        f.write(f"// PNG size: {EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}\n")
        f.write(f"// Total bytes: {len(png_bytes)}\n\n")

        f.write(f"const uint8_t {array_name}[] = {{\n")

        for i, b in enumerate(png_bytes):
            if i % 12 == 0:
                f.write("    ")
            f.write(f"0x{b:02X}, ")
            if i % 12 == 11:
                f.write("\n")

        f.write("\n};\n\n")
        f.write(f"const size_t {len_name} = sizeof({array_name});\n")

    print("OK")
    print(f"PNG : {png_file}")
    print(f"HPP : {hpp_file}")
    print(f"SIZE: {len(png_bytes)} bytes")

if __name__ == "__main__":
    main()
