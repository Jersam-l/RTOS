/*
 * pi_sender.c  –  Raspberry Pi (Linux) file sender
 * =================================================
 * Connects to the Windows Bridge Server and sends every file
 * found in the WATCH_DIR directory, one at a time.
 *
 * Build (on Pi / any Linux):
 *   gcc -o pi_sender pi_sender.c -lpthread
 *
 * Usage:
 *   ./pi_sender <windows_bridge_ip> [watch_dir]
 *
 *   windows_bridge_ip  : IP address of the Windows machine
 *   watch_dir          : folder to watch for files (default: ./send_files)
 *
 * Behaviour:
 *   - Scans WATCH_DIR every POLL_INTERVAL_SEC seconds.
 *   - For each file found, sends it to the bridge server, then
 *     moves the file to ./sent_files/ so it is not re-sent.
 *   - Retries on connection failure after RETRY_DELAY_SEC seconds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../common/file_protocol.h"

/* ── Configuration ──────────────────────────────────────────── */
#define DEFAULT_WATCH_DIR    "send_files"
#define SENT_DIR             "sent_files"
#define POLL_INTERVAL_SEC    5
#define RETRY_DELAY_SEC      3
/* ─────────────────────────────────────────────────────────────── */

/* Send exactly `len` bytes from `buf` over socket `sock`. */
static int send_all(int sock, const void *buf, size_t len)
{
    const char *ptr = (const char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = send(sock, ptr, remaining, 0);
        if (sent <= 0) return -1;
        ptr       += sent;
        remaining -= (size_t)sent;
    }
    return 0;
}

/*
 * send_file – Transfers a single file over an open socket.
 * Returns 0 on success, -1 on error.
 */
static int send_file(int sock, const char *filepath, const char *filename)
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        fprintf(stderr, "[pi_sender] Cannot open file: %s (%s)\n",
                filepath, strerror(errno));
        return -1;
    }

    /* --- Determine file size --- */
    fseek(fp, 0, SEEK_END);
    uint64_t file_size = (uint64_t)ftell(fp);
    rewind(fp);

    /* --- Build and send header --- */
    uint32_t magic      = htonl(PROTO_MAGIC);
    uint32_t fname_len  = htonl((uint32_t)strlen(filename));
    uint64_t fsize_net  = htonll(file_size);

    if (send_all(sock, &magic,     sizeof(magic))     < 0 ||
        send_all(sock, &fname_len, sizeof(fname_len)) < 0 ||
        send_all(sock, filename,   strlen(filename))   < 0 ||
        send_all(sock, &fsize_net, sizeof(fsize_net)) < 0) {
        fprintf(stderr, "[pi_sender] Failed to send header for %s\n", filename);
        fclose(fp);
        return -1;
    }

    /* --- Stream file data --- */
    char chunk[PROTO_CHUNK_SIZE];
    uint64_t total_sent = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (send_all(sock, chunk, n) < 0) {
            fprintf(stderr, "[pi_sender] Send interrupted after %llu bytes\n",
                    (unsigned long long)total_sent);
            fclose(fp);
            return -1;
        }
        total_sent += n;
    }
    fclose(fp);

    /* --- Wait for ACK from bridge --- */
    char ack = 0;
    if (recv(sock, &ack, 1, 0) != 1 || ack != PROTO_ACK_OK) {
        fprintf(stderr, "[pi_sender] Bad or missing ACK for %s (got '%c')\n",
                filename, ack);
        return -1;
    }

    printf("[pi_sender] ✓ Sent: %s (%llu bytes)\n",
           filename, (unsigned long long)file_size);
    return 0;
}

/*
 * connect_to_bridge – Opens a TCP connection to the Windows bridge.
 * Returns socket fd on success, -1 on failure.
 */
static int connect_to_bridge(const char *bridge_ip)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[pi_sender] socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT_PI_TO_BRIDGE);
    if (inet_pton(AF_INET, bridge_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[pi_sender] Invalid bridge IP: %s\n", bridge_ip);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[pi_sender] Cannot connect to bridge %s:%d – %s\n",
                bridge_ip, PORT_PI_TO_BRIDGE, strerror(errno));
        close(sock);
        return -1;
    }
    printf("[pi_sender] Connected to bridge at %s:%d\n",
           bridge_ip, PORT_PI_TO_BRIDGE);
    return sock;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <windows_bridge_ip> [watch_dir]\n", argv[0]);
        return 1;
    }
    const char *bridge_ip = argv[1];
    const char *watch_dir = (argc >= 3) ? argv[2] : DEFAULT_WATCH_DIR;

    /* Create required directories if they do not exist */
    mkdir(watch_dir,  0755);
    mkdir(SENT_DIR,   0755);

    printf("[pi_sender] Watching '%s' – sending to bridge %s:%d\n",
           watch_dir, bridge_ip, PORT_PI_TO_BRIDGE);

    for (;;) {                         /* Main polling loop */
        DIR *dir = opendir(watch_dir);
        if (!dir) {
            fprintf(stderr, "[pi_sender] Cannot open watch dir: %s\n", watch_dir);
            sleep(POLL_INTERVAL_SEC);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            /* Skip hidden files and directories */
            if (entry->d_name[0] == '.') continue;

            char filepath[1024];
            snprintf(filepath, sizeof(filepath), "%s/%s", watch_dir, entry->d_name);

            struct stat st;
            if (stat(filepath, &st) < 0 || !S_ISREG(st.st_mode)) continue;

            /* Connect per-file (simple; reconnect if broken) */
            int sock = -1;
            int retries = 0;
            while (sock < 0 && retries < 5) {
                sock = connect_to_bridge(bridge_ip);
                if (sock < 0) {
                    fprintf(stderr, "[pi_sender] Retry %d/5 in %ds...\n",
                            ++retries, RETRY_DELAY_SEC);
                    sleep(RETRY_DELAY_SEC);
                }
            }
            if (sock < 0) {
                fprintf(stderr, "[pi_sender] Failed to connect – skipping %s\n",
                        entry->d_name);
                continue;
            }

            int ok = send_file(sock, filepath, entry->d_name);
            close(sock);

            if (ok == 0) {
                /* Move to sent_files/ */
                char dest[1024];
                snprintf(dest, sizeof(dest), "%s/%s", SENT_DIR, entry->d_name);
                rename(filepath, dest);
            }
        }
        closedir(dir);
        sleep(POLL_INTERVAL_SEC);
    }

    return 0;
}
