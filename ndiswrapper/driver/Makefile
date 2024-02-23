# Name of the module
MODNAME = ndiswrapper

DISTFILES = \
	Makefile nvmalloc.c nvmalloc.h crt.c divdi3.c hal.c iw_ndis.c iw_ndis.h lin2win.S lin2win.h \
	loader.c loader.h longlong.h mkexport.sh mkstubs.sh ndis.c ndis.h \
	ndiswrapper.h ntoskernel.c ntoskernel.h ntoskernel_io.c pe_linker.c \
	pe_linker.h pnp.c pnp.h proc.c rtl.c usb.c usb.h win2lin_stubs.S \
	winnt_types.h workqueue.c wrapmem.c wrapmem.h wrapndis.c wrapndis.h \
	wrapper.c wrapper.h

# By default, we try to compile the modules for the currently running
# kernel.  But it's the first approximation, as we will re-read the
# version from the kernel sources.
KVERS_UNAME ?= $(shell uname -r)

# KBUILD is the path to the Linux kernel build tree.  It is usually the
# same as the kernel source tree, except when the kernel was compiled in
# a separate directory.
KBUILD ?= $(shell readlink -f /lib/modules/$(KVERS_UNAME)/build)

ifeq (,$(KBUILD))
$(error Kernel build tree not found - please set KBUILD to configured kernel)
endif

KCONFIG := $(KBUILD)/.config
ifeq (,$(wildcard $(KCONFIG)))
$(error No .config found in $(KBUILD), please set KBUILD to configured kernel)
endif

ifneq (,$(wildcard $(KBUILD)/include/linux/version.h))
ifneq (,$(wildcard $(KBUILD)/include/generated/uapi/linux/version.h))
$(error Multiple copies of version.h found, please clean your build tree)
endif
endif

# Kernel Makefile doesn't always know the exact kernel version, so we
# get it from the kernel headers instead and pass it to make.
VERSION_H := $(KBUILD)/include/generated/utsrelease.h
ifeq (,$(wildcard $(VERSION_H)))
VERSION_H := $(KBUILD)/include/linux/utsrelease.h
endif
ifeq (,$(wildcard $(VERSION_H)))
VERSION_H := $(KBUILD)/include/linux/version.h
endif
ifeq (,$(wildcard $(VERSION_H)))
$(error Please run 'make modules_prepare' in $(KBUILD))
endif

KVERS := $(shell sed -ne 's/"//g;s/^\#define UTS_RELEASE //p' $(VERSION_H))

ifeq (,$(KVERS))
$(error Cannot find UTS_RELEASE in $(VERSION_H), please report)
endif

INST_DIR = /lib/modules/$(KVERS)/misc

SRC_DIR=$(shell pwd)

include $(KCONFIG)

# returns of structs and unions in registers when possible, like Windows
EXTRA_CFLAGS += -freg-struct-return

# to produce debug trace, add option "DEBUG=<n>" where <n> is 1 to 6
ifdef DEBUG
EXTRA_CFLAGS += -DDEBUG=$(DEBUG) -g
endif

# to debug timers, add option "TIMER_DEBUG=1"
ifdef TIMER_DEBUG
EXTRA_CFLAGS += -DTIMER_DEBUG
endif

# to debug event layer, add option "EVENT_DEBUG=1"
ifdef EVENT_DEBUG
EXTRA_CFLAGS += -DEVENT_DEBUG
endif

# to debug USB layer, add option "USB_DEBUG=1"
ifdef USB_DEBUG
EXTRA_CFLAGS += -DUSB_DEBUG
endif

# to debug I/O layer, add option "IO_DEBUG=1"
ifdef IO_DEBUG
EXTRA_CFLAGS += -DIO_DEBUG
endif

# to debug worker threads, add option "WORK_DEBUG=1"
ifdef WORK_DEBUG
EXTRA_CFLAGS += -DWORK_DEBUG
endif

# to debug memory allocation, add option "ALLOC_DEBUG=<n>" where <n> is 1 or 2
ifdef ALLOC_DEBUG
EXTRA_CFLAGS += -DALLOC_DEBUG=$(ALLOC_DEBUG)
endif

OBJS = nvmalloc.o crt.o hal.o iw_ndis.o loader.o ndis.o ntoskernel.o ntoskernel_io.o \
	pe_linker.o pnp.o proc.o rtl.o wrapmem.o wrapndis.o wrapper.o

