#!/usr/bin/env python3
# The logo for the app's screens (assets/logo.png), scaled, as raw pixels
# the app draws without a PNG decoder (embedded by data.S):
#
#   python3 app/src/ui/make_logo.py assets/logo.png 104 app/src/ui/logo_small.bin
#   python3 app/src/ui/make_logo.py assets/logo.png 360 app/src/ui/logo_big.bin
#
# Format, little-endian: "WSLG", u16 width, u16 height, then 0xAARRGGBB
# pixels row by row (opaque: the logo is on black, as the screens are).
import struct, sys
from PIL import Image

src, size, out_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
img = Image.open(src).convert("RGB").resize((size, size), Image.LANCZOS)
out = bytearray(b"WSLG" + struct.pack("<HH", size, size))
rgb = img.tobytes()
for i in range(0, len(rgb), 3):
    out += bytes((rgb[i + 2], rgb[i + 1], rgb[i], 255))
with open(out_path, "wb") as f:
    f.write(out)
