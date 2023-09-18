// SPDX-License-Identifier: GPL-2.0
/*
 * Fascontek FS028VG047 panel driver
 *
 * Copyright (C) 2023 John Watts <contact@jookia.org>
 */

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

static int do_panel_init(struct mipi_dbi *dbi)
{
	// PAGE 3
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13);
	mipi_dbi_command(dbi, 0xEF, 0x08);
	// PAGE 1
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x10);
	// Display Line Setting
	mipi_dbi_command(dbi, 0xC0, 0x4F, 0x00);
	// Porch Control
	mipi_dbi_command(dbi, 0xC1, 0x10, 0x02);
	// Inversion set
	// 0x31 = 31 2-DOT 37-Column
	mipi_dbi_command(dbi, 0xC2, 0x31, 0x02);
	mipi_dbi_command(dbi, 0xCC, 0x10);
	// Positive Voltage Gamma Control
	mipi_dbi_command(dbi, 0xB0, 0x00, 0x10, 0x17, 0x0D, 0x11, 0x06, 0x05,
	0x08, 0x07, 0x1F, 0x04, 0x11, 0x0E, 0x29, 0x30, 0x1F);
	// Negative Voltage Gamma Control
	mipi_dbi_command(dbi, 0xB1, 0x00, 0x0D, 0x14, 0x0E, 0x11, 0x06, 0x04,
	0x08, 0x08, 0x20, 0x05, 0x13, 0x13, 0x26, 0x30, 0x1F);
	//*******power set********//
	// PAGE 1
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x11);
	// Vop Amplitude setting
	// 0x65 = Vop
	mipi_dbi_command(dbi, 0xB0, 0x65);
	// VCOM amplitude setting
	mipi_dbi_command(dbi, 0xB1, 0x7C);
	// VGH Voltage setting
	// 0x87 = VGH=+15V
	mipi_dbi_command(dbi, 0xB2, 0x87);
	// TEST Command Setting
	mipi_dbi_command(dbi, 0xB3, 0x80);
	// VGL Voltage setting
	// 0x49 = VGL-10V
	mipi_dbi_command(dbi, 0xB5, 0x49);
	// Power Control 1
	mipi_dbi_command(dbi, 0xB7, 0x85);
	// Power Control 2
	// 0x20 = AVDD=6.6 & AVCL=-4.6
	mipi_dbi_command(dbi, 0xB8, 0x20);
	// Source pre_drive timing set1
	mipi_dbi_command(dbi, 0xC1, 0x78);
	// Source EQ2 Setting
	mipi_dbi_command(dbi, 0xC2, 0x78);
	// Source EQ2 Setting
	mipi_dbi_command(dbi, 0xD0, 0x88);
	mipi_dbi_command(dbi, 0xEE, 0x42);
	//*********GIP SET*************//
	mipi_dbi_command(dbi, 0xE0, 0x00, 0x00, 0x02);
	mipi_dbi_command(dbi, 0xE1, 0x04, 0xA0, 0x06, 0xA0, 0x05, 0xA0, 0x07,
	0xA0, 0x00, 0x44, 0x44);
	mipi_dbi_command(dbi, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dbi_command(dbi, 0xE3, 0x00, 0x00, 0x22, 0x22);
	mipi_dbi_command(dbi, 0xE4, 0x44, 0x44);
	mipi_dbi_command(dbi, 0xE5, 0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0,
	0xA0, 0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0);
	mipi_dbi_command(dbi, 0xE6, 0x00, 0x00, 0x22, 0x22);
	mipi_dbi_command(dbi, 0xE7, 0x44, 0x44);
	mipi_dbi_command(dbi, 0xE8, 0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0,
	0xA0, 0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0);
	mipi_dbi_command(dbi, 0xEB, 0x00, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x00);
	mipi_dbi_command(dbi, 0xED, 0xFF, 0xF5, 0x47, 0x6F, 0x0B, 0xA1, 0xAB,
	0xFF, 0xFF, 0xBA, 0x1A, 0xB0, 0xF6, 0x74, 0x5F, 0xFF);
	mipi_dbi_command(dbi, 0xEF, 0x08, 0x08, 0x08, 0x40, 0x3F, 0x64);
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00);
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13);
	mipi_dbi_command(dbi, 0xE6, 0x7C);
	mipi_dbi_command(dbi, 0xE8, 0x00, 0x0E);
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00);
	mipi_dbi_command(dbi, 0x11);
	msleep(120);
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13);
	mipi_dbi_command(dbi, 0xE8, 0x00, 0x0C);
	msleep(10);
	mipi_dbi_command(dbi, 0xE8, 0x00, 0x00);
	// PAGE 0
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00);
	mipi_dbi_command(dbi, 0x11);
	msleep(120);
	mipi_dbi_command(dbi, 0xE8, 0x00, 0x00);
	mipi_dbi_command(dbi, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00);
	// 0x70 = 24 bit
	mipi_dbi_command(dbi, 0x3A, 0x70);
	mipi_dbi_command(dbi, 0x29);
	msleep(25);
	return 0;
}

