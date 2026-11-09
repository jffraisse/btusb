# Out-of-tree / DKMS build for the patched btusb driver (Barrot/CSR USB clones).
obj-m := btusb.o

# DKMS sets KERNELRELEASE and invokes make -C $kernel_source_dir M=...
# For a manual build, fall back to the running kernel.
KVER ?= $(KERNELRELEASE)
ifeq ($(KVER),)
KVER := $(shell uname -r)
endif

KDIR ?= /lib/modules/$(KVER)/build
ifeq ($(wildcard $(KDIR)/Makefile),)
KDIR := /usr/src/linux-headers-$(KVER)
endif

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
