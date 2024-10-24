// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2024 John Watts <contact@jookia.org>

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* TODO:
 * - Check if LRCK switches channel numbers in TDM
 * - SPI support
 * - Non-TDM mode
 * - LRCK generation
 * - MCLK from oscillator
 * - DAPM power down unused ADC channels
 * - SDEN pin selection in device tree
 * - Overflow checking
 */

/* NOTES:
 * - TDM is a shift register that starts clocking out when LRCK goes low,
 *   but you can keep LRCK low and use the DSP_A format.
 * - You don't have to clock out all bits, clocking out just 2 or 4 slots
 *   out of 8 works well.
 * - The data sheet says the TDM format is left justified, but it's
 *   actually standard I2S format. Figure 12 seems to confirm this.
 * - All SDOUT pins do not remain active during TDM mode.
 */

#define REG_REVI 0x00
#define REG_GCTL 0x01
#define REG_OVFL 0x02
#define REG_OVFM 0x03
#define REG_HPF 0x04
#define REG_RSVD1 0x05
#define REG_PDN 0x06
#define REG_RSVD2 0x07
#define REG_MUTE 0x08
#define REG_RSVD3 0x09
#define REG_SDEN 0x0A
#define MAX_REG REG_SDEN

#define REG_GCTL_MDIV_MASK 0x30

static const struct reg_sequence cs5368_reg_init[] = {
	{ REG_GCTL, 0x8B }, /* CP-EN, TDM format, slave audio clocking */
	{ REG_OVFM, 0x00 }, /* Mask all overflows */
	{ REG_SDEN, 0x0A }, /* Only enable TDM and _TDM pins */
};

static const struct reg_default cs5368_reg_defaults[] = {
	{ REG_REVI, 0x80 }, /* Assume revision A by default */
	{ REG_GCTL, 0x00 },
	{ REG_OVFL, 0xFF },
	{ REG_OVFM, 0xFF },
	{ REG_HPF, 0x00 },
	{ REG_PDN, 0x00 },
	{ REG_MUTE, 0x00 },
	{ REG_SDEN, 0x00 },
};

static const struct regmap_range cs5368_rd_no_ranges[] = {
	regmap_reg_range(REG_RSVD1, REG_RSVD1),
	regmap_reg_range(REG_RSVD2, REG_RSVD2),
	regmap_reg_range(REG_RSVD3, REG_RSVD3),
};

static const struct regmap_range cs5368_wr_no_ranges[] = {
	regmap_reg_range(REG_REVI, REG_REVI),
	regmap_reg_range(REG_RSVD1, REG_RSVD1),
	regmap_reg_range(REG_RSVD2, REG_RSVD2),
	regmap_reg_range(REG_RSVD3, REG_RSVD3),
};

static const struct regmap_range cs5368_volatile_yes_ranges[] = {
	regmap_reg_range(REG_OVFL, REG_OVFL),
};

static const struct regmap_access_table cs5368_rd_table = {
	.no_ranges = cs5368_rd_no_ranges,
	.n_no_ranges = ARRAY_SIZE(cs5368_rd_no_ranges),
};

static const struct regmap_access_table cs5368_wr_table = {
	.no_ranges = cs5368_wr_no_ranges,
	.n_no_ranges = ARRAY_SIZE(cs5368_wr_no_ranges),
};

static const struct regmap_access_table cs5368_volatile_table = {
	.yes_ranges = cs5368_volatile_yes_ranges,
	.n_yes_ranges = ARRAY_SIZE(cs5368_volatile_yes_ranges),
};

static const struct regmap_config cs5368_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.reg_defaults = cs5368_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs5368_reg_defaults),
	.max_register = MAX_REG,
	.rd_table = &cs5368_rd_table,
	.wr_table = &cs5368_wr_table,
	.volatile_table = &cs5368_volatile_table,
	.read_flag_mask = 0x80, /* Set INCR bit so batch reads work */
	.write_flag_mask = 0x80, /* Set INCR bit so batch writes work */
};

static const char *const supply_names[] = {
	"va",
	"vd",
	"vlc",
	"vls",
	"vx",
};

struct cs5368_priv {
	struct regulator_bulk_data regulators[ARRAY_SIZE(supply_names)];
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	unsigned int mclk_freq;
	bool tdm;
};

