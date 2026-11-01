#!/usr/bin/env bash
# Writes build/ps5/THIRD-PARTY-LICENSES.txt: the license of every Go module
# compiled into the PS5 payload, from Go's module cache. The binaries are
# shipped with it.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build/ps5/THIRD-PARTY-LICENSES.txt"
mkdir -p "$(dirname "$out")"
cd "$root"
{
    echo "Third-party code in the TailStation payload, with each license."
    echo
    GOOS=freebsd GOARCH=amd64 CGO_ENABLED=0 go list -deps -f '{{with .Module}}{{if not .Main}}{{.Path}} {{.Version}} {{.Dir}}{{end}}{{end}}' ./node |
        sort -u | while read -r path ver dir; do
            echo "================================================================"
            echo "$path $ver"
            echo "================================================================"
            found=
            for f in "$dir"/LICENSE* "$dir"/LICENCE* "$dir"/COPYING* "$dir"/NOTICE*; do
                [ -f "$f" ] || continue
                cat "$f"
                echo
                found=1
            done
            [ -n "$found" ] || echo "(no license file in the module; see https://pkg.go.dev/$path?tab=licenses)"
            echo
        done
    echo "================================================================"
    echo "The Go standard library ($(go env GOVERSION))"
    echo "================================================================"
    cat "$(go env GOROOT)/LICENSE"
    echo
    echo "################################################################"
    echo "The C++ build (tailstation-lite.elf)"
    echo "################################################################"
    echo
    lic() {  # name, then the files with its license
        echo "================================================================"
        echo "$1"
        echo "================================================================"
        shift
        for f in "$@"; do cat "$f"; echo; done
    }
    lic "lwIP 2.2.1 (third_party/lwip, with TailStation's two changes)" third_party/lwip/COPYING
    lic "wireguard-lwip (third_party/wireguard-lwip)" third_party/wireguard-lwip/LICENSE \
        third_party/wireguard-lwip/src/crypto/refc/x25519-license.txt
    lic "QR Code generator (third_party/qrcodegen), MIT, Copyright (c) Project Nayuki" <(sed -n '1,/\*\//p' third_party/qrcodegen/qrcodegen.hpp)
    mbed=$(ls -d .deps/lite/mbedtls-* 2>/dev/null | head -1)
    if [ -n "$mbed" ]; then lic "Mbed TLS ($(basename "$mbed"))" "$mbed/LICENSE"; fi
    echo "================================================================"
    echo "The Mozilla CA certificate bundle, as certifi ships it"
    echo "================================================================"
    echo "Mozilla Public License 2.0, unmodified: https://www.mozilla.org/MPL/2.0/"
    echo "Source: https://github.com/certifi/python-certifi"
    echo
    echo "================================================================"
    echo "ps5-payload-dev/sdk v0.45 (its C library, libc++ and start-up code are linked in)"
    echo "================================================================"
    echo "GPL-3.0-or-later for the SDK's own code; libc++ and libc++abi under the Apache-2.0"
    echo "License with LLVM Exceptions; the C library from FreeBSD under BSD licenses."
    echo "Source: https://github.com/ps5-payload-dev/sdk"
} > "$out"
echo "wrote $out ($(grep -c '^====' "$out") separators)"
