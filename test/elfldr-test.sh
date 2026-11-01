#!/usr/bin/env bash
# Starts the program the way the PS5's ELF loader does (test/elfldr-sim.c),
# on Linux, and checks that it comes up: the status page answers, and /stop
# ends it. Needs to write /data/wirestation (run as root, or in CI).
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cc -O1 -static -Wl,-Ttext-segment=0x40000000 -o "$tmp/elfldr-sim" "$root/test/elfldr-sim.c"
(cd "$root" && CGO_ENABLED=0 go build -tags ws_loadertest -ldflags "-E ws_entry" -o "$tmp/wirestation" ./node)

"$tmp/elfldr-sim" "$tmp/wirestation" >"$tmp/out.txt" 2>&1 &
pid=$!
for _ in $(seq 30); do
    if curl -sf -m 2 http://127.0.0.1:9039/ >"$tmp/page.html"; then break; fi
    sleep 1
done
curl -sf -m 5 http://127.0.0.1:9039/stop >/dev/null || true
status=0
wait "$pid" || status=$?
cat "$tmp/out.txt"
grep -q "<h1>WireStationVPN</h1>" "$tmp/page.html" || { echo "FAIL: the status page didn't answer"; exit 1; }
[ "$status" = 0 ] || { echo "FAIL: exit status $status"; exit 1; }
echo "OK: started like a PS5 payload, served the status page, stopped"
