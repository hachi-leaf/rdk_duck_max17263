# rdk_duck_max17263

RDK Duck 电池板 MAX17263 电量计的 Linux 内核驱动。

I2C-5 / 地址 0x36 / RDK X5 / 内核 6.1.83

## 安装

```bash
git clone git@github.com:hachi-leaf/rdk_duck_max17263.git
cd rdk_duck_max17263
sudo ./install.sh
```

## 使用

```bash
# 电压
cat /sys/class/power_supply/max17263/voltage_now

# 电量 %
cat /sys/class/power_supply/max17263/capacity

# 电流，负为放电
cat /sys/class/power_supply/max17263/current_now

# 设置电池容量，mAh = mWh / 3.7
echo 1500 > /sys/class/power_supply/max17263/design_capacity_mah
```

一次看全部：

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

## 接口

| 文件 | 权限 | 含义 |
|---|---|---|
| `voltage_now` | 读 | 电压 uV |
| `capacity` | 读 | 电量 % |
| `current_now` | 读 | 电流 uA |
| `temp` | 读 | 温度 0.1°C |
| `status` | 读 | 充电状态 |
| `charge_full` | 读 | 满充容量 uAh |
| `charge_full_design` | 读 | 设计容量 uAh |
| `design_capacity_mah` | 读/写 | 设计容量 mAh |
| `refresh` | 写 | 刷新电量计 |

## 参数

`/etc/modprobe.d/max17263.conf`：

| 参数 | 默认 | 说明 |
|---|---|---|
| `design_capacity_mah` | 1500 | 电池容量 |
| `led_bars` | 8 | LED 格数 |
| `led_brightness` | 15 | 亮度 0-31 |
| `led_always_on` | 1 | 0 = 按键点亮 10 秒 |
| `led_gray` | 0 | 1 = 灰阶 |

## 只编译 / 卸载

```bash
./build.sh              # 生成 max17263.ko
sudo ./uninstall.sh     # 卸载
```

寄存器级配置（MCU 移植用）见 [docs/寄存器配置.md](docs/寄存器配置.md)。

## 许可

驱动 GPL-2.0，其余 MIT。
