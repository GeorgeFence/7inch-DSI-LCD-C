// SPDX-License-Identifier: GPL-2.0
/*
 * Waveshare 7inch DSI LCD (C) 1024x600 Screen Driver
 *
 * Copyright (C) Waveshare
 *
 * Based on panel-jdi-lt070me05000.c
 * Authors: Vinay Simha BN <simhavcs@gmail.com>
 *          Sumit Semwal <sumit.semwal@linaro.org>
 *
 * Updated for Linux kernel 6.12.62+rpt-rpi-v8 (aarch64)
 *
 * Hardware:   Waveshare 7inch DSI LCD (C), 1024x600 @ 60 Hz
 * Interface:  MIPI DSI (2 lanes, RGB888)
 * Backlight:  I2C-connected ATtiny MCU on the LCD board
 *             Bus number supplied via DT property "I2C_bus"
 *             I2C address: 0x45
 *             Brightness register: 0x86  (0–255)
 *             Power register:      0x85  (1 = on, 0 = off)
 *
 * Kernel API changes handled vs 5.15.x:
 *   - mipi_dsi_driver_register_full() removed → module_mipi_dsi_driver()
 *   - .remove() in mipi_dsi_driver returns void (since 6.0)
 *   - devm_backlight_device_register() preferred for backlight
 *   - Boot config at /boot/firmware/config.txt (Bookworm / RPi OS 64-bit)
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/version.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

/* ---------------------------------------------------------------------------
 * I2C backlight / power controller (ATtiny MCU on display board)
 * ---------------------------------------------------------------------------
 */
#define WS_BL_I2C_ADDR          0x45

/* Register map exposed by the ATtiny MCU (verified from 5.x pre-built binaries) */
#define WS_REG_PWR              0xAD  /* 1 = display on, 0 = display off   */
#define WS_REG_BRIGHTNESS       0xAB  /* 0 (off) … 254 (full brightness)   */
#define WS_REG_BRIGHTNESS_APPLY 0xAA  /* write 1 to commit brightness      */

#define WS_MAX_BRIGHTNESS       254
#define WS_DEFAULT_BRIGHTNESS   128

/* ---------------------------------------------------------------------------
 * Display timing (extracted from 5.15.32 pre-built binary, rodata section)
 * Resolution: 1024 × 600, pixel clock 50 MHz → ~60 Hz
 * ---------------------------------------------------------------------------
 */
#define WS_HACTIVE              1024
#define WS_HSYNC_START          1124  /* HFP = 100 px */
#define WS_HSYNC_END            1224  /* HSW = 100 px */
#define WS_HTOTAL               1324  /* HBP = 100 px */

#define WS_VACTIVE               600
#define WS_VSYNC_START           610  /* VFP = 10 ln  */
#define WS_VSYNC_END             620  /* VSW = 10 ln  */
#define WS_VTOTAL                630  /* VBP = 10 ln  */

#define WS_PIXEL_CLK_KHZ       50000  /* 50 MHz → 50 000 kHz               */

/* ---------------------------------------------------------------------------
 * DSI configuration
 * ---------------------------------------------------------------------------
 */
#define WS_DSI_LANES               2
#define WS_DSI_FORMAT    MIPI_DSI_FMT_RGB888

/* Physical dimensions of the visible area (mm) */
#define WS_WIDTH_MM               154
#define WS_HEIGHT_MM               85

/* ---------------------------------------------------------------------------
 * Driver private state
 * ---------------------------------------------------------------------------
 */
struct ws_panel {
	struct drm_panel        base;
	struct mipi_dsi_device  *dsi;

	/* I2C backlight controller */
	struct i2c_adapter      *i2c_adap;
	struct i2c_client       *i2c_client;
	struct regmap           *regmap;

	struct backlight_device *backlight;

	bool    prepared;
	bool    enabled;
	u8      brightness;  /* cached brightness (0–255) */
};

static inline struct ws_panel *panel_to_ws(struct drm_panel *panel)
{
	return container_of(panel, struct ws_panel, base);
}

/* ---------------------------------------------------------------------------
 * regmap configuration for the I2C backlight MCU
 * ---------------------------------------------------------------------------
 */
static const struct regmap_config ws_regmap_config = {
	.reg_bits   = 8,
	.val_bits   = 8,
	.max_register = 0xFF,
	.cache_type  = REGCACHE_NONE,
};

