/*
 * file_protocol.h
 * ---------------
 * Shared protocol definition used by all three components:
 *   Pi Sender  →  Windows Bridge Server  →  RTOS Receiver
 *
 * Transfer sequence (TCP, reliable):
 *
 *   Sender                              Receiver
 *   ──────                              ────────
 *   [MAGIC 4B]              ──────────▶
 *   [filename_len 4B]       ──────────▶
 *   [filename N bytes]      ──────────▶
 *   [file_size 8B]          ──────────▶
 *   [file data M bytes]     ──────────▶
 *                           ◀──────────  [ACK 1B: 'A'=ok / 'E'=error]
 *
 * All multi-byte integers are in network byte order (big-endian).
 */

#ifndef FILE_PROTOCOL_H
#define FILE_PROTOCOL_H

#include <stdint.h>

/* ── Ports ──────────────────────────────────────────────────── */
#define PORT_PI_TO_BRIDGE   5001   /* Pi  → Windows bridge  */
#define PORT_BRIDGE_TO_RTOS 5002   /* Windows bridge → RTOS  */

/* ── Protocol constants ─────────────────────────────────────── */
#define PROTO_MAGIC         0xFEEDBEEFU
#define PROTO_ACK_OK        'A'
#define PROTO_ACK_ERR       'E'

/* ── Sizes ──────────────────────────────────────────────────── */
#define PROTO_MAGIC_SIZE    4
#define PROTO_FNAMELEN_SIZE 4
#define PROTO_FILESIZE_SIZE 8
#define PROTO_MAX_FILENAME  256
#define PROTO_CHUNK_SIZE    4096

/*
 * On-wire header layout (sent before file data):
 *
 *   Offset  Size  Field
 *   ──────  ────  ─────────────────────────────────────
 *    0      4     magic  = 0xFEEDBEEF
 *    4      4     filename_len  (uint32 big-endian)
 *    8      N     filename bytes (no NUL terminator)
 *    8+N    8     file_size (uint64 big-endian)
 */

/* Utility: portable htonll / ntohll (host ↔ network 64-bit) */
#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  static __inline uint64_t htonll(uint64_t v) {
      return (((uint64_t)htonl((uint32_t)(v & 0xFFFFFFFFU))) << 32)
             | htonl((uint32_t)(v >> 32));
  }
  static __inline uint64_t ntohll(uint64_t v) { return htonll(v); }
#else
  #include <arpa/inet.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #ifndef htonll
    #define htonll(x) \
        ((((uint64_t)htonl((uint32_t)((x) & 0xFFFFFFFFUL))) << 32) \
         | (uint64_t)htonl((uint32_t)((x) >> 32)))
  #endif
  #ifndef ntohll
    #define ntohll(x) htonll(x)
  #endif
#endif

#endif /* FILE_PROTOCOL_H */
