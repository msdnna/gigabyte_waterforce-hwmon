// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Gigabyte AORUS Waterforce AIO CPU coolers: X240, X280 and X360.
 *
 * Copyright 2023 Aleksa Savic <savicaleksa83@gmail.com>
 * Copyright 2026 Vasily Sokolov <extracker0mail@gmail.com>
 */

#include <linux/cpufreq.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

/* LCD command bytes */
#define LCD_CMD_BYTE0	0x99
#define LCD_CMD_BYTE1	0xE0

/* Maximum number of hwmon devices and attributes to scan for LCD sensors */
#define LCD_HWMON_MAX_DEVS	128
#define LCD_TEMP_ATTR_MAX	32
#define LCD_POWER_ATTR_MAX	16

/* Minimum LCD refresh interval in ms */
#define LCD_REFRESH_MS_MIN	100

#define DRIVER_NAME	"gigabyte_waterforce"

#define USB_VENDOR_ID_CHU_YUEN		0x1044
#define USB_PRODUCT_ID_WATERFORCE_X	0x7a4d	/* Gigabyte AORUS WATERFORCE X 240, X 280, X 360 */

#define USB_VENDOR_ID_GIGABYTE		    0x0414
#define USB_PRODUCT_ID_WATERFORCE_X_II	0x7a5e	/* Gigabyte AORUS WATERFORCE X II 240, X II 280, X II 360 */

#define STATUS_VALIDITY		(2 * 1000)	/* ms */
#define MAX_REPORT_LENGTH	6144

#define WATERFORCE_TEMP_SENSOR	0xD
#define WATERFORCE_FAN_SPEED	0x02
#define WATERFORCE_PUMP_SPEED	0x05
#define WATERFORCE_FAN_DUTY	    0x08
#define WATERFORCE_PUMP_DUTY	0x09

/* Module parameters for LCD sensor discovery */
static char *lcd_temp_label = "Tctl";
module_param(lcd_temp_label, charp, 0444);
MODULE_PARM_DESC(lcd_temp_label, "hwmon sensor label for LCD CPU temperature (default: Tctl)");

static char *lcd_power_label = "RAPL_P_Package";
module_param(lcd_power_label, charp, 0444);
MODULE_PARM_DESC(lcd_power_label, "hwmon sensor label for LCD CPU power in watts (default: RAPL_P_Package)");

static unsigned int lcd_refresh_ms = 2000;
module_param(lcd_refresh_ms, uint, 0444);
MODULE_PARM_DESC(lcd_refresh_ms, "LCD update interval in milliseconds (default: 2000)");

/* Control commands, inner offsets and lengths */
static const u8 get_status_cmd[] = { 0x99, 0xDA };

#define FIRMWARE_VER_START_OFFSET_1	2
#define FIRMWARE_VER_START_OFFSET_2	3
static const u8 get_firmware_ver_cmd[] = { 0x99, 0xD6 };

/* Command lengths */
#define GET_STATUS_CMD_LENGTH		2
#define GET_FIRMWARE_VER_CMD_LENGTH	2

static const char *const waterforce_temp_label[] = {
	"Coolant temp"
};

static const char *const waterforce_speed_label[] = {
	"Fan speed",
	"Pump speed"
};

struct waterforce_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	/* For locking access to buffer */
	struct mutex buffer_lock;
	/* For queueing multiple readers */
	struct mutex status_report_request_mutex;
	/* For reinitializing the completion below */
	spinlock_t status_report_request_lock;
	struct completion status_report_received;
	struct completion fw_version_processed;

	/* Sensor data */
	s32 temp_input[1];
	u16 speed_input[2];	/* Fan and pump speed in RPM */
	u8 duty_input[2];	/* Fan and pump duty in 0-100% */

	u8 *buffer;
	int firmware_version;
	unsigned long updated;	/* jiffies */

	/* LCD update worker */
	struct delayed_work lcd_work;
	char lcd_temp_path[256];	/* sysfs path to temp sensor (empty = not yet found) */
	char lcd_power_path[256];	/* sysfs path to power sensor (empty = not yet found) */
	u64 lcd_prev_idle;		/* previous aggregate idle ticks for CPU usage delta */
	u64 lcd_prev_total;		/* previous aggregate total ticks for CPU usage delta */
};

