obj-m := scdwr.o
KDIR ?= /home/smiley/own_kernel/linux-6.12
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
