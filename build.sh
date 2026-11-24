#!/bin/bash
# 编译 MAX17263 内核模块
#
# 处理 D-Robotics RDK 板子上的三个坑:
#   1) RTC 坏了导致系统时间回到 2000 年, 内核源码时间戳是"未来", make 会无限重编
#   2) 头文件包没带预编译的 scripts/mod/modpost
#   3) 上次中断留下 0 字节的 scripts/basic/fixdep
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
KV=$(uname -r)

# ---------- 1) 找内核构建树 ----------
pick_kdir() {
	for d in "/lib/modules/$KV/build" "/usr/src/linux-headers-$KV" "/usr/src/linux-headers-6"; do
		[ -e "$d/Makefile" ] && [ -d "$d/include/generated" ] && { echo "$d"; return 0; }
	done
	return 1
}

KDIR=${KDIR:-$(pick_kdir || true)}
if [ -z "$KDIR" ] || [ ! -e "$KDIR/Makefile" ]; then
	echo "错误: 找不到完整的内核头文件树"
	echo "  试过: /lib/modules/$KV/build  /usr/src/linux-headers-$KV  /usr/src/linux-headers-6"
	echo "  可手动指定: KDIR=/path/to/headers ./build.sh"
	exit 1
fi
echo "内核树: $KDIR"

# ---------- 2) 时钟检查 ----------
YEAR=$(date +%Y)
if [ "$YEAR" -lt 2020 ]; then
	echo "警告: 系统时间是 $YEAR 年, 内核源码时间戳在未来, make 会无限重编"
	NEWEST=$(find "$KDIR" -name Makefile -printf '%T@\n' 2>/dev/null | sort -n | tail -1 | cut -d. -f1)
	if [ -n "$NEWEST" ]; then
		NEWTIME=$((NEWEST + 60))
		echo "  自动把时间调到 $NEWTIME (内核树最新时间戳+60s)"
		date -s "@$NEWTIME" > /dev/null
		date
	else
		echo "  无法自动修正, 请手动执行: date -s 'YYYY-MM-DD HH:MM:SS'"
		exit 1
	fi
fi

# ---------- 3) fixdep ----------
FIXDEP="$KDIR/scripts/basic/fixdep"
if [ ! -s "$FIXDEP" ]; then
	echo "重建 fixdep ..."
	rm -f "$FIXDEP"
	make -C "$KDIR" scripts > /tmp/kbuild-scripts.log 2>&1 || {
		echo "make scripts 失败, 见 /tmp/kbuild-scripts.log"; tail -20 /tmp/kbuild-scripts.log; exit 1; }
fi

# ---------- 4) modpost ----------
MODPOST="$KDIR/scripts/mod/modpost"
if [ ! -x "$MODPOST" ]; then
	echo "重建 modpost (头文件包没带) ..."
	INC="-I./arch/arm64/include -I./arch/arm64/include/generated -I./include"
	INC="$INC -I./arch/arm64/include/uapi -I./arch/arm64/include/generated/uapi"
	INC="$INC -I./include/uapi -I./include/generated/uapi"
	(
		cd "$KDIR"
		export NM=nm AWK=awk CC=cc HOSTCC=cc LD=ld HOSTLD=ld AR=ar HOSTAR=ar \
		       STRIP=strip OBJCOPY=objcopy READELF=readelf
		make -f scripts/Makefile.build obj=scripts/mod srctree=. \
		     LINUXINCLUDE="$INC" \
		     KBUILD_CPPFLAGS="-D__KERNEL__ -include ./include/linux/stddef.h"
	) > /tmp/kbuild-modpost.log 2>&1 || {
		echo "modpost 构建失败, 见 /tmp/kbuild-modpost.log"; tail -20 /tmp/kbuild-modpost.log; exit 1; }
fi

# ---------- 5) 编译模块 ----------
echo "编译 max17263.ko ..."
make -C "$KDIR" M="$HERE" modules

if [ -f "$HERE/max17263.ko" ]; then
	echo
	echo "成功: $HERE/max17263.ko"
	modinfo "$HERE/max17263.ko" 2>/dev/null | grep -E 'vermagic|filename' || true
else
	echo "失败: 没有生成 max17263.ko"
	exit 1
fi
