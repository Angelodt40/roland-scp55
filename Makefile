KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C src KDIR=$(KDIR)

clean:
	$(MAKE) -C src clean

install: all
	install -Dm644 src/scp55.ko /lib/modules/$(KVER)/extra/scp55.ko
	depmod -a

.PHONY: all clean install
