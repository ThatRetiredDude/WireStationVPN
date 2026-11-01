# wireguard-lwip, vendored

[wireguard-lwip](https://github.com/smartalock/wireguard-lwip), commit `c54f20dbe76ac8b3411ad21e0ed7deea6f0cfd4d`. BSD-3-Clause:
[LICENSE](LICENSE), "Copyright (c) 2021 Daniel Hope (www.floorsense.nz)"; its
crypto (`src/crypto/refc`) is its own ChaCha20-Poly1305 (the same license),
BLAKE2s from RFC 7693, Poly1305 from poly1305-donna (public domain or MIT) and
X25519 from Mike Hamburg's STROBE (MIT, "Copyright (c) 2015-2016 Cryptography
Research, Inc.", `src/crypto/refc/x25519-license.txt`). NOTICE.md at the top
of this repo lists it.

WireGuard for the C++ build's tunnel (`lite/src/tunnel.cpp`), as an lwIP
network interface. Taken from TokenStation5's copy, with one change: up to 16
peers at a time, not 1 (`src/wireguard-platform.h`), so each device has its
own session; `tunnel.cpp` makes room for a new one by dropping the one quiet
longest. Its UDP goes out through Tailscale's own paths (direct or DERP):
`lite/src/port/glue.c` stands in for the lwIP UDP calls `wireguardif.c` makes.

Kept: `src/` (the protocol, the interface, the crypto glue) and
`src/crypto/refc`. Left out: the Cortex-M assembly, the examples.
