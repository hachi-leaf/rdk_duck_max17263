// SPDX-License-Identifier: GPL-2.0
/*
 * max17263.c - Maxim MAX17263 fuel gauge + LED driver
 *
 * Target: RDK Duck Battery Board V0P2 (I2C addr 0x36)
 *
 * sysfs interface (under /sys/class/power_supply/max17263/):
 *   voltage_now           (RO) uV            battery voltage
 *   capacity              (RO) %             state of charge
 *   current_now           (RO) uA            negative = discharging
 *   temp                  (RO) 0.1 degC
 *   status                (RO) Charging / Discharging / Not charging
 *   charge_full           (RO) uAh           FullCapRep
 *   charge_full_design    (RO) uAh           DesignCap
 *   soc_percent           (RO) %             same as capacity, convenience
 *   voltage_uv            (RO) uV            same as voltage_now, convenience
 *   design_capacity_mah   (RW) echo 1500  -> write DesignCap + refresh
 *   refresh               (WO) echo 1     -> trigger ModelCfg refresh
 *
 * The LED bargraph is initialised on probe, because MAX17263 has no NVM and
 * every power-on needs the LED registers rewritten.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/swab.h>
#include <linux/sysfs.h>
#include <linux/kstrtox.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/device.h>

/* ---- register map: 8-bit address, 16-bit word data ---- */
#define M5_REG_REPSOC		0x06
#define M5_REG_TEMP		0x08
#define M5_REG_VCELL		0x09
#define M5_REG_CURRENT		0x0A
#define M5_REG_FULLCAPREP	0x10
#define M5_REG_DESIGNCAP	0x18
#define M5_REG_LEDCFG3		0x37
#define M5_REG_LEDCFG1		0x40
#define M5_REG_LEDCFG2		0x4B
#define M5_REG_CUSTLED		0x64
#define M5_REG_MODELCFG		0xDB

#define M5_MODELCFG_REFRESH	BIT(15)

/* ---- module parameters ---- */
static int design_capacity_mah = 1500;
module_param(design_capacity_mah, int, 0644);
MODULE_PARM_DESC(design_capacity_mah,
		 "Battery design capacity in mAh, 0 = leave untouched (default 1500)");

static int led_bars = 8;
module_param(led_bars, int, 0644);
MODULE_PARM_DESC(led_bars, "Number of LED bars 1-15 (default 8)");

static int led_brightness = 15;
module_param(led_brightness, int, 0644);
MODULE_PARM_DESC(led_brightness, "LED brightness 0-31 (default 15)");

static int led_always_on = 1;
module_param(led_always_on, int, 0644);
MODULE_PARM_DESC(led_always_on, "1 = always on, 0 = push button (default 1)");

static int led_gray = 0;
module_param(led_gray, int, 0644);
MODULE_PARM_DESC(led_gray, "1 = gray remainder bar, 0 = rounding (default 0)");

static int do_refresh = 1;
module_param(do_refresh, int, 0644);
MODULE_PARM_DESC(do_refresh, "1 = ModelCfg refresh on probe (default 1)");

struct max17263_data {
	struct i2c_client *client;
	struct power_supply *battery;
	struct mutex lock;	/* serialises word access and byte-order probing */
	bool byteswap;		/* true if word data needs swab16() */
};

/* ---------------- word access ---------------- */

static int m5_read_locked(struct max17263_data *d, u8 reg, u16 *val)
{
	int ret = i2c_smbus_read_word_data(d->client, reg);

	if (ret < 0)
		return ret;
	*val = d->byteswap ? swab16((u16)ret) : (u16)ret;
	return 0;
}

static int m5_write_raw_locked(struct max17263_data *d, u8 reg, u16 val)
{
	return i2c_smbus_write_word_data(d->client, reg,
					 d->byteswap ? swab16(val) : val);
}

/* Write without read-back. Required for ModelCfg (bit15 self-clears) and
 * LEDCfg3 (bit14 is recomputed by the chip), where a verify would misfire. */
static int m5_write_noverify(struct max17263_data *d, u8 reg, u16 val)
{
	int ret;

	mutex_lock(&d->lock);
	ret = m5_write_raw_locked(d, reg, val);
	mutex_unlock(&d->lock);
	return ret;
}

