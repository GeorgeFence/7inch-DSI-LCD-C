# 7inch DSI LCD (C)

Related links：

Website: https://www.waveshare.com/7inch-dsi-lcd-c.htm

WIKI: https://www.waveshare.com/wiki/7inch_DSI_LCD_(C)_

These are the drivers for 7inch DSI LCD (C)

Pi3: Support Raspberry Pi 3 Module B+, Raspberry Pi 3 Model A+ and Compute Module 3+

Pi4: Support Raspberry Pi 4 Module B and the Compute Module 4

Please choose the correct driver according to the type of Raspberry Pi board

Backlight: Software for controlling the brightness of 7inch DSI LCD (C), if you do not need to adjust the brightness of display, you needn't to install it.

---

## Supported Kernel Versions

| Directory | Architecture | Kernel | Board |
|-----------|-------------|--------|-------|
| `32/5.10.17` | armv7l (32-bit) | 5.10.17 | Pi3, Pi4 |
| `32/5.10.63` | armv7l (32-bit) | 5.10.63 | Pi3, Pi4 |
| `32/5.10.92` | armv7l (32-bit) | 5.10.92 | Pi3, Pi4 |
| `32/5.10.103` | armv7l (32-bit) | 5.10.103 | Pi3, Pi4 |
| `32/5.15.32` | armv7l (32-bit) | 5.15.32 | Pi3, Pi4 |
| `64/5.10.92` | aarch64 (64-bit) | 5.10.92 | Pi3, Pi4 |
| `64/5.10.103` | aarch64 (64-bit) | 5.10.103 | Pi3, Pi4 |
| `64/5.15.32` | aarch64 (64-bit) | 5.15.32 | Pi3, Pi4 |
| **`64/6.12.47`** | **aarch64 (64-bit)** | **6.12.47+rpt-rpi-v8** | **Pi4, CM4** |

---

## Kernel 6.12.47+rpt-rpi-v8 (aarch64) — Build from Source

The `64/6.12.47/` directory contains **C source code** and build infrastructure
that must be compiled on the target Raspberry Pi against the running kernel headers.

### Target System

- **Board**: Raspberry Pi 4 Model B or Compute Module 4
- **Kernel**: `6.12.47+rpt-rpi-v8 #1 SMP PREEMPT Debian 1:6.12.47-1+rpt1 (2025-09-16) aarch64`
- **OS**: Raspberry Pi OS Bookworm (64-bit)
- **Boot config**: `/boot/firmware/config.txt`

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install raspberrypi-kernel-headers build-essential device-tree-compiler
```

### Quick Install (automated)

```bash
cd 64/6.12.47
chmod +x WS_7inchDSI1024x600_MAIN.sh
sudo ./WS_7inchDSI1024x600_MAIN.sh
sudo reboot
```

The script auto-detects your board (Pi 4 or CM4), builds the modules and
device tree overlays from source, installs them, and updates
`/boot/firmware/config.txt`.

### Manual Build & Install

```bash
cd 64/6.12.47/pi4/Driver_package

# 1. Build kernel modules
make

# 2. Build device tree overlays
make dtbs

# 3. Install (copies files and runs depmod)
make install

# 4. Add to /boot/firmware/config.txt
echo "ignore_lcd=1"                            | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=vc4-kms-v3d"                  | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=WS_7inchDSI1024x600_Screen"   | sudo tee -a /boot/firmware/config.txt
echo "dtparam=i2c_arm=on"                     | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=WS_7inchDSI1024x600_Touch"    | sudo tee -a /boot/firmware/config.txt

