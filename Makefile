KERNEL_VERSION  ?= $(shell uname -r)
KERNEL_MODLIB   ?= /lib/modules/$(KERNEL_VERSION)
KERNEL_BUILD    ?= $(KERNEL_MODLIB)/build
KERNEL_SOURCE   ?= $(KERNEL_MODLIB)/source

K_VERSION    := $(shell echo $(KERNEL_VERSION) | cut -d . -f 1)
K_PATCHLEVEL := $(shell echo $(KERNEL_VERSION) | cut -d . -f 2)
K_SUBLEVEL   := $(shell echo $(KERNEL_VERSION) | cut -d . -f 3 | cut -d '-' -f 1)

SUB = linux/drivers/char/fusion

export CONFIG_FUSION_DEVICE=m


ifeq ($(DEBUG),yes)
  CPPFLAGS += -DFUSION_DEBUG_SKIRMISH_DEADLOCK
endif

ifeq ($(shell test -e $(KERNEL_BUILD)/include/linux/autoconf.h && echo yes),yes)
  AUTOCONF_H = -include $(KERNEL_BUILD)/include/linux/autoconf.h
endif

ifeq ($(shell test -e $(KERNEL_BUILD)/include/linux/config.h && echo yes),yes)
  CPPFLAGS += -DHAVE_LINUX_CONFIG_H
endif

check-version = $(shell expr \( $(K_VERSION) \* 65536 + $(K_PATCHLEVEL) \* 256 + $(K_SUBLEVEL) \) \>= \( $(1) \* 65536 + $(2) \* 256 + $(3) \))

.PHONY: all install clean

all:
	rm -f $(SUB)/Makefile
	ln -s Makefile-2.$(K_PATCHLEVEL) $(SUB)/Makefile
ifeq ($(call check-version,2,6,24),1)
	$(MAKE) -C $(KERNEL_BUILD) \
		KCPPFLAGS="$(CPPFLAGS) -I`pwd`/linux/include" \
		SUBDIRS=`pwd`/$(SUB) modules
else
	$(MAKE) -C $(KERNEL_BUILD) \
		CPPFLAGS="$(CPPFLAGS) -D__KERNEL__ -I`pwd`/linux/include -I$(KERNEL_BUILD)/include -I$(KERNEL_SOURCE)/include $(AUTOCONF_H)" \
		SUBDIRS=`pwd`/$(SUB) modules
endif

install: all
	install -d $(DESTDIR)/usr/include/linux
	install -m 644 linux/include/linux/fusion.h $(DESTDIR)/usr/include/linux

	install -d $(DESTDIR)$(KERNEL_MODLIB)/drivers/char/fusion

ifeq ($(K_PATCHLEVEL),4)
	install -m 644 $(SUB)/fusion.o $(DESTDIR)$(KERNEL_MODLIB)/drivers/char/fusion
	rm -f $(DESTDIR)$(KERNEL_MODLIB)/fusion.o
else
	install -m 644 $(SUB)/fusion.ko $(DESTDIR)$(KERNEL_MODLIB)/drivers/char/fusion
	rm -f $(DESTDIR)$(KERNEL_MODLIB)/fusion.ko
endif
ifneq ($(strip $(DESTDIR)),)
	/sbin/depmod -ae -b $(DESTDIR) $(KERNEL_VERSION)
else
	/sbin/depmod -ae $(KERNEL_VERSION)
endif



clean:
	find $(SUB) -name *.o -o -name *.ko -o -name .*.o.cmd -o \
		-name fusion.mod.* -o -name .fusion.* | xargs rm -f
	rm -f $(SUB)/Makefile
