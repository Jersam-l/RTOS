/*
 * win_bridge_server.c  –  Windows Bridge Server
 * ===============================================
 * Listens for file-sending connections from the Raspberry Pi on
 * PORT_PI_TO_BRIDGE (5001), then immediately forwards each received
 * file to the RTOS Receiver on PORT_BRIDGE_TO_RTOS (5002).
 *
 * Build (Windows, MSVC or MinGW):
 *   cl win_bridge_server.c ws2_32.lib              (MSVC)
 *   gcc -o win_bridge_server win_bridge_server.c -lws2_32  (MinGW)
 *
 * Usage:
 *   win_bridge_server.exe [rtos_ip]
 *   rtos_ip defaults to 127.0.0.1 (same machine as RTOS app)
 *
 * Architecture:
 *   Pi Sender ──[port 5001]──▶ win_bridge_server ──[port 5002]──▶ RTOS Receiver
 *
 * The bridge handles one transfer at a time (sequential).  For a
 * high-throughput version, spawn a thread per connection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../common/file_protocol.h"

/* ── Configuration ──────────────────────────────────────────── */
#define DEFAULT_RTOS_IP  "127.0.0.1"
#define BACKLOG           3
/* ─────────────────────────────────────────────────────────────── */

/* ── Low-level helpers ──────────────────────────────────────── */

/* Receive exactly `len` bytes into `buf`. Returns 0 on success, -1 on error. */
static int recv_all(SOCKET sock, void *buf, size_t len)
{
    char *ptr = (char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int r = recv(sock, ptr, (int)remaining, 0);
        if (r <= 0) return -1;
        ptr       += r;
        remaining -= (size_t)r;
    }
    return 0;
}

/* Send exactly `len` bytes from `buf`. Returns 0 on success, -1 on error. */
static int send_all(SOCKET sock, const void *buf, size_t len)
{
    const char *ptr = (const char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int s = send(sock, ptr, (int)remaining, 0);
        if (s <= 0) return -1;
        ptr       += s;
        remaining -= (size_t)s;
    }
    return 0;
}

/* ── Protocol helpers ───────────────────────────────────────── */

/*
 * read_header – Read and validate the file transfer header from `src`.
 * Populates `out_filename` and `out_filesize`.
 * Returns 0 on success, -1 on error.
 */
static int read_header(SOCKET src, char *out_filename, uint64_t *out_filesize)
{
    /* Magic */
    uint32_t magic_net = 0;
    if (recv_all(src, &magic_net, sizeof(magic_net)) < 0) return -1;
    if (ntohl(magic_net) != PROTO_MAGIC) {
        fprintf(stderr, "[bridge] Bad magic: 0x%08X\n", ntohl(magic_net));
        return -1;
    }

    /* Filename length */
    uint32_t fname_len_net = 0;
    if (recv_all(src, &fname_len_net, sizeof(fname_len_net)) < 0) return -1;
    uint32_t fname_len = ntohl(fname_len_net);
    if (fname_len == 0 || fname_len >= PROTO_MAX_FILENAME) {
        fprintf(stderr, "[bridge] Invalid filename length: %u\n", fname_len);
        return -1;
    }

    /* Filename */
    if (recv_all(src, out_filename, fname_len) < 0) return -1;
    out_filename[fname_len] = '\0';

    /* File size */
    uint64_t fsize_net = 0;
    if (recv_all(src, &fsize_net, sizeof(fsize_net)) < 0) return -1;
    *out_filesize = ntohll(fsize_net);

    return 0;
}

/*
 * write_header – Send a file transfer header to `dst`.
 */
static int write_header(SOCKET dst, const char *filename, uint64_t filesize)
{
    uint32_t magic     = htonl(PROTO_MAGIC);
    uint32_t fname_len = htonl((uint32_t)strlen(filename));
    uint64_t fsize_net = htonll(filesize);

    if (send_all(dst, &magic,     sizeof(magic))     < 0) return -1;
    if (send_all(dst, &fname_len, sizeof(fname_len)) < 0) return -1;
    if (send_all(dst, filename,   strlen(filename))   < 0) return -1;
    if (send_all(dst, &fsize_net, sizeof(fsize_net)) < 0) return -1;
    return 0;
}

/*
 * bridge_transfer – Stream file data from `pi_sock` to `rtos_sock`.
 * Also receives and proxies the final ACK back to the Pi.
 */
