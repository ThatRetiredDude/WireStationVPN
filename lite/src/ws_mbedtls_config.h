// Changes to Mbed TLS's default configuration (MBEDTLS_USER_CONFIG_FILE):
// a TLS client on sockets this program opens itself.

// Randomness from mbedtls_hardware_poll (base.cpp), not Mbed TLS's own probing.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
// UTC dates without the C library's gmtime_r (base.cpp).
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
// base.cpp does its own sockets; nothing is kept on disk.
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_FS_IO
