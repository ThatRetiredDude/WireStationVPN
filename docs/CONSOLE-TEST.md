# Testing on the PS5

TailStation has only run on Linux so far. These steps find out whether the
app and its background part run on the PS5, and whether a phone can use the
PS5 as an exit node. Write down what you see in the table at the end.

You need:

- the PS5 with the jailbreak running: its ELF loader (port 9021), FTP
  (port 2121) and ShadowMountPlus, as for TokenStation5
- a computer on the same home network
- a phone with the **Tailscale** app (free, from the App Store or Google
  Play), and a Tailscale account (the free plan is enough)

Below, `PS5_IP` is the PS5's address on your home network, for example
192.168.1.50. (PS5: Settings > Network > Connection Status > View Connection
Status, "IPv4 Address".)

## 1. Download and install the app

On GitHub: the repo's **Actions** tab, the latest green **build** run, and
**TailStation-PPSA99556** under Artifacts. Unzip it: you get a folder of
files (`eboot.bin`, `tailstation.elf`, `sce_sys/` and others).

Copy them over FTP into `/data/homebrew/PPSA99556/` on the PS5 (the same way
TokenStation5's folder went to `/data/homebrew/PPSA99555/`), so that
`/data/homebrew/PPSA99556/eboot.bin` exists. ShadowMountPlus then shows
**TailStation**, with the red dragon icon, on the home screen.

## 2. Open it

Open **TailStation** from the home screen.

- **"Starting TailStation", then a QR code:** it works. Go to step 3.
- **"TailStation isn't running" with "Couldn't start it":** the ELF
  loader isn't running. Run the jailbreak's ELF loader, then press Cross.
- **It keeps saying "Starting" for a minute, then gives up:** send me
  `/data/tailstation/log.txt` and `app-log.txt` (over FTP).
- **The app doesn't open, or the TV stays black:** send me
  `/data/tailstation/app-log.txt` if it exists, and say what the TV showed.
  Then try the background part on its own: send
  `/data/homebrew/PPSA99556/tailstation.elf` (or the **tailstation-ps5**
  artifact's) to port 9021 with
  `python3 scripts/send-elf.py PS5_IP tailstation.elf`, wait 10 seconds and
  open `http://PS5_IP:9039/` on the computer. If that shows the QR code, the
  rest of the test works from that page instead of the TV.

## 3. Sign the PS5 in

Scan the QR code on the TV with your phone and sign in with your Tailscale
account. Within a few seconds the TV changes to the Home page: the PS5's
name (ps5), a Tailscale address (100.x.y.z), and "Exit node waiting for
your approval".

## 4. Approve the exit node

Open the [Tailscale admin page](https://login.tailscale.com/admin/machines).
Find **ps5**, click **⋯** on its row, then **Edit route settings**. Tick
**Use as exit node**, and the home network route (192.168.x.0/24) if it's
listed. Save. Within a few seconds the TV says "Exit node ready" (and the home
network "reachable").

## 5. Use it from the phone, away from home

1. On the phone, turn **Wi-Fi off** (mobile data only).
2. In the Tailscale app, sign in with the same account, then **Exit node** →
   **ps5**.
3. Open a website, for example https://ifconfig.me. It should show your
   **home** internet address, not the phone carrier's.
4. Open `http://PS5_IP:9039/` on the phone. If it opens, the home network
   route works too.

## 6. The PS5 on your tailnet

Still on mobile data, with the Tailscale app on:

1. Open `http://ps5/`. The same page as at home should open.
2. Tap **shared folders**. Plug a USB drive into the PS5 first if you want
   something to see: it shows as `/mnt/usb0`.
3. Send the PS5 a photo: in the phone's photos, **Share** → **Tailscale** →
   **ps5**. Within a few seconds the page lists it under "Received", and it's
   in `/data/tailstation/received/` on the PS5.

## 7. Does it keep running?

Leave the phone on the exit node.

1. **Home screen**, 10 minutes: does the website still load?
2. **A game**: start one, play 10 minutes, reload a website on the phone now
   and then. Then quit the game.
3. **Rest mode**: put the PS5 in rest mode for 5 minutes, try a website on
   the phone, then wake the PS5.

After each, open `http://PS5_IP:9039/` (does it still answer?). Get
`/data/tailstation/log.txt` over FTP at the end: it has a line a minute, so
gaps show when the PS5 paused it.

## 8. The app's buttons

1. On the TV: L1 and R1 go through the pages. Devices should list your
   phone, Files the photo from step 6.
2. Settings: switch **Home network** off, go down to **Apply**, press
   Cross. The TV says it's restarting, then comes back connected.
3. Home: press **Cross** ("Run in background"). The app closes; the phone's
   websites keep loading through the PS5.
4. Open the app again: it finds the background part running.

## 9. Stop it

In the app, on Home, press **Triangle**, then Cross to confirm. (Without the
app: `http://PS5_IP:9039/`, **Stop TailStation**, then **Stop it**.)
Opening the app again starts it again, already signed in. After a PS5
restart: run the jailbreak, then open the app.

## 10. Optional: the PS5's own tailnet (no account)

Only after the steps above work. Needs access to your router.

1. On the router, forward TCP port 8080 to the PS5's address.
2. In the app's Settings, switch on **Run my own tailnet (Headscale)**, then
   Apply. It restarts; Home shows it connected again, with a new name
   ending in `.ps5net`.
3. The Devices page shows the address to join (`http://YOUR_HOME_IP:8080`)
   and a QR code with it.
4. On the phone, on mobile data: in the Tailscale app, sign out of your
   account, then add an account with a custom (alternate) server and enter
   that address. A page says it's waiting for approval.
5. On the TV, Devices shows "... asks to join": press **Cross**. The phone
   connects by itself.
6. On the phone, pick **ps5** as the exit node, and check a website and
   ifconfig.me again.
7. To go back: switch it off in Settings, Apply. The PS5 is back on your
   Tailscale account without signing in again; on the phone, switch back to
   your Tailscale account.

## Results

| Check | Result | Notes |
| --- | --- | --- |
| The app opens, shows the logo | | |
| The app starts the background part (QR code on the TV) | | |
| Status page opens | | |
| Sign-in works, the TV shows the PS5's name and address | | |
| Exit node approved, the TV says "ready" | | |
| Phone on mobile data: website loads | | |
| ifconfig.me shows the home address | | |
| Home network page opens on the phone | | |
| `http://ps5/` opens on mobile data | | |
| Shared folders list and download | | |
| Photo sent with Taildrop arrives | | |
| Home screen, 10 min | | |
| During a game | | |
| Rest mode | | |
| Settings: Apply restarts it, comes back connected | | |
| Run in background: app closes, exit node keeps working | | |
| Guide page: sections switch with up/down, long ones scroll with left/right | | |
| Fixed port on: Devices shows direct for the phone on mobile data | | |
| Own tailnet: PS5 comes back connected as ps5.ps5net | | |
| Own tailnet: the phone's join shows on the TV, Cross lets it in | | |
| Own tailnet: exit node works from mobile data | | |
| Own tailnet off again: back on the Tailscale account | | |

Send back the filled table, plus `log.txt` (and `tailscale.log`, or
`headscale.log` for the own tailnet, if something failed) from
`/data/tailstation/`.
