# WireStationVPN: plan

Date: 2026-11-01. Desk study; nothing run on the console yet.

## What it is

A homebrew program, separate from TokenStation5, that puts the PS5 on the
owner's Tailscale network (tailnet) and offers it as an exit node: a phone
or computer away from home picks the PS5 in the Tailscale app, and its
traffic goes out through the home connection. It also offers the home
network as a route, so those devices reach things at home.

It started as a plain WireGuard server (PR #1). On 2026-11-01 it became a
Tailscale program, because Tailscale handles the part that differs on
every home network: getting in. Tailscale connects devices directly
through most routers without a port forward (NAT traversal), and through
Tailscale's relays when it can't. A plain WireGuard server needs a router
port forward and a public address, which many homes don't have.

## How it runs Tailscale

Tailscale's own Go code, not a rewrite: `tsnet`, its library form, which
runs entirely in user space (its own TCP/IP stack, gVisor's netstack; no
network interface, no root, no kernel routing). The program sets
Tailscale's network stack to forward what devices send through the PS5 to
the internet and the LAN, TCP and UDP, the way `tailscaled` does in its
user-space mode. Connections to the PS5's own Tailscale address are
refused, so the console's own services (ELF loader, FTP) stay off the
tailnet.

What the user does: sign in once with the link or QR code on the status
page (or put an auth key in settings.conf), then approve the exit node and
the home route once in Tailscale's admin page. Devices use the normal
Tailscale app.

**The big unknown is Go on the PS5.** No Go program is known to have run as
a PS5 payload. `wirestation.elf` is plain Go for FreeBSD (the PS5 kernel is
FreeBSD-based); Go talks to the kernel directly, as the payload SDK's libc
does. The ELF loader starts a payload like a C function call, not like a
new program, which crashes Go's start-up code, so the ELF starts at a small
entry (`node/entry_freebsd_amd64.s`) that sets up what Go expects first.
CI checks this on Linux with a copy of the loader's start-up
(`test/elfldr-test.sh`). Still unknown: whether the payload sandbox allows
every system call Go uses, and whether the address Go's ELF loads at
(0x400000) is free in the payload process.

A second build linked with the payload SDK (Go as a C archive) was tried
and dropped: the SDK's thread-local storage model can't link Go's runtime.

If Go doesn't run, the fallback is TokenStation5's own Tailscale client
(C++, `ps5/ts/`, already working on the PS5 inside that app), moved into a
payload, with exit node and route offering added: it has neither today.

## How "drop into the background like Spotify" can work

Spotify on PS5 uses a system media feature that homebrew can't register
for. The way homebrew gets the same effect is two parts:

1. **The app** (a normal title, like TokenStation5). It's the screen you
   see: turn it on, sign the PS5 in to Tailscale (QR code on the TV), see
   which devices are online, read the About page.
2. **The background part** (a payload). When you press "Run in background",
   the app hands a small built-in program to the jailbreak's ELF loader
   (127.0.0.1:9021, already used by TokenStation5's memprobe payload) and
   closes itself. That background part keeps running on the home screen
   and while you play games. Opening the app again finds it over localhost
   and shows its status.

Why this is OK here but wasn't for TokenStation5's engine: background
programs get no GPU and limited memory (~1.5 GB safe). Tailscale needs
tens of MB and a little CPU.

## Keep-alive

- Tailscale keeps its own connections alive and reconnects by itself after
  network changes; the PS5 stays signed in across restarts (its keys are in
  `/data/wirestation/tailscale`).
- The background part writes a line a minute to its log, and the app (step
  2) restarts it if it has died.

## Exit node

Tailscale's user-space exit node: connections from devices end inside the
program and are reopened as normal PS5 connections to the internet or LAN.
Works: web, apps, DNS, other TCP and UDP (tested on Linux). Doesn't: ping
and anything that isn't TCP or UDP. DNS through the exit node uses the
PS5's own DNS settings, which needs checking on the console.

## What's reused

Tailscale's client (BSD-3-Clause), from its Go module. From TokenStation5:
the release workflow and store record (step 4), the ELF loader approach.
The logo and icon are in `assets/`.

## What works and what doesn't (for the About page)

- Works: home screen, during games (background part), phones and
  computers with the Tailscale app, behind almost any router (no port
  forward), LAN access, exit node for web and apps.
- Doesn't: rest mode (expected to stop; to check), after a PS5 restart
  until you re-jailbreak (and re-open the app), ping through the exit node.
- Needs a Tailscale account (the free plan is enough).
- Speed: not measured yet. Likely above a typical home upload, which is the
  real limit away from home. When Tailscale can only connect through its
  relays, it's slower.
- It runs next to TokenStation5's own WireGuard without clashing.

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

- **Store updates replace the whole app folder.** Keep user data
  (settings) in /download0, not in the app folder, from the first release.
  Tailscale's state stays in /data/wirestation/tailscale, outside the app,
  so neither an update nor deleting the app signs the PS5 out.
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
- **Icon:** 512x512 PNG from the project's own logo, padded on black
  (`assets/icon0.png`).
- **UDP port:** Tailscale picks its own port and needs no forward, so the
  9030/51820 question from the WireGuard version is gone.

### How step 1 follows this

- **Where the data lives.** /download0 is the title's own mount; a payload
  started by the ELF loader runs in another process and is not expected to
  see it under that name. So in step 2 the app owns the settings in
  /download0 and hands the background part what it needs when it starts it.
  Tailscale's own state (the PS5's keys and sign-in) lives in
  /data/wirestation/tailscale, so a store update of the app never signs the
  PS5 out.
- **Name.** User-facing text says WireStationVPN.

## Unknowns that need the console (MASTER CHAT owns it)

1. Does a Go program run as a payload (`wirestation.elf`)?
2. Does it sign in and connect (outbound HTTPS and UDP from a payload)?
3. Exit node from a phone on mobile data: web, and DNS.
4. Can a title send to 127.0.0.1:9021 and close without killing the payload
   it started?
5. Does the background part survive starting a game, and rest mode?
6. Throughput through the exit node.

## Build order

0. Reserve PPSA99556 / WireStationVPN in the catalog (opened from the
   ThatRetiredDude account).
1. The Tailscale payload, started by hand over 9021: answers unknowns 1, 2,
   3 and 5. **This is `node/`; test it with [CONSOLE-TEST.md](CONSOLE-TEST.md).**
2. App front end that launches it, shows the sign-in QR code on the TV and
   the status: unknown 4.
3. About page, keep-alive from the app, polish.
4. Speed test, then the first store release.
