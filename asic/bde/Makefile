obj-m := linux-kernel-bde.o linux-user-bde.o

# When built standalone
KSRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KSRC) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KSRC) M=$(CURDIR) clean 2>/dev/null || rm -f *.o *.ko *.mod* .*.cmd