static umode_t waterforce_is_visible(const void *data,
				     enum hwmon_sensor_types type, u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
		case hwmon_temp_input:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
		case hwmon_fan_input:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0444;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

/* Writes the command to the device with the rest of the report filled with zeroes */
static int waterforce_write_expanded(struct waterforce_data *priv, const u8 *cmd, int cmd_length)
{
	int ret;

	mutex_lock(&priv->buffer_lock);

	memcpy_and_pad(priv->buffer, MAX_REPORT_LENGTH, cmd, cmd_length, 0x00);
	ret = hid_hw_output_report(priv->hdev, priv->buffer, MAX_REPORT_LENGTH);

	mutex_unlock(&priv->buffer_lock);
	return ret;
}

static int waterforce_get_status(struct waterforce_data *priv)
{
	int ret = mutex_lock_interruptible(&priv->status_report_request_mutex);

	if (ret < 0)
		return ret;

	if (!time_after(jiffies, priv->updated + msecs_to_jiffies(STATUS_VALIDITY))) {
		/* Data is up to date */
		goto unlock_and_return;
	}

	/*
	 * Disable raw event parsing for a moment to safely reinitialize the
	 * completion. Reinit is done because hidraw could have triggered
	 * the raw event parsing and marked the priv->status_report_received
	 * completion as done.
	 */
	spin_lock_bh(&priv->status_report_request_lock);
	reinit_completion(&priv->status_report_received);
	spin_unlock_bh(&priv->status_report_request_lock);

	/* Send command for getting status */
	ret = waterforce_write_expanded(priv, get_status_cmd, GET_STATUS_CMD_LENGTH);
	if (ret < 0)
		goto unlock_and_return;

	ret = wait_for_completion_interruptible_timeout(&priv->status_report_received,
							msecs_to_jiffies(STATUS_VALIDITY));
	if (ret == 0)
		ret = -ETIMEDOUT;

unlock_and_return:
	mutex_unlock(&priv->status_report_request_mutex);
	if (ret < 0)
		return ret;

	return 0;
}

static int waterforce_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct waterforce_data *priv = dev_get_drvdata(dev);
	int ret = waterforce_get_status(priv);

	if (ret < 0)
		return ret;

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	case hwmon_fan:
		*val = priv->speed_input[channel];
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = DIV_ROUND_CLOSEST(priv->duty_input[channel] * 255, 100);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;	/* unreachable */
	}

	return 0;
}

static int waterforce_read_string(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = waterforce_temp_label[channel];
		break;
	case hwmon_fan:
		*str = waterforce_speed_label[channel];
		break;
	default:
		return -EOPNOTSUPP;	/* unreachable */
	}

	return 0;
}

static int waterforce_get_fw_ver(struct hid_device *hdev)
{
	struct waterforce_data *priv = hid_get_drvdata(hdev);
	int ret;

	ret = waterforce_write_expanded(priv, get_firmware_ver_cmd, GET_FIRMWARE_VER_CMD_LENGTH);
	if (ret < 0)
		return ret;

	ret = wait_for_completion_interruptible_timeout(&priv->fw_version_processed,
							msecs_to_jiffies(STATUS_VALIDITY));
	if (ret == 0)
		return -ETIMEDOUT;
	else if (ret < 0)
		return ret;

	return 0;
}

static const struct hwmon_ops waterforce_hwmon_ops = {
	.is_visible = waterforce_is_visible,
	.read = waterforce_read,
	.read_string = waterforce_read_string
};

