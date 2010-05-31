obj-m = rm.o
KDIR := /home/quentin/scaleochip/kernels/linux-2.6.21.1-leon23-1.0.2/

#KDIR := /usr/src/linux-2.6.31-ARCH

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(shell pwd) modules

clean:
	rm -rf *.[oas] .*.flags *.ko .*.cmd .*.d .*.tmp *.mod.c .tmp_versions Module*.symvers
	rm -rf Module.markers modules.order
	rm -f ioctl
