#!/usr/bin/env bash
# Builds the PS5 payload two ways, as nobody knows yet which one the PS5
# runs:
#
#   build/ps5/wirestation.elf       plain Go, for FreeBSD (the PS5's kernel
#                                   is FreeBSD-based); needs only Go
#   build/ps5/wirestation-sdk.elf   the same program as a C archive, linked
#                                   with the PS5 payload SDK's start-up code
#                                   and libc; needs PS5_PAYLOAD_SDK
#
#   PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk scripts/build-ps5.sh
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build/ps5"
mkdir -p "$out"
version=$(git -C "$root" describe --always --dirty 2>/dev/null || echo dev)
ldflags="-s -w -X main.version=$version"
cd "$root"

CGO_ENABLED=0 GOOS=freebsd GOARCH=amd64 go build -trimpath -ldflags "$ldflags" -o "$out/wirestation.elf" ./node
echo "built $out/wirestation.elf"

if [ -z "${PS5_PAYLOAD_SDK:-}" ]; then
    echo "PS5_PAYLOAD_SDK isn't set: skipping wirestation-sdk.elf"
    exit 0
fi
cc="$PS5_PAYLOAD_SDK/bin/prospero-clang"
CGO_ENABLED=1 GOOS=freebsd GOARCH=amd64 CC="$cc" \
    go build -trimpath -tags ws_payload -buildmode=c-archive -ldflags "$ldflags" -o "$out/libwirestation.a" ./node
"$cc" -O2 -Wall -o "$out/wirestation-sdk.elf" payload/main.c "$out/libwirestation.a" -lpthread
echo "built $out/wirestation-sdk.elf"
