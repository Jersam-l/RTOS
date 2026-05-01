/*
 * rtos_receiver.c  –  RTOS File Receiver (Windows / RTOS on Windows)
 * ====================================================================
 * Runs inside the RTOS application context on Windows.
 * Listens on PORT_BRIDGE_TO_RTOS (5002) for file-forwarding
 * connections from the Windows Bridge Server.
 *
 * Each received file is written to SAVE_DIR, which you can map
 * to any directory your RTOS application uses.
 *
 * Build (Windows, MSVC or MinGW):
 *   cl rtos_receiver.c ws2_32.lib              (MSVC)
 *   gcc -o rtos_receiver rtos_receiver.c -lws2_32  (MinGW)
 *
 * Usage:
 *   rtos_receiver.exe [save_directory]
 *   save_directory defaults to C:\rtos_received
 *
 * Integration with your RTOS app:
 *   Option A – Run as a separate thread: call rtos_receiver_run()
 *              from a dedicated FreeRTOS / RTOS task.
 *   Option B – Run as a standalone .exe alongside your RTOS app.
 *
 * The RTOS app can then read files from SAVE_DIR via normal
 * file-system calls (fopen / f_open, depending on your RTOS FS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../common/file_protocol.h"

/* ── Configuration ──────────────────────────────────────────── */
#define DEFAULT_SAVE_DIR  "C:\\rtos_received"
#define BACKLOG            3
/* ─────────────────────────────────────────────────────────────── */

/* Globals – set once at startup */
static char g_save_dir[512];

/* ── Low-level helpers ──────────────────────────────────────── */

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

/* ── File reception ─────────────────────────────────────────── */

/*
 * receive_file – Read header + data from bridge socket, write to disk.
 * Sends PROTO_ACK_OK or PROTO_ACK_ERR to the bridge.
 */
static void receive_file(SOCKET bridge_sock)
{
    char  ack = PROTO_ACK_ERR;   /* assume failure until proven otherwise */

    /* --- Magic --- */
    uint32_t magic_net = 0;
    if (recv_all(bridge_sock, &magic_net, sizeof(magic_net)) < 0) {
        fprintf(stderr, "[rtos_recv] Failed to read magic\n");
        goto done;
    }
    if (ntohl(magic_net) != PROTO_MAGIC) {
        fprintf(stderr, "[rtos_recv] Bad magic: 0x%08X\n", ntohl(magic_net));
        goto done;
    }

    /* --- Filename length --- */
    uint32_t fname_len_net = 0;
    if (recv_all(bridge_sock, &fname_len_net, sizeof(fname_len_net)) < 0) goto done;
    uint32_t fname_len = ntohl(fname_len_net);
    if (fname_len == 0 || fname_len >= PROTO_MAX_FILENAME) {
        fprintf(stderr, "[rtos_recv] Invalid filename length: %u\n", fname_len);
        goto done;
    }

    /* --- Filename --- */
    char filename[PROTO_MAX_FILENAME];
    if (recv_all(bridge_sock, filename, fname_len) < 0) goto done;
    filename[fname_len] = '\0';

    /* Sanitise filename: strip any path separators to prevent path traversal */
    {
        char *p = filename;
        while (*p) {
            if (*p == '/' || *p == '\\') *p = '_';
            p++;
        }
    }

    /* --- File size --- */
    uint64_t fsize_net = 0;
    if (recv_all(bridge_sock, &fsize_net, sizeof(fsize_net)) < 0) goto done;
    uint64_t filesize = ntohll(fsize_net);

    printf("[rtos_recv] Receiving '%s' (%llu bytes)...\n",
           filename, (unsigned long long)filesize);

    /* --- Build full save path --- */
    char save_path[1024];
    snprintf(save_path, sizeof(save_path), "%s\\%s", g_save_dir, filename);

    /* --- Open destination file --- */
    FILE *fp = fopen(save_path, "wb");
    if (!fp) {
        fprintf(stderr, "[rtos_recv] Cannot create file: %s\n", save_path);
        /* Still drain the socket so the bridge does not hang */
        char drain[PROTO_CHUNK_SIZE];
        uint64_t left = filesize;
        while (left > 0) {
            int n = recv(bridge_sock, drain,
                         (int)(left < sizeof(drain) ? left : sizeof(drain)), 0);
            if (n <= 0) break;
            left -= (uint64_t)n;
        }
        goto done;
    }

    /* --- Receive and write data --- */
    char chunk[PROTO_CHUNK_SIZE];
    uint64_t remaining = filesize;
    int io_error = 0;

    while (remaining > 0) {
        int to_read = (int)(remaining < sizeof(chunk) ? remaining : sizeof(chunk));
        int n = recv(bridge_sock, chunk, to_read, 0);
        if (n <= 0) {
            fprintf(stderr, "[rtos_recv] Connection lost mid-transfer\n");
            io_error = 1;
            break;
        }
        if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
            fprintf(stderr, "[rtos_recv] Disk write error\n");
            io_error = 1;
            break;
        }
        remaining -= (uint64_t)n;
    }
    fclose(fp);

    if (!io_error && remaining == 0) {
        printf("[rtos_recv] ✓ Saved: %s\n", save_path);
        ack = PROTO_ACK_OK;

        /*
         * ── RTOS app notification hook ──────────────────────────
         * Call your RTOS task notification / event flag here so the
         * application knows a new file has arrived.
         *
         *   xTaskNotifyGive(xFileProcessorTask);          // FreeRTOS
         *   tx_event_flags_set(&g_evt, FILE_READY, TX_OR); // ThreadX
         *   k_sem_give(&file_ready_sem);                  // Zephyr
         *
         * ────────────────────────────────────────────────────────
         */
        printf("[rtos_recv] Notifying RTOS app: new file '%s' ready in %s\n",
               filename, g_save_dir);
    }

