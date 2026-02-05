# SPDX-License-Identifier: GPL-2.0-only
#
# PCIe Accelerator Device Driver - Makefile
#
# Build:
#   make                  - Build the module
#   make clean            - Clean build artifacts
#   make install          - Install the module
#   make uninstall        - Remove the module
#
# Options:
#   KERNELDIR=/path/to/kernel  - Build against specific kernel
#   DEBUG=1                    - Enable debug output
#

MODULE_NAME := pcie-accel

# Module sources
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-y := pcie-accel-core.o \
                    pcie-accel-queue.o \
                    pcie-accel-chardev.o \
                    pcie-accel-p2p.o

# Kernel source directory
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Build flags
ccflags-y := -Wall -Werror
ccflags-$(DEBUG) += -DDEBUG -g

# Default target
all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

# Clean target
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean
	rm -f Module.symvers modules.order

# Install module
install: all
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules_install
	depmod -a

# Uninstall module
uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/$(MODULE_NAME).ko
	depmod -a

# Load module
load: all
	insmod $(MODULE_NAME).ko

# Unload module
unload:
	rmmod $(MODULE_NAME) 2>/dev/null || true

# Reload module
reload: unload load

# Show module info
info: all
	modinfo $(MODULE_NAME).ko

# Check coding style
checkpatch:
	$(KERNELDIR)/scripts/checkpatch.pl --no-tree -f *.c *.h

.PHONY: all clean install uninstall load unload reload info checkpatch