struct panel_priv {
	struct device *dev;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	struct mipi_dbi dbi;
	struct drm_panel panel;
};

static const struct drm_display_mode default_mode = {
	.clock = 20000,
	.hdisplay = 480,
	.hsync_start = 480 + 8,
	.hsync_end = 480 + 8 + 2,
	.htotal = 480 + 8 + 2 + 12,
	.vdisplay = 640,
	.vsync_start = 640 + 8,
	.vsync_end = 640 + 8 + 2,
	.vtotal = 640 + 8 + 2 + 12,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static struct panel_priv *to_panel_priv(struct drm_panel *panel)
{
	return container_of(panel, struct panel_priv, panel);
}

static int panel_prepare(struct drm_panel *panel)
{
	struct panel_priv *priv = to_panel_priv(panel);
	struct mipi_dbi *dbi = &priv->dbi;
	int err;

	err = regulator_enable(priv->supply);
	if (err) {
		dev_err(priv->dev, "Failed to enable power supply: %d\n", err);
		return err;
	}

	/* Reset the chip */
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	usleep_range(10, 1000);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	msleep(150);

	err = do_panel_init(dbi);
	if (err) {
		dev_err(priv->dev, "Failed to run panel initialization: %d\n", err);
		goto err_disable_reg;
	}

	err = mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	if (err) {
		dev_err(priv->dev, "Failed to exit sleep mode: %d\n", err);
		goto err_disable_reg;
	}

	msleep(150);

	return 0;

err_disable_reg:
	regulator_disable(priv->supply);
	return err;
}

static int panel_unprepare(struct drm_panel *panel)
{
	struct panel_priv *priv = to_panel_priv(panel);

	regulator_disable(priv->supply);

	return 0;
}

static int panel_enable(struct drm_panel *panel)
{
	struct panel_priv *priv = to_panel_priv(panel);
	struct mipi_dbi *dbi = &priv->dbi;
	int err;

	err = mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	if (err) {
		dev_err(priv->dev, "Failed to set display on: %d\n", err);
		return err;
	}

	return 0;
}

static int panel_disable(struct drm_panel *panel)
{
	struct panel_priv *priv = to_panel_priv(panel);
	struct mipi_dbi *dbi = &priv->dbi;
	int err;

	err = mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);
	if (err) {
		dev_err(priv->dev, "Failed to set display off: %d\n", err);
		return err;
	}

	return 0;
}

static int panel_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "Failed to duplicate panel mode\n");
		return -ENOMEM;
	}

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	/* Rounded up from 57.6mm x 43.2mm */
	connector->display_info.width_mm = 58;
	connector->display_info.height_mm = 43;

	connector->display_info.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;

	return 1;
}

static const struct drm_panel_funcs panel_funcs = {
	.prepare	= panel_prepare,
	.unprepare	= panel_unprepare,
	.enable		= panel_enable,
	.disable	= panel_disable,
	.get_modes	= panel_get_modes,
};

static int panel_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct panel_priv *priv;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spi_set_drvdata(spi, priv);

	priv->dev = dev;

	priv->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(priv->supply))
		return dev_err_probe(dev, PTR_ERR(priv->supply), "Failed to get power supply\n");

	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio), "Failed to get reset GPIO\n");

	err = mipi_dbi_spi_init(spi, &priv->dbi, NULL);
	if (err)
		return dev_err_probe(dev, err, "Failed to init MIPI DBI\n");

	priv->dbi.read_commands = NULL;

	drm_panel_init(&priv->panel, dev, &panel_funcs, DRM_MODE_CONNECTOR_DPI);

	err = drm_panel_of_backlight(&priv->panel);
	if (err)
		return dev_err_probe(dev, err, "Failed to attach backlight\n");

	drm_panel_add(&priv->panel);

	return 0;
}

static void panel_remove(struct spi_device *spi)
{
	struct panel_priv *priv = spi_get_drvdata(spi);
	struct drm_panel *panel = &priv->panel;

	drm_panel_remove(panel);
	drm_panel_disable(panel);
	drm_panel_unprepare(panel);
}

static const struct spi_device_id spi_ids[] = {
	{ "fs028vg047", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, spi_ids);

static const struct of_device_id of_matches[] = {
	{ .compatible = "fascontek,fs028vg047", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_matches);

static struct spi_driver panel_driver = {
	.driver = {
		.name = "fs028vg047",
		.of_match_table = of_matches,
	},
	.id_table = spi_ids,
	.probe = panel_probe,
	.remove = panel_remove,
};
module_spi_driver(panel_driver);

MODULE_AUTHOR("John Watts <contact@jookia.org>");
MODULE_LICENSE("GPL v2");
