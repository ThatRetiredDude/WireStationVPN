#!/usr/bin/env bash
# Builds the C++ build of the background part:
#
#   lite/build.sh host    build/host/tailstation-lite   for Linux (the tests)
#   lite/build.sh ps5     build/ps5/tailstation-lite.elf  a PS5 payload, with
#                         ps5-payload-dev's payload SDK (fetched, or
#                         PS5_PAYLOAD_SDK); needs clang-18 and lld-18
#
# Fetches Mbed TLS and the CA bundle first (lite/deps.sh).
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
target=${1:-host}
read -r mbedtls ca_pem < <(bash "$root/lite/deps.sh" | tail -1)
version=$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo dev)
src="$root/lite/src"
lw="$root/third_party/lwip/src"
wgl="$root/third_party/wireguard-lwip/src"
qr="$root/third_party/qrcodegen"

case $target in
    host)
        out="$root/build/host"
        cc() { ${CC:-clang} "$@"; }
        cxx() { ${CXX:-clang++} "$@"; }
        defs=()
        # WS_SANITIZE=1: with AddressSanitizer and UBSan, in build/host-asan.
        if [ -n "${WS_SANITIZE:-}" ]; then
            out="$root/build/host-asan"
            defs=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        fi
        link() { cxx -o "$out/tailstation-lite" "$@" -pthread "${defs[@]}"; }
        ;;
    ps5)
        out="$root/build/ps5"
        sdk="${PS5_PAYLOAD_SDK:-}"
        if [ -z "$sdk" ]; then
            # ps5-payload-dev/sdk's release, pinned and checked.
            sdk="$root/.deps/ps5-payload-sdk-0.45/ps5-payload-sdk"
            if [ ! -e "$sdk/bin/prospero-clang" ]; then
                mkdir -p "$root/.deps/ps5-payload-sdk-0.45"
                zip="$root/.deps/ps5-payload-sdk-0.45/sdk.zip"
                curl -fsSL --retry 3 -o "$zip" https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip
                echo "5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c  $zip" | sha256sum -c --quiet -
                unzip -q -o "$zip" -d "$root/.deps/ps5-payload-sdk-0.45"
                rm -f "$zip"
            fi
        fi
        [ -e "$sdk/bin/prospero-clang" ] || { echo "build: no payload SDK at $sdk" >&2; exit 2; }
        cc() { "$sdk/bin/prospero-clang" "$@"; }
        cxx() { "$sdk/bin/prospero-clang++" "$@"; }
        defs=(-DWS_PS5=1)
        link() { cxx -o "$out/tailstation-lite.elf" "$@"; }
        ;;
    *) echo "usage: lite/build.sh host|ps5" >&2; exit 2 ;;
esac
obj="$out/lite-obj"
mkdir -p "$obj"

# The CA bundle, as a C array.
python3 - "$ca_pem" "$obj/ca_bundle.c" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('#include <stddef.h>\nconst char ws_ca_bundle[] = {')
    f.write(','.join(str(b) for b in data))
    f.write(',0};\nconst size_t ws_ca_bundle_len = %d;\n' % len(data))
PY

mbedflags=(-I "$mbedtls/include" -I "$src" "-DMBEDTLS_USER_CONFIG_FILE=\"ws_mbedtls_config.h\"")
lwflags=(-I "$src/port" -I "$lw/include" -I "$wgl")
cflags=(-O2 -ffunction-sections -fdata-sections "${defs[@]}")
cxxflags=(-std=c++17 -O2 -Wall -ffunction-sections -fdata-sections -fexceptions -frtti "${defs[@]}"
    "-DWS_VERSION=\"$version\"" -I "$src" -I "$qr" "${lwflags[@]}" "${mbedflags[@]}")

objects=()
pids=()
build() {  # src obj compiler flags...
    local s=$1 o=$2
    shift 2
    objects+=("$o")
    if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ "$src/port/lwipopts.h" -nt "$o" ]; then
        "$@" -c "$s" -o "$o" &
        pids+=($!)
    fi
}
for s in "$mbedtls"/library/*.c; do
    build "$s" "$obj/mbedtls_$(basename "$s" .c).o" cc -std=c11 "${cflags[@]}" "${mbedflags[@]}" -I "$mbedtls/library"
done
for s in "$lw"/core/*.c "$lw"/core/ipv4/*.c "$wgl"/*.c "$wgl"/crypto/refc/*.c "$src"/port/glue.c; do
    [ "$s" = "$lw/core/udp.c" ] && continue
    rel=${s#"$root"/}
    # gnu11: glue.c reads the clocks, and lwIP wants ssize_t from the system.
    build "$s" "$obj/${rel//\//_}.o" cc -std=gnu11 "${cflags[@]}" "${lwflags[@]}"
done
for s in "$src"/*.cpp "$src"/ts/*.cpp "$qr"/qrcodegen.cpp; do
    rel=${s#"$root"/}
    build "$s" "$obj/${rel//\//_}.o" cxx "${cxxflags[@]}"
done
build "$obj/ca_bundle.c" "$obj/ca_bundle.o" cc -std=c11 "${cflags[@]}"
for p in "${pids[@]}"; do wait "$p"; done
link "${objects[@]}"
echo "built $(ls "$out"/tailstation-lite*)"