EXPORT_SRCS = nvmalloc.c crt.c hal.c ndis.c ntoskernel.c ntoskernel_io.c rtl.c

STUB_SRCS = nvmalloc.c crt.c hal.c ndis.c ntoskernel.c ntoskernel_io.c \
	pnp.c rtl.c wrapndis.c


# By default, USB layer is compiled in if USB support is in kernel;
# to disable USB support in ndiswrapper even if USB support is in kernel,
# add option "DISABLE_USB=1"
ifndef DISABLE_USB
ifeq ($(CONFIG_USB),y)
ENABLE_USB = 1
endif
ifeq ($(CONFIG_USB),m)
ENABLE_USB = 1
endif
endif

ifdef ENABLE_USB
EXPORT_SRCS += usb.c
STUB_SRCS += usb.c
OBJS += usb.o
EXTRA_CFLAGS += -DENABLE_USB
endif

ifdef WRAP_WQ
EXTRA_CFLAGS += -DWRAP_WQ
OBJS += workqueue.o
endif


all: config_check modules

# generate exports symbol table from C files
quiet_cmd_mkexport = MKEXPORT $@
cmd_mkexport = $(SHELL) $(obj)/mkexport.sh $< $@

extra-y += $(EXPORT_SRCS:.c=_exports.h)
%_exports.h: %.c $(obj)/mkexport.sh FORCE
	$(call if_changed,mkexport)

$(addprefix $(obj)/,$(EXPORT_SRCS:.c=.o)): %.o: %_exports.h

ifeq ($(CONFIG_X86_64),y)
quiet_cmd_mkstubs = MKSTUBS $@
cmd_mkstubs = $(SHELL) $(obj)/mkstubs.sh $(addprefix $(src)/,$(STUB_SRCS)) >$@

extra-y += win2lin_stubs.h
$(obj)/win2lin_stubs.h: $(addprefix $(src)/,$(STUB_SRCS)) FORCE
	$(call if_changed,mkstubs)

$(obj)/win2lin_stubs.o: $(obj)/win2lin_stubs.h
OBJS += win2lin_stubs.o lin2win.o
else
OBJS += divdi3.o
endif

MODULE := $(MODNAME).ko
obj-m := $(MODNAME).o

$(MODNAME)-objs := $(OBJS)


config_check:
	@if [ -z "$(CONFIG_WIRELESS_EXT)$(CONFIG_NET_RADIO)" ]; then \
		echo; echo; \
		echo "*** WARNING: This kernel lacks wireless extensions."; \
		echo "Wireless drivers will not work properly."; \
		echo; echo; \
	fi
	@if [ -z "$(CONFIG_X86_64)" ] && [ -n "$(CONFIG_4KSTACKS)" ]; then \
		echo; echo; \
		echo "*** WARNING: This kernel uses 4K stack size option"; \
		echo "(CONFIG_4KSTACKS); many Windows drivers will not work"; \
		echo "with this option enabled. Disable CONFIG_4KSTACKS"; \
		echo "in kernel's .config file, recompile and install kernel"; \
		echo; echo; \
	fi

modules:
	$(MAKE) -C $(KBUILD) M=$(SRC_DIR)

$(MODULE):
	$(MAKE) modules

clean:
	rm -f *.o *.ko .*.cmd *.mod.c *.symvers modules.order *~ .\#*
	rm -f *_exports.h win2lin_stubs.h
	rm -rf .tmp_versions

install: config_check $(MODULE)
	@/sbin/modinfo $(MODULE) | grep -q "^vermagic: *$(KVERS) " || \
		{ echo "$(MODULE)" is not for Linux $(KVERS); exit 1; }
	mkdir -p -m 755 $(DESTDIR)$(INST_DIR)
	install -m 0644 $(MODULE) $(DESTDIR)$(INST_DIR)
ifndef DESTDIR
	-/sbin/depmod -a $(KVERS)
endif

uninstall:
	rm -f $(DESTDIR)$(INST_DIR)/$(MODULE)
ifndef DESTDIR
	-/sbin/depmod -a $(KVERS)
endif

dist:
	@for file in $(DISTFILES); do \
		cp $$file $(distdir)/$$file || exit 1; \
	done

.PHONY: all modules clean install config_check dist
