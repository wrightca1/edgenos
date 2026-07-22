# Out-of-tree build for the FM6000 DMA kmod.
#   KDIR=/path/to/linux-6.12 make        # build fm6000dma.ko
#   KDIR=... make ROOTFS=<dir> install    # install into a rootfs
KDIR ?= /home/smiley/own_kernel/linux-6.12
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	install -d $(ROOTFS)/lib/modules/extra
	install -m 0644 fm6000dma.ko $(ROOTFS)/lib/modules/extra/

.PHONY: all clean install
