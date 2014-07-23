
# Comment/uncomment the following line to enable/disable debugging
#DEBUG = y

#use KBUILD_EXTRA_SYMBOLS to fix no symbols err, but it seams not working.
KBUILD_EXTRA_SYMBOLS+=../lddbus/Module.symvers
export KBUILD_EXTRA_SYMBOLS



ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSCULLD_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

ccflags-y += $(DEBFLAGS) -I$(LDDINC)

TARGET = sculld

ifneq ($(KERNELRELEASE),)

sculld-objs := main.o mmap.o

obj-m	:= sculld.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) modules

endif


install:
	install -d $(INSTALLDIR)
	install -c $(TARGET).o $(INSTALLDIR)

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions


depend .depend dep:
	$(CC) $(ccflags-y) -M *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif
