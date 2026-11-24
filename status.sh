#!/bin/bash
# 打印 MAX17263 全部状态（人类可读）
P=/sys/class/power_supply/max17263

if [ ! -d "$P" ]; then
	echo "设备未就绪：$P 不存在"
	echo "尝试加载：modprobe max17263 && echo max17263 0x36 > /sys/bus/i2c/devices/i2c-5/new_device"
	exit 1
fi

V=$(cat $P/voltage_now    2>/dev/null)
C=$(cat $P/capacity       2>/dev/null)
I=$(cat $P/current_now    2>/dev/null)
T=$(cat $P/temp           2>/dev/null)
S=$(cat $P/status         2>/dev/null)
F=$(cat $P/charge_full    2>/dev/null)
D=$(cat $P/design_capacity_mah 2>/dev/null)

awk -v v="$V" -v c="$C" -v i="$I" -v t="$T" -v s="$S" -v f="$F" -v d="$D" 'BEGIN{
	printf "电压      %.3f V\n", v/1000000;
	printf "电量      %d %%\n",   c;
	printf "电流      %.1f mA%s\n", i/1000, (i<0?"  (放电)":"  (充电)");
	printf "温度      %.1f C\n",  t/10;
	printf "状态      %s\n",      s;
	printf "设计容量  %d mAh\n",  d;
	printf "满充容量  %d mAh\n",  f/1000;
}'
