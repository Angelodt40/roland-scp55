obj-m := scp55play.o
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
