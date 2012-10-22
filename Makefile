# for native builds:
# make modules modules_install
# make KERNELDIR=<not currently running kernel's build tree> modules modules_install
# make KERNEL_VERSION=<uname -r of the not currently running kernel> modules modules_install
#
# for cross builds, using standard kernel make environment, i.e.
# make KERNELDIR=<linux build tree> INSTALL_MOD_PATH=<target root fs> modules modules_install

KERNEL_VERSION   ?= $(shell uname -r)
INSTALL_MOD_PATH ?= /
KERNELDIR        ?= $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)/build
FUSIONCORE       ?= single
ONECORE          ?= single

export FUSIONCORE
export ONECORE

FUSION_CPPFLAGS += -DFUSION_CALL_INTERRUPTIBLE \
	-I`pwd`/linux/drivers/char/fusion \
	-I`pwd`/linux/drivers/char/fusion/multi \
	-I`pwd`/linux/drivers/char/fusion/$(FUSIONCORE)

ONE_CPPFLAGS += \
	-I`pwd`/one \
	-I`pwd`/one/$(ONECORE) \
	-I`pwd`/include \

KERNEL_BUILD  = $(KERNELDIR)
# works for 2.6.23
KERNEL_SOURCE = $(shell grep ^KERNELSRC $(KERNEL_BUILD)/Makefile | cut -d ' ' -f 6)
ifeq ($(KERNEL_SOURCE), )
  # works for 2.6.32
  KERNEL_SOURCE = $(shell grep '^MAKEARGS := -C ' $(KERNEL_BUILD)/Makefile | cut -d ' ' -f 4)
endif
ifneq ($(KERNEL_SOURCE), )
  K_VERSION    = $(shell grep '^VERSION =' $(KERNEL_SOURCE)/Makefile | cut -d ' ' -f 3)
  K_PATCHLEVEL = $(shell grep '^PATCHLEVEL =' $(KERNEL_SOURCE)/Makefile | cut -d ' ' -f 3)
  K_SUBLEVEL   = $(shell grep '^SUBLEVEL =' $(KERNEL_SOURCE)/Makefile | cut -d ' ' -f 3)
else
  K_VERSION    = $(shell grep '^VERSION =' $(KERNEL_BUILD)/Makefile | cut -d ' ' -f 3)
  K_PATCHLEVEL = $(shell grep '^PATCHLEVEL =' $(KERNEL_BUILD)/Makefile | cut -d ' ' -f 3)
  K_SUBLEVEL   = $(shell grep '^SUBLEVEL =' $(KERNEL_BUILD)/Makefile | cut -d ' ' -f 3)
endif

FUSION_SUB = linux/drivers/char/fusion
ONE_SUB    = one
SUBMOD     = drivers/char/fusion

export CONFIG_FUSION_DEVICE=m
export CONFIG_LINUX_ONE=m

ifeq ($(DEBUG),yes)
  FUSION_CPPFLAGS += -DFUSION_DEBUG_SKIRMISH_DEADLOCK -DFUSION_ENABLE_DEBUG  
  ONE_CPPFLAGS    += -DONE_ENABLE_DEBUG
endif

ifeq ($(shell test -e $(KERNEL_BUILD)/include/linux/autoconf.h && echo yes),yes)
  AUTOCONF_H = -include $(KERNEL_BUILD)/include/linux/autoconf.h
endif

ifeq ($(shell test -e $(KERNEL_BUILD)/include/linux/config.h && echo yes),yes)
  CPPFLAGS += -DHAVE_LINUX_CONFIG_H
endif

ifeq ($(K_VERSION),3)
  KMAKEFILE = Makefile-2.6
else
  KMAKEFILE = Makefile-2.$(K_PATCHLEVEL)
endif

check-version = $(shell expr \( $(K_VERSION) \* 65536 + $(K_PATCHLEVEL) \* 256 + $(K_SUBLEVEL) \) \>= \( $(1) \* 65536 + $(2) \* 256 + $(3) \))

