# Standalone PS5 WireGuard app: plan

Date: 2026-10-26, updated the same day with the store section. Desk study;
nothing run on the console. (Jared's plan, kept here so the code can point
at it.)

## What it is

A new homebrew program, separate from TokenStation5, that turns the PS5 into
a WireGuard server and (optionally) an exit node: a phone or computer away
from home connects to the PS5 and its traffic goes out through your home
connection.

## How "drop into the background like Spotify" can work

Spotify on PS5 uses a system media feature that homebrew can't register
for. The way homebrew gets the same effect is two parts:

1. **The app** (a normal title, like TokenStation5). It's the screen you
   see: turn the server on, add a phone (QR code on the TV), add a computer
   (join page), see who's connected, read the About page.
2. **The background part** (a payload). When you press "Run in background",
   the app hands a small built-in program to the jailbreak's ELF loader
   (127.0.0.1:9021, already used by TokenStation5's memprobe payload) and
   closes itself. That background part keeps running on the home screen
   and while you play games. Opening the app again finds it over localhost
   and shows its status.

Why this is OK for WireGuard but wasn't for the AI engine: background
programs get no GPU and limited memory (~1.5 GB safe). WireGuard needs a
few MB and a little CPU.

## Keep-alive (on by default)

- Every device config the app hands out gets `PersistentKeepalive = 25`, so
  the phone keeps the tunnel and the home router's mapping open even when
  idle.
- The background part also watches itself: if the tunnel loop stalls it
  restarts it, and the app can restart the background part if it has died.

## Exit node

No raw sockets or kernel routing needed. The tunnel's connections end
inside the program (lwIP, same as TokenStation5 PR #80) and the program
reopens them as normal PS5 connections to the LAN or internet (the
wireproxy / Tailscale userspace approach). Works: web, apps, DNS. Doesn't:
ping, and anything that isn't TCP or UDP. Device configs set
`AllowedIPs = 0.0.0.0/0` for exit-node mode, or only the home network
(e.g. 192.168.1.0/24) for "reach my home stuff" mode.

## Reused from TokenStation5 PR #80

lwIP 2.2.1 + wireguard-lwip, the UDP-socket glue (ps5/wg/), the device list,
phone QR and computer join page. These are copied into this repo
(GPL-3.0, like TokenStation5's ps5/).

## What works and what doesn't (for the About page)

- Works: home screen, during games (background part), phone and computer,
  LAN access, exit node for web/apps.
- Doesn't: rest mode (expected to stop; to check), after a PS5 restart
  until you re-jailbreak (and re-open the app), ping through the exit node.
- Speed: not measured yet. Likely above a typical home upload, which is the
  real limit away from home.
- Running it next to TokenStation5's own WireGuard needs a second UDP port
  and a second router forward. Simpler: use one or the other.

## Publishing on the Prospero Store

WireStationVPN ships on the Prospero Store (darkxex/ps5-homebrew-catalog),
the same way as TokenStation5. Copy TokenStation5's docs/STORE.md and
.github/workflows/release.yml and change the IDs.

### Name and title ID: avoid duplicates

What went wrong before: TokenStation5's first title ID, PPSA05005, belonged
to a real PS Store game (A Monster's Expedition), so the console showed our
app under that game's name. That's why it moved to PPSA99555.

- **Use the homebrew range PPSA99xxx**, never an ID a real game could have.
- **Check the catalog before picking.** Look for the ID in apps/ and also
  for apps/<ID>-*.json. As of 2026-10-26, **PPSA99556 is free**
  (TokenStation5 is PPSA99555). Re-check before reserving.
- **Reserve it early.** Open a reservation PR for apps/PPSA99556.json from
  the ThatRetiredDude account (the reservation belongs to whoever opens the
  PR; max 5 per account; touch it every 180 days).
- **The display name must be unique in the catalog, ignoring case.**
  "WireStationVPN" is free as of 2026-10-26 (the nearest are TokenStation5,
  SymphonyStation5 and SwanStationPS5).
- **Keep one ID forever.** The console holds one app per title ID. Two apps
  that share an ID replace each other and share save data. titleId,
  conceptId and contentId must never change between releases, or updates
  stop being updates.
- param.json: titleId PPSA99556, conceptId 99556, contentId
  UP9000-PPSA99556_00-WIRESTATIONVPN00 (36 characters, same pattern as
  TokenStation5's), titleName "WireStationVPN".

### Store lessons from TokenStation5

- **Store updates replace the whole app folder.** Keep user data (device
  list, server keys, settings) in /download0, not in the app folder, from
  the first release. Deleting the app wipes /download0 too, so tell people
  to save their phone configs. Here that matters more than for
  TokenStation5: losing the server key means re-adding every device.
- **Never set downloadDataSize above 256.** 65536 made the PS5 refuse to
  start the app (CE-107694-7). WireStationVPN only needs a few KB.
- **A store restore loses the jailbreak-only bits.** After a PS5 restart
  the app won't open until the jailbreak is re-run, and the background part
  won't be running either. Say so on the About page.
- **Versions:** bump contentVersion in param.json (PlayStation format, e.g.
  01.000.000) and tag the release with the same string. Never replace a
  listed release's ZIP; its SHA-256 would stop matching.
- **The ZIP must have the PPSA99556/ folder at its root.** release.yml
  already does this.
- **Catalog PRs come from the ThatRetiredDude account** and change only one
  file. Owner PRs that change only release fields, description or license
  get auto-merged.
- **The description is capped at 200 characters.** Plain words: what it
  does and what you need (jailbroken PS5, a router port forward).
- **The author field is "ThatRetiredDude"**, never a real name. Build the
  record from the release workflow, which already uses the pseudonym.
- **License field:** the repo is GPL-3.0 at the root, so GitHub detects it
  and the record says GPL-3.0.
- **Icon:** 512x512 PNG from Jared's own logo, padded on black. Claude
  doesn't make the logo.
- **UDP port:** the store build defaults to a port a title can bind (9030
  works; 51820 got EACCES as a title). If the background part can bind
  51820, use that, since people expect it. Don't default to the same port
  as TokenStation5's tunnel.

### How step 1 follows this

- **Port.** The server tries 51820 first, then 9031 (not 9030, which is
  TokenStation5's tunnel), unless `server.conf` names one. It writes the
  port it settled on to `server.conf` so device configs stay valid.
- **Where the data lives.** /download0 is the title's own mount; a payload
  started by the ELF loader runs in another process and is not expected to
  see it under that name. So in step 2 the app owns the keys, devices and
  settings in /download0 and hands the background part what it needs when
  it starts it (files copied to /data/wirestation, or arguments). Step 1,
  started by hand with no app, keeps its own files in /data/wirestation;
  those are for testing and won't carry over to the store app.
- **Name.** User-facing text says WireStationVPN.

## Unknowns that need the console (MASTER CHAT owns it)

1. Does a payload's UDP socket on 9030 (or another port) receive from
   outside? (Titles get EACCES on 51820; payloads can raise their
   privileges and may not.)
2. Can a title send to 127.0.0.1:9021 and close without killing the
   payload it started?
3. Does the background part survive starting a game, and rest mode?
4. Throughput through the exit node.

## Build order

0. Reserve PPSA99556 / WireStationVPN in the catalog (Jared opens the PR).
1. Payload-only WireGuard server (no exit node), started by hand over 9021:
   answers unknowns 1 and 3. **This is `server/`; test it with
   [CONSOLE-TEST.md](CONSOLE-TEST.md).**
2. App front end that launches it and talks to it: unknown 2.
3. Exit node proxy, keep-alive, About page.
4. Speed test, then the first store release.
