/*
 * rtos_main_app.c  –  AppCoe RTOS Main Application (Windows)
 * ============================================================
 * This is the SINGLE entry point for the entire file-transfer system.
 *
 * What it does when you run it:
 *   1. Automatically starts rtos_receiver.exe  (listens on port 5002)
 *   2. Automatically starts win_bridge_server.exe (listens on port 5001)
 *   3. Watches C:\rtos_received\ for incoming files from the Pi
 *   4. Calls on_file_received() the moment each file arrives
 *   5. Cleans up and stops both servers when you press Ctrl+C
 *
 * Build:
 *   gcc -Wall -o rtos_main_app.exe rtos_main_app.c -lws2_32
 *
 * Usage:
 *   rtos_main_app.exe [bridge_rtos_ip] [watch_dir]
 *
 *   bridge_rtos_ip : IP of the RTOS receiver (default: 127.0.0.1)
 *   watch_dir      : folder to watch  (default: C:\rtos_received)
 *
 * ── Customise file processing ──────────────────────────────────
 * Edit the on_file_received() function at the bottom of this file
 * to define what your RTOS app does with each received file.
 * ─────────────────────────────────────────────────────────────
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ── Configuration ──────────────────────────────────────────── */
#define DEFAULT_RTOS_IP    "127.0.0.1"
#define DEFAULT_WATCH_DIR  "C:\\rtos_received"
#define WATCH_INTERVAL_MS  500       /* how often to check for new files  */
#define FILE_SETTLE_MS     300       /* wait for file to finish writing   */
#define MAX_PATH_LEN       1024

/*
 * Paths to the server executables.
 * IF running from AppCOE or a different working directory, use full paths:
 *   #define RECEIVER_EXE "C:\\path\\to\\appcode\\rtos_app\\rtos_receiver.exe"
 *   #define BRIDGE_EXE   "C:\\path\\to\\appcode\\win_bridge\\win_bridge_server.exe"
 *
 * IF running from the appcode\ folder (PowerShell), relative paths work:
 *   #define RECEIVER_EXE "rtos_app\\rtos_receiver.exe"
 *   #define BRIDGE_EXE   "win_bridge\\win_bridge_server.exe"
 *
 * UPDATE THESE TWO LINES to your actual appcode folder location:
 */
#define APPCODE_DIR    "C:\\Users\\Staff\\Documents\\2024-2028 B-Tech\\jersam_1\\appcode"
#define RECEIVER_EXE   APPCODE_DIR "\\rtos_app\\rtos_receiver.exe"
#define BRIDGE_EXE     APPCODE_DIR "\\win_bridge\\win_bridge_server.exe"
/* ─────────────────────────────────────────────────────────────── */

/* Global process handles so we can shut them down on exit */
static PROCESS_INFORMATION g_receiver_proc;
static PROCESS_INFORMATION g_bridge_proc;
static volatile int        g_running = 1;

/* ── Console Ctrl+C handler ─────────────────────────────────── */
static BOOL WINAPI ctrl_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        printf("\n[app] Shutting down...\n");
        g_running = 0;
        return TRUE;
    }
    return FALSE;
}

/* ── Launch a child process ─────────────────────────────────── */
/*
 * start_process – Launches an .exe with arguments in a new console window.
 * Returns 1 on success, 0 on failure.
 */
static int start_process(const char *exe, const char *args,
                          PROCESS_INFORMATION *out_pi)
{
    char cmd[MAX_PATH_LEN];
    if (args && args[0])
        snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    else
        snprintf(cmd, sizeof(cmd), "\"%s\"", exe);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(out_pi, sizeof(*out_pi));

    /* Each server gets its own visible console window */
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    if (!CreateProcessA(
            NULL,           /* application name (in cmd) */
            cmd,            /* command line              */
            NULL,           /* process security          */
            NULL,           /* thread security           */
            TRUE,           /* inherit handles           */
            CREATE_NEW_CONSOLE,
            NULL,           /* environment               */
            NULL,           /* working directory         */
            &si,
            out_pi)) {
        fprintf(stderr, "[app] Failed to start: %s (error %lu)\n",
                exe, GetLastError());
        return 0;
    }
    printf("[app] Started: %s (PID %lu)\n", exe, out_pi->dwProcessId);
    return 1;
}

