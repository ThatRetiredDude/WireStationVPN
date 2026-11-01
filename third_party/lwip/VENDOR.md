# lwIP, vendored

[lwIP](https://github.com/lwip-tcpip/lwip) 2.2.1 (tag `STABLE-2_2_1_RELEASE`, commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`).
BSD-3-Clause: [COPYING](COPYING), "Copyright (c) 2001, 2002 Swedish Institute of
Computer Science". NOTICE.md at the top of this repo lists it.

The TCP/IP stack inside the C++ build's WireGuard tunnel (`lite/src/tunnel.cpp`):
a payload can't make a network interface on the PS5, so the tunnel's packets
are taken apart here. `lite/src/port/lwipopts.h` and `lite/src/port/arch/cc.h`
configure it (no OS: one thread drives it).

Three changes of ours, each marked in the code, so the PS5 can be an exit node
and a way into the home network (packets for addresses that aren't its own):

- `src/core/ipv4/ip4.c`: with `WS_ACCEPT_ANY`, a packet on the tunnel's
  interface is taken whatever its destination (TCP and UDP only).
- `src/core/tcp_in.c`: with `WS_TCP_CATCHALL`, a connection no listener
  matches goes to one catch-all listener; the new connection keeps the port
  it was made to (`npcb->local_port = tcphdr->dest`).
- `lwipopts.h` sets `LWIP_HOOK_IP4_INPUT`, which records each packet's
  protocol for the tunnel (no change to lwIP's files).

`src/core/udp.c` isn't built: `tunnel.cpp` has its own `udp_input`, which
forwards each flow through a socket of its own.

Kept: `src/core` and `src/core/ipv4` (IPv4, TCP, the buffers), the headers in
`src/include/lwip` but its `apps`, and those in `src/include/netif`, which the
core includes whatever the options (none of their code is kept). Left out:
IPv6, the Ethernet, PPP and 6LoWPAN interfaces, the apps (HTTP, MQTT, SNMP ...),
the ports, tests and docs.
