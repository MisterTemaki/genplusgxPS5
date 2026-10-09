// Genesis Plus GX PS5: Mbed TLS's settings for the helper's own HTTPS (fe_tlshttp.cpp), on top of Mbed TLS's
// default configuration (third_party/mbedtls/include/mbedtls/mbedtls_config.h): TLS 1.2 and 1.3 clients, the
// certificate checks, nothing that needs files or a platform entropy source.
// SPDX-License-Identifier: MIT
#pragma once

// randomness comes from fe_tlshttp.cpp (mbedtls_hardware_poll: the kernel's kern.arandom, /dev/urandom)
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

// the sockets are fe_tlshttp.cpp's; no files, no key storage, no servers
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_SELF_TEST
