// lwIP's UDP as wireguardif.c uses it: one pcb, the tailnet's WireGuard
// device. lwIP's udp.c isn't built; tunnel.cpp has udp_input, for the UDP
// that devices send through the PS5 (exit node, home network).
//
// Also the platform functions wireguard-lwip asks for, and lwIP's clock.
#include <string.h>
#include <time.h>

#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/udp.h"
#include "wireguard-platform.h"
#include "glue.h"

static struct udp_pcb device_pcb;

void ws_wg_input(const void *data, size_t len, uint32_t ip_be, uint16_t port) {
    struct udp_pcb *pcb = &device_pcb;
    if (!pcb->recv || len == 0 || len > 0xFFFF) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, data, len);
    ip_addr_t from;
    ip_addr_set_ip4_u32_val(from, ip_be);
    // wireguardif_network_rx frees it.
    pcb->recv(pcb->recv_arg, pcb, p, &from, port);
}

struct udp_pcb *udp_new(void) {
    memset(&device_pcb, 0, sizeof device_pcb);
    return &device_pcb;
}

err_t udp_bind(struct udp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port) {
    (void)ipaddr;
    pcb->local_port = port;
    return ERR_OK;
}

void udp_bind_netif(struct udp_pcb *pcb, const struct netif *netif) {
    (void)pcb;
    (void)netif;
}

void udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg) {
    pcb->recv = recv;
    pcb->recv_arg = recv_arg;
}

void udp_remove(struct udp_pcb *pcb) {
    pcb->recv = NULL;
    pcb->recv_arg = NULL;
}

err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *dst_ip, u16_t dst_port) {
    unsigned char buf[2048];
    (void)pcb;
    if (p->tot_len > sizeof buf) return ERR_MEM;
    const u16_t n = pbuf_copy_partial(p, buf, p->tot_len, 0);
    return ws_ts_send(buf, n, ip4_addr_get_u32(ip_2_ip4(dst_ip)), dst_port) ? ERR_OK : ERR_RTE;
}

void udp_init(void) {}
void udp_netif_ip_addr_changed(const ip_addr_t *old_addr, const ip_addr_t *new_addr) {
    (void)old_addr;
    (void)new_addr;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

u32_t sys_now(void) { return (u32_t)monotonic_ms(); }

uint32_t wireguard_sys_now(void) { return (uint32_t)monotonic_ms(); }

void wireguard_random_bytes(void *bytes, size_t size) { ws_wg_random(bytes, size); }

// TAI64N: seconds since 1970 plus 2^62 and the 10 leap seconds TAI was ahead
// then, big-endian, and nanoseconds. Peers only check it grows.
void wireguard_tai64n_now(uint8_t *output) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t s = 0x400000000000000aULL + (uint64_t)ts.tv_sec;
    const uint32_t ns = (uint32_t)ts.tv_nsec;
    for (int i = 0; i < 8; i++) output[i] = (uint8_t)(s >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) output[8 + i] = (uint8_t)(ns >> (24 - 8 * i));
}

bool wireguard_is_under_load(void) { return false; }
