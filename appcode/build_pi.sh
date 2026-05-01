#!/bin/bash
# build_pi.sh
# ===========
# Builds the Pi Sender on Raspberry Pi OS (or any Linux).
# Run:  chmod +x build_pi.sh && ./build_pi.sh

set -e

echo "============================"
echo " Building Pi Sender"
echo "============================"

# Create output directory for sent files
mkdir -p pi_sender/send_files
mkdir -p pi_sender/sent_files

gcc -Wall -o pi_sender/pi_sender pi_sender/pi_sender.c -lpthread
echo "[OK] pi_sender/pi_sender"

echo ""
echo "============================"
echo " Pi build succeeded."
echo "============================"
echo ""
echo "Usage:"
echo "  Copy files to pi_sender/send_files/ then run:"
echo "  ./pi_sender/pi_sender <WINDOWS_BRIDGE_IP>"
echo ""
echo "  Example:"
echo "  ./pi_sender/pi_sender 192.168.1.100"