/* ── Stop a child process gracefully ────────────────────────── */
static void stop_process(PROCESS_INFORMATION *pi, const char *name)
{
    if (pi->hProcess == NULL) return;
    printf("[app] Stopping %s (PID %lu)...\n", name, pi->dwProcessId);
    TerminateProcess(pi->hProcess, 0);
    WaitForSingleObject(pi->hProcess, 2000);
    CloseHandle(pi->hProcess);
    CloseHandle(pi->hThread);
    pi->hProcess = NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  FILE PROCESSING HOOK
 *  ════════════════════
 *  Edit this function to define what your RTOS app does with
 *  each received file.
 *
 *  filepath  – full path e.g. C:\rtos_received\data.bin
 *  filename  – just the name e.g. data.bin
 * ══════════════════════════════════════════════════════════════ */
static void on_file_received(const char *filepath, const char *filename)
{
    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║  NEW FILE RECEIVED: %-20s║\n", filename);
    printf("╚══════════════════════════════════════════╝\n");

    /* ── Read and display the file ───────────────────────────── */
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        fprintf(stderr, "[app] Cannot open file: %s\n", filepath);
        return;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    printf("[app] File size : %ld bytes\n", size);
    printf("[app] Saved at  : %s\n\n", filepath);

    /* Print first 512 bytes as text preview */
    long preview = size < 512 ? size : 512;
    char *buf = (char *)malloc((size_t)preview + 1);
    if (buf) {
        fread(buf, 1, (size_t)preview, fp);
        buf[preview] = '\0';
        printf("── File preview ───────────────────────────\n");
        printf("%s", buf);
        if (size > 512)
            printf("\n... (%ld more bytes) ...", size - 512);
        printf("\n───────────────────────────────────────────\n\n");
        free(buf);
    }
    fclose(fp);

    /*
     * ── Add your RTOS task logic here ─────────────────────────
     *
     * Examples:
     *   parse_sensor_data(filepath);
     *   load_firmware_update(filepath);
     *   queue_for_processing(filepath);
     *
     * If using FreeRTOS / ThreadX / Zephyr tasks, signal them:
     *   xTaskNotifyGive(xProcessorTask);           // FreeRTOS
     *   tx_event_flags_set(&evt, 0x01, TX_OR);     // ThreadX
     *   k_sem_give(&file_sem);                     // Zephyr
     * ─────────────────────────────────────────────────────────
     */
}

/* ── File watcher ───────────────────────────────────────────── */
/*
 * watch_directory – Monitors `dir` for new files.
 * Uses ReadDirectoryChangesW for instant notification (no polling delay).
 * Falls back on new file detection by tracking seen filenames.
 */
static void watch_directory(const char *dir)
{
    printf("[app] Watching for files in: %s\n\n", dir);

    /* Convert to wide string for Windows API */
    wchar_t wdir[MAX_PATH_LEN];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH_LEN);

    HANDLE h_dir = CreateFileW(
        wdir,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);

    if (h_dir == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[app] Cannot watch dir (error %lu): %s\n",
                GetLastError(), dir);
        fprintf(stderr, "[app] Make sure the directory exists.\n");
        return;
    }

    /* Buffer for change notifications */
    BYTE change_buf[8192];
    OVERLAPPED overlapped;
    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    /* Start first async watch */
    ReadDirectoryChangesW(
        h_dir, change_buf, sizeof(change_buf),
        FALSE,   /* do not watch subdirectories */
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE,
        NULL, &overlapped, NULL);

    while (g_running) {
        DWORD wait = WaitForSingleObject(overlapped.hEvent, 500);

        if (!g_running) break;

        if (wait == WAIT_OBJECT_0) {
            DWORD bytes_returned = 0;
            if (!GetOverlappedResult(h_dir, &overlapped, &bytes_returned, FALSE)
                || bytes_returned == 0) {
                goto next_watch;
            }

            /* Walk the change records */
            BYTE *ptr = change_buf;
            for (;;) {
                FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)ptr;

                /* Only care about new files being created or renamed-to */
                if (info->Action == FILE_ACTION_ADDED ||
                    info->Action == FILE_ACTION_RENAMED_NEW_NAME) {

                    /* Convert wide filename to narrow */
                    char fname[MAX_PATH_LEN] = {0};
                    WideCharToMultiByte(CP_ACP, 0,
                        info->FileName,
                        (int)(info->FileNameLength / sizeof(WCHAR)),
                        fname, sizeof(fname) - 1, NULL, NULL);

                    /* Wait a moment for the file to finish being written */
                    Sleep(FILE_SETTLE_MS);

                    /* Build full path */
                    char full_path[MAX_PATH_LEN];
                    snprintf(full_path, sizeof(full_path), "%s\\%s", dir, fname);

                    /* Make sure it is a regular file */
                    DWORD attrs = GetFileAttributesA(full_path);
                    if (attrs != INVALID_FILE_ATTRIBUTES &&
                        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        on_file_received(full_path, fname);
                    }
                }

                if (info->NextEntryOffset == 0) break;
                ptr += info->NextEntryOffset;
            }

next_watch:
            ResetEvent(overlapped.hEvent);
            ReadDirectoryChangesW(
                h_dir, change_buf, sizeof(change_buf),
                FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE,
                NULL, &overlapped, NULL);
        }
    }

    CancelIo(h_dir);
    CloseHandle(overlapped.hEvent);
    CloseHandle(h_dir);
}

