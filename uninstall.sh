#!/bin/bash
# 卸载 MAX17263 驱动（含 systemd 单元、模块文件、自启配置）
set -e

BUS=${BUS:-5}

systemctl disable --now max17263-instantiate.service 2>/dev/null || true
rm -f /etc/systemd/system/max17263-instantiate.service
systemctl daemon-reload

[ -e "/sys/bus/i2c/devices/${BUS}-0036" ] && \
	echo 0x36 > "/sys/bus/i2c/devices/${BUS}-0036/delete_device" 2>/dev/null || true
rmmod max17263 2>/dev/null || true

rm -f /etc/modules-load.d/max17263.conf
rm -f "/lib/modules/$(uname -r)/extra/max17263.ko"
depmod -a

echo "已卸载"