static const struct snd_kcontrol_new cs5368_snd_controls[] = {
	SOC_SINGLE("AIN1 High-Pass Filter Switch", 0x4, 0, 1, 1),
	SOC_SINGLE("AIN2 High-Pass Filter Switch", 0x4, 1, 1, 1),
	SOC_SINGLE("AIN3 High-Pass Filter Switch", 0x4, 2, 1, 1),
	SOC_SINGLE("AIN4 High-Pass Filter Switch", 0x4, 3, 1, 1),
	SOC_SINGLE("AIN5 High-Pass Filter Switch", 0x4, 4, 1, 1),
	SOC_SINGLE("AIN6 High-Pass Filter Switch", 0x4, 5, 1, 1),
	SOC_SINGLE("AIN7 High-Pass Filter Switch", 0x4, 6, 1, 1),
	SOC_SINGLE("AIN8 High-Pass Filter Switch", 0x4, 7, 1, 1),
};

static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain1 =
	SOC_DAPM_SINGLE("Switch", 0x8, 0, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain2 =
	SOC_DAPM_SINGLE("Switch", 0x8, 1, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain3 =
	SOC_DAPM_SINGLE("Switch", 0x8, 2, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain4 =
	SOC_DAPM_SINGLE("Switch", 0x8, 3, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain5 =
	SOC_DAPM_SINGLE("Switch", 0x8, 4, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain6 =
	SOC_DAPM_SINGLE("Switch", 0x8, 5, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain7 =
	SOC_DAPM_SINGLE("Switch", 0x8, 6, 1, 1);
static const struct snd_kcontrol_new cs5368_snd_controls_mute_ain8 =
	SOC_DAPM_SINGLE("Switch", 0x8, 7, 1, 1);

static const struct snd_soc_dapm_widget cs5368_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("AIN1"),
	SND_SOC_DAPM_INPUT("AIN2"),
	SND_SOC_DAPM_INPUT("AIN3"),
	SND_SOC_DAPM_INPUT("AIN4"),
	SND_SOC_DAPM_INPUT("AIN5"),
	SND_SOC_DAPM_INPUT("AIN6"),
	SND_SOC_DAPM_INPUT("AIN7"),
	SND_SOC_DAPM_INPUT("AIN8"),
	SND_SOC_DAPM_ADC("AIN12", NULL, 0x6, 0, 1),
	SND_SOC_DAPM_ADC("AIN34", NULL, 0x6, 1, 1),
	SND_SOC_DAPM_ADC("AIN56", NULL, 0x6, 2, 1),
	SND_SOC_DAPM_ADC("AIN78", NULL, 0x6, 3, 1),
	SND_SOC_DAPM_SWITCH("AIN1 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain1),
	SND_SOC_DAPM_SWITCH("AIN2 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain2),
	SND_SOC_DAPM_SWITCH("AIN3 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain3),
	SND_SOC_DAPM_SWITCH("AIN4 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain4),
	SND_SOC_DAPM_SWITCH("AIN5 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain5),
	SND_SOC_DAPM_SWITCH("AIN6 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain6),
	SND_SOC_DAPM_SWITCH("AIN7 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain7),
	SND_SOC_DAPM_SWITCH("AIN8 Capture", SND_SOC_NOPM, 0, 0,
			    &cs5368_snd_controls_mute_ain8),
	SND_SOC_DAPM_AIF_OUT("TDM1", "Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM2", "Capture", 1, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM3", "Capture", 2, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM4", "Capture", 3, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM5", "Capture", 4, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM6", "Capture", 5, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM7", "Capture", 6, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TDM8", "Capture", 7, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route cs5368_dapm_routes[] = {
	{ "AIN12", NULL, "AIN1" },
	{ "AIN12", NULL, "AIN2" },
	{ "AIN34", NULL, "AIN3" },
	{ "AIN34", NULL, "AIN4" },
	{ "AIN56", NULL, "AIN5" },
	{ "AIN56", NULL, "AIN6" },
	{ "AIN78", NULL, "AIN7" },
	{ "AIN78", NULL, "AIN8" },
	{ "AIN1 Capture", "Switch", "AIN12" },
	{ "AIN2 Capture", "Switch", "AIN12" },
	{ "AIN3 Capture", "Switch", "AIN34" },
	{ "AIN4 Capture", "Switch", "AIN34" },
	{ "AIN5 Capture", "Switch", "AIN56" },
	{ "AIN6 Capture", "Switch", "AIN56" },
	{ "AIN7 Capture", "Switch", "AIN78" },
	{ "AIN8 Capture", "Switch", "AIN78" },
	{ "TDM1", NULL, "AIN1 Capture" },
	{ "TDM2", NULL, "AIN2 Capture" },
	{ "TDM3", NULL, "AIN3 Capture" },
	{ "TDM4", NULL, "AIN4 Capture" },
	{ "TDM5", NULL, "AIN5 Capture" },
	{ "TDM6", NULL, "AIN6 Capture" },
	{ "TDM7", NULL, "AIN7 Capture" },
	{ "TDM8", NULL, "AIN8 Capture" },
};

static int cs5368_dai_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct cs5368_priv *priv = dev_get_drvdata(dev);

	int rate = params_rate(params);
	int lrck_speed = 0;
	int rc = 0;

	if (rate < 54000)
		lrck_speed = 256;
	else if (rate < 108000)
		lrck_speed = 128;
	else
		lrck_speed = 64;

	int mclk_div = priv->mclk_freq / rate / lrck_speed;
	int mdiv = 0;

	if (mclk_div == 4)
		mdiv = 3;
	else if (mclk_div == 2)
		mdiv = 1;
	else if (mclk_div == 1)
		mdiv = 0;
	else {
		dev_err(dev, "unknown mclk divider\n");
		return -EINVAL;
	}

	rc = regmap_update_bits(priv->regmap, REG_GCTL, REG_GCTL_MDIV_MASK, mdiv << 4);
	if (rc != 0) {
		dev_err(dev, "failed to set mclk divider\n");
		return rc;
	}

	return 0;
}

static int cs5368_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	int format_mode = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
	int clock_mode = fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK;

	struct device *dev = dai->dev;

	if (format_mode != SND_SOC_DAIFMT_I2S &&
	    format_mode != SND_SOC_DAIFMT_DSP_A) {
		dev_err(dev, "codec only supports I2S or DSP_A TDM formats\n");
		return -EINVAL;
	}

	if (clock_mode != SND_SOC_DAIFMT_BC_FC) {
		dev_err(dev, "driver currently only supports clock slave mode\n");
		return -EINVAL;
	}

	return 0;
}

static int cs5368_dai_set_tdm_slot(struct snd_soc_dai *dai,
				   unsigned int tx_mask,
				   unsigned int rx_mask,
				   int slots, int slot_width)
{
	struct device *dev = dai->dev;
	struct cs5368_priv *priv = dev_get_drvdata(dev);

	if (slots != 8 && slots != 4 && slots != 2) {
		dev_err(dev, "codec requires 8, 4 or 2 TDM slots\n");
		return -EINVAL;
	}

	if (slot_width != 32) {
		dev_err(dev, "codec requires 32-bit TDM slot width\n");
		return -EINVAL;
	}

	priv->tdm = true;

	return 0;
}

static const struct snd_soc_dai_ops snd_soc_dai_ops_cs5368 = {
	.hw_params = cs5368_dai_hw_params,
	.set_fmt = cs5368_dai_set_fmt,
	.set_tdm_slot = cs5368_dai_set_tdm_slot,
};

static int cs5368_codec_set_sysclk(struct snd_soc_component *comp, int clk_id,
				   int source, unsigned int freq, int dir)
{
	struct device *dev = comp->dev;
	struct cs5368_priv *priv = dev_get_drvdata(dev);

	if (dir != SND_SOC_CLOCK_IN) {
		dev_err(dev, "driver currently only supports clock input\n");
		return -EINVAL;
	}

	priv->mclk_freq = freq;

	return 0;
}

struct snd_soc_dai_driver soc_dai_cs5368 = {
	.capture = {
		.channels_max = 8,
		.channels_min = 2,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
		.rate_max = 216000,
		.rate_min = 2000,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.sig_bits = 24,
		.stream_name = "Capture",
	},
	.name = "cs5368",
	.ops = &snd_soc_dai_ops_cs5368,
};

static const struct snd_soc_component_driver soc_component_dev_cs5368 = {
	.controls = cs5368_snd_controls,
	.dapm_routes = cs5368_dapm_routes,
	.dapm_widgets = cs5368_dapm_widgets,
	.endianness = 1,
	.num_controls = ARRAY_SIZE(cs5368_snd_controls),
	.num_dapm_routes = ARRAY_SIZE(cs5368_dapm_routes),
	.num_dapm_widgets = ARRAY_SIZE(cs5368_dapm_widgets),
	.set_sysclk = cs5368_codec_set_sysclk,
};

static int cs5368_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cs5368_priv *priv;
	int rc;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);

	priv->regmap = devm_regmap_init_i2c(client, &cs5368_regmap_config);
	if (IS_ERR(priv->regmap)) {
		rc = PTR_ERR(priv->regmap);
		dev_err(dev, "regmap init failed: %d\n", rc);
		return rc;
	}
	regcache_cache_only(priv->regmap, true);
	rc = regmap_multi_reg_write(priv->regmap, cs5368_reg_init,
				    ARRAY_SIZE(cs5368_reg_init));
	if (rc != 0) {
		dev_err(dev, "regmap_multi_reg_write failed: %d\n", rc);
		return rc;
	}

	regulator_bulk_set_supply_names(priv->regulators, supply_names,
					ARRAY_SIZE(priv->regulators));
	rc = devm_regulator_bulk_get(dev, ARRAY_SIZE(priv->regulators),
				     priv->regulators);
	if (rc != 0) {
		dev_err(dev, "regulator_bulk_get failed: %d\n", rc);
		return rc;
	}

	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio)) {
		rc = PTR_ERR(priv->reset_gpio);
		dev_err(dev, "failed to get reset gpio: %d\n", rc);
		return rc;
	}

	rc = devm_pm_runtime_enable(dev);
	if (rc != 0) {
		dev_err(dev, "devm_pm_runtime_enable failed: %d\n", rc);
		return rc;
	}

	return devm_snd_soc_register_component(
		&client->dev, &soc_component_dev_cs5368, &soc_dai_cs5368, 1);
}

static int cs5368_pm_runtime_suspend(struct device *dev)
{
	struct cs5368_priv *priv = dev_get_drvdata(dev);
	int rc;

	regcache_cache_only(priv->regmap, true);
	gpiod_set_value_cansleep(priv->reset_gpio, 1);

	rc = regulator_bulk_disable(ARRAY_SIZE(priv->regulators),
				    priv->regulators);
	if (rc != 0) {
		dev_err(dev, "regulator_bulk_disable failed: %d\n", rc);
		return rc;
	}

	return 0;
}

static int cs5368_pm_runtime_resume(struct device *dev)
{
	struct cs5368_priv *priv = dev_get_drvdata(dev);
	int rc;

	rc = regulator_bulk_enable(ARRAY_SIZE(priv->regulators),
				   priv->regulators);
	if (rc != 0) {
		dev_err(dev, "regulator_bulk_enable failed: %d\n", rc);
		return rc;
	}

	gpiod_set_value_cansleep(priv->reset_gpio, 0);

	regcache_cache_only(priv->regmap, false);
	regcache_mark_dirty(priv->regmap);
	rc = regcache_sync(priv->regmap);
	if (rc != 0) {
		dev_err(dev, "regcache_sync failed: %d\n", rc);
		return rc;
	}

	return 0;
}

static const struct of_device_id cs5368_of_match[] = {
	{ .compatible = "cirrus,cs5368" },
	{}
};
MODULE_DEVICE_TABLE(of, cs5368_of_match);

static const struct i2c_device_id cs5368_i2c_id[] = {
	{ "cs5368", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, cs5368_i2c_id);

DEFINE_RUNTIME_DEV_PM_OPS(cs5368_pm_ops, cs5368_pm_runtime_suspend,
			  cs5368_pm_runtime_resume, NULL);

static struct i2c_driver cs5368_codec_driver = {
	.driver = {
		.name = "cs5368",
		.of_match_table = cs5368_of_match,
		.pm = pm_ptr(&cs5368_pm_ops),
	},
	.id_table = cs5368_i2c_id,
	.probe = cs5368_i2c_probe,
};
module_i2c_driver(cs5368_codec_driver);

MODULE_DESCRIPTION("ASoC CS5368 driver");
MODULE_AUTHOR("John Watts <contact@jookia.org>");
MODULE_LICENSE("GPL");