static int bridge_transfer(SOCKET pi_sock, SOCKET rtos_sock,
                            const char *filename, uint64_t filesize)
{
    char chunk[PROTO_CHUNK_SIZE];
    uint64_t remaining = filesize;

    printf("[bridge] Transferring '%s' (%llu bytes) to RTOS...\n",
           filename, (unsigned long long)filesize);

    while (remaining > 0) {
        int to_read = (int)(remaining < sizeof(chunk) ? remaining : sizeof(chunk));
        int n = recv(pi_sock, chunk, to_read, 0);
        if (n <= 0) {
            fprintf(stderr, "[bridge] Pi disconnected mid-transfer\n");
            return -1;
        }
        if (send_all(rtos_sock, chunk, (size_t)n) < 0) {
            fprintf(stderr, "[bridge] RTOS disconnected mid-transfer\n");
            return -1;
        }
        remaining -= (uint64_t)n;
    }

    /* Get ACK from RTOS and proxy it back to Pi */
    char ack = PROTO_ACK_ERR;
    if (recv(rtos_sock, &ack, 1, 0) == 1) {
        send_all(pi_sock, &ack, 1);
    } else {
        char err = PROTO_ACK_ERR;
        send_all(pi_sock, &err, 1);
        return -1;
    }

    if (ack == PROTO_ACK_OK)
        printf("[bridge] ✓ Transfer complete: '%s'\n", filename);
    else
        fprintf(stderr, "[bridge] RTOS reported error for '%s'\n", filename);

    return (ack == PROTO_ACK_OK) ? 0 : -1;
}

/*
 * connect_to_rtos – Establish connection to the RTOS receiver.
 */
static SOCKET connect_to_rtos(const char *rtos_ip)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT_BRIDGE_TO_RTOS);
    addr.sin_addr.s_addr = inet_addr(rtos_ip);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[bridge] Cannot reach RTOS at %s:%d – error %d\n",
                rtos_ip, PORT_BRIDGE_TO_RTOS, WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }
    printf("[bridge] Connected to RTOS at %s:%d\n", rtos_ip, PORT_BRIDGE_TO_RTOS);
    return sock;
}

/* ── Main ───────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *rtos_ip = (argc >= 2) ? argv[1] : DEFAULT_RTOS_IP;

    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[bridge] WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    /* Create listening socket for Pi connections */
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "[bridge] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    /* Allow rapid restart without TIME_WAIT delays */
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family      = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port        = htons(PORT_PI_TO_BRIDGE);

    if (bind(listen_sock, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
        fprintf(stderr, "[bridge] bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, BACKLOG) == SOCKET_ERROR) {
        fprintf(stderr, "[bridge] listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    printf("=== Windows Bridge Server ===\n");
    printf("Listening for Pi on port %d\n", PORT_PI_TO_BRIDGE);
    printf("Forwarding to RTOS at %s:%d\n\n", rtos_ip, PORT_BRIDGE_TO_RTOS);

    /* ── Accept loop ────────────────────────────────────────── */
    for (;;) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET pi_sock = accept(listen_sock,
                                (struct sockaddr *)&client_addr,
                                &client_len);
        if (pi_sock == INVALID_SOCKET) {
            fprintf(stderr, "[bridge] accept() failed: %d\n", WSAGetLastError());
            continue;
        }
        printf("[bridge] Pi connected from %s\n", inet_ntoa(client_addr.sin_addr));

        /* Read header from Pi */
        char filename[PROTO_MAX_FILENAME];
        uint64_t filesize = 0;
        if (read_header(pi_sock, filename, &filesize) < 0) {
            fprintf(stderr, "[bridge] Failed to read header from Pi\n");
            char err = PROTO_ACK_ERR;
            send_all(pi_sock, &err, 1);
            closesocket(pi_sock);
            continue;
        }
        printf("[bridge] Incoming file: '%s' (%llu bytes)\n",
               filename, (unsigned long long)filesize);

        /* Connect to RTOS receiver */
        SOCKET rtos_sock = connect_to_rtos(rtos_ip);
        if (rtos_sock == INVALID_SOCKET) {
            char err = PROTO_ACK_ERR;
            send_all(pi_sock, &err, 1);
            closesocket(pi_sock);
            continue;
        }

        /* Forward header to RTOS */
        if (write_header(rtos_sock, filename, filesize) < 0) {
            fprintf(stderr, "[bridge] Failed to send header to RTOS\n");
            char err = PROTO_ACK_ERR;
            send_all(pi_sock, &err, 1);
            closesocket(rtos_sock);
            closesocket(pi_sock);
            continue;
        }

        /* Relay file data */
        bridge_transfer(pi_sock, rtos_sock, filename, filesize);

        closesocket(rtos_sock);
        closesocket(pi_sock);
    }

    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
