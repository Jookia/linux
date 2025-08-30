// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2017 Chen-Yu Tsai <wens@csie.org>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#include "ccu_sdm.h"

/*
 * The Allwinner SDM clock is a sigma-delta modulated clock with support for
 * spread spectrum operation. This allows specifying a pair of fractional
 * frequencies to have a clock (relatively) slowly move between.
 * The main purpose of this is to reduce EMI emissions, but by specifying a
 * single frequency we're able to treat it as a regular as a fractional clock.
 *
 * To program it we must program the main PLL's clock, specifying an N value in
 * the process as usual to the main PLL registers. Along side that we use the
 * fraction of N to configure the SDM settings for the clock.
 *
 * For a fractional clock, the registers must be set like this:
 * - SIG_DELT_PAT_EN 1
 * - SPR_FREQ_MODE 2 (possibly 3 in spread spectrum mode?)
 * - WAVE_STEP 0
 * - SDM_CLK_SEL 0 (or 1 if the PLL divides the 24 MHz oscillator by 2)
 * - FREQ 0
 * - WAVE_BOTTOM frac
 *
 * frac is a fraction of 2**17, so 0 is 0, 0.3 is 436907, 0.5 is 262144, etc.
 * This is the way most fractions are reprsented in binary numbers, but limited
 * to 17 bits. It is important we make the calculator for N aware of the
 * precision of our fraction so it can round it and estimate the final rate.
 *
 * Spread spectrum mode works by taking two fractions of N (frac1 and frac2),
 * assigning the lower one to WAVE_BOTTOM and then taking the delta between
 * frac1 and frac2 as WAVE_STEP.
 *
 * Full details can be found in the Allwinner T113-S3 manual.
 */

#define SUNXI_SDM_PAT_SIG_DELT_PAT_EN BIT(31)
#define SUNXI_SDM_PAT_SPR_FREQ_MODE   GENMASK(30, 29)
#define SUNXI_SDM_PAT_WAVE_STEP       GENMASK(28, 20)
#define SUNXI_SDM_PAT_SDM_CLK_SEL     BIT(19)
#define SUNXI_SDM_PAT_FREQ            GENMASK(18, 17)
#define SUNXI_SDM_PAT_WAVE_BOTTOM     GENMASK(16, 0)
#define WAVE_BOTTOM_BITS 17

int ccu_sdm_helper_precision(struct ccu_common *common,
			     struct ccu_sdm_internal *sdm)
{
	if (!(common->features & CCU_FEATURE_SIGMA_DELTA_MOD))
		return 0;

	return 17;
}
EXPORT_SYMBOL_NS_GPL(ccu_sdm_helper_precision, SUNXI_CCU);

static u32 ccu_sdm_create_pattern_reg(unsigned long parent_rate, u32 frac)
{
	int use12mhz = parent_rate == 12000000;
	u32 wave_bottom = frac >> (32 - WAVE_BOTTOM_BITS);

	return FIELD_PREP(SUNXI_SDM_PAT_SIG_DELT_PAT_EN, 1) |
		FIELD_PREP(SUNXI_SDM_PAT_SPR_FREQ_MODE, 2) |
		FIELD_PREP(SUNXI_SDM_PAT_WAVE_STEP, 0) |
		FIELD_PREP(SUNXI_SDM_PAT_SDM_CLK_SEL, use12mhz) |
		FIELD_PREP(SUNXI_SDM_PAT_FREQ, 0) |
		FIELD_PREP(SUNXI_SDM_PAT_WAVE_BOTTOM, wave_bottom);
}

static u32 ccu_sdm_read_pattern_reg_frac(u32 reg)
{
	int wave_bottom = FIELD_GET(SUNXI_SDM_PAT_WAVE_BOTTOM, reg);
	u32 frac = wave_bottom << (32 - WAVE_BOTTOM_BITS);

	return frac;
}

int ccu_sdm_helper_set(struct ccu_common *common,
		       struct ccu_sdm_internal *sdm,
		       unsigned long parent_rate, u32 frac)
{
	u32 pattern_reg, reg;
	unsigned long flags;

	if (!(common->features & CCU_FEATURE_SIGMA_DELTA_MOD))
		return 0;

	spin_lock_irqsave(common->lock, flags);

	pattern_reg = ccu_sdm_create_pattern_reg(parent_rate, frac);
	writel(pattern_reg, common->base + sdm->tuning_reg);
	reg = readl(common->base + sdm->tuning_reg);
	writel(reg | sdm->tuning_enable, common->base + sdm->tuning_reg);
	reg = readl(common->base + common->reg);
	writel(reg | sdm->enable, common->base + common->reg);

	spin_unlock_irqrestore(common->lock, flags);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(ccu_sdm_helper_set, SUNXI_CCU);

u32 ccu_sdm_helper_get(struct ccu_common *common,
		       struct ccu_sdm_internal *sdm,
		       unsigned long parent_rate)
{
	u32 reg, frac;

	if (!(common->features & CCU_FEATURE_SIGMA_DELTA_MOD))
		return 0;

	reg = readl(common->base + sdm->tuning_reg);
	frac = ccu_sdm_read_pattern_reg_frac(reg);

	return frac;
}
EXPORT_SYMBOL_NS_GPL(ccu_sdm_helper_get, SUNXI_CCU);
