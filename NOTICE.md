# Notices

WireStationVPN is GPL-3.0-or-later ([LICENSE](LICENSE)). It is built from
third-party open-source code under its own licenses, which the GPL allows.
The main parts:

| Code | License | What it does here |
| --- | --- | --- |
| [Tailscale](https://github.com/tailscale/tailscale) (`tailscale.com`) | BSD-3-Clause | The whole Tailscale client: sign-in, WireGuard, NAT traversal, the user-space network stack that makes the exit node |
| [gVisor](https://github.com/google/gvisor) netstack | Apache-2.0 | The TCP/IP stack inside Tailscale's user-space mode |
| [wireguard-go](https://git.zx2c4.com/wireguard-go) | MIT | WireGuard, inside Tailscale |
| [go-qrcode](https://github.com/skip2/go-qrcode) | MIT | The sign-in QR code on the status page |
| The Go standard library | BSD-3-Clause | |
| The [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) | GPL-3.0-or-later | Start-up code and libc for `wirestation-sdk.elf` |

Every module compiled into the payload is in `go.sum`. `scripts/licenses.sh`
writes `THIRD-PARTY-LICENSES.txt` with each one's license text, and the
build ships it next to the `.elf` files.

WireStationVPN isn't affiliated with or endorsed by Tailscale Inc.
"Tailscale" is their trademark, used here only to say what the program
works with.