/* ---------------------------------------------------------------------------
 * I2C helpers
 * ---------------------------------------------------------------------------
 */
static int ws_i2c_write(struct ws_panel *ws, u8 reg, u8 val)
{
	int ret;

	if (!ws->regmap)
		return 0;  /* backlight MCU not present — non-fatal */

	ret = regmap_write(ws->regmap, reg, val);
	if (ret)
		dev_warn(ws->base.dev,
			 "I2C write reg=0x%02x val=0x%02x failed: %d\n",
			 reg, val, ret);
	return ret;
}

static int ws_set_brightness(struct ws_panel *ws, u8 brightness)
{
	int ret;

	ws->brightness = brightness;
	ret = ws_i2c_write(ws, WS_REG_BRIGHTNESS, brightness);
	if (ret)
		return ret;
	return ws_i2c_write(ws, WS_REG_BRIGHTNESS_APPLY, 1);
}

/* ---------------------------------------------------------------------------
 * backlight_ops – hooked into the Linux backlight framework
 * ---------------------------------------------------------------------------
 */
static int ws_bl_update_status(struct backlight_device *bl)
{
	struct ws_panel *ws = bl_get_data(bl);
	u8 brightness;

	if (bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;
	else
		brightness = (u8)bl->props.brightness;

	return ws_set_brightness(ws, brightness);
}

static int ws_bl_get_brightness(struct backlight_device *bl)
{
	struct ws_panel *ws = bl_get_data(bl);

	return ws->brightness;
}

static const struct backlight_ops ws_bl_ops = {
	.update_status  = ws_bl_update_status,
	.get_brightness = ws_bl_get_brightness,
};

/* ---------------------------------------------------------------------------
 * drm_panel_funcs
 * ---------------------------------------------------------------------------
 */

/*
 * ws_panel_prepare – called before the DSI host enables the video stream.
 *
 * Powers on the display via the I2C backlight MCU and waits for the panel
 * to stabilise.  Safe to call multiple times (idempotent).
 */
static int ws_panel_prepare(struct drm_panel *panel)
{
	struct ws_panel *ws = panel_to_ws(panel);

	if (ws->prepared)
		return 0;

	/* Assert panel power */
	ws_i2c_write(ws, WS_REG_PWR, 1);

	/* Allow the panel TCON and display engine to stabilise */
	msleep(120);

	ws->prepared = true;
	dev_dbg(ws->base.dev, "panel prepared\n");
	return 0;
}

/*
 * ws_panel_unprepare – called after the video stream has been disabled.
 *
 * Powers off the display so the panel can be left in a low-power state.
 */
static int ws_panel_unprepare(struct drm_panel *panel)
{
	struct ws_panel *ws = panel_to_ws(panel);

	if (!ws->prepared)
		return 0;

	ws_i2c_write(ws, WS_REG_PWR, 0);
	msleep(10);

	ws->prepared = false;
	dev_dbg(ws->base.dev, "panel unprepared\n");
	return 0;
}

/*
 * ws_panel_enable – called after the display engine starts generating pixels.
 *
 * Turns on the backlight to make the image visible.
 */
static int ws_panel_enable(struct drm_panel *panel)
{
	struct ws_panel *ws = panel_to_ws(panel);

	if (ws->enabled)
		return 0;

	if (ws->backlight) {
		ws->backlight->props.state &= ~BL_CORE_FBBLANK;
		backlight_update_status(ws->backlight);
	} else {
		u8 br = ws->brightness ? ws->brightness : WS_DEFAULT_BRIGHTNESS;

		ws_set_brightness(ws, br);
	}

	ws->enabled = true;
	dev_dbg(ws->base.dev, "panel enabled\n");
	return 0;
}

/*
 * ws_panel_disable – called before the display engine stops.
 *
 * Turns off the backlight while keeping the panel powered so it can be
 * quickly re-enabled without the long prepare delay.
 */
static int ws_panel_disable(struct drm_panel *panel)
{
	struct ws_panel *ws = panel_to_ws(panel);

	if (!ws->enabled)
		return 0;

	if (ws->backlight) {
		ws->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(ws->backlight);
	} else {
		ws_set_brightness(ws, 0);
	}

	ws->enabled = false;
	dev_dbg(ws->base.dev, "panel disabled\n");
	return 0;
}

/*
 * ws_panel_get_modes – advertise the single native mode to DRM.
 *
 * Returns the number of modes added (1 on success, negative on error).
 */
static int ws_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return -ENOMEM;

	/*
	 * Timings extracted from the 5.15.32 pre-built binary (.rodata):
	 *   clock  = 50000 kHz
	 *   H:  1024 + 100 (FP) + 100 (SW) + 100 (BP) = 1324
	 *   V:   600 +  10 (FP) +  10 (SW) +  10 (BP) =  630
	 *   => ~60 Hz
	 */
	mode->clock       = WS_PIXEL_CLK_KHZ;
	mode->hdisplay    = WS_HACTIVE;
	mode->hsync_start = WS_HSYNC_START;
	mode->hsync_end   = WS_HSYNC_END;
	mode->htotal      = WS_HTOTAL;
	mode->hskew       = 0;
	mode->vdisplay    = WS_VACTIVE;
	mode->vsync_start = WS_VSYNC_START;
	mode->vsync_end   = WS_VSYNC_END;
	mode->vtotal      = WS_VTOTAL;
	mode->vscan       = 0;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm  = WS_WIDTH_MM;
	connector->display_info.height_mm = WS_HEIGHT_MM;

	return 1;
}