/* Write and verify. On mismatch, flip the byte order once and retry. */
static int m5_write_verify(struct max17263_data *d, u8 reg, u16 val)
{
	u16 rb;
	int ret;

	mutex_lock(&d->lock);

	ret = m5_write_raw_locked(d, reg, val);
	if (ret < 0)
		goto out;
	ret = m5_read_locked(d, reg, &rb);
	if (ret < 0)
		goto out;
	if (rb == val)
		goto out;

	d->byteswap = !d->byteswap;
	ret = m5_write_raw_locked(d, reg, val);
	if (ret < 0)
		goto out;
	ret = m5_read_locked(d, reg, &rb);
	if (ret < 0)
		goto out;
	if (rb != val) {
		dev_err(&d->client->dev,
			"reg 0x%02x write failed: want 0x%04x got 0x%04x\n",
			reg, val, rb);
		ret = -EIO;
		goto out;
	}
	dev_info(&d->client->dev, "byte order flipped while writing 0x%02x\n", reg);
out:
	mutex_unlock(&d->lock);
	return ret;
}

static int m5_read(struct max17263_data *d, u8 reg, u16 *val)
{
	int ret;

	mutex_lock(&d->lock);
	ret = m5_read_locked(d, reg, val);
	mutex_unlock(&d->lock);
	return ret;
}

/*
 * Determine word byte order from DesignCap, which must land in 100..20000 mAh.
 * Only one interpretation can qualify, so this is unambiguous in practice.
 */
static int m5_detect_order(struct max17263_data *d)
{
	u16 ns, sw;
	int ret;

	ret = i2c_smbus_read_word_data(d->client, M5_REG_DESIGNCAP);
	if (ret < 0)
		return ret;

	ns = (u16)ret;
	sw = swab16((u16)ret);

	if (ns >= 100 && ns <= 20000 && !(sw >= 100 && sw <= 20000))
		d->byteswap = false;
	else if (!(ns >= 100 && ns <= 20000) && sw >= 100 && sw <= 20000)
		d->byteswap = true;
	else
		dev_warn(&d->client->dev,
			 "byte order ambiguous (raw 0x%04x), assuming direct\n", ns);

	dev_info(&d->client->dev, "DesignCap read as %u mAh, byteswap=%d\n",
		 d->byteswap ? sw : ns, d->byteswap);
	return 0;
}

/* ---------------- ModelCfg refresh ---------------- */

static int m5_refresh(struct max17263_data *d)
{
	u16 cfg;
	int ret;

	ret = m5_read(d, M5_REG_MODELCFG, &cfg);
	if (ret < 0)
		return ret;
	ret = m5_write_noverify(d, M5_REG_MODELCFG, cfg | M5_MODELCFG_REFRESH);
	if (ret < 0)
		return ret;
	msleep(100);
	return 0;
}

/* ---------------- LED bargraph ---------------- */

static int m5_led_init(struct max17263_data *d)
{
	int bars = clamp_val(led_bars, 1, 15);
	int bright = clamp_val(led_brightness, 0, 31);
	u16 cfg1, cfg2;
	int ret;

	/* LEDCfg1: [15:13] LEDTimer [12:10] AniStep [9:8] AniMd
	 *          [7:6] LEDMd [5] LChg [4] GrEn [3:0] NBARS */
	if (led_always_on) {
		cfg1 = 0x8000;		/* timer field unused in always-on mode */
		cfg1 |= (u16)(3 << 6);	/* LEDMd = 11 */
	} else {
		cfg1 = (u16)(7 << 13);	/* 10 s */
		cfg1 |= (u16)(1 << 6);	/* LEDMd = 01 */
	}
	cfg1 |= BIT(5);			/* LChg: stay on while charging */
	if (led_gray)
		cfg1 |= BIT(4);		/* GrEn */
	cfg1 |= (u16)(bars & 0xF);	/* NBARS = total bar count */

	/*
	 * LEDCfg2: D8 EnAutoLEDCnt MUST stay 0. With it set the chip runs LED
	 * auto-detection at power-up, fails to count the bars on this board and
	 * clears NBARS to 0, leaving the display dark. D6 EBlink keeps the
	 * lowest bar blinking near empty.
	 */
	cfg2 = BIT(6) | (u16)(bright & 0x1F);

	/* LEDCfg3 bit14 is chip-computed, so no verify here. */
	ret = m5_write_noverify(d, M5_REG_LEDCFG3, 0xC000);
	if (ret < 0)
		return ret;
	ret = m5_write_verify(d, M5_REG_LEDCFG2, cfg2);
	if (ret < 0)
		return ret;
	ret = m5_write_verify(d, M5_REG_LEDCFG1, cfg1);
	if (ret < 0)
		return ret;
	ret = m5_write_verify(d, M5_REG_CUSTLED, 0x0000);
	if (ret < 0)
		return ret;

	dev_info(&d->client->dev,
		 "LED: bars=%d bright=%d always_on=%d gray=%d cfg1=0x%04x cfg2=0x%04x\n",
		 bars, bright, led_always_on, led_gray, cfg1, cfg2);
	return 0;
}

