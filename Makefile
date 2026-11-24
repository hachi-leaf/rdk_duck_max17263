# SPDX-License-Identifier: GPL-2.0
#
# MAX17263 fuel gauge driver (RDK Duck Battery Board)
#
# 用法:
#   make            编译 max17263.ko
#   make install    安装到 /lib/modules/$(uname -r)/extra/
#   make clean      清理
#
# 注意: RDK 板子的内核树不在标准位置，若默认路径不对，手动指定:
#   make KDIR=/usr/src/linux-headers-6

obj-m += max17263.o

KDIR ?= /usr/src/linux-headers-6
PWD  := $(shell pwd)

.PHONY: all build install clean

all: build

build:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

install: build
	install -D -m 644 max17263.ko /lib/modules/$(shell uname -r)/extra/max17263.ko
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
