KERNEL_SOURCE=/usr/src/linux

all:
	make -C $(KERNEL_SOURCE) CPPFLAGS="-D__KERNEL__ -I`pwd`/linux/include -I$(KERNEL_SOURCE)/include" SUBDIRS=`pwd`/linux/drivers/char/fusion modules
	cp -f linux/drivers/char/fusion/fusion.o .

clean:
	rm -rf linux/drivers/char/fusion/*.o fusion.o
