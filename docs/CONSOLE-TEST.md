# Testing on the PS5

WireStationVPN has only run on Linux so far. These steps find out whether it
runs on the PS5, and whether a phone can use the PS5 as an exit node. Write
down what you see in the table at the end.

You need:

- the PS5 with the jailbreak running (its ELF loader on port 9021)
- a computer on the same home network
- a phone with the **Tailscale** app (free, from the App Store or Google
  Play), and a Tailscale account (the free plan is enough)

Below, `PS5_IP` is the PS5's address on your home network, for example
192.168.1.50. (PS5: Settings > Network > Connection Status > View Connection
Status, "IPv4 Address".)

## 1. Download the files

On GitHub: the repo's **Actions** tab, the latest green **build** run, and
**wirestation-ps5** under Artifacts. Unzip it. You get `wirestation.elf`.

## 2. Start it

Send `wirestation.elf` to the PS5 the same way you send other payloads (port
9021). For example, from the repo folder:

```sh
python3 scripts/send-elf.py PS5_IP wirestation.elf
```

Wait 10 seconds, then open `http://PS5_IP:9039/` in the computer's browser.

- **The page opens and shows a QR code:** it runs. Go to step 3.
- **The page doesn't open:** wait 30 seconds more and try again.
- **Still nothing:** check whether `/data/wirestation/log.txt` exists on the
  PS5 (over FTP, port 2121), and send it to me if it does. If the folder
  doesn't exist, the program never started: tell me what the TV showed, if
  anything.

## 3. Sign the PS5 in

Scan the QR code on that page with your phone (or click the link on the
computer) and sign in with your Tailscale account. Within a few seconds the
page changes to "This PS5", with its name (ps5) and a Tailscale address
(100.x.y.z).

## 4. Approve the exit node

Open the [Tailscale admin page](https://login.tailscale.com/admin/machines).
Find **ps5**, click **⋯** on its row, then **Edit route settings**. Tick
**Use as exit node**, and the home network route (192.168.x.0/24) if it's
listed. Save. The PS5's page now says "Exit node: ready".

## 5. Use it from the phone, away from home

1. On the phone, turn **Wi-Fi off** (mobile data only).
2. In the Tailscale app, sign in with the same account, then **Exit node** →
   **ps5**.
3. Open a website, for example https://ifconfig.me. It should show your
   **home** internet address, not the phone carrier's.
4. Open `http://PS5_IP:9039/` on the phone. If it opens, the home network
   route works too.

## 6. Does it keep running?

Leave the phone on the exit node.

1. **Home screen**, 10 minutes: does the website still load?
2. **A game**: start one, play 10 minutes, reload a website on the phone now
   and then. Then quit the game.
3. **Rest mode**: put the PS5 in rest mode for 5 minutes, try a website on
   the phone, then wake the PS5.

After each, open `http://PS5_IP:9039/` (does it still answer?). Get
`/data/wirestation/log.txt` over FTP at the end: it has a line a minute, so
gaps show when the PS5 paused it.

## 7. Stop it

Open `http://PS5_IP:9039/stop`. Sending the file again while it runs does
nothing harmful: the second copy sees the first and leaves. Sending it after
a PS5 restart starts it again, already signed in.

## Results

| Check | Result | Notes |
| --- | --- | --- |
| The program started (yes / no, what the TV showed) | | |
| Status page opens | | |
| Sign-in works, page shows "This PS5" | | |
| Exit node approved, page says "ready" | | |
| Phone on mobile data: website loads | | |
| ifconfig.me shows the home address | | |
| Home network page opens on the phone | | |
| Home screen, 10 min | | |
| During a game | | |
| Rest mode | | |

Send back the filled table, plus `log.txt` (and `tailscale.log` if something
failed) from `/data/wirestation/`.
