/* NucleoOS — libssh2 build config for ESP-IDF (newlib + lwip, mbedTLS crypto backend).
   Pulled in by src/libssh2_setup.h when HAVE_CONFIG_H is defined (set in CMakeLists). Only the
   platform features actually present on the ESP32-S3 toolchain are enabled; the rest stay off and
   libssh2 falls back gracefully. No zlib (compression omitted to save RAM/flash). */
#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

#include <stdio.h>
#include <sys/types.h>

/* Headers available in ESP-IDF (newlib + lwip BSD sockets) */
#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETINET_IN_H 1

/* Functions */
#define HAVE_GETTIMEOFDAY 1
#define HAVE_STRTOLL 1
#define HAVE_SNPRINTF 1
#define HAVE_SELECT 1

/* Non-blocking sockets via fcntl(O_NONBLOCK) (lwip/VFS) */
#define HAVE_O_NONBLOCK 1

/* Crypto backend: mbedTLS (ships with ESP-IDF) */
#ifndef LIBSSH2_MBEDTLS
#define LIBSSH2_MBEDTLS 1
#endif

#endif /* LIBSSH2_CONFIG_H */
