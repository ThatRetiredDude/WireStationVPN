// lwIP's platform header for the tunnel (tunnel.cpp): the C library's types,
// little-endian x86-64, and its diagnostics into the log.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif
void ws_wg_diag(const char *fmt, ...);
void ws_wg_assert(const char *what, const char *file, int line);
#ifdef __cplusplus
}
#endif

#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_PLATFORM_DIAG(x) \
    do { ws_wg_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) ws_wg_assert(x, __FILE__, __LINE__)
