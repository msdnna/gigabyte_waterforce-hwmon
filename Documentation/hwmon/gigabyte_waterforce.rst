.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver gigabyte_waterforce
=================================

Supported devices:

* Gigabyte AORUS WATERFORCE X 240 (USB ID 1044:7a4d)
* Gigabyte AORUS WATERFORCE X 280 (USB ID 1044:7a4d)
* Gigabyte AORUS WATERFORCE X 360 (USB ID 1044:7a4d)
* Gigabyte AORUS WATERFORCE X II 240 (USB ID 0414:7a5e)
* Gigabyte AORUS WATERFORCE X II 280 (USB ID 0414:7a5e)
* Gigabyte AORUS WATERFORCE X II 360 (USB ID 0414:7a5e)

Authors: Aleksa Savic, Vasily Sokolov

Description
-----------

This driver enables hardware monitoring support for the listed Gigabyte Waterforce
all-in-one CPU liquid coolers. Available sensors are pump and fan speed in RPM, as
well as coolant temperature. Also available through debugfs is the firmware version.

Attaching a fan is optional and allows it to be controlled from the device. If
it's not connected, the fan-related sensors will report zeroes.

The addressable RGB LEDs are not supported in this driver and should be controlled
through userspace tools (e.g. OpenRGB).

LCD display
-----------

This fork adds LCD display update support. The driver periodically sends the
following information to the cooler display:

* CPU temperature (read from a configurable hwmon sensor, default: ``Tctl``)
* CPU frequency (read from the cpufreq subsystem)
* CPU usage (computed from kernel tick counters)
* CPU package power (read from a configurable hwmon sensor, default: ``RAPL_P_Package``)

The sensor labels used for temperature and power lookup can be overridden via
module parameters (see below). The driver scans ``/sys/class/hwmon/`` at runtime
and caches the found sensor paths, retrying automatically if a sensor module is
loaded after the cooler driver.

Usage notes
-----------

As these are USB HIDs, the driver can be loaded automatically by the kernel and
supports hot swapping.

Module parameters
-----------------

================== ======= ===================================================
Parameter          Default Description
================== ======= ===================================================
lcd_temp_label     Tctl    hwmon sensor label for LCD CPU temperature display
lcd_power_label    RAPL_P_ hwmon sensor label for LCD CPU power display (W)
                   Package
lcd_refresh_ms     2000    LCD update interval in milliseconds (minimum: 100)
================== ======= ===================================================

To set parameters persistently, create ``/etc/modprobe.d/gigabyte_waterforce.conf``::

    options gigabyte_waterforce lcd_temp_label=Tctl lcd_power_label=Esocket0 lcd_refresh_ms=2000

.. note::
   The label names refer to values in ``/sys/class/hwmon/hwmon*/temp*_label`` and
   ``/sys/class/hwmon/hwmon*/power*_label`` respectively. These may differ from
   sensor names shown by ``lm-sensors``. To find the correct label for your system::

       grep -r '' /sys/class/hwmon/hwmon*/temp*_label /sys/class/hwmon/hwmon*/power*_label 2>/dev/null

Sysfs entries
-------------

=========== =============================================
fan1_input  Fan speed (in rpm)
fan2_input  Pump speed (in rpm)
temp1_input Coolant temperature (in millidegrees Celsius)
=========== =============================================

Debugfs entries
---------------

================ =======================
firmware_version Device firmware version
================ =======================
