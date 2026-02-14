# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for zen-freq kernel module
#
# AMD Zen 2+ Perfect Potential CPU Frequency Driver
#

obj-$(CONFIG_X86_ZEN_FREQ)	+= zen-freq.o

ccflags-y += -Wall -Werror
ccflags-$(CONFIG_ZEN_FREQ_DEBUG) += -DDEBUG

EXTRA_CFLAGS += -O2 -fno-strict-aliasing

ifeq ($(ARCH),x86)
    EXTRA_CFLAGS += -march=native
endif

ZEN_FREQ_VERSION = 2.0.0

clean:
	rm -f *.o *.ko *.mod.c *.mod.o Module.symvers modules.order
	rm -rf .tmp_versions

install: all
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/zen-freq.ko
	depmod -a

load:
	modprobe zen-freq

unload:
	modprobe -r zen-freq

info:
	modinfo zen-freq.ko

.PHONY: clean install uninstall load unload info
