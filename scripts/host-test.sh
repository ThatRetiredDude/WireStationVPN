#!/usr/bin/env bash
# Builds the server for Linux and checks it against wireguard-go (the
# WireGuard project's own implementation, in user space): a first start
# makes keys and device1.conf, a device connects, pings 10.66.0.1 and
# fetches the status page through the tunnel; a second device added by name
# connects too; a second copy of the server leaves. Needs cc and Go 1.22+.
#
#   scripts/host-test.sh [--hold 3m]    (--hold: also keep a tunnel up that long)
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
hold=0s
[ "${1:-}" = --hold ] && hold=${2:?}
make -s -C "$root/server" host
(cd "$root/tools/wgtest" && go build -o "$root/build/host/wgtest" .)
bin="$root/build/host"
data=$(mktemp -d)
port=$((20000 + RANDOM % 20000))
printf 'port = %d\nstatus_port = %d\nendpoint = 127.0.0.1\n' "$port" $((port + 1)) > "$data/server.conf"
pid=
stop() { [ -n "$pid" ] && kill "$pid" 2>/dev/null; wait 2>/dev/null || true; }
trap 'stop; rm -rf "$data"' EXIT
start() {
    "$bin/wirestation-server" "$data" > "$data/out.txt" 2>&1 &
    pid=$!
    for _ in $(seq 50); do curl -fs "http://127.0.0.1:$((port + 1))/" > /dev/null && return; sleep 0.1; done
    echo "host-test: the server didn't start"; cat "$data/out.txt"; exit 1
}

start
"$bin/wgtest" -n 20 "$data/device1.conf"
if "$bin/wirestation-server" "$data" > /dev/null 2>&1; then echo "host-test: a second copy kept running"; exit 1; fi
stop
echo laptop >> "$data/peers.conf"
start
grep -q "^laptop " "$data/peers.conf"
"$bin/wgtest" -n 5 -hold "$hold" "$data/laptop.conf"
"$bin/wgtest" -n 5 "$data/device1.conf" > /dev/null
curl -fs "http://127.0.0.1:$((port + 1))/stop" > /dev/null
wait "$pid"
pid=
grep -q "stopping" "$data/log.txt"
echo "host-test: ok"
