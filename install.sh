#!/bin/bash
# 编译 + 安装 MAX17263 驱动，并设置开机自动加载、自动实例化 i2c 设备
#
# 用法:
#   sudo ./install.sh                 # 默认 i2c-5, 地址 0x36
#   sudo BUS=5 ADDR=0x36 ./install.sh
set -e

BUS=${BUS:-5}
ADDR=${ADDR:-0x36}
DEVNAME="$(printf '%s-00%x' "$BUS" "$ADDR")"      # 例: 5-0036
HERE=$(cd "$(dirname "$0")" && pwd)

echo "[1/5] 编译"
KDIR=${KDIR:-} "$HERE/build.sh"

echo "[2/5] 安装到 /lib/modules/$(uname -r)/extra/"
install -D -m 644 "$HERE/max17263.ko" "/lib/modules/$(uname -r)/extra/max17263.ko"
depmod -a

echo "[3/5] 开机自动 modprobe"
echo max17263 > /etc/modules-load.d/max17263.conf

echo "[4/5] 开机自动实例化 i2c 设备 ($DEVNAME)"
cat > /etc/systemd/system/max17263-instantiate.service <<EOF
[Unit]
Description=Instantiate MAX17263 fuel gauge on i2c-${BUS}
After=systemd-modules-load.service
Wants=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe max17263
ExecStart=/bin/sh -c 'test -e /sys/bus/i2c/devices/${DEVNAME} || echo max17263 ${ADDR} > /sys/bus/i2c/devices/i2c-${BUS}/new_device'

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable max17263-instantiate.service > /dev/null

echo "[5/5] 立即加载"
modprobe max17263 2>/dev/null || true
if [ ! -e "/sys/bus/i2c/devices/${DEVNAME}" ]; then
	echo max17263 "$ADDR" > "/sys/bus/i2c/devices/i2c-${BUS}/new_device"
fi

sleep 1
echo
echo "===== 安装完成 ====="
if [ -d /sys/class/power_supply/max17263 ]; then
	echo "接口: /sys/class/power_supply/max17263/"
	awk '{printf "  电压 %.3f V\n", $1/1000000}' /sys/class/power_supply/max17263/voltage_now 2>/dev/null || true
	awk '{printf "  电量 %d %%\n",   $1}'        /sys/class/power_supply/max17263/capacity   2>/dev/null || true
	awk '{printf "  电流 %.1f mA\n", $1/1000}'   /sys/class/power_supply/max17263/current_now 2>/dev/null || true
else
	echo "!! 设备节点未出现，请执行: dmesg | tail -20"
fi