static const struct drm_panel_funcs ws_panel_funcs = {
	.prepare   = ws_panel_prepare,
	.unprepare = ws_panel_unprepare,
	.enable    = ws_panel_enable,
	.disable   = ws_panel_disable,
	.get_modes = ws_panel_get_modes,
};

/* ---------------------------------------------------------------------------
 * I2C backlight client setup
 *
 * The screen driver creates its own I2C client programmatically so that the
 * backlight MCU can be addressed without a separate DT node.  The I2C bus
 * number is read from the "I2C_bus" DT property (u32).
 * ---------------------------------------------------------------------------
 */
static int ws_setup_backlight_i2c(struct ws_panel *ws,
				  struct device *dev)
{
	struct i2c_board_info binfo = {
		/*
		 * Use a driver-specific name to avoid binding the upstream
		 * rpi_backlight driver to this device.
		 */
		I2C_BOARD_INFO("ws-backlight", WS_BL_I2C_ADDR),
	};
	struct backlight_properties bl_props = {
		.type       = BACKLIGHT_RAW,
		.max_brightness = WS_MAX_BRIGHTNESS,
		.brightness = WS_DEFAULT_BRIGHTNESS,
	};
	u32 bus_nr = 0;
	int ret;

	/* Read I2C bus number from DT; fall back to bus 0 if absent */
	ret = of_property_read_u32(dev->of_node, "I2C_bus", &bus_nr);
	if (ret)
		dev_info(dev, "I2C_bus not set in DT, using bus 0\n");

	ws->i2c_adap = i2c_get_adapter(bus_nr);
	if (!ws->i2c_adap) {
		dev_warn(dev, "I2C adapter %u not found, backlight disabled\n",
			 bus_nr);
		return 0;  /* non-fatal — panel still works without backlight */
	}

	ws->i2c_client = i2c_new_client_device(ws->i2c_adap, &binfo);
	if (IS_ERR(ws->i2c_client)) {
		dev_warn(dev, "Failed to register backlight I2C client: %ld\n",
			 PTR_ERR(ws->i2c_client));
		ws->i2c_client = NULL;
		i2c_put_adapter(ws->i2c_adap);
		ws->i2c_adap = NULL;
		return 0;
	}

	ws->regmap = devm_regmap_init_i2c(ws->i2c_client,
					  &ws_regmap_config);
	if (IS_ERR(ws->regmap)) {
		dev_warn(dev, "Failed to init regmap: %ld\n",
			 PTR_ERR(ws->regmap));
		ws->regmap = NULL;
	}

