#!/bin/bash
# Main dispatcher for Waveshare 7inch DSI LCD (C) installation
# Target: kernel 6.12.47+rpt-rpi-v8 (aarch64)
#
# Detects the Raspberry Pi board model and runs the appropriate
# per-board installation script.
#
# Supported boards:
#   Raspberry Pi 4 Model B
#   Raspberry Pi Compute Module 4 (CM4)
#
# Usage:
#   chmod +x WS_7inchDSI1024x600_MAIN.sh
#   sudo ./WS_7inchDSI1024x600_MAIN.sh

HARDWARE_PATH="/proc/device-tree/model"

detect_hardware() {
    if grep -q "Raspberry Pi 4" "$HARDWARE_PATH" 2>/dev/null; then
        echo "pi4"
    elif grep -q "Raspberry Pi Compute Module 4" "$HARDWARE_PATH" 2>/dev/null; then
        echo "cm4"
    elif grep -q "Raspberry Pi 3" "$HARDWARE_PATH" 2>/dev/null; then
        echo "pi3"
    elif grep -q "Raspberry Pi Compute Module 3" "$HARDWARE_PATH" 2>/dev/null; then
        echo "cm3"
    else
        echo "unknown"
    fi
}

HW=$(detect_hardware)

case "$HW" in
    pi4)
        echo "Detected: Raspberry Pi 4"
        cd ./pi4
        ;;
    cm4)
        echo "Detected: Raspberry Pi Compute Module 4"
        cd ./pi4
        ;;
    pi3|cm3)
        echo "ERROR: This driver package (64-bit, kernel 6.12.47) is for"
        echo "       Raspberry Pi 4 / CM4 only."
        echo "       For Raspberry Pi 3 / CM3, use the 32-bit driver in 32/"
        exit 1
        ;;
    *)
        echo "ERROR: Unrecognised hardware.  Supported boards:"
        echo "         Raspberry Pi 4 Model B"
        echo "         Raspberry Pi Compute Module 4"
        exit 1
        ;;
esac

chmod +x WS_7inchDSI1024x600.sh
sudo ./WS_7inchDSI1024x600.sh
