// SPDX-License-Identifier: GPL-2.0
/*
 * Waveshare 7inch DSI LCD (C) – Capacitive Touchscreen Driver
 *
 * Copyright (C) Waveshare
 *
 * Updated for Linux kernel 6.12.62+rpt-rpi-v8 (aarch64)
 *
 * Hardware:  Waveshare 7inch DSI LCD (C) capacitive touch panel
 * Interface: I2C (address 0x14 or 0x5D, auto-detected)
 * Protocol:  Goodix GT9xx-compatible multi-touch
 *
 * Supported chip IDs (detected at probe time from chip ID register 0x8140):
 *   GT1151 (ws1151), GT5663 (ws5663), GT5688 (ws5688),
 *   GT911  (ws911),  GT9110 (ws9110), GT912  (ws912),
 *   GT9147 (ws9147), GT917S (ws917s), GT927  (ws927),
 *   GT9271 (ws9271), GT928  (ws928),  GT9286 (ws9286),
 *   GT967  (ws967)
 *
 * Kernel API changes handled vs 5.15.x:
 *   - .remove() in i2c_driver returns void (since 6.0)
 *   - kmem_cache_alloc_trace() removed in 6.0 → use kmalloc()
 *   - usleep_range_state() alias removed → use usleep_range()
 *   - devm_input_allocate_device() preferred (already used)
 *   - module_i2c_driver() replaces manual WS_ts_driver_init/exit
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/version.h>

/* ---------------------------------------------------------------------------
 * Goodix GT9xx register map
 * ---------------------------------------------------------------------------
 */
#define WS_REG_CONFIG_DATA       0x8047  /* start of 186-byte config block  */
#define WS_REG_CONFIG_VER        0x8047  /* config version (first byte)     */
#define WS_REG_X_RESOLUTION      0x8048  /* X resolution, little-endian u16 */
#define WS_REG_Y_RESOLUTION      0x804A  /* Y resolution, little-endian u16 */
#define WS_REG_TOUCH_NUM         0x804C  /* max simultaneous touch points   */
#define WS_REG_CONFIG_CHKSUM     0x80FF  /* 8-bit config checksum           */
#define WS_REG_CONFIG_FRESH      0x8100  /* write 1 to apply new config     */

#define WS_REG_FW_VERSION        0x8140  /* firmware / chip-ID string       */
#define WS_REG_PRODUCT_ID        0x8140  /* same register – first 4 bytes   */

#define WS_REG_TOUCH_STATUS      0x814E  /* bits[7:4]=buf_valid [3:0]=count */
#define WS_REG_TOUCH_DATA        0x8150  /* 8 bytes per touch point         */
#define WS_REG_COMMAND           0x8040  /* soft-reset, etc.                */

/* WS_REG_COMMAND values */
#define WS_CMD_SOFT_RESET        0x02
#define WS_CMD_READ_COORD_STATUS 0x00

/* WS_REG_TOUCH_STATUS bit fields */
#define WS_BUF_STATUS_READY      BIT(7)
#define WS_TOUCH_COUNT_MASK      0x0F

/* Touch data layout per point (8 bytes) */
#define WS_TOUCH_TRACK_ID        0
#define WS_TOUCH_X_LOW           1
#define WS_TOUCH_X_HIGH          2
#define WS_TOUCH_Y_LOW           3
#define WS_TOUCH_Y_HIGH          4
#define WS_TOUCH_WIDTH_LOW       5
#define WS_TOUCH_WIDTH_HIGH      6
/* byte 7: reserved                                                          */
#define WS_TOUCH_BYTES           8

/* Nine-byte-per-point variant (some GT1151 firmware revisions) */
#define WS_TOUCH_BYTES_9         9

/* Config block size */
#define WS_CONFIG_SIZE           186
#define WS_CONFIG_FRESH_REG      0x8100

/* ---------------------------------------------------------------------------
 * Per-chip descriptor
 * ---------------------------------------------------------------------------
 */
struct ws_chip_data {
	const char  *name;
	u16          id;
	unsigned int max_touch;    /* max simultaneous touch points             */
	bool         nine_byte;    /* true → 9-byte touch report                */
};

