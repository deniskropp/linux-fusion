KERNEL_MODLIB     = /lib/modules/$(shell uname -r)
KERNEL_BUILD     = $(KERNEL_MODLIB)/build
KERNEL_SOURCE    = $(KERNEL_MODLIB)/source
KERNEL_PATCHLEVEL = $(shell uname -r | cut -d . -f 2)
#KERNEL_PATCHLEVEL = $(shell grep 'PATCHLEVEL =' $(KERNEL_BUILD)/Makefile | cut -d ' ' -f 3)

SUB = linux/drivers/char/fusion

export CONFIG_FUSION_DEVICE=m


ifeq ($(DEBUG),yes)
  CPPFLAGS += -DFUSION_DEBUG_SKIRMISH_DEADLOCK
endif


.PHONY: all install clean

all:
	rm -f $(SUB)/Makefile
	ln -s Makefile-2.$(KERNEL_PATCHLEVEL) $(SUB)/Makefile
	$(MAKE) -C $(KERNEL_BUILD) \
		CPPFLAGS="$(CPPFLAGS) -D__KERNEL__ -I`pwd`/linux/include -I$(KERNEL_SOURCE)/include" \
		SUBDIRS=`pwd`/$(SUB) modules

install: all
	install -d $(DESTDIR)/usr/include/linux
	install -m 644 linux/include/linux/fusion.h $(DESTDIR)/usr/include/linux

	install -d $(DESTDIR)$(KERNEL_MODLIB)/drivers/char/fusion

ifeq ($(KERNEL_PATCHLEVEL),4)
	install -m 644 $(SUB)/fusion.o $(DESTDIR)$(KERNEL_MODLIB)/drivers/char/fusion
	rm -f $(DESTDIR)$(KERNEL_MODLIB)/fusion.o
else
	install -m 644 $(SUB)/fusion.ko $(DESTDIR)$(KERNEL_MODLIB)/drivers/char/fusion
	rm -f $(DESTDIR)$(KERNEL_MODLIB)/fusion.ko
endif
	depmod -ae


clean:
	find $(SUB) -name *.o -o -name *.ko -o -name .*.o.cmd -o \
		-name fusion.mod.* -o -name .fusion.* | xargs rm -f
	rm -f $(SUB)/Makefile
