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
} > "$out"
echo "wrote $out ($(grep -c '^====' "$out") separators)"
