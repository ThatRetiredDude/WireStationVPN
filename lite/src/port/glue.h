// Between tunnel.cpp and glue.c: lwIP's UDP calls that wireguard-lwip makes,
// for the tailnet's WireGuard device, whose packets go through the
// Tailscale client (relay or direct path), not a socket of its own.
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// A WireGuard packet from a device (its stand-in address ip_be:port).
void ws_wg_input(const void *data, size_t len, uint32_t ip_be, uint16_t port);
// Defined in tunnel.cpp: a WireGuard packet to the device with the stand-in
// address ip_be:port, through the Tailscale client. False if none has it.
bool ws_ts_send(const void *data, size_t len, uint32_t ip_be, uint16_t port);
// Defined in tunnel.cpp: random bytes from the system.
void ws_wg_random(void *out, size_t n);
#ifdef __cplusplus
}
#endif
