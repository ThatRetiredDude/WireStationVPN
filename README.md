# TailStation

Puts a jailbroken PS5 on your [Tailscale](https://tailscale.com) network.
Your phone and computer can then use the PS5 as an **exit node**: away from
home, their browsing goes out through your home internet. They can also
reach things on your home network. Nobody has to forward a port on the
router, so it works behind almost any home connection.

Or, with no Tailscale account at all, the PS5 can **run the tailnet itself**
([Headscale](https://github.com/juanfont/headscale) built in): see
[Your own tailnet](#your-own-tailnet-no-account).

It runs Tailscale's own open-source code (`tsnet`, in user space) as a PS5
payload, in the background, with a PS5 app (PPSA99556) to start it, sign in
with a QR code on the TV, and see your devices.

**[The guide](docs/GUIDE.md)** has every step, from installing to port
forwarding and troubleshooting. The app has the same guide on its Guide
page. The plan is [docs/PLAN.md](docs/PLAN.md).

## Status

The background part is tested on Linux against Tailscale's test server:
sign-in, exit node, TCP and UDP through it, the tailnet page and Taildrop.
Its own-tailnet mode is tested too: a phone joins the PS5's Headscale, is
let in on the status page, and browses through the PS5.
The app's screens are drawn and checked on Linux too. **Neither has run on
a PS5 yet.** [docs/CONSOLE-TEST.md](docs/CONSOLE-TEST.md) has the steps.

## Using it

1. Open the **TailStation** app on the PS5 (or send `tailstation.elf` to
   the ELF loader on port 9021 yourself). It starts the background part.
2. Scan the QR code on the TV with your phone and sign in with your
   Tailscale account. (Without the app: open `http://PS5_IP:9039/` at home
   for the same code.)
3. In the [Tailscale admin page](https://login.tailscale.com/admin/machines),
   find **ps5**, open its menu (⋯), choose **Edit route settings**, and tick
   **Use as exit node** (and the home network route, if you want it).
4. On your phone or computer, in the Tailscale app, choose **Exit node** →
   **ps5**.

On your tailnet, `http://ps5/` shows the same page from anywhere, and the
shared folders (USB drives, the Taildrop folder) to browse and download.
To send a file to the PS5, share it to **Tailscale** → **ps5** from your phone,
or run `tailscale file cp FILE ps5:` on a computer. The PS5's Tailscale keys
and `settings.conf` are never served, even from a shared folder.

The PS5 stays signed in: after a restart (and the jailbreak), send the
payload again and it reconnects by itself.

## Your own tailnet (no account)

Switch on **Run my own tailnet** in the app's Settings (or `server = on` in
`settings.conf`). The PS5 then runs Headscale, the open-source Tailscale
server, and joins its own tailnet with its exit node and home network
already approved. Devices still connect to each other directly where they
can, and through Tailscale's public relays where they can't.

1. On your router, forward TCP port **8080** (`server_port`) to the PS5.
2. In the Tailscale app on your phone, use a custom (alternate) server and
   enter the address the app's **Devices** page shows (by default
   `http://YOUR_HOME_IP:8080`; set `server_url` if your home has a name, such
   as a DDNS one). On a computer: `tailscale up --login-server ADDRESS`.
3. The device shows up on the PS5 asking to join. Let it in on the app's
   **Devices** page (Cross), or on the status page.

MagicDNS names end in `server_domain` (`ps5.ps5net`). The server's keys and
database are in `/data/tailstation/headscale/`, never shared on the tailnet.
Switching back to Tailscale's service keeps both sign-ins: each has its own
folder. The PS5 must be running for devices to join or get new keys, and
for now the address is plain `http`: the devices' traffic is still
end-to-end encrypted by Tailscale, but some phones may refuse a non-https
server.

## Settings

`/data/tailstation/settings.conf`, all optional:

| Setting | Default | |
| --- | --- | --- |
| `hostname` | ps5 | The PS5's name in your tailnet |
| `exit_node` | on | Offer the PS5 as an exit node |
| `home_network` | on | Offer your home network (the PS5's LAN) as a route |
| `auth_key` | | A Tailscale auth key, to join without the sign-in link |
| `control_url` | Tailscale's | Another coordination server, such as Headscale |
| `port` | 0 (any) | Tailscale's UDP port |
| `fixed_port` | off | Always use UDP 41641, so it can be forwarded on the router for more direct connections ([guide](docs/GUIDE.md#port-forwarding)) |
| `status_port` | 9039 | The status page on your home network |
| `dns` | 1.1.1.1 | The DNS server, used only when the PS5 names none |
| `tags` | | ACL tags for the PS5, such as `tag:ps5` (comma-separated; the tailnet's ACL must allow them) |
| `tailnet_page` | on | This page and the shared folders at `http://ps5/` from your tailnet |
| `shared_folders` | taildrop, /mnt/usb0, /mnt/usb1, /mnt/ext0, /mnt/ext1 | Folders your tailnet can browse and download from (read-only). Add `/data` to share it |
| `taildrop` | on | Take files sent to the PS5 with the Tailscale app |
| `taildrop_folder` | /data/tailstation/received | Where they go |
| `server` | off | Run the PS5's own tailnet (Headscale) instead of using Tailscale's service |
| `server_port` | 8080 | The port devices reach it on; forward it on the router |
| `server_url` | `http://HOME_IP:8080` | The address devices use; found by asking api.ipify.org when not set |
| `server_domain` | ps5net | The end of MagicDNS names in the PS5's own tailnet |

The status page also has buttons to stop the program and to sign the PS5 out.
`log.txt` and `tailscale.log` in the same folder say what it did.

## Layout

| Path | What |
| --- | --- |
| `node/` | The background part (Go): Tailscale, the status page and `/api/status`, files, settings, the own-tailnet server (`server.go`), and their tests |
| `app/` | The PS5 app (C++): its screens (`src/app.cpp`), starting and watching the background part (`src/main.cpp`), the build; `host/` draws the screens on a PC |
| `scripts/` | `build-ps5.sh` (the PS5 build), `send-elf.py` (sends a payload to port 9021) |
| `test/` | Starts the program the way the PS5's ELF loader does, on Linux |
| `docs/` | The plan and the console test |
| `assets/` | The logo (`logo.png`), the mark alone (`logo-mark.png`, the app's header), and `icon0.png` (512x512) for the app and the store |

## Build and test

```sh
go test ./node                                              # Linux: against Tailscale's test server, and as its own server
test/elfldr-test.sh                                         # started like a PS5 payload
scripts/build-ps5.sh                                        # build/ps5/tailstation.elf
app/host/screens.sh /tmp/screens                            # the app's pages as pictures
app/setup.sh && app/build.sh                                # build/ps5/PPSA99556 (the app)
```

GitHub Actions does all of it on every push and attaches the `.elf`, the
screens and the app folder to each run. Store releases: [docs/STORE.md](docs/STORE.md).

## License

GPL-3.0-or-later ([LICENSE](LICENSE)). Tailscale's code is BSD-3-Clause;
it and the other third-party code are listed in [NOTICE.md](NOTICE.md).

TailStation isn't affiliated with Tailscale Inc. The TailStation name
and logo (`assets/`) are not covered by the GPL; no trademark rights are
granted.
