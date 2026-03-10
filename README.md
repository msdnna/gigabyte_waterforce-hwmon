# gigabyte_waterforce-hwmon

_Hwmon Linux kernel driver for monitoring Gigabyte AORUS Waterforce AIO coolers_

## Overview

The following devices are supported by this driver:

| Device                              | USB ID     |
|-------------------------------------|------------|
| Gigabyte AORUS WATERFORCE X 240     | 1044:7a4d  |
| Gigabyte AORUS WATERFORCE X 280     | 1044:7a4d  |
| Gigabyte AORUS WATERFORCE X 360     | 1044:7a4d  |
| Gigabyte AORUS WATERFORCE X II 240  | 0414:7a5e  |
| Gigabyte AORUS WATERFORCE X II 280  | 0414:7a5e  |
| Gigabyte AORUS WATERFORCE X II 360  | 0414:7a5e  |

Being a standard `hwmon` driver, it provides readings via `sysfs`, accessible through `lm-sensors` as usual.

Report offsets were initially taken from [here](https://github.com/namidairo/liquidctl/commit/a56db61350d01db8c3a008bb8816d870f6f2350d)
and confirmed when acquiring a X240.

## What this fork adds

This is a fork of the [original driver](https://github.com/aleksamagicka/waterforce-hwmon) by Aleksa Savic,
extended with the following features:

- **LCD display support** — the driver periodically updates the cooler's LCD with CPU temperature,
  frequency, usage percentage, and package power. This replaces the need for a separate userspace
  daemon (e.g. AorusWaterForce360-linux), which conflicts with the kernel driver over the HID channel.
- **Support for WATERFORCE X II** — added USB ID `0414:7a5e` (X II 240/280/360).
- **Configurable sensors** — module parameters control which hwmon sensors are used for temperature
  and power readings.

## Kernel availability

The original driver is mainlined since kernel v6.8. This fork contains additional features not yet
present upstream. Install from this repository using DKMS (recommended) or manually.

## Installation

### DKMS (recommended)

DKMS automatically rebuilds the module on kernel updates.

```bash
git clone <repo-url>
cd gigabyte_waterforce-hwmon
make dkms
```

To verify the installation:

```bash
make dkms-status
```

To remove:

```bash
make dkms-remove
```

### Manual (quick test)

Compile and load the module, replacing the running instance if present:

```bash
make dev
```

## Configuration

LCD sensor labels can be overridden via module parameters. The defaults work for most AMD systems:

| Parameter        | Default          | Description                                      |
|------------------|------------------|--------------------------------------------------|
| `lcd_temp_label` | `Tctl`           | hwmon label for CPU temperature (°C)             |
| `lcd_power_label`| `RAPL_P_Package` | hwmon label for CPU package power (W)            |
| `lcd_refresh_ms` | `2000`           | LCD update interval in milliseconds (min: 100)   |

To find the correct labels for your system:

```bash
grep -r '' /sys/class/hwmon/hwmon*/temp*_label /sys/class/hwmon/hwmon*/power*_label 2>/dev/null
```

To set parameters persistently, create `/etc/modprobe.d/gigabyte_waterforce.conf`:

```
options gigabyte_waterforce lcd_temp_label=Tctl lcd_power_label=Esocket0 lcd_refresh_ms=2000
```

On startup the driver logs its configuration to `dmesg`:

```
gigabyte_waterforce: Device:          Gigabyte AORUS WATERFORCE X II 240/280/360
gigabyte_waterforce:   Firmware:      v43
gigabyte_waterforce:   Temp sensor:   Tctl
gigabyte_waterforce:   Power sensor:  RAPL_P_Package
gigabyte_waterforce:   Refresh:       2000 ms
```

## Usage

After loading, run `sensors` — the cooler will appear as `waterforce-hid-*`:

```
waterforce-hid-3-1:00
Adapter: HID adapter
fan1:        1234 RPM
fan2:        2345 RPM
temp1:        +28.0°C
```
