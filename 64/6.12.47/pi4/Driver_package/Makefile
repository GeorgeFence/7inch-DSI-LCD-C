# Makefile for Waveshare 7inch DSI LCD (C) out-of-tree kernel modules
#
# Targets:
#   all     – compile both kernel modules (.ko)
#   dtbs    – compile both device tree overlays (.dtbo)
#   install – compile + copy .ko and .dtbo to their system locations
#   clean   – remove all generated files
#
# Usage (on Raspberry Pi 4 running kernel 6.12.47+rpt-rpi-v8):
#
#   sudo apt-get install raspberrypi-kernel-headers device-tree-compiler
#   make
#   make dtbs
#   make install
#   sudo reboot

# Auto-detect the running kernel or allow override:
#   make KERNEL_VERSION=6.12.47+rpt-rpi-v8
KERNEL_VERSION ?= $(shell uname -r)

# Kernel build directory (headers package installs here on Raspberry Pi OS)
KDIR ?= /lib/modules/$(KERNEL_VERSION)/build

# Current directory (source tree)
PWD := $(shell pwd)

# Boot partition (Raspberry Pi OS Bookworm: /boot/firmware)
BOOT_DIR ?= /boot/firmware

# ---------------------------------------------------------------------------
# Module build
# ---------------------------------------------------------------------------
.PHONY: all
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ---------------------------------------------------------------------------
# Device tree overlay compilation
# Requires: device-tree-compiler (apt-get install device-tree-compiler)
# ---------------------------------------------------------------------------
.PHONY: dtbs
dtbs: WS_7inchDSI1024x600_Screen.dtbo WS_7inchDSI1024x600_Touch.dtbo

WS_7inchDSI1024x600_Screen.dtbo: WS_7inchDSI1024x600_Screen.dts
	dtc -@ -I dts -O dtb -o $@ $<

WS_7inchDSI1024x600_Touch.dtbo: WS_7inchDSI1024x600_Touch.dts
	dtc -@ -I dts -O dtb -o $@ $<

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
.PHONY: install
install: all dtbs
	@echo "Installing kernel modules to /lib/modules/$(KERNEL_VERSION)/"
	sudo cp WS_7inchDSI1024x600_Screen.ko /lib/modules/$(KERNEL_VERSION)/
	sudo cp WS_7inchDSI1024x600_Touch.ko  /lib/modules/$(KERNEL_VERSION)/
	@echo "Installing device tree overlays to $(BOOT_DIR)/overlays/"
	sudo cp WS_7inchDSI1024x600_Screen.dtbo $(BOOT_DIR)/overlays/
	sudo cp WS_7inchDSI1024x600_Touch.dtbo  $(BOOT_DIR)/overlays/
	sudo depmod -a $(KERNEL_VERSION)
	@echo "Install complete.  Edit $(BOOT_DIR)/config.txt and reboot."

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.dtbo
