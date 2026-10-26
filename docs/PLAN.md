# Standalone PS5 WireGuard app: plan

Date: 2026-10-26. Desk study; nothing run on the console. (Jared's plan, kept
here as written, so the code can point at it.)

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

## Unknowns that need the console

1. Does a payload's UDP socket on 9030 (or another port) receive from
   outside? (Titles get EACCES on 51820; payloads can raise their
   privileges and may not.)
2. Can a title send to 127.0.0.1:9021 and close without killing the
   payload it started?
3. Does the background part survive starting a game, and rest mode?
4. Throughput through the exit node.

## Build order

1. Payload-only WireGuard server (no exit node), started by hand over 9021:
   answers unknowns 1 and 3. **This is `server/`; test it with
   [CONSOLE-TEST.md](CONSOLE-TEST.md).**
2. App front end that launches it and talks to it: unknown 2.
3. Exit node proxy, keep-alive, About page.
4. Speed test, then a release.
