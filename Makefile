KERNEL_MODLIB     = /lib/modules/$(shell uname -r)
KERNEL_SOURCE     = $(KERNEL_MODLIB)/build
KERNEL_PATCHLEVEL = $(shell uname -r | cut -d . -f 2)
#KERNEL_PATCHLEVEL = $(shell grep 'PATCHLEVEL =' $(KERNEL_SOURCE)/Makefile | cut -d ' ' -f 3)

SUB = linux/drivers/char/fusion

export CONFIG_FUSION_DEVICE=m

all:
	rm -f $(SUB)/Makefile
	ln -s Makefile-2.$(KERNEL_PATCHLEVEL) $(SUB)/Makefile
	make -C $(KERNEL_SOURCE) \
		CPPFLAGS="-D__KERNEL__ -I`pwd`/linux/include -I$(KERNEL_SOURCE)/include" \
		SUBDIRS=`pwd`/$(SUB) modules

install-header:
	install -m 644 linux/include/linux/fusion.h /usr/include/linux

install-module:
	mkdir -p $(KERNEL_MODLIB)/drivers/char/fusion
ifeq ($(KERNEL_PATCHLEVEL),4)
	cp $(SUB)/fusion.o $(KERNEL_MODLIB)/drivers/char/fusion
else
	cp $(SUB)/fusion.ko $(KERNEL_MODLIB)/drivers/char/fusion
endif
	depmod -ae

install: all install-module install-header

clean:
	find $(SUB) -name *.o -o -name *.ko -o -name .*.o.cmd -o \
		-name fusion.mod.* -o -name .fusion.* | xargs rm -f
	rm -f $(SUB)/Makefile