ifeq ($(call check-version,2,6,24),1)
		FUSION_EXTRAFLAGS = KCPPFLAGS="$(CPPFLAGS) $(FUSION_CPPFLAGS) -I`pwd`/linux/include" INSTALL_MOD_DIR="$(SUBMOD)"
		ONE_EXTRAFLAGS    = KCPPFLAGS="$(CPPFLAGS) $(ONE_CPPFLAGS) -I`pwd`/linux/include" INSTALL_MOD_DIR="$(SUBMOD)"
else
		FUSION_EXTRAFLAGS = CPPFLAGS="$(CPPFLAGS) $(FUSION_CPPFLAGS) -D__KERNEL__ -I`pwd`/linux/include -I$(KERNEL_BUILD)/include -I$(KERNEL_BUILD)/include2 -I$(KERNEL_SOURCE)/include $(AUTOCONF_H)"
		ONE_EXTRAFLAGS    = CPPFLAGS="$(CPPFLAGS) $(ONE_CPPFLAGS) -D__KERNEL__ -I`pwd`/linux/include -I$(KERNEL_BUILD)/include -I$(KERNEL_BUILD)/include2 -I$(KERNEL_SOURCE)/include $(AUTOCONF_H)"
endif

.PHONY: all modules modules_install install clean

all: modules
install: modules_install headers_install

modules:
	rm -f $(FUSION_SUB)/Makefile
	rm -f $(ONE_SUB)/Makefile
	cp $(FUSION_SUB)/$(KMAKEFILE) $(FUSION_SUB)/Makefile
	cp $(ONE_SUB)/$(KMAKEFILE) $(ONE_SUB)/Makefile
	echo kernel is in $(KERNEL_SOURCE) and version is $(K_SUBLEVEL), building module in $(FUSION_SUB)
	$(MAKE) -C $(KERNEL_BUILD) \
		$(FUSION_EXTRAFLAGS) \
		SUBDIRS=`pwd`/$(FUSION_SUB) modules
	echo kernel is in $(KERNEL_SOURCE) and version is $(K_SUBLEVEL), building module in $(ONE_SUB)
	$(MAKE) -C $(KERNEL_BUILD) \
		$(ONE_EXTRAFLAGS) \
		SUBDIRS=`pwd`/$(ONE_SUB) modules

modules_install: modules
ifeq ($(K_VERSION)$(K_PATCHLEVEL),24)
	install -d $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)/drivers/char/fusion
	install -m 644 linux/drivers/char/fusion/fusion.o $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)/drivers/char/fusion
	install -m 644 one/linux-one.o                    $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)/drivers/char/fusion
	rm -f $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)/fusion.o
	rm -f $(INSTALL_MOD_PATH)/lib/modules/$(KERNEL_VERSION)/linux-one.o
	/sbin/depmod -ae -b $(INSTALL_MOD_PATH) $(KERNEL_VERSION)
else
	$(MAKE) -C $(KERNEL_BUILD) \
		$(FUSION_EXTRAFLAGS) \
		SUBDIRS=`pwd`/$(FUSION_SUB) modules_install
	$(MAKE) -C $(KERNEL_BUILD) \
		$(ONE_EXTRAFLAGS) \
		SUBDIRS=`pwd`/$(ONE_SUB) modules_install
endif

headers_install:
	install -d $(INSTALL_MOD_PATH)/usr/include/linux
	install -m 644 linux/include/linux/fusion.h $(INSTALL_MOD_PATH)/usr/include/linux
	install -m 644 include/linux/one.h $(INSTALL_MOD_PATH)/usr/include/linux



clean:
	find $(FUSION_SUB) \( -name .git -prune \
		-o -name *.o -o -name *.ko -o -name .*.o.cmd \
		-o -name fusion.mod.* -o -name .fusion.* \
		-o -name Module.symvers -o -name modules.order \) \
		-type f -print | xargs rm -f
	find $(FUSION_SUB) -name .tmp_versions -type d -print |  xargs rm -rf
	rm -f $(FUSION_SUB)/Makefile
	find $(ONE_SUB) \( -name .git -prune \
		-o -name *.o -o -name *.ko -o -name .*.o.cmd \
		-o -name fusion.mod.* -o -name .fusion.* \
		-o -name Module.symvers -o -name modules.order \) \
		-type f -print | xargs rm -f
	find $(ONE_SUB) -name .tmp_versions -type d -print |  xargs rm -rf
	rm -f $(ONE_SUB)/Makefile
