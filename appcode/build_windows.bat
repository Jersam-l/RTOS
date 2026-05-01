@echo off
REM build_windows.bat
REM =================
REM Builds all three Windows components:
REM   1. win_bridge_server  (Pi relay)
REM   2. rtos_receiver      (file saver)
REM   3. rtos_main_app      (launcher - run this one to start everything)
REM
REM Run this on the Windows machine with MinGW gcc in PATH.

setlocal

echo ============================
echo  Building Windows Bridge Server
echo ============================
gcc -Wall -o win_bridge\win_bridge_server.exe win_bridge\win_bridge_server.c -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] win_bridge_server build failed.
    exit /b 1
)
echo [OK] win_bridge\win_bridge_server.exe

echo.
echo ============================
echo  Building RTOS Receiver
echo ============================
gcc -Wall -o rtos_app\rtos_receiver.exe rtos_app\rtos_receiver.c -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] rtos_receiver build failed.
    exit /b 1
)
echo [OK] rtos_app\rtos_receiver.exe

echo.
echo ============================
echo  Building RTOS Main App (Launcher)
echo ============================
gcc -Wall -o rtos_launcher\rtos_main_app.exe rtos_launcher\rtos_main_app.c -lws2_32
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] rtos_main_app build failed.
    exit /b 1
)
echo [OK] rtos_launcher\rtos_main_app.exe

echo.
echo ============================
echo  All Windows builds succeeded.
echo ============================
echo.
echo HOW TO RUN:
echo   Just run the launcher - it starts everything automatically:
echo.
echo   rtos_launcher\rtos_main_app.exe
echo.
echo   With custom RTOS IP (if receiver is on a different machine):
echo   rtos_launcher\rtos_main_app.exe 192.168.1.50
echo.
echo   With custom save folder:
echo   rtos_launcher\rtos_main_app.exe 127.0.0.1 D:\my_files
endlocal
