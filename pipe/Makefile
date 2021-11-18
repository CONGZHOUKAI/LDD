obj-m := pipep.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build

CONFIG_MODULE_SIG=n

all default: modules
install: modules_install

modules modules_install help clean:
	$(MAKE) -C $(KERNELDIR) M=$(shell pwd) $@
