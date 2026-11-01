#!/usr/bin/env bash
# Builds app/host/screens.cpp with the PC's compiler and draws the app's
# pages into OUT_DIR as PNG files (BMP without Pillow).
#
#   app/host/screens.sh OUT_DIR [STATUS_FILE | live]
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app="$here/.."
out="$1"
mkdir -p "$out"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 "$app/src/ui/make_logo.py" "$app/../assets/logo-mark.png" 104 "$tmp/logo_small.bin"
python3 "$app/src/ui/make_logo.py" "$app/../assets/logo.png" 360 "$tmp/logo_big.bin"
cp "$app/src/ui/font.bin" "$tmp/"
python3 "$app/src/make_guide.py" "$app/../docs/GUIDE.md" "$tmp/guide.txt"
"${CXX:-c++}" -std=c++17 -O2 -Wall -I "$app/src" -I "$app/third_party/qrcodegen" -o "$tmp/screens" \
    "$here/screens.cpp" "$app/src/app.cpp" "$app/src/status.cpp" "$app/src/link.cpp" "$app/src/ui/canvas.cpp" \
    "$app/third_party/qrcodegen/qrcodegen.cpp"
"$tmp/screens" "$tmp" "$out" "${@:2}"
if python3 -c "import PIL" 2>/dev/null; then
    python3 - "$out" <<'PY'
import glob, os, sys
from PIL import Image
for bmp in glob.glob(os.path.join(sys.argv[1], "*.bmp")):
    Image.open(bmp).save(bmp[:-4] + ".png")
    os.remove(bmp)
PY
fi
