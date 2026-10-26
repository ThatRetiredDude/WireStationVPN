# Testing the server on the PS5

The server (`server/`, build step 1 of [PLAN.md](PLAN.md)) has only been run
on Linux so far (`scripts/host-test.sh`, against wireguard-go). These steps
answer the plan's unknowns 1 and 3 on the console. Fill in the table at the
end as you go.

You need: the jailbreak running with its ELF loader on port 9021 and FTP on
2121, a computer on the same network, and a phone with the WireGuard app.
Below, `PS5` is the console's address (e.g. 192.168.1.50).

## 0. Build

Get the payload SDK (the [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)
release, or the one TokenStation5's `ps5/setup.sh` fetches) and:

```sh
make -C server PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
# server/wirestation-server.elf
```

The server picks its UDP port on the first start: 51820 (WireGuard's own,
which titles are refused) if it can bind it, else 9031 (TokenStation5's
tunnel has 9030). It writes the one it chose to `server.conf` and keeps it.
Below, `PORT` is that port.

## 1. Start it

```sh
python3 scripts/send-elf.py PS5 server/wirestation-server.elf
```

Expected: a notification on the TV, "WireStationVPN: on, UDP port PORT". Then:

```sh
curl http://PS5:9039/
```

shows the status: the line `UDP port PORT: ...` says how the port was bound
("bound as a payload", or "bound with system privileges (refused as a
payload)"). `log.txt` also has a line for each port it tried and why one
failed. If the TV says "can't use UDP port ...", note the errno.

The first start makes everything it needs in `/data/wirestation/`:
`server.key`, `peers.conf`, `device1.conf` and `log.txt`. To try another
port, change the `port =` line in `/data/wirestation/server.conf` (over FTP),
stop it (`curl http://PS5:9039/stop`), delete `device1.conf` and the
`device1` line of `peers.conf` (its config names the old port) and send it
again.

## 2. At home first

```sh
curl -o device1.conf ftp://PS5:2121/data/wirestation/device1.conf
```

Its `Endpoint` is the PS5's LAN address. Either:

- on the computer: `cd tools/wgtest && go run . ../../device1.conf`
  (wireguard-go in user space, no install, no admin rights), or
- on the phone, on home Wi-Fi: WireGuard app, +, import `device1.conf`
  (or "create from file"), turn it on, open `http://10.66.0.1/` in the
  browser.

Expected: the status page, with `device1 ... connected`. The TV says
"WireStationVPN: device1 connected".

If this fails, `log.txt` and the status page's `packets in / out / sends
failed` line say how far it got: no packets in means nothing reached the
socket.

## 3. Unknown 1: from outside

1. On the router, forward UDP PORT to the PS5.
2. In the phone's tunnel, set Endpoint to the home's public address
   (`curl ifconfig.me` from home) and port PORT.
3. Turn Wi-Fi off on the phone (mobile data only), turn the tunnel on, and
   open `http://10.66.0.1/`.

Expected: the status page, with `device1 connected, from <the carrier's
address>`. Answer for unknown 1: yes if the page loads, and which way the
port was bound (status line).

If it doesn't load: `curl http://PS5:9039/` from home. `packets in` not
growing means the packets don't reach the PS5 (the forward, or the console);
growing with `sends failed` means it can't answer.

## 4. Unknown 3: does it survive?

Keep the phone's tunnel on throughout. With `PersistentKeepalive = 25`,
the status page's `last heard` stays under 30 s while the tunnel is alive.
The server writes an `alive` line to `log.txt` every minute, and a line if
its loop stops for more than 10 s. Each line has the time of day and the
seconds since the start (`+NNNs`): if the time of day jumps further than the
seconds do, the process was frozen in between.

1. **Home screen**, 10 minutes. Expected: 10 `alive` lines.
2. **A game**: start one, play 10 minutes. Meanwhile, reload
   `http://10.66.0.1/` on the phone now and then. Then quit the game.
3. **Rest mode**: note whether Settings > Power Saving > Features Available
   in Rest Mode > Stay Connected to the Internet is on. Put the PS5 in rest
   mode for 5 minutes, try the phone's page during it, then wake the PS5.
4. After each: `curl http://PS5:9039/` (does it answer, what's the uptime)
   and `curl -o log.txt ftp://PS5:2121/data/wirestation/log.txt`.

## 5. Stop it

```sh
curl http://PS5:9039/stop
```

The TV says "WireStationVPN: off". Sending it again while one runs does
nothing: the second copy says "already running" and leaves.

## Results

| Check | Result | Notes (errno, status line, log) |
| --- | --- | --- |
| Starts, notification shown | | |
| UDP 51820: bound as payload / with privileges / refused (errno) | | |
| Port it settled on | | |
| Device connects at home | | |
| 1: connects from mobile data | | |
| 3a: home screen, 10 min | | |
| 3b: during a game | | |
| 3c: rest mode (Stay Connected on/off) | | |
| Memory (ts5-memprobe, optional) | | |

Send back the filled table and `log.txt`.
