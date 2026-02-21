#!/bin/bash
# Installation script for Waveshare 7inch DSI LCD (C)
# Target: Raspberry Pi 4 / CM4, kernel 6.12.62+rpt-rpi-v8 (aarch64)
#
# This script:
#   1. Builds the kernel modules from source
#   2. Builds the device tree overlays
#   3. Installs modules and overlays
#   4. Updates /boot/firmware/config.txt (Raspberry Pi OS Bookworm)
#
# Run as: sudo ./WS_7inchDSI1024x600.sh
#         (or without sudo; the script will call sudo internally)

set -e

# ---------------------------------------------------------------------------
# Helper: append a line to a file only if it is not already present
# ---------------------------------------------------------------------------
data_insertion() {
    local file="$1"
    local line="$2"
    if grep -qxF "$line" "$file" 2>/dev/null; then
        echo "  Already present: $line"
    else
        echo "$line" | sudo tee -a "$file" > /dev/null
        echo "  Added: $line"
    fi
}

# ---------------------------------------------------------------------------
# Detect boot partition path (Bookworm: /boot/firmware, Bullseye: /boot)
# ---------------------------------------------------------------------------
if [ -d /boot/firmware ]; then
    BOOT_DIR="/boot/firmware"
else
    BOOT_DIR="/boot"
fi
echo "Boot partition: $BOOT_DIR"

# ---------------------------------------------------------------------------
# Check for required tools
# ---------------------------------------------------------------------------
for tool in make dtc depmod; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: '$tool' not found."
        echo "Install with: sudo apt-get install build-essential device-tree-compiler"
        exit 1
    fi
done

KERNEL_VERSION=$(uname -r)
KDIR="/lib/modules/${KERNEL_VERSION}/build"

if [ ! -d "$KDIR" ]; then
    echo "ERROR: Kernel headers not found at $KDIR"
    echo "Install with: sudo apt-get install raspberrypi-kernel-headers"
    exit 1
fi

# ---------------------------------------------------------------------------
# Move into the driver package directory
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/Driver_package"

echo "Driver Start Settings"

# ---------------------------------------------------------------------------
# Build kernel modules
# ---------------------------------------------------------------------------
echo "Building kernel modules for kernel ${KERNEL_VERSION}..."
make KERNEL_VERSION="$KERNEL_VERSION"

# ---------------------------------------------------------------------------
# Build device tree overlays
# ---------------------------------------------------------------------------
echo "Building device tree overlays..."
make dtbs

# ---------------------------------------------------------------------------
# Install modules
# ---------------------------------------------------------------------------
echo "Installing kernel modules..."
sudo cp WS_7inchDSI1024x600_Touch.ko  /lib/modules/"${KERNEL_VERSION}"/
sudo cp WS_7inchDSI1024x600_Screen.ko /lib/modules/"${KERNEL_VERSION}"/

# ---------------------------------------------------------------------------
# Install device tree overlays
# ---------------------------------------------------------------------------
echo "Installing device tree overlays to ${BOOT_DIR}/overlays/..."
sudo cp WS_7inchDSI1024x600_Screen.dtbo "${BOOT_DIR}/overlays/"
sudo cp WS_7inchDSI1024x600_Touch.dtbo  "${BOOT_DIR}/overlays/"

# ---------------------------------------------------------------------------
# Update module dependencies and load modules
# ---------------------------------------------------------------------------
sudo depmod
sudo modprobe WS_7inchDSI1024x600_Touch  || true
sudo modprobe WS_7inchDSI1024x600_Screen || true

# ---------------------------------------------------------------------------
# Update config.txt
# ---------------------------------------------------------------------------
CONFIG="${BOOT_DIR}/config.txt"
echo "Updating ${CONFIG}..."

data_insertion "$CONFIG" "ignore_lcd=1"
data_insertion "$CONFIG" "dtoverlay=vc4-kms-v3d"
data_insertion "$CONFIG" "dtoverlay=WS_7inchDSI1024x600_Screen"
data_insertion "$CONFIG" "dtparam=i2c_arm=on"
data_insertion "$CONFIG" "dtoverlay=WS_7inchDSI1024x600_Touch"

echo ""
echo "Driver end Settings"
echo ""
echo "Installation complete.  Please reboot to activate the display."
echo "  sudo reboot"