static const struct hwmon_channel_info *waterforce_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info waterforce_chip_info = {
	.ops = &waterforce_hwmon_ops,
	.info = waterforce_info,
};

static int waterforce_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
				int size)
{
	struct waterforce_data *priv = hid_get_drvdata(hdev);

	if (data[0] == get_firmware_ver_cmd[0] && data[1] == get_firmware_ver_cmd[1]) {
		/* Received a firmware version report */
		priv->firmware_version =
		    data[FIRMWARE_VER_START_OFFSET_1] * 10 + data[FIRMWARE_VER_START_OFFSET_2];

		if (!completion_done(&priv->fw_version_processed))
			complete_all(&priv->fw_version_processed);
		return 0;
	}

	if (data[0] != get_status_cmd[0] || data[1] != get_status_cmd[1])
		return 0;

	priv->temp_input[0] = data[WATERFORCE_TEMP_SENSOR] * 1000;
	priv->speed_input[0] = get_unaligned_le16(data + WATERFORCE_FAN_SPEED);
	priv->speed_input[1] = get_unaligned_le16(data + WATERFORCE_PUMP_SPEED);
	priv->duty_input[0] = data[WATERFORCE_FAN_DUTY];
	priv->duty_input[1] = data[WATERFORCE_PUMP_DUTY];

	spin_lock(&priv->status_report_request_lock);
	if (!completion_done(&priv->status_report_received))
		complete_all(&priv->status_report_received);
	spin_unlock(&priv->status_report_request_lock);

	priv->updated = jiffies;

	return 0;
}

/*
 * Read an integer value from a sysfs pseudo-file.
 * Used to read hwmon sensor values (temp*_input, power*_input) from other
 * kernel drivers without requiring a userspace intermediary.
 */
static int waterforce_sysfs_read_long(const char *path, long *val)
{
	struct file *f;
	char buf[32];
	ssize_t len;
	loff_t pos = 0;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	len = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);

	if (len <= 0)
		return len < 0 ? len : -EIO;

	buf[len] = '\0';
	return kstrtol(strim(buf), 10, val);
}

/*
 * Scan hwmon devices to find a sensor whose *_label attribute matches
 * sensor_label. attr_type is "temp" or "power", max_n is the highest
 * attribute index to try. On success stores the path to the corresponding
 * *_input file in result and returns 0.
 */
static int waterforce_find_hwmon_sensor(const char *sensor_label,
					const char *attr_type, int max_n,
					char *result, size_t result_len)
{
	char path[256], label_buf[64];
	struct file *f;
	ssize_t len;
	loff_t pos;
	int hwmon_n, idx;

	for (hwmon_n = 0; hwmon_n < LCD_HWMON_MAX_DEVS; hwmon_n++) {
		for (idx = 1; idx <= max_n; idx++) {
			snprintf(path, sizeof(path),
				 "/sys/class/hwmon/hwmon%d/%s%d_label",
				 hwmon_n, attr_type, idx);

			f = filp_open(path, O_RDONLY, 0);
			if (IS_ERR(f))
				continue;

			pos = 0;
			len = kernel_read(f, label_buf, sizeof(label_buf) - 1, &pos);
			filp_close(f, NULL);

			if (len <= 0)
				continue;

			label_buf[len] = '\0';
			strim(label_buf);

			if (strcmp(label_buf, sensor_label) == 0) {
				snprintf(result, result_len,
					 "/sys/class/hwmon/hwmon%d/%s%d_input",
					 hwmon_n, attr_type, idx);
				return 0;
			}
		}
	}
	return -ENODEV;
}

/*
 * Compute overall CPU usage as a percentage by comparing aggregate idle and
 * total tick counts between consecutive work invocations. Matches the
 * calculation used by /proc/stat: idle = CPUTIME_IDLE, total = all fields.
 */
