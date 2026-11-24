# RDK Duck MAX17263 驱动

RDK Duck 电池板（Battery Board V0P2）上 MAX17263 电量计的 Linux 内核驱动。

提供标准 `power_supply` 接口：读电压、电量、电流、温度，写电池容量。

---

## 一、适用硬件

| 项 | 值 |
|---|---|
| 芯片 | Maxim MAX17263（电量计 + LED 驱动） |
| I2C 总线 | `i2c-5` |
| 地址 | `0x36` |
| 采样电阻 | 10 mΩ |
| 平台 | RDK X5，内核 6.1.83 aarch64 |
| 电池 | 1S 锂电，1500 mAh（5550 mWh） |

---

## 二、安装

```bash
git clone git@github.com:hachi-leaf/rdk_duck_max17263.git
cd rdk_duck_max17263
sudo ./install.sh
```

`install.sh` 会完成：编译 → 安装到 `/lib/modules/$(uname -r)/extra/` →
设置开机自动加载 → 设置开机自动创建 I2C 设备 → 立即加载。

装完看状态：

```bash
./status.sh
```

```
电压      3.755 V
电量      50 %
电流      -726.6 mA  (放电)
温度      34.5 C
状态      Discharging
设计容量  1500 mAh
满充容量  1500 mAh
```

---

## 三、编译

```bash
./build.sh
```

生成 `max17263.ko`。只编译不安装时用这个。

也可以直接用 Makefile：

```bash
make                             # 编译
sudo make install                # 安装
make clean                       # 清理
```

内核树不在默认位置时指定：

```bash
make KDIR=/usr/src/linux-headers-6
```

---

## 四、sysfs 接口

全部在 `/sys/class/power_supply/max17263/`：

| 文件 | 权限 | 含义 |
|---|---|---|
| `voltage_now` | 读 | 电压，uV |
| `capacity` | 读 | 电量，0-100 % |
| `current_now` | 读 | 电流，uA，负=放电，正=充电 |
| `temp` | 读 | 温度，0.1 °C |
| `status` | 读 | `Charging` / `Discharging` / `Not charging` |
| `charge_full` | 读 | 满充容量，uAh |
| `charge_full_design` | 读 | 设计容量，uAh |
| `voltage_uv` | 读 | 电压 uV（便捷别名） |
| `soc_percent` | 读 | 电量 %（便捷别名） |
| `design_capacity_mah` | **读/写** | 设计容量 mAh，写入后自动刷新 |
| `refresh` | 写 | 手动触发电量计刷新 |

### 常用命令

```bash
# 电压（V）
cat /sys/class/power_supply/max17263/voltage_now | awk '{printf "%.3f V\n",$1/1e6}'

# 电量（%）
cat /sys/class/power_supply/max17263/capacity

# 电流（mA，负为放电）
cat /sys/class/power_supply/max17263/current_now | awk '{printf "%.1f mA\n",$1/1000}'

# 设置电池容量，mAh = mWh / 3.7
echo 1500 > /sys/class/power_supply/max17263/design_capacity_mah

# 换电池后刷新电量计
echo 1 > /sys/class/power_supply/max17263/refresh
```

---

## 五、模块参数

写在 `/etc/modprobe.d/max17263.conf`：

| 参数 | 默认 | 说明 |
|---|---|---|
| `design_capacity_mah` | 1500 | 电池设计容量 mAh，`0` = 不改动 |
| `led_bars` | 8 | LED 总格数 1-15 |
| `led_brightness` | 15 | 亮度 0-31 |
| `led_always_on` | 1 | `1` = 常亮，`0` = 按键点亮 10 秒 |
| `led_gray` | 0 | `1` = 余数用半亮表示，`0` = 四舍五入 |
| `do_refresh` | 1 | 需要时自动刷新电量计 |

例：改成按键点亮、亮度 24：

```
options max17263 led_always_on=0 led_brightness=24
```

---

## 六、LED 电量显示

芯片按 SOC 自动点亮对应格数，电量变化时格数自己跟着变，软件不需要参与。

`NBARS` 是"总共几格"，驱动默认设 8。下面是 8 格对照：

| 电量 | 亮几格 |
|---|---|
| 93.75-100% | 8 |
| 81.25-93.75% | 7 |
| 68.75-81.25% | 6 |
| 56.25-68.75% | 5 |
| 43.75-56.25% | 4 |
| 31.25-43.75% | 3 |
| 18.75-31.25% | 2 |
| 6.25-18.75% | 1 |
| 0-6.25% | 全灭 |

要改成按键点亮 10 秒：

```bash
# /etc/modprobe.d/max17263.conf
options max17263 led_always_on=0
```

在 MCU 上自己写寄存器时，寄存器级配置见 [docs/寄存器配置.md](docs/寄存器配置.md)。

---

## 七、卸载

```bash
sudo ./uninstall.sh
```

---

## 八、目录结构

```
.
├── max17263.c        驱动源码
├── Makefile
├── build.sh          编译
├── install.sh        编译 + 安装 + 开机自启
├── uninstall.sh      卸载
├── status.sh         打印全部状态
├── docs/
│   └── 寄存器配置.md  寄存器级参考（移植到 MCU 时看这个）
└── LICENSE
```

---

## 九、许可

驱动源码按 **GPL-2.0** 发布（内核模块需要 GPL 兼容），文件头已有 SPDX 标识。
仓库其余部分按 [MIT](LICENSE)。
