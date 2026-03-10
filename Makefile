.PHONY: all modules install modules_install clean checkpatch dev \
        dkms-add dkms-build dkms-install dkms-remove dkms-status

MODULE_NAME  := gigabyte_waterforce
MODULE_VER   := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DKMS_SRC     := /usr/src/$(MODULE_NAME)-$(MODULE_VER)

# External KDIR specification is supported
KDIR ?= /lib/modules/$(shell uname -r)/build

SOURCES := drivers/hwmon/gigabyte_waterforce.c

all: modules

install: modules_install

modules modules_install clean:
	make W=1 C=1 -C $(KDIR) M=$$PWD $@

checkpatch:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree $(SOURCES)

# Quick local test cycle (no DKMS)
dev:
	make clean
	make
	sudo rmmod gigabyte_waterforce || true
	sudo insmod drivers/hwmon/gigabyte_waterforce.ko

# --- DKMS targets ---

dkms-add:
	sudo mkdir -p $(DKMS_SRC)
	sudo cp -r --no-preserve=ownership . $(DKMS_SRC)/
	sudo dkms add $(MODULE_NAME)/$(MODULE_VER)

dkms-build:
	sudo dkms build $(MODULE_NAME)/$(MODULE_VER)

dkms-install:
	sudo dkms install --force $(MODULE_NAME)/$(MODULE_VER)

# Build + install in one step (MOK signing is handled automatically via /etc/dkms/framework.conf)
dkms: dkms-add dkms-build dkms-install

dkms-remove:
	sudo dkms remove $(MODULE_NAME)/$(MODULE_VER) --all
	sudo rm -rf $(DKMS_SRC)

dkms-status:
	dkms status $(MODULE_NAME)/$(MODULE_VER)