/* ---------------- power_supply ---------------- */

static int m5_get_property(struct power_supply *psy,
			   enum power_supply_property psp,
			   union power_supply_propval *val)
{
	struct max17263_data *d = power_supply_get_drvdata(psy);
	u16 raw;
	s16 s;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = m5_read(d, M5_REG_VCELL, &raw);
		if (ret < 0)
			return ret;
		val->intval = (int)((u32)raw * 625u / 8u);	/* 78.125 uV */
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = m5_read(d, M5_REG_CURRENT, &raw);
		if (ret < 0)
			return ret;
		s = (s16)raw;
		val->intval = (int)((s32)s * 625 / 4);		/* 156.25 uA */
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = m5_read(d, M5_REG_REPSOC, &raw);
		if (ret < 0)
			return ret;
		val->intval = (int)(raw / 256);			/* LSB 1/256 % */
		if (val->intval > 100)
			val->intval = 100;
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		ret = m5_read(d, M5_REG_TEMP, &raw);
		if (ret < 0)
			return ret;
		s = (s16)raw;
		val->intval = (int)((s32)s * 10 / 256);		/* 1/256 degC */
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = m5_read(d, M5_REG_FULLCAPREP, &raw);
		if (ret < 0)
			return ret;
		val->intval = (int)raw * 1000;			/* mAh -> uAh */
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = m5_read(d, M5_REG_DESIGNCAP, &raw);
		if (ret < 0)
			return ret;
		val->intval = (int)raw * 1000;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		ret = m5_read(d, M5_REG_CURRENT, &raw);
		if (ret < 0)
			return ret;
		s = (s16)raw;
		if (s < -1000)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (s > 1000)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property m5_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

/* ---------------- custom attributes ---------------- */

static struct max17263_data *psy_dev_to_data(struct device *dev)
{
	return power_supply_get_drvdata(dev_get_drvdata(dev));
}

static ssize_t voltage_uv_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct max17263_data *d = psy_dev_to_data(dev);
	u16 raw;
	int ret = m5_read(d, M5_REG_VCELL, &raw);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%u\n", (u32)raw * 625u / 8u);
}
static DEVICE_ATTR_RO(voltage_uv);

static ssize_t soc_percent_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct max17263_data *d = psy_dev_to_data(dev);
	u16 raw;
	int ret = m5_read(d, M5_REG_REPSOC, &raw);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%u\n", (unsigned int)(raw / 256));
}
static DEVICE_ATTR_RO(soc_percent);

static ssize_t design_capacity_mah_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct max17263_data *d = psy_dev_to_data(dev);
	u16 raw;
	int ret = m5_read(d, M5_REG_DESIGNCAP, &raw);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%u\n", (unsigned int)raw);
}

static ssize_t design_capacity_mah_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct max17263_data *d = psy_dev_to_data(dev);
	u16 cur, v;
	int ret;

	ret = kstrtou16(buf, 0, &v);
	if (ret)
		return ret;
	if (v < 1 || v > 30000)
		return -EINVAL;

	ret = m5_read(d, M5_REG_DESIGNCAP, &cur);
	if (ret < 0)
		return ret;

	ret = m5_write_verify(d, M5_REG_DESIGNCAP, v);
	if (ret < 0)
		return ret;

	/* DesignCap only reaches FullCapRep/SOC through a refresh. */
	if (v != cur) {
		ret = m5_refresh(d);
		if (ret < 0)
			return ret;
		dev_info(&d->client->dev,
			 "design capacity %u -> %u mAh, ModelCfg refreshed\n",
			 cur, v);
	}
	return count;
}
static DEVICE_ATTR_RW(design_capacity_mah);