/* ── Main ───────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *rtos_ip   = (argc >= 2) ? argv[1] : DEFAULT_RTOS_IP;
    const char *watch_dir = (argc >= 3) ? argv[2] : DEFAULT_WATCH_DIR;

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    printf("╔════════════════════════════════════════════╗\n");
    printf("║        AppCoe RTOS File Transfer App       ║\n");
    printf("╚════════════════════════════════════════════╝\n\n");

    /* ── Step 1: Create the save directory ─────────────────── */
    if (!CreateDirectoryA(watch_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[app] Failed to create save dir: %s\n", watch_dir);
        return 1;
    }
    printf("[app] Save directory: %s\n", watch_dir);

    /* ── Step 2: Start RTOS Receiver ────────────────────────── */
    char recv_args[MAX_PATH_LEN];
    snprintf(recv_args, sizeof(recv_args), "\"%s\"", watch_dir);

    printf("[app] Starting RTOS Receiver...\n");
    if (!start_process(RECEIVER_EXE, recv_args, &g_receiver_proc)) {
        fprintf(stderr, "[app] Could not start receiver. "
                "Make sure %s exists.\n", RECEIVER_EXE);
        return 1;
    }
    Sleep(1000);   /* let the receiver bind its port first */

    /* ── Step 3: Start Bridge Server ────────────────────────── */
    printf("[app] Starting Bridge Server (→ %s)...\n", rtos_ip);
    if (!start_process(BRIDGE_EXE, rtos_ip, &g_bridge_proc)) {
        fprintf(stderr, "[app] Could not start bridge. "
                "Make sure %s exists.\n", BRIDGE_EXE);
        stop_process(&g_receiver_proc, "rtos_receiver");
        return 1;
    }
    Sleep(500);

    printf("\n[app] Both servers running. Waiting for files from Pi...\n");
    printf("[app] Press Ctrl+C to stop everything.\n\n");

    /* ── Step 4: Watch for incoming files ───────────────────── */
    watch_directory(watch_dir);

    /* ── Step 5: Cleanup on exit ────────────────────────────── */
    stop_process(&g_bridge_proc,   "win_bridge_server");
    stop_process(&g_receiver_proc, "rtos_receiver");

    printf("[app] All servers stopped. Goodbye.\n");
    return 0;
}