	/* Register with the backlight framework */
	ws->backlight = devm_backlight_device_register(dev,
				"ws-backlight", dev, ws,
				&ws_bl_ops, &bl_props);
	if (IS_ERR(ws->backlight)) {
		dev_warn(dev, "Failed to register backlight device: %ld\n",
			 PTR_ERR(ws->backlight));
		ws->backlight = NULL;
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * MIPI DSI driver probe / remove
 * ---------------------------------------------------------------------------
 */

/*
 * ws_panel_probe – called when the DSI host matches this driver.
 *
 * Initialises the private state, creates the I2C backlight client,
 * registers the DRM panel, and attaches to the DSI host.
 */
static int ws_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ws_panel *ws;
	int ret;

	ws = devm_kzalloc(dev, sizeof(*ws), GFP_KERNEL);
	if (!ws)
		return -ENOMEM;

	ws->dsi        = dsi;
	ws->brightness = WS_DEFAULT_BRIGHTNESS;

	mipi_dsi_set_drvdata(dsi, ws);

	/* Configure the DSI link */
	dsi->lanes      = WS_DSI_LANES;
	dsi->format     = WS_DSI_FORMAT;
	/*
	 * MIPI_DSI_MODE_VIDEO_BURST: burst mode (required by the panel TCON)
	 * MIPI_DSI_MODE_LPM:         use low-power mode for DCS commands
	 * MIPI_DSI_MODE_NO_EOT_PACKET: suppress End-of-Transmission packets
	 *                              (panel does not expect them)
	 */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO      |
			  MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM         |
			  MIPI_DSI_MODE_NO_EOT_PACKET;

	/* Optionally set up I2C backlight controller */
	ret = ws_setup_backlight_i2c(ws, dev);
	if (ret)
		return ret;

	/*
	 * Register the DRM panel.
	 *
	 * drm_panel_init() API (stable since 5.12):
	 *   drm_panel_init(panel, dev, funcs, connector_type)
	 */
	drm_panel_init(&ws->base, dev, &ws_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ws->base);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
		drm_panel_remove(&ws->base);
		return ret;
	}

	dev_info(dev, "Waveshare 7inch DSI LCD (C) 1024x600 probed\n");
	return 0;
}

/*
 * ws_panel_remove – called when the DSI device is unbound.
 *
 * Note: return type is void since kernel 6.0.  Use the compatibility
 * guard below for building against older kernels.
 *
 * Kernel compatibility:
 *   < 6.0  : int remove(struct mipi_dsi_device *)
 *   >= 6.0 : void remove(struct mipi_dsi_device *)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
static void ws_panel_remove(struct mipi_dsi_device *dsi)
#else
static int ws_panel_remove(struct mipi_dsi_device *dsi)
#endif
{
	struct ws_panel *ws = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ws->base);

	if (ws->i2c_client) {
		i2c_unregister_device(ws->i2c_client);
		ws->i2c_client = NULL;
	}
	if (ws->i2c_adap) {
		i2c_put_adapter(ws->i2c_adap);
		ws->i2c_adap = NULL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
	return 0;
#endif
}

/* ---------------------------------------------------------------------------
 * OF match table
 *
 * The compatible string must match what is declared in the DT overlay
 * WS_7inchDSI1024x600_Screen.dts.
 * ---------------------------------------------------------------------------
 */
static const struct of_device_id ws_panel_of_match[] = {
	{ .compatible = "WS_7inchDSI1024x600_Screen_compatible" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ws_panel_of_match);

/* ---------------------------------------------------------------------------
 * MIPI DSI driver registration
 *
 * module_mipi_dsi_driver() expands to module_init / module_exit wrappers
 * that call mipi_dsi_driver_register() / mipi_dsi_driver_unregister().
 *
 * This replaces the old mipi_dsi_driver_register_full() / unregister()
 * pattern used in the 5.15 binary (jdi_panel_driver_init/exit).
 * ---------------------------------------------------------------------------
 */
static struct mipi_dsi_driver ws_panel_dsi_driver = {
	.probe  = ws_panel_probe,
	.remove = ws_panel_remove,
	.driver = {
		.name           = "WS_7inchDSI1024x600_Screen",
		.of_match_table = ws_panel_of_match,
	},
};
module_mipi_dsi_driver(ws_panel_dsi_driver);

MODULE_AUTHOR("Vinay Simha BN <simhavcs@gmail.com>");
MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_AUTHOR("Waveshare <www.waveshare.com> (7inch DSI LCD (C) adaptation)");
/*
 * MODULE_DESCRIPTION intentionally matches the original 5.15.32 pre-built
 * binary (verifiable via `modinfo`), which was derived from the JDI panel
 * driver template.
 */
MODULE_DESCRIPTION("JDI LT070ME05000 WUXGA");
MODULE_LICENSE("GPL v2");