static int waterforce_get_cpu_usage(struct waterforce_data *priv)
{
	u64 idle = 0, total = 0, delta_idle, delta_total;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		struct kernel_cpustat ks = kcpustat_cpu(cpu);

		idle += ks.cpustat[CPUTIME_IDLE];
		for (i = 0; i < NR_STATS; i++)
			total += ks.cpustat[i];
	}

	delta_idle  = idle  - priv->lcd_prev_idle;
	delta_total = total - priv->lcd_prev_total;

	priv->lcd_prev_idle  = idle;
	priv->lcd_prev_total = total;

	if (delta_total == 0)
		return 0;

	return clamp_t(int, 100 * (delta_total - delta_idle) / delta_total, 0, 100);
}

static void waterforce_lcd_work_func(struct work_struct *work)
{
	struct waterforce_data *priv =
		container_of(work, struct waterforce_data, lcd_work.work);
	long temp_mc, power_uw;
	int temp_c = 0, power_w = 0, cpu_usage, freq_mhz;
	u8 *buf;

	/*
	 * Lazily discover sensor paths. Paths are cached so the scan only
	 * runs once (or again if a sensor disappears and reappears).
	 */
	if (!priv->lcd_temp_path[0])
		waterforce_find_hwmon_sensor(lcd_temp_label, "temp",
					     LCD_TEMP_ATTR_MAX,
					     priv->lcd_temp_path,
					     sizeof(priv->lcd_temp_path));

	if (!priv->lcd_power_path[0])
		waterforce_find_hwmon_sensor(lcd_power_label, "power",
					     LCD_POWER_ATTR_MAX,
					     priv->lcd_power_path,
					     sizeof(priv->lcd_power_path));

	/* Read temperature (hwmon reports in millidegrees Celsius) */
	if (priv->lcd_temp_path[0]) {
		if (waterforce_sysfs_read_long(priv->lcd_temp_path, &temp_mc) == 0)
			temp_c = clamp_t(int, temp_mc / 1000, 0, 255);
		else
			priv->lcd_temp_path[0] = '\0'; /* sensor gone, retry next time */
	}

	/* Read power (hwmon reports in microwatts) */
	if (priv->lcd_power_path[0]) {
		if (waterforce_sysfs_read_long(priv->lcd_power_path, &power_uw) == 0)
			power_w = clamp_t(int, power_uw / 1000000, 0, 255);
		else
			priv->lcd_power_path[0] = '\0'; /* sensor gone, retry next time */
	}

	/* CPU usage: delta of kernel tick counters since last invocation */
	cpu_usage = waterforce_get_cpu_usage(priv);

	/* CPU frequency from cpufreq subsystem (returns kHz, we need MHz) */
	freq_mhz = cpufreq_quick_get(0) / 1000;

	/*
	 * Build and send the LCD update payload. Protocol reverse-engineered
	 * from the AorusWaterForce360-linux Go daemon.
	 * buf[0..1] = command { 0x99, 0xE0 }
	 * buf[3]    = CPU temperature in degrees C
	 * buf[4]    = 0x10 (temperature field marker)
	 * buf[5]    = CPU frequency, integer GHz
	 * buf[6]    = CPU frequency, first decimal digit (100 MHz resolution)
	 * buf[7]    = 0x08 (frequency field marker)
	 * buf[8]    = 0x18 (frequency field marker)
	 * buf[10]   = CPU usage in percent
	 * buf[11]   = CPU package power in watts
	 */
	mutex_lock(&priv->buffer_lock);
	buf = priv->buffer;
	memset(buf, 0, MAX_REPORT_LENGTH);
	buf[0]  = LCD_CMD_BYTE0;
	buf[1]  = LCD_CMD_BYTE1;
	buf[3]  = (u8)temp_c;
	buf[4]  = 0x10;
	buf[5]  = (u8)(freq_mhz / 1000);
	buf[6]  = (u8)((freq_mhz / 100) % 10);
	buf[7]  = 0x08;
	buf[8]  = 0x18;
	buf[10] = (u8)cpu_usage;
	buf[11] = (u8)power_w;
	hid_hw_output_report(priv->hdev, buf, MAX_REPORT_LENGTH);
	mutex_unlock(&priv->buffer_lock);

	schedule_delayed_work(&priv->lcd_work,
			      msecs_to_jiffies(max(lcd_refresh_ms,
						   (unsigned int)LCD_REFRESH_MS_MIN)));
}

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct waterforce_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void waterforce_debugfs_init(struct waterforce_data *priv)
{
	char name[64];

	if (!priv->firmware_version)
		return;	/* There's nothing to show in debugfs */

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
}

