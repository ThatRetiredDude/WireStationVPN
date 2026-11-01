# TailStation guide

TailStation puts a jailbroken PS5 on a Tailscale network, so your phone and
computer can use it from anywhere: as an **exit node** (your browsing goes
out through your home internet), to reach your **home network**, to
**send files** to the PS5 and to **browse its folders**. The same guide is in
the app, on the **Guide** page.

## Contents

1. [What you need](#what-you-need)
2. [Install the app](#install-the-app)
3. [Sign the PS5 in](#sign-the-ps5-in)
4. [Approve the exit node](#approve-the-exit-node)
5. [Use it from your phone or computer](#use-it-from-your-phone-or-computer)
6. [Files: send, browse, download](#files-send-browse-download)
7. [Keep it running](#keep-it-running)
8. [Run your own tailnet (no account)](#run-your-own-tailnet-no-account)
9. [A home name and https](#a-home-name-and-https)
10. [The guard](#the-guard)
11. [Port forwarding](#port-forwarding)
12. [Settings](#settings)
13. [The status page at home](#the-status-page-at-home)
14. [Troubleshooting](#troubleshooting)
15. [Privacy and safety](#privacy-and-safety)
16. [Update or remove it](#update-or-remove-it)
17. [Words used here](#words-used-here)

## What you need

- A PS5 with a jailbreak that has an **ELF loader** on port 9021. TailStation
  uses it to start its background part.
- The **Tailscale app** on each phone or computer you want to use (free, from
  the app stores or tailscale.com/download).
- One of these:
  - a **Tailscale account**. The free plan is enough. This is the easy way,
    with no router setup. Start at [Install the app](#install-the-app).
  - **no account**: the PS5 runs the tailnet itself. That needs one port
    forwarded on your router. See
    [Run your own tailnet](#run-your-own-tailnet-no-account).

[Back to contents](#contents)

## Install the app

1. Download `PPSA99556.zip` from the store listing or the project's
   Releases page, and unzip it. You get a folder named `PPSA99556`.
2. Copy that folder to the PS5 with your jailbreak's file transfer (FTP),
   into `/data/homebrew/` (or wherever your homebrew launcher installs
   apps), or install it from the homebrew store.
3. Open **TailStation** from the home screen.

The app starts its background part by itself. The background part keeps
running after you close the app. See [Keep it running](#keep-it-running).

[Back to contents](#contents)

## Sign the PS5 in

With a Tailscale account (for no account, see
[Run your own tailnet](#run-your-own-tailnet-no-account)):

1. The app's **Home** page shows a QR code. Scan it with your phone's camera.
2. Sign in with your Tailscale account. No account yet? You can make one on
   that page.
3. The TV changes by itself to **Connected**, with the PS5's name (`ps5`)
   and its Tailscale address.

No phone handy? The link under the QR code works on any computer.
The PS5 stays signed in from then on, also after a restart.

[Back to contents](#contents)

## Approve the exit node

Tailscale asks the owner to approve an exit node once:

1. On a computer, open the Tailscale admin page,
   [login.tailscale.com/admin/machines](https://login.tailscale.com/admin/machines).
2. Find **ps5**, open its menu (**⋯**), choose **Edit route settings**.
3. Tick **Use as exit node**. Tick the home network route too if you want
   to reach your home devices (see
   [Use it from your phone or computer](#use-it-from-your-phone-or-computer)).

The app's Home page then says **Exit node ready**. On your own tailnet the
PS5 approves itself, so you skip this.

[Back to contents](#contents)

## Use it from your phone or computer

**As an exit node** (your traffic goes out through your home internet, for
example on public Wi-Fi or abroad):

1. Open the Tailscale app and make sure it's connected.
2. Choose **Exit node**, then **ps5**.
3. To check: open ifconfig.me. It shows your home's address.

To stop, choose **Exit node**, then **None**.

**Your home network:** with the home network route approved, the phone can
open things at home by their usual address, such as your router's page or a
printer, while away.

**The PS5 by name:** `ps5` (or its full name, shown on the Home page) works
from every device in the tailnet. This is called MagicDNS.

[Back to contents](#contents)

## Files: send, browse, download

**Send files to the PS5** (Taildrop):

- On a phone: share a photo or file, choose **Tailscale**, then **ps5**.
- On a computer: right-click a file, **Send with Tailscale**, or run
  `tailscale file cp FILE ps5:`.

Files land in `/data/tailstation/received` on the PS5. The app's **Files**
page lists the latest ones.

**Browse and download from the PS5:** open `http://ps5/files/` in a browser
on any of your devices, at home or away. Shared by default: received files
and USB drives. Videos play straight from the PS5. Everything is read-only.
To share more folders, see `shared_folders` in [Settings](#settings).

The PS5's keys and its settings file are never shown there, even inside a
shared folder.

[Back to contents](#contents)

## Keep it running

- On **Home**, press **Cross**: the app closes and TailStation keeps
  running in the background, also while you play.
- Open the app again any time: it finds the background part running.
- **Stop** it: on Home press **Triangle**, then Cross.
- **Rest mode** pauses it. Waking the PS5 resumes it.
- **After a PS5 restart:** run the jailbreak, then open the app once. It
  reconnects without signing in again.

[Back to contents](#contents)

## Run your own tailnet (no account)

The PS5 can be the tailnet's server itself (it runs Headscale, the
open-source Tailscale server). No Tailscale account is involved. Devices
still connect directly where they can, and through Tailscale's public
relays where they can't.

**Set it up:**

1. On your router, forward **TCP 8080** to the PS5. See
   [Port forwarding](#port-forwarding).
2. In the app, **Settings**, switch on **Run my own tailnet**, then
   **Apply**. It restarts and comes back connected, as `ps5.ps5net`.
3. The **Devices** page shows the address devices join at, such as
   `http://203.0.113.7:8080`, with a QR code.

**Add a phone or computer:**

1. In the Tailscale app, before signing in, choose to use a custom
   (alternate) coordination server, and enter that address. On a computer:
   `tailscale up --login-server ADDRESS`.
2. The device shows a page saying it's waiting for approval.
3. On the TV, **Devices** shows "... asks to join". Press **Cross** to let it
   in, or **Square** to turn it away. (Or use **Let it in** on the
   [status page](#the-status-page-at-home).)

The PS5 approves its own exit node and home network. Then use it as in
[Use it from your phone or computer](#use-it-from-your-phone-or-computer).

**Good to know:**

- The PS5 has to be running for devices to join and stay in touch.
- If your home's internet address changes, the join address changes too.
  A free home name fixes that, and also gives it `https`. See
  [A home name and https](#a-home-name-and-https).
- Without a home name the address is plain `http`. Your traffic is still
  end-to-end encrypted by Tailscale, but some phones refuse a server that
  isn't `https`.
- The PS5 blocks addresses that misbehave. See [The guard](#the-guard).
- **Switch back** to your Tailscale account: switch it off and Apply. Each
  mode keeps its own sign-in, so neither has to sign in again.

[Back to contents](#contents)

## A home name and https

For your own tailnet. A home name such as `myps5.duckdns.org` keeps working
when your home's internet address changes, and lets TailStation get a free
`https` certificate, which every Tailscale app accepts.

1. On a phone or computer, open [duckdns.org](https://www.duckdns.org), sign
   in (with Google, GitHub and others) and add a name, such as `myps5`.
2. At home, open the [status page](#the-status-page-at-home), find **Home
   name and https**, enter the name and the **token** DuckDNS shows, and
   press **Save**.
3. In the app, **Settings**, **Apply**. TailStation points the name at your
   home, then asks Let's Encrypt for a certificate. The first one takes a
   minute or two.
4. The **Devices** page now shows `https://myps5.duckdns.org:8080` and when
   the certificate runs out. TailStation renews it by itself, a month
   before.

No other port has to be forwarded: Let's Encrypt checks the name through
DuckDNS, not through your router. Devices that joined with the old address
keep working, and the old plain `http` address still answers.

**Other home name services** (No-IP, Dynu, FreeDNS and others): put the
update link they give you in `ddns_update_url`, and the name in
`server_url`, such as `http://myname.ddns.net:8080`. TailStation opens the
link every 5 minutes. For `https` with them, use a certificate of your own
(`tls_cert`, `tls_key`). See [Settings](#settings).

[Back to contents](#contents)

## The guard

Your own tailnet's server faces the internet, so like the tools fail2ban
and CrowdSec, TailStation blocks addresses that misbehave:

| Who | Blocked for |
| --- | --- |
| An address making more than 120 requests a minute | 1 hour |
| An address asking for more than 20 pages that don't exist in 10 minutes (scanning) | 1 hour |
| A device you turn away on the PS5 (Square), by its address | 1 day |

One address can also have at most 3 devices waiting to join. Addresses on
your home network are never blocked. The **Devices** page and the status
page show how many addresses are blocked, and the status page has
**Unblock all**.

**With CrowdSec at home** (on a PC or server that runs it): run
`cscli bouncers add tailstation` there, then set `crowdsec_url` (such as
`http://192.168.1.10:8080`) and `crowdsec_key` to the key it printed.
Addresses CrowdSec blocks are then blocked here too, including its shared
list of known attackers.

The guard is on by default. `guard = off` turns it off.

**What the internet can reach.** Only the join port, and only what a
device needs to join and stay connected. Headscale's admin API and its
other pages answer "not found" from outside. On top of that, whether or
not the guard is on:

- One address can have at most 16 connections open at once.
- Requests have to arrive within 10 seconds and stay small.
- With a certificate, a browser opening a join link over plain http is
  sent to https.

**Keep it that way:**

- Forward only the join port (and the UDP port if you use it).
- Never forward the status page port (9039), FTP or the ELF loader. The
  status page refuses visitors from outside the home anyway.
- Turn **Run my own tailnet** off when you don't need it.
- Turn away (Square) any device you don't recognise.

[Back to contents](#contents)

## Port forwarding

A port forward tells your router to pass connections from the internet to
the PS5. Whether you need one:

| You use | Forward | Why |
| --- | --- | --- |
| A Tailscale account | Nothing | Tailscale's service handles sign-in; connections find their way through the router by themselves |
| Your own tailnet | **TCP 8080** (`server_port`) | Your devices reach the PS5's server to join and keep in touch while away. The same port serves https once you have a [home name](#a-home-name-and-https) |
| Either, for speed (optional) | **UDP 41641**, with **Fixed port** on | More devices connect directly instead of through a relay |

**How to forward** (the words differ by router):

1. Find the PS5's home address: on the PS5, **Settings**, **Network**,
   **Connection Status**, **IPv4 Address** (such as 192.168.1.50).
   Giving the PS5 a fixed address in the router (often called DHCP
   reservation) keeps it from changing.
2. Open your router's page (often 192.168.1.1 or 192.168.0.1, or the
   router's own phone app) and find **Port forwarding** (sometimes under
   NAT, Virtual servers or Gaming).
3. Add a rule: the port (8080 or 41641), the protocol (TCP or UDP), to the
   PS5's address, same port.

**Faster direct connections (optional):**

- In the app's Settings, switch on **Fixed port**, Apply, and forward
  **UDP 41641** to the PS5.
- Many routers open it by themselves (UPnP, NAT-PMP), so try without the
  forward first.
- On the Devices page and the status page, each device shows **direct** or
  **relay**. Relay works too, a bit slower.

**When forwarding can't work:** some internet providers share one public
address between many homes (CGNAT). If your router's internet (WAN) address
starts with 100.64 to 100.127, 10., 172.16 to 172.31 or 192.168., or differs
from what ifconfig.me shows at home, you're likely behind one. Then use a
Tailscale account: it needs no forwarding.

[Back to contents](#contents)

## Settings

The switches on the app's **Settings** page (press **Apply** to restart
with them):

| Switch | Default | What it does |
| --- | --- | --- |
| Exit node | on | Your devices can send their internet traffic through the PS5 |
| Home network | on | Your devices can reach your home network through the PS5 |
| This page on your tailnet | on | `http://ps5/` and the shared folders from your devices |
| Receive files (Taildrop) | on | Take files sent with the Tailscale app |
| Run my own tailnet | off | The PS5 is the tailnet's server. See [Run your own tailnet](#run-your-own-tailnet-no-account) |
| Fixed port | off | Always use UDP 41641, to forward it. See [Port forwarding](#port-forwarding) |

Everything, in `/data/tailstation/settings.conf` (edit it over FTP, then
Apply in the app). One `name = value` per line, `#` starts a comment:

| Setting | Default | What it does |
| --- | --- | --- |
| `hostname` | ps5 | The PS5's name in the tailnet |
| `exit_node` | on | As the switch above |
| `home_network` | on | As the switch above |
| `tailnet_page` | on | As the switch above |
| `taildrop` | on | As the switch above |
| `taildrop_folder` | /data/tailstation/received | Where received files go |
| `shared_folders` | taildrop, /mnt/usb0, /mnt/usb1, /mnt/ext0, /mnt/ext1 | Folders to browse, comma-separated (`taildrop` means the received folder). Add `/data` to share it |
| `auth_key` | | A Tailscale auth key, to sign in without the QR code |
| `tags` | | Tailscale ACL tags, such as `tag:ps5` |
| `control_url` | Tailscale's | A different coordination server (one you run elsewhere) |
| `server` | off | Run my own tailnet |
| `server_port` | 8080 | Its TCP port, the one to forward |
| `server_url` | `http://HOME_IP:8080` | The address devices join at |
| `server_domain` | ps5net | Names end in it: `ps5.ps5net` |
| `duckdns_domain` | | Your DuckDNS name, such as `myps5`. See [A home name and https](#a-home-name-and-https) |
| `duckdns_token` | | Your DuckDNS token |
| `ddns_update_url` | | Another home name service's update link |
| `https` | on | Use https when there's a certificate (DuckDNS, or your own) |
| `tls_cert`, `tls_key` | | A certificate and key of your own (PEM files, full paths) |
| `acme_email` | | An email for Let's Encrypt's renewal warnings (optional) |
| `guard` | on | Block addresses that misbehave. See [The guard](#the-guard) |
| `crowdsec_url`, `crowdsec_key` | | Your CrowdSec's local API and bouncer key |
| `fixed_port` | off | Use UDP 41641 |
| `port` | 0 (any) | A UDP port of your choice instead |
| `status_port` | 9039 | The [status page](#the-status-page-at-home)'s port |
| `dns` | 1.1.1.1 | DNS server, only used when the PS5 has none |

[Back to contents](#contents)

## The status page at home

Any browser on your home network: `http://PS5_ADDRESS:9039/` (the PS5's
address is in [Port forwarding](#port-forwarding), step 1). It shows what
the app shows: the sign-in QR code, the PS5's name and address, the exit
node, devices, and on your own tailnet the join address and **Let it in**
buttons. It also has **Sign this PS5 out** and **Stop TailStation**.

From any device in the tailnet, the same page is at `http://ps5/`.

[Back to contents](#contents)

## Troubleshooting

**The app says TailStation isn't running, and Cross doesn't start it.**
The jailbreak's ELF loader isn't running. Run the jailbreak, then press
Cross again.

**No QR code, it keeps saying Starting.** The PS5 can't reach Tailscale:
check its internet connection. The background part's log is
`/data/tailstation/log.txt`.

**The exit node says waiting for approval.** Approve it in the admin page.
See [Approve the exit node](#approve-the-exit-node).

**The phone doesn't list ps5 as an exit node.** Same as above, or the phone
is signed in to a different tailnet (account) than the PS5.

**Websites don't load through the PS5.** Check the PS5 is still connected
(app Home page). Choose **None** and then **ps5** again as the exit node.

**Own tailnet: the phone can't reach the join address from mobile data.**
The port forward is missing or points at an old address, or you're behind
CGNAT. See [Port forwarding](#port-forwarding).

**Own tailnet: the phone refuses the address.** Some phones want `https`.
Set up a [home name](#a-home-name-and-https).

**Own tailnet: no https after Apply.** The log says why (look for
`https:`). Most often the DuckDNS token is wrong. TailStation goes on with
plain `http` meanwhile.

**Own tailnet: a device can't reach the server any more.** The guard may
have blocked its address. Use **Unblock all** on the
[status page](#the-status-page-at-home).

**Everything is slow.** Devices marked **relay** go through a Tailscale
relay. See *Faster direct connections* in [Port forwarding](#port-forwarding).

**It stopped after a restart.** Run the jailbreak, then open the app. See
[Keep it running](#keep-it-running).

When asking for help, include `log.txt` (and `tailscale.log`, or
`headscale.log` on your own tailnet) from `/data/tailstation/`.

[Back to contents](#contents)

## Privacy and safety

- Traffic between your devices and the PS5 is end-to-end encrypted
  (WireGuard), in both modes.
- With a Tailscale account, Tailscale's service sees which devices are in
  your tailnet, not your traffic. On your own tailnet, nobody else is
  involved, apart from relays passing along encrypted data.
- The PS5's own services (FTP, the ELF loader) are **not** reachable from
  the tailnet, and neither is the PS5's own loopback through the exit node.
- Shared folders are read-only. The PS5's keys and settings are never
  served.
- Only devices you approve can join your own tailnet.
- The status page, which has the Approve buttons, answers only the home
  network and the tailnet, and only when opened by the PS5's address or
  name. Another website can't use your browser to press its buttons.
- `settings.conf` is readable only by TailStation, since it can hold your
  DuckDNS token and keys.
- Anyone using the PS5 as their exit node browses with your home's address:
  only let in people you trust.

[Back to contents](#contents)

## Update or remove it

- **Update:** install the new version over the old one. Settings, the sign-in
  and received files are kept: they live in `/data/tailstation/`, outside
  the app.
- **Remove:** stop it (Home, Triangle), delete the app, and delete
  `/data/tailstation/` to remove its keys and files too. With a Tailscale
  account, also remove **ps5** in the admin page.

[Back to contents](#contents)

## Words used here

| Word | Meaning |
| --- | --- |
| Tailnet | Your private network of devices, wherever they are |
| Tailscale | The service and apps that make the tailnet |
| Exit node | A device others send their internet traffic through: here, the PS5 |
| Home network route | Lets tailnet devices reach the other devices at your home |
| MagicDNS | Devices reachable by name, such as `ps5` |
| Taildrop | Sending files between your devices with the Tailscale app |
| Relay (DERP) | Tailscale's servers that pass encrypted traffic when devices can't connect directly |
| Headscale | The open-source server the PS5 runs for your own tailnet |
| Port forward | A router rule that passes connections from the internet to one device |
| Home name (DDNS) | A name that follows your home's internet address when it changes |
| https certificate | Proof that the server is really yours, so apps trust the address. Let's Encrypt gives them free |
| fail2ban, CrowdSec | Tools that block addresses that attack a server; the guard does the same job |
| CGNAT | When your provider shares one internet address between many homes; port forwards can't work |
| ELF loader | The part of the jailbreak that starts programs like TailStation's background part |

[Back to contents](#contents)
