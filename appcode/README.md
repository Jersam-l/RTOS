# File Transfer System – Pi → Windows Bridge → RTOS App

## Architecture

```
  ┌──────────────────────┐        ┌──────────────────────┐        ┌──────────────────────┐
  │   Raspberry Pi OS    │        │   Windows Machine     │        │   Windows Machine     │
  │                      │        │                        │        │   (RTOS App)          │
  │   pi_sender.c        │─TCP──▶│  win_bridge_server.c  │─TCP──▶│  rtos_receiver.c      │
  │   port → 5001        │        │  port 5001 (listen)    │        │  port 5002 (listen)   │
  │                      │        │  port 5002 (connect)   │        │                        │
  │  ./send_files/       │        │  (relay only, no disk) │        │  C:\rtos_received\    │
  └──────────────────────┘        └──────────────────────┘        └──────────────────────┘
```

> If the RTOS app runs on the **same Windows PC** as the bridge, the RTOS IP is `127.0.0.1`.  
> If they are on different machines, provide the RTOS machine's IP when starting the bridge.

---

## File Structure

```
appcode/
├── common/
│   └── file_protocol.h          ← Shared protocol (ports, magic, helpers)
├── pi_sender/
│   └── pi_sender.c              ← Runs on Raspberry Pi (Linux)
├── win_bridge/
│   └── win_bridge_server.c      ← Runs on Windows (relay)
├── rtos_app/
│   └── rtos_receiver.c          ← Runs inside / alongside your RTOS app
├── build_windows.bat            ← Builds both Windows executables (MinGW/MSVC)
└── build_pi.sh                  ← Builds Pi sender on Linux
```

---

## Quick Start

### Step 1 – Build on Windows
Open a terminal in `appcode/` with MinGW `gcc` in your PATH:
```bat
build_windows.bat
```
This produces:
- `win_bridge\win_bridge_server.exe`
- `rtos_app\rtos_receiver.exe`

### Step 2 – Build on Raspberry Pi
```bash
chmod +x build_pi.sh
./build_pi.sh
```
This produces `pi_sender/pi_sender`.

---

## Running (Startup Order Matters)

### On Windows – start RTOS Receiver first
```bat
rtos_app\rtos_receiver.exe C:\rtos_received
```
Waits on **port 5002**, saves every file to `C:\rtos_received\`.

### On Windows – start Bridge Server second
```bat
:: If RTOS app is on the same machine:
win_bridge\win_bridge_server.exe 127.0.0.1

:: If RTOS app is on a different machine:
win_bridge\win_bridge_server.exe 192.168.1.50
```
Waits on **port 5001** for Pi connections.

### On Raspberry Pi – send files
```bash
# Drop any file into send_files/ then run:
cp mydata.bin pi_sender/send_files/
./pi_sender/pi_sender 192.168.1.100   # ← IP of the Windows bridge machine
```
Each file is sent then moved to `pi_sender/sent_files/` so it is not resent.

---

## Transfer Protocol

All multi-byte integers are **network byte order (big-endian)**.

```
Offset  Size  Field
──────  ────  ──────────────────────────────────────────────────
 0       4    Magic = 0xFEEDBEEF     (identifies our protocol)
 4       4    filename_len           (uint32)
 8       N    filename (UTF-8, no NUL)
 8+N     8    file_size              (uint64)
 8+N+8   M    raw file bytes
```

After all data is received the receiver sends 1 byte ACK:
- `'A'` – file saved successfully
- `'E'` – error (file rejected)

---

## Integrating `rtos_receiver` into Your RTOS Task

`rtos_receiver.c` exposes:
```c
void rtos_receiver_run(const char *save_dir);
```

Call it from a dedicated RTOS task instead of `main()`:

```c
/* FreeRTOS example */
void vFileReceiverTask(void *pvParameters)
{
    rtos_receiver_run("C:\\rtos_received");
    vTaskDelete(NULL);   /* never returns normally */
}

/* In your app init: */
xTaskCreate(vFileReceiverTask, "FileRecv", 4096, NULL, 2, NULL);
```

Inside `rtos_receiver.c`, find the **"RTOS app notification hook"** comment  
and add your event/semaphore signal so the processing task knows a file arrived:

```c
/* FreeRTOS */    xTaskNotifyGive(xFileProcessorTask);
/* ThreadX  */    tx_event_flags_set(&g_evt, FILE_READY_FLAG, TX_OR);
/* Zephyr   */    k_sem_give(&file_ready_sem);
```

---

## Port Reference

| Port | Direction              | Purpose                 |
|------|------------------------|-------------------------|
| 5001 | Pi → Windows Bridge    | Pi uploads files        |
| 5002 | Windows Bridge → RTOS  | Bridge forwards files   |

Both ports are defined in `common/file_protocol.h` and can be changed there.

---

## Firewall Notes (Windows)

Allow inbound TCP on ports **5001** and **5002**:
```bat
netsh advfirewall firewall add rule name="RTOS Bridge 5001" ^
      dir=in action=allow protocol=TCP localport=5001
netsh advfirewall firewall add rule name="RTOS Receiver 5002" ^
      dir=in action=allow protocol=TCP localport=5002
```