static int waterforce_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	const char *model_name = (const char *)id->driver_data;
	struct waterforce_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	/*
	 * Initialize priv->updated to STATUS_VALIDITY seconds in the past, making
	 * the initial empty data invalid for waterforce_read() without the need for
	 * a special case there.
	 */
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		return ret;
	}

	/*
	 * Enable hidraw so existing user-space tools can continue to work.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_and_stop;
	}

	priv->buffer = devm_kzalloc(&hdev->dev, MAX_REPORT_LENGTH, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	mutex_init(&priv->status_report_request_mutex);
	mutex_init(&priv->buffer_lock);
	spin_lock_init(&priv->status_report_request_lock);
	init_completion(&priv->status_report_received);
	init_completion(&priv->fw_version_processed);
	INIT_DELAYED_WORK(&priv->lcd_work, waterforce_lcd_work_func);

	hid_device_io_start(hdev);
	ret = waterforce_get_fw_ver(hdev);
	if (ret < 0)
		hid_warn(hdev, "fw version request failed with %d\n", ret);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "waterforce",
							  priv, &waterforce_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_and_close;
	}

	waterforce_debugfs_init(priv);

	schedule_delayed_work(&priv->lcd_work,
			      msecs_to_jiffies(max(lcd_refresh_ms,
						   (unsigned int)LCD_REFRESH_MS_MIN)));

	hid_info(hdev, "Device:          %s", model_name);
	hid_info(hdev, "  Firmware:      v%d", priv->firmware_version);
	hid_info(hdev, "  Temp sensor:   %s", lcd_temp_label);
	hid_info(hdev, "  Power sensor:  %s", lcd_power_label);
	hid_info(hdev, "  Refresh:       %u ms", lcd_refresh_ms);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void waterforce_remove(struct hid_device *hdev)
{
	struct waterforce_data *priv = hid_get_drvdata(hdev);

	cancel_delayed_work_sync(&priv->lcd_work);
	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id waterforce_table[] = {
	{
		HID_USB_DEVICE(USB_VENDOR_ID_CHU_YUEN, USB_PRODUCT_ID_WATERFORCE_X),
		.driver_data = (unsigned long)"Gigabyte AORUS WATERFORCE X 240/280/360"
	},
	{
		HID_USB_DEVICE(USB_VENDOR_ID_GIGABYTE, USB_PRODUCT_ID_WATERFORCE_X_II),
		.driver_data = (unsigned long)"Gigabyte AORUS WATERFORCE X II 240/280/360"
	},
	{ }
};

MODULE_DEVICE_TABLE(hid, waterforce_table);

static struct hid_driver waterforce_driver = {
	.name = "waterforce",
	.id_table = waterforce_table,
	.probe = waterforce_probe,
	.remove = waterforce_remove,
	.raw_event = waterforce_raw_event,
};

static int __init waterforce_init(void)
{
	return hid_register_driver(&waterforce_driver);
}

static void __exit waterforce_exit(void)
{
	hid_unregister_driver(&waterforce_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(waterforce_init);
module_exit(waterforce_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>, Vasily Sokolov <extracker0mail@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for Gigabyte AORUS Waterforce AIO coolers");