sudo reboot
```

### Driver API Documentation

#### Screen Driver (`WS_7inchDSI1024x600_Screen.ko`)

| Item | Value |
|------|-------|
| Module name | `WS_7inchDSI1024x600_Screen` |
| OF compatible | `WS_7inchDSI1024x600_Screen_compatible` |
| Interface | MIPI DSI (2 lanes, RGB888) |
| Resolution | 1024 x 600 @ 60 Hz |
| Pixel clock | 50 MHz |
| Backlight I2C | Bus from DT `I2C_bus` property, addr 0x45 |
| DRM panel funcs | `prepare`, `unprepare`, `enable`, `disable`, `get_modes` |
| Backlight framework | `devm_backlight_device_register()` |

**Initialization sequence:**
1. `ws_panel_probe()` — DSI link configured, I2C backlight client created, DRM panel registered, `mipi_dsi_attach()` called
2. `ws_panel_prepare()` — powers on display via I2C register 0x85, waits 120 ms
3. `ws_panel_enable()` — turns on backlight via I2C register 0x86
4. `ws_panel_disable()` — turns off backlight
5. `ws_panel_unprepare()` — powers off display
6. `ws_panel_remove()` — detaches DSI, removes DRM panel, releases I2C resources

#### Touch Driver (`WS_7inchDSI1024x600_Touch.ko`)

| Item | Value |
|------|-------|
| Module name | `WS_7inchDSI1024x600_Touch` |
| OF compatible | `WS_7inchDSI1024x600_Touch_compatible` |
| I2C ID | `WS1001` |
| I2C address | 0x14 (or 0x5D — change in DTS) |
| Protocol | Goodix GT9xx-compatible |
| Touch points | Up to 10 (chip-dependent) |
| Input mode | Polling at ~125 Hz |
| Multi-touch | `input_mt_init_slots()` / `INPUT_MT_DIRECT` |

**Supported chips** (detected from register 0x8140):
`GT1151`, `GT5663`, `GT5688`, `GT911`, `GT9110`, `GT912`, `GT9147`,
`GT917S`, `GT927`, `GT9271`, `GT928`, `GT9286`, `GT967`

### Kernel Version Compatibility Guards

The source files use `#if LINUX_VERSION_CODE` to remain compilable on both
5.x and 6.x kernels:

```c
/* .remove() return type changed from int to void in kernel 6.0 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
static void ws_panel_remove(struct mipi_dsi_device *dsi)
#else
static int ws_panel_remove(struct mipi_dsi_device *dsi)
#endif
```

### Key API Changes: 5.15 to 6.12

| Old API (5.15) | New API (6.12) | Notes |
|----------------|----------------|-------|
| `mipi_dsi_driver_register_full()` | `module_mipi_dsi_driver()` | macro preferred |
| `int remove(struct mipi_dsi_device *)` | `void remove(...)` | return type change |
| `int remove(struct i2c_client *)` | `void remove(...)` | return type change |
| `kmem_cache_alloc_trace()` | `kmalloc()` | removed in 6.0 |
| `usleep_range_state()` | `usleep_range()` | use standard variant |
| `/boot/config.txt` | `/boot/firmware/config.txt` | Bookworm boot path |
| `/boot/overlays/` | `/boot/firmware/overlays/` | Bookworm overlay path |

---

--------------------------------------------------------------------------------

相关链接：

中文官网：https://www.waveshare.net/shop/7inch-DSI-LCD-C.htm

中文WIKI：https://www.waveshare.net/wiki/7inch_DSI_LCD_(C)_

7inch DSI LCD (C)的驱动程序分为Pi3和Pi4版本

Pi3目录驱动支持的主板有：Raspberry Pi 3 Model B+、Raspberry Pi 3 Model A+和Compute Module 3+

Pi4目录驱动支持的主板有：Raspberry Pi 4 Model B和Compute Module 4

需要根据相应的树莓派版本选择安装对应驱动。

**新增支持 (64/6.12.47)**: 适用于 Raspberry Pi 4 / CM4，内核版本 6.12.47+rpt-rpi-v8 (aarch64)。
此版本提供源码，需要在目标树莓派上编译安装。

Backlight是7inch DSI LCD (C)的背光控制上位机程序，如果你不需要控制7inch DSI LCD (C)的亮度，则可不使用。
