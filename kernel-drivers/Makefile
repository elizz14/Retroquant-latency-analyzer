# Define the modules to be built
obj-m += hackpad.o
obj-m += st7789fb.o

# Architecture and Cross-Compiler setup
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

# Path to the pre-compiled Linux kernel source tree
KDIR ?= ../../repos/lkss-linux

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean
