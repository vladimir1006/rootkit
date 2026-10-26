ifneq ($(KERNELRELEASE),)

obj-m := rk2600.o
ccflags-y := -I$(src)/../include

else

KDIR ?= ../infra/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
