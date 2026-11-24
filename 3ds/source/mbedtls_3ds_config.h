// Adjustments to mbedTLS 3.6's default configuration for the 3DS.
#pragma once

// We do our own socket I/O and have no POSIX timing/fs layer worth using.
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_PADLOCK_C

// Entropy comes from the console's hardware RNG (ps:ps), see http_3ds.cpp.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// Smaller buffers save RAM; AnkiWeb responses are streamed anyway.
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384
