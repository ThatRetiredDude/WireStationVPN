#!/usr/bin/env bash
# Builds the PS5 payload: build/ps5/wirestation.elf, plain Go for FreeBSD
# (the PS5's kernel is FreeBSD-based). Needs only Go.
#
# The PS5's ELF loader starts a payload like a C function, not like a new
# program, so the ELF's entry is ws_entry (node/entry_freebsd_amd64.s),
# which sets up what Go's start-up code expects and goes on to it.
# test/elfldr-test.sh checks the same thing on Linux.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/build/ps5"
mkdir -p "$out"
version=$(git -C "$root" describe --always --dirty 2>/dev/null || echo dev)
cd "$root"
CGO_ENABLED=0 GOOS=freebsd GOARCH=amd64 go build -trimpath \
    -ldflags "-s -w -E ws_entry -X main.version=$version" -o "$out/wirestation.elf" ./node
echo "built $out/wirestation.elf"