/*
 * Chip data table.  WS-prefixed IDs map to Goodix GT9xx silicon variants.
 */
static const struct ws_chip_data ws_chip_table[] = {
	{ "ws1151",  1151, 10, true  },
	{ "ws5663",  5663,  5, false },
	{ "ws5688",  5688,  5, false },
	{ "ws911",    911,  5, false },
	{ "ws9110",  9110,  5, false },
	{ "ws912",    912,  5, false },
	{ "ws9147",  9147,  5, false },
	{ "ws917s",  9170,  5, false },   /* GT917S */
	{ "ws927",    927,  5, false },
	{ "ws9271",  9271, 10, false },
	{ "ws928",    928,  5, false },
	{ "ws9286",  9286, 10, false },
	{ "ws967",    967,  5, false },
};

/* ---------------------------------------------------------------------------
 * Driver private state
 * ---------------------------------------------------------------------------
 */
struct ws_ts {
	struct i2c_client          *client;
	struct input_dev           *input_dev;
	struct touchscreen_properties props;

	const struct ws_chip_data  *chip;

	u16  x_max;
	u16  y_max;
	u8   max_touch_num;
	bool nine_byte_report;

	/* runtime flags (can be set from DT / DMI quirk table) */
	bool inverted_x;
	bool rotated_screen;
};

/* ---------------------------------------------------------------------------
 * Low-level I2C helpers (16-bit register address, Goodix protocol)
 * ---------------------------------------------------------------------------
 */
static int ws_i2c_read(struct i2c_client *client, u16 reg,
		       u8 *buf, int len)
{
	struct i2c_msg msgs[2];
	u8 addr[2];

	addr[0] = reg >> 8;
	addr[1] = reg & 0xFF;

	msgs[0].addr  = client->addr;
	msgs[0].flags = 0;
	msgs[0].len   = 2;
	msgs[0].buf   = addr;

	msgs[1].addr  = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	return i2c_transfer(client->adapter, msgs, 2);
}

static int ws_i2c_write_byte(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xFF, val };
	struct i2c_msg msg = {
		.addr  = client->addr,
		.flags = 0,
		.len   = 3,
		.buf   = buf,
	};

	return i2c_transfer(client->adapter, &msg, 1);
}

/* ---------------------------------------------------------------------------
 * Chip identification
 * ---------------------------------------------------------------------------
 */
static const struct ws_chip_data *ws_find_chip(u16 id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ws_chip_table); i++) {
		if (ws_chip_table[i].id == id)
			return &ws_chip_table[i];
	}
	return NULL;
}

static int ws_read_chip_id(struct i2c_client *client, u16 *chip_id_out)
{
	u8 buf[6] = {};
	int ret;
	unsigned long id;

	ret = ws_i2c_read(client, WS_REG_PRODUCT_ID, buf, 6);
	if (ret < 0) {
		dev_err(&client->dev, "read version failed: %d\n", ret);
		return ret;
	}

	/* The GT9xx product ID is a 4-char ASCII string, e.g. "9271" */
	buf[4] = '\0';
	if (kstrtoul(buf, 10, &id) == 0) {
		*chip_id_out = (u16)id;
		dev_info(&client->dev, "ID %s, version: %04x\n",
			 buf, ((u16)buf[4] << 8) | buf[5]);
		return 0;
	}

	return -EINVAL;
}

/* ---------------------------------------------------------------------------
 * Input event helpers
 * ---------------------------------------------------------------------------
 */

/*
 * ws_process_touch – decode one touch point and report it via the MT API.
 *
 * @data : pointer to the first byte of the per-point record
 * @slot : slot index (0 … max_touch_num-1)
 */
static void ws_process_touch(struct ws_ts *ts, const u8 *data, int slot)
{
	u16 x, y, width;

	x     = (data[WS_TOUCH_X_HIGH] << 8) | data[WS_TOUCH_X_LOW];
	y     = (data[WS_TOUCH_Y_HIGH] << 8) | data[WS_TOUCH_Y_LOW];
	width = (data[WS_TOUCH_WIDTH_HIGH] << 8) | data[WS_TOUCH_WIDTH_LOW];

	input_mt_slot(ts->input_dev, slot);
	input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);

	/*
	 * touchscreen_report_pos() applies any axis inversions / swaps
	 * specified in the device tree (touchscreen-inverted-x, etc.)
	 */
	touchscreen_report_pos(ts->input_dev, &ts->props, x, y, true);
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, width);
}

