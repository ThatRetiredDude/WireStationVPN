#!/usr/bin/env bash
# Builds the PS5 app (from TokenStation5's ps5/build.sh, without the engine):
#
#   build/ps5/PPSA99556/eboot.bin            the app, with RADV linked in (its screen)
#   build/ps5/PPSA99556/tailstation.elf      the background part (scripts/build-ps5.sh)
#   build/ps5/PPSA99556/sce_sys/             param.json, icon0.png
#   build/ps5/PPSA99556/sce_module/libc.prx  the runtime module titles carry
#   build/ps5/PPSA99556/LICENSE, NOTICE.md, THIRD-PARTY-LICENSES.txt, SOURCE.txt
#
# Needs app/setup.sh run once, and scripts/build-ps5.sh and
# scripts/licenses.sh run first.
#
#   app/build.sh
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$here/.."
vk="${PS5_VULKAN_ROOT:-${WS_PS5_DEPS:-$root/.deps/ps5}/PS5_Vulkan}"
sdk="$vk/.deps/native/ps5-payload-sdk"
radv="$vk/.deps/native/radv-release"
archive="$radv/lib/libvulkan_radeon.ps5.a"
tool="$vk/build/host/ps5-native-tool"
param="$here/sce_sys/param.json"
out="$root/build/ps5"
payload="$out/tailstation.elf"
for f in "$archive" "$tool" "$vk/runtime/libc.prx" "$sdk/bin/prospero-lld"; do
    [ -e "$f" ] || { echo "build: missing $f (run app/setup.sh)" >&2; exit 2; }
done
[ -f "$payload" ] || { echo "build: missing $payload (run scripts/build-ps5.sh)" >&2; exit 2; }
version=$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo dev)
title_id=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
obj="$out/app-obj"
mkdir -p "$obj" "$out/stubs"

cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" "$@"; }

# The logo at the two sizes the screens draw it.
python3 "$here/src/ui/make_logo.py" "$root/assets/logo-mark.png" 104 "$obj/logo_small.bin"
python3 "$here/src/ui/make_logo.py" "$root/assets/logo.png" 360 "$obj/logo_big.bin"
cp "$here/src/ui/font.bin" "$obj/"

cxxflags=(-std=c++17 -O2 -Wall -ffunction-sections -fdata-sections -fexceptions -fcxx-exceptions -frtti
    -DWS_PS5=1 "-DWS_VERSION=\"$version\"" -I "$here/src" -I "$here/third_party/qrcodegen" -I "$radv/include")
objects=()
pids=()
for src in "$here"/src/{main,app,status,link,vk_context,log}.cpp "$here"/src/ui/{canvas,display,pad}.cpp \
        "$here"/third_party/qrcodegen/qrcodegen.cpp; do
    o="$obj/$(basename "$src" .cpp).o"
    objects+=("$o")
    cc "${cxxflags[@]}" -c "$src" -o "$o" &
    pids+=($!)
done
objects+=("$obj/data.o")
cc -x assembler-with-cpp -Wa,-I"$obj" -c "$here/src/ui/data.S" -o "$obj/data.o" &
pids+=($!)
cc -std=c11 -O2 -Wall -ffunction-sections -fdata-sections -c "$here/src/compat.c" -o "$obj/compat.o" &
pids+=($!)
# PS5_Vulkan's title start code: IEEE floating point, the platform runtime,
# static constructors, then main.
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$vk/tooling/native/app_crt.cpp" -o "$obj/app_crt.o" &
pids+=($!)
for p in "${pids[@]}"; do wait "$p"; done
# In an archive, so --exclude-libs keeps its names local: the SDK's stub
# modules define some of them too, and the title converter refuses exports.
rm -f "$obj/libwscompat.a"
"$sdk/bin/llvm-ar" rcs "$obj/libwscompat.a" "$obj/compat.o"

# The AGC modules RADV's winsys submits through, named for the import tables.
stub() {
    cc -std=c11 -O2 -fPIC -c "$vk/vendor/ps5/sdk/stubs/$2" -o "$obj/$1_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "$1.prx" -o "$out/stubs/$1.so" "$obj/$1_stub.o"
}
stub libSceAgc agc_canary_link_stub.c
stub libSceAgcDriver agc_driver_canary_link_stub.c

# The link PS5_Vulkan's RADV titles use: RADV whole, libc++, the platform
# layer's heap and threads, and its stand-ins for libc functions titles lack.
# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk" "$archive" || exit 2
# Mesa names every Vulkan entry point through a weak reference that must read
# as NULL, and the title converter takes every undefined dynamic symbol for an
# SDK import: --no-dynamic-linker keeps the weak ones out of the dynamic table
# on LLD 18, -z nodynamic-undefined-weak on newer LLD.
weak=(--no-dynamic-linker)
"$sdk/bin/ld.lld" -z nodynamic-undefined-weak --version 2>&1 | grep -q "unknown -z" ||
    weak+=(-z nodynamic-undefined-weak)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${weak[@]}" "${radv_link_flags[@]}" \
    --version-script "$vk/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$out/app-pie.elf" \
    "$obj/app_crt.o" "${objects[@]}" "$obj/libwscompat.a" \
    "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
# An import that only a module titles get nothing from provides is a call
# through NULL at run time: refuse to ship one.
python3 "$here/check-imports.py" "$sdk" "$out/app-pie.elf" "$out/stubs"
"$tool" link --in "$out/app-pie.elf" --out "$out/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$out/stubs/libSceAgc.so" --stub "$out/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > /dev/null

app="$out/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > /dev/null
cp "$param" "$here/sce_sys/icon0.png" "$app/sce_sys/"
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
cp "$payload" "$app/tailstation.elf"
# What every copy must carry: the license, the notices, and where the source is.
cp "$root/LICENSE" "$root/NOTICE.md" "$app/"
[ -f "$out/THIRD-PARTY-LICENSES.txt" ] && cp "$out/THIRD-PARTY-LICENSES.txt" "$app/"
commit() { git -C "$1" rev-parse HEAD 2> /dev/null || echo "$2"; }
cat > "$app/SOURCE.txt" << SRC
TailStation $version is GPL-3.0-or-later as a whole (LICENSE). NOTICE.md
lists the third-party code in it. Its source is these repositories at these
commits:

https://github.com/ThatRetiredDude/TailStation  ${WS_SOURCE_COMMIT:-$(commit "$root" "$version")}
https://github.com/mihawk-99/PS5_Vulkan  $(commit "$vk" "$(sed -n 's/^vulkan_rev=//p' "$here/setup.sh")")
https://github.com/mihawk-99/PS5_PayloadSDK  $(sed -n 's/^sdk_revision=//p' "$vk/tools/setup-native-dependencies.sh")
https://github.com/mihawk-99/PS5_Mesa  $(sed -n 's/^mesa_revision=//p' "$vk/tools/build-radv.sh")
SRC
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
echo "build: $app (TailStation $version, eboot.bin $(stat -c %s "$app/eboot.bin") bytes)"
