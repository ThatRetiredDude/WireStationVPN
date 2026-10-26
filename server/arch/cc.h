// lwIP's platform header for the server: the C library's types,
// little-endian x86-64, and its diagnostics into the server's log.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif
void ws_diag(const char *fmt, ...);
void ws_assert(const char *what, const char *file, int line);
#ifdef __cplusplus
}
#endif

#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_PLATFORM_DIAG(x) \
    do { ws_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) ws_assert(x, __FILE__, __LINE__)