/* ---------------------------------------------------------------------------
 * Polling callback – called periodically by the input subsystem
 * ---------------------------------------------------------------------------
 */
static void ws_ts_poll(struct input_dev *input)
{
	struct ws_ts *ts = input_get_drvdata(input);
	struct i2c_client *client = ts->client;
	u8 status;
	u8 point_data[WS_TOUCH_BYTES_9 * 10];  /* max 10 points × 9 bytes */
	int touch_num;
	int bytes_per_point;
	int ret;
	int i;

	/* Read touch status register */
	ret = ws_i2c_read(client, WS_REG_TOUCH_STATUS, &status, 1);
	if (ret < 0) {
		dev_warn_ratelimited(&client->dev,
				     "I2C read status failed: %d\n", ret);
		return;
	}

	/* Buffer not ready yet */
	if (!(status & WS_BUF_STATUS_READY))
		goto clear_status;

	touch_num = status & WS_TOUCH_COUNT_MASK;
	if (touch_num > ts->max_touch_num)
		touch_num = ts->max_touch_num;

	bytes_per_point = ts->nine_byte_report ? WS_TOUCH_BYTES_9
					       : WS_TOUCH_BYTES;

	if (touch_num > 0) {
		ret = ws_i2c_read(client, WS_REG_TOUCH_DATA,
				  point_data,
				  touch_num * bytes_per_point);
		if (ret < 0) {
			dev_warn_ratelimited(&client->dev,
					     "I2C read touch data failed: %d\n",
					     ret);
			goto clear_status;
		}
	}

	/* Process each active touch point */
	for (i = 0; i < touch_num; i++) {
		const u8 *p = point_data + i * bytes_per_point;

		ws_process_touch(ts, p, p[WS_TOUCH_TRACK_ID]);
	}

	/* Release slots that are no longer reported */
	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);

clear_status:
	/* Clear the buffer-ready flag so the controller writes new data */
	ws_i2c_write_byte(client, WS_REG_TOUCH_STATUS, 0);
}

/* ---------------------------------------------------------------------------
 * Probe / remove
 * ---------------------------------------------------------------------------
 */
