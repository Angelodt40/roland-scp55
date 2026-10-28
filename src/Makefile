obj-m := scp55.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: all
	install -Dm644 scp55.ko /lib/modules/$(KVER)/extra/scp55.ko
	depmod -a

.PHONY: all clean install
