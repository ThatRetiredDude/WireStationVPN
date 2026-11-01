# WireStationVPN

Puts a jailbroken PS5 on your [Tailscale](https://tailscale.com) network.
Your phone and computer can then use the PS5 as an **exit node**: away from
home, their browsing goes out through your home internet. They can also
reach things on your home network. Nobody has to forward a port on the
router, so it works behind almost any home connection.

It runs Tailscale's own open-source code (`tsnet`, in user space) as a PS5
payload. The plan is [docs/PLAN.md](docs/PLAN.md).

## Status

The payload, started by hand through the jailbreak's ELF loader. It's
tested on Linux against Tailscale's test server: sign-in, exit node,
TCP and UDP through it. **It hasn't run on a PS5 yet.**
[docs/CONSOLE-TEST.md](docs/CONSOLE-TEST.md) has the steps. The PS5 app with
a screen and a "run in background" button comes after.

## Using it

1. Send `wirestation.elf` to the PS5's ELF loader (port 9021).
2. On a computer or phone at home, open `http://PS5_IP:9039/`. Scan the QR
   code or open the link, and sign in with your Tailscale account.
3. In the [Tailscale admin page](https://login.tailscale.com/admin/machines),
   find **ps5**, open its menu (⋯), choose **Edit route settings**, and tick
   **Use as exit node** (and the home network route, if you want it).
4. On your phone or computer, in the Tailscale app, choose **Exit node** →
   **ps5**.

The PS5 stays signed in: after a restart (and the jailbreak), send the
payload again and it reconnects by itself.

## Settings

`/data/wirestation/settings.conf`, all optional:

| Setting | Default | |
| --- | --- | --- |
| `hostname` | ps5 | The PS5's name in your tailnet |
| `exit_node` | on | Offer the PS5 as an exit node |
| `home_network` | on | Offer your home network (the PS5's LAN) as a route |
| `auth_key` | | A Tailscale auth key, to join without the sign-in link |
| `control_url` | Tailscale's | Another coordination server, such as Headscale |
| `port` | 0 (any) | Tailscale's UDP port |
| `status_port` | 9039 | The status page on your home network |

The status page also stops the program (`/stop`) and signs the PS5 out.
`log.txt` and `tailscale.log` in the same folder say what it did.

## Layout

| Path | What |
| --- | --- |
| `node/` | The program (Go): Tailscale, the status page, settings, and its test |
| `payload/` | The C entry point for the build linked with the PS5 payload SDK |
| `scripts/` | `build-ps5.sh` (both PS5 builds), `send-elf.py` (sends a payload to port 9021) |
| `docs/` | The plan and the console test |
| `assets/` | The logo, and `icon0.png` (512x512) for the app and the store |

## Build and test

```sh
go test ./node                                              # Linux, against Tailscale's test server
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk scripts/build-ps5.sh   # build/ps5/*.elf
```

GitHub Actions does both on every push; the `.elf` files are attached to
each run.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)). Tailscale's code is BSD-3-Clause;
it and the other third-party code are listed in [NOTICE.md](NOTICE.md).

WireStationVPN isn't affiliated with Tailscale Inc. The WireStationVPN name
and logo (`assets/`) are not covered by the GPL; no trademark rights are
granted.