done:
    send_all(bridge_sock, &ack, 1);
}

/* ── Main server loop (can be called from an RTOS task) ─────── */

/*
 * rtos_receiver_run – blocking server loop.
 * Embed this in an RTOS task or call from main().
 */
void rtos_receiver_run(const char *save_dir)
{
    strncpy(g_save_dir, save_dir ? save_dir : DEFAULT_SAVE_DIR,
            sizeof(g_save_dir) - 1);

    /* Create save directory if it does not exist */
    CreateDirectoryA(g_save_dir, NULL);

    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[rtos_recv] WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "[rtos_recv] socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT_BRIDGE_TO_RTOS);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[rtos_recv] bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return;
    }

    if (listen(listen_sock, BACKLOG) == SOCKET_ERROR) {
        fprintf(stderr, "[rtos_recv] listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return;
    }

    printf("=== RTOS File Receiver ===\n");
    printf("Listening on port %d\n", PORT_BRIDGE_TO_RTOS);
    printf("Saving files to: %s\n\n", g_save_dir);

    for (;;) {
        struct sockaddr_in bridge_addr;
        int addr_len = sizeof(bridge_addr);
        SOCKET bridge_sock = accept(listen_sock,
                                    (struct sockaddr *)&bridge_addr,
                                    &addr_len);
        if (bridge_sock == INVALID_SOCKET) {
            fprintf(stderr, "[rtos_recv] accept() failed: %d\n", WSAGetLastError());
            continue;
        }
        printf("[rtos_recv] Bridge connected from %s\n",
               inet_ntoa(bridge_addr.sin_addr));

        receive_file(bridge_sock);
        closesocket(bridge_sock);
    }

    /* Cleanup (never reached in normal operation) */
    closesocket(listen_sock);
    WSACleanup();
}

/* ── Entry point (standalone mode) ─────────────────────────── */
int main(int argc, char *argv[])
{
    const char *save_dir = (argc >= 2) ? argv[1] : DEFAULT_SAVE_DIR;
    rtos_receiver_run(save_dir);
    return 0;
}
