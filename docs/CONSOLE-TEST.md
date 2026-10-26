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

If TokenStation5's own WireGuard is on, turn it off first: it also uses UDP
9030. (Or give this one another port, step 1.)

## 1. Start it

```sh
python3 scripts/send-elf.py PS5 server/wirestation-server.elf
```

Expected: a notification on the TV, "WireStation: on, UDP port 9030". Then:

```sh
curl http://PS5:9039/
```

shows the status: the line `UDP port 9030: ...` says how the port was bound
("bound as a payload", or "bound with system privileges (refused as a
payload)"). If the TV says "can't use UDP port 9030", note the errno.

The first start makes everything it needs in `/data/wirestation/`:
`server.key`, `peers.conf`, `device1.conf` and `log.txt`. To try another
port, write `/data/wirestation/server.conf` (over FTP) with `port = 9031`,
stop it (`curl http://PS5:9039/stop`) and send it again. Worth one try with
`port = 51820`, the port titles are refused, to see whether a payload is too.

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
"WireStation: device1 connected".

If this fails, `log.txt` and the status page's `packets in / out / sends
failed` line say how far it got: no packets in means nothing reached the
socket.

## 3. Unknown 1: from outside

1. On the router, forward UDP 9030 to the PS5.
2. In the phone's tunnel, set Endpoint to the home's public address
   (`curl ifconfig.me` from home) and port 9030.
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

The TV says "WireStation: off". Sending it again while one runs does
nothing: the second copy says "already running" and leaves.

## Results

| Check | Result | Notes (errno, status line, log) |
| --- | --- | --- |
| Starts, notification shown | | |
| UDP 9030 bound as | | |
| UDP 51820 bound as | | |
| Device connects at home | | |
| 1: connects from mobile data | | |
| 3a: home screen, 10 min | | |
| 3b: during a game | | |
| 3c: rest mode (Stay Connected on/off) | | |
| Memory (ts5-memprobe, optional) | | |

Send back the filled table and `log.txt`.
