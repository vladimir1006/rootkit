# SPDX-License-Identifier: GPL-2.0
#
# Build du module noyau rk2600.
#
# KDIR doit pointer vers l'arbre de build du noyau utilise pour bzImage
# (celui produit par infra/scripts). Exemple :
#   make KDIR=$(PWD)/../../infra/build/linux

KERNEL_RELEASE ?= $(shell uname -r)
KDIR           ?= /lib/modules/$(KERNEL_RELEASE)/build
MODULE_DIR     := $(shell pwd)

ccflags-y += -I$(src)/../../include

obj-m := rk2600.o

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MODULE_DIR) clean
