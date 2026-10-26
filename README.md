# WireStation

Turns a jailbroken PS5 into a WireGuard server, and later an exit node: a
phone or computer away from home connects to the PS5, and (in exit-node
mode) its traffic goes out through the home connection. Separate from
TokenStation5, from which it takes the WireGuard tunnel (lwIP and
wireguard-lwip, TokenStation5 PR #80). The plan is [docs/PLAN.md](docs/PLAN.md).

## Status

Build step 1: **the server as a payload**, started by hand through the
jailbreak's ELF loader. Devices connect and reach the PS5 itself (ping and
a status page at `http://10.66.0.1/` in the tunnel). No exit node and no app
yet. Tested on Linux against wireguard-go; not yet run on the console
([docs/CONSOLE-TEST.md](docs/CONSOLE-TEST.md) has the steps).

## Layout

| Path | What |
| --- | --- |
| `server/` | The WireGuard server: `main.c`, and the lwIP glue from TokenStation5 (`wg_udp.c`, `lwipopts.h`, `arch/cc.h`) |
| `third_party/` | lwIP 2.2.1 and wireguard-lwip, vendored by `scripts/vendor-wireguard.sh` |
| `tools/wgtest/` | A test device: wireguard-go in user space, connects with a `NAME.conf` and checks the tunnel |
| `scripts/` | `host-test.sh` (the Linux test), `send-elf.py` (sends a payload to port 9021) |
| `docs/` | The plan and the console test |

## Build and run

```sh
make -C server PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk    # server/wirestation-server.elf
python3 scripts/send-elf.py PS5_IP server/wirestation-server.elf
curl http://PS5_IP:9039/                               # status; /stop stops it
```

On its first start it makes its key and a first device in
`/data/wirestation/`: import `device1.conf` into the WireGuard app. To add
a device, add a line with its name to `peers.conf` and start the server
again; it writes `NAME.conf`.

| Setting (`server.conf`) | Default | |
| --- | --- | --- |
| `port` | 9030 | UDP, WireGuard. Forward it on the router for use away from home |
| `address` | 10.66.0.1 | The PS5 in the tunnel, a /24 (TokenStation5 uses 10.55.0.x) |
| `status_port` | 9039 | TCP, the status page on the LAN |
| `endpoint` | the LAN address | What `NAME.conf` dials: set it to the home's public address or name |

Linux test (needs cc and Go): `scripts/host-test.sh`.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)): it links the PS5 payload SDK, and
the code it takes from TokenStation5 is GPL-3.0 there. Third-party notices
are in [NOTICE.md](NOTICE.md).