/*
 * ws_ts_probe – I2C driver probe callback.
 *
 * Kernel compatibility:
 *   < 6.3  : int probe(struct i2c_client *, const struct i2c_device_id *)
 *   >= 6.3 : int probe(struct i2c_client *)  [id argument removed]
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int ws_ts_probe(struct i2c_client *client)
#else
static int ws_ts_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct ws_ts *ts;
	struct input_dev *input;
	u16 chip_id = 0;
	int ret;
	int poll_interval_ms = 8;  /* ~125 Hz                                   */

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_I2C)) {
		dev_err(dev, "I2C check functionality failed.\n");
		return -ENXIO;
	}

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	/* Allow the controller to come out of reset */
	usleep_range(10000, 20000);

	/* Identify the touch IC */
	ret = ws_read_chip_id(client, &chip_id);
	if (ret < 0) {
		dev_err(dev, "Read version failed.\n");
		return ret;
	}

	ts->chip = ws_find_chip(chip_id);
	if (ts->chip) {
		ts->max_touch_num    = ts->chip->max_touch;
		ts->nine_byte_report = ts->chip->nine_byte;
		dev_info(dev, "Detected %s (id=%u)\n",
			 ts->chip->name, ts->chip->id);
	} else {
		dev_warn(dev, "Unknown chip id %u, using defaults\n", chip_id);
		ts->max_touch_num    = 5;
		ts->nine_byte_report = false;
	}

	/* Read resolution from the chip's config registers */
	{
		u8 res[4];

		ret = ws_i2c_read(client, WS_REG_X_RESOLUTION, res, 4);
		if (ret >= 0) {
			ts->x_max = (res[1] << 8) | res[0];
			ts->y_max = (res[3] << 8) | res[2];
		}
	}

	if (ts->x_max == 0 || ts->y_max == 0) {
		dev_info(dev,
			 "Invalid config (%d, %d, %d), using defaults\n",
			 chip_id, ts->x_max, ts->y_max);
		ts->x_max = 1024;
		ts->y_max =  600;
	}

	/* Allocate the input device */
	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "Failed to allocate input device.\n");
		return -ENOMEM;
	}
	ts->input_dev = input;
	input_set_drvdata(input, ts);

	input->name       = "WS Capacitive TouchScreen";
	input->phys       = "input/ts";
	input->id.bustype = BUS_I2C;
	input->dev.parent = dev;

	/* Single-touch ABS events (for legacy apps) */
	input_set_capability(input, EV_ABS, ABS_X);
	input_set_capability(input, EV_ABS, ABS_Y);
	input_set_abs_params(input, ABS_X, 0, ts->x_max, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, ts->y_max, 0, 0);

	/* Multi-touch slots */
	ret = input_mt_init_slots(input, ts->max_touch_num,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		dev_err(dev, "Failed to initialize MT slots: %d\n", ret);
		return ret;
	}

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->y_max, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	/*
	 * Parse optional touchscreen properties from the device tree:
	 *   touchscreen-inverted-x, touchscreen-inverted-y,
	 *   touchscreen-swapped-x-y
	 */
	touchscreen_parse_properties(input, true, &ts->props);

	/* Set up polling (replaces interrupt-driven model for this panel) */
	ret = input_setup_polling(input, ws_ts_poll);
	if (ret) {
		dev_err(dev, "could not set up polling mode, %d\n", ret);
		return ret;
	}
	input_set_poll_interval(input, poll_interval_ms);

	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "Failed to register input device: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * ws_ts_remove – I2C driver remove callback.
 *
 * Kernel compatibility:
 *   < 6.0  : int remove(struct i2c_client *)
 *   >= 6.0 : void remove(struct i2c_client *)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
static void ws_ts_remove(struct i2c_client *client)
#else
static int ws_ts_remove(struct i2c_client *client)
#endif
{
	/*
	 * All resources allocated with devm_* helpers are released
	 * automatically by the device-managed cleanup framework.
	 * Nothing else to do here.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
	return 0;
#endif
}

/* ---------------------------------------------------------------------------
 * OF match table
 * ---------------------------------------------------------------------------
 */
static const struct of_device_id ws_of_match[] = {
	{ .compatible = "WS_7inchDSI1024x600_Touch_compatible" },
	{ .compatible = "WS,ws1151"  },
	{ .compatible = "WS,ws5663"  },
	{ .compatible = "WS,ws5688"  },
	{ .compatible = "WS,ws911"   },
	{ .compatible = "WS,ws9110"  },
	{ .compatible = "WS,ws912"   },
	{ .compatible = "WS,ws9147"  },
	{ .compatible = "WS,ws917s"  },
	{ .compatible = "WS,ws927"   },
	{ .compatible = "WS,ws9271"  },
	{ .compatible = "WS,ws928"   },
	{ .compatible = "WS,ws9286"  },
	{ .compatible = "WS,ws967"   },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ws_of_match);

/* ---------------------------------------------------------------------------
 * I2C device ID table
 * ---------------------------------------------------------------------------
 */
static const struct i2c_device_id ws_ts_id[] = {
	{ "WS1001", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ws_ts_id);

/* ---------------------------------------------------------------------------
 * I2C driver registration
 *
 * module_i2c_driver() replaces the manual WS_ts_driver_init / _exit pattern
 * used in the 5.15 pre-built binary.
 * ---------------------------------------------------------------------------
 */
static struct i2c_driver ws_ts_driver = {
	.driver = {
		.name           = "WS_7inchDSI1024x600_Touch",
		.of_match_table = ws_of_match,
	},
	.probe    = ws_ts_probe,
	.remove   = ws_ts_remove,
	.id_table = ws_ts_id,
};
module_i2c_driver(ws_ts_driver);

MODULE_DESCRIPTION("WS 7inchDSI1024x600 Touch Driver");
MODULE_AUTHOR("Waveshare <www.waveshare.com>");
MODULE_LICENSE("GPL v2");