static ssize_t refresh_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct max17263_data *d = psy_dev_to_data(dev);
	int ret = m5_refresh(d);

	if (ret < 0)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(refresh);

static struct attribute *m5_attrs[] = {
	&dev_attr_voltage_uv.attr,
	&dev_attr_soc_percent.attr,
	&dev_attr_design_capacity_mah.attr,
	&dev_attr_refresh.attr,
	NULL,
};

static const struct attribute_group m5_attr_group = {
	.attrs = m5_attrs,
};

static const struct attribute_group *m5_attr_groups[] = {
	&m5_attr_group,
	NULL,
};

/* ---------------- probe ---------------- */

static int max17263_probe(struct i2c_client *client)
{
	struct max17263_data *d;
	struct power_supply_desc *desc;
	struct power_supply_config psy_cfg = {};
	u16 cap;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WORD_DATA))
		return -EIO;

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	mutex_init(&d->lock);
	i2c_set_clientdata(client, d);

	ret = m5_detect_order(d);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "byte order detect failed\n");

	ret = m5_read(d, M5_REG_DESIGNCAP, &cap);
	if (ret < 0)
		return ret;

	if (design_capacity_mah > 0 && cap != design_capacity_mah) {
		ret = m5_write_verify(d, M5_REG_DESIGNCAP,
				      (u16)design_capacity_mah);
		if (ret < 0)
			return ret;
		dev_info(&client->dev, "DesignCap %u -> %d mAh\n",
			 cap, design_capacity_mah);
		cap = (u16)design_capacity_mah;
	}

	/*
	 * Only refresh when FullCapRep and DesignCap disagree, i.e. after a
	 * battery swap or a capacity change. Refreshing on every boot would
	 * make SOC jump, since a refresh resets FullCap to DesignCap.
	 */
	if (do_refresh) {
		u16 full;

		ret = m5_read(d, M5_REG_FULLCAPREP, &full);
		if (ret < 0)
			return ret;
		if (full != cap) {
			ret = m5_refresh(d);
			if (ret < 0)
				return ret;
			dev_info(&client->dev,
				 "ModelCfg refreshed (FullCapRep %u != DesignCap %u)\n",
				 full, cap);
		}
	}

	ret = m5_led_init(d);
	if (ret < 0)
		dev_warn(&client->dev, "LED init failed: %d\n", ret);

	/* desc must outlive probe: power_supply keeps a pointer to it. */
	desc = devm_kzalloc(&client->dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	desc->name = "max17263";
	desc->type = POWER_SUPPLY_TYPE_BATTERY;
	desc->properties = m5_props;
	desc->num_properties = ARRAY_SIZE(m5_props);
	desc->get_property = m5_get_property;

	psy_cfg.drv_data = d;
	psy_cfg.fwnode = dev_fwnode(&client->dev);
	psy_cfg.attr_grp = m5_attr_groups;

	d->battery = devm_power_supply_register(&client->dev, desc, &psy_cfg);
	if (IS_ERR(d->battery))
		return dev_err_probe(&client->dev, PTR_ERR(d->battery),
				     "failed to register power supply\n");

	dev_info(&client->dev, "MAX17263 ready at 0x%02x\n", client->addr);
	return 0;
}

static const struct i2c_device_id max17263_id[] = {
	{ "max17263", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max17263_id);

static const struct of_device_id max17263_of_match[] = {
	{ .compatible = "maxim,max17263" },
	{ }
};
MODULE_DEVICE_TABLE(of, max17263_of_match);

static struct i2c_driver max17263_driver = {
	.driver = {
		.name = "max17263",
		.of_match_table = max17263_of_match,
	},
	.probe_new = max17263_probe,
	.id_table = max17263_id,
};
module_i2c_driver(max17263_driver);

MODULE_AUTHOR("RDK Duck");
MODULE_DESCRIPTION("MAX17263 fuel gauge and LED driver");
MODULE_LICENSE("GPL");
