// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>

#include "ccu_frac.h"
#include "ccu_gate.h"
#include "ccu_nm.h"

static u64 frac_create(u32 value, u32 frac)
{
	return ((u64)value << 32) + frac;
}

static u64 frac_floor_bits(u64 value, int bits)
{
	int frac_bits = 32 - bits;
	u64 floored = value >> frac_bits;

	return floored << frac_bits;
}

static u64 frac_round_bits(u64 value, int bits)
{
	int frac_bits = 32 - bits;
	int round_bit = (value >> (frac_bits - 1)) & 1;
	u64 floored = value >> frac_bits;
	u64 rounded = floored + round_bit;

	return rounded << frac_bits;
}

static u32 frac_frac(u64 value)
{
	return (u32)value;
}

static unsigned long frac_floor(u64 value)
{
	return value >> 32;
}

static unsigned long frac_round(u64 value)
{
	return frac_round_bits(value, 0) >> 32;
}

struct _ccu_nm {
	u64		n_frac;
	unsigned long	min_n, max_n;
	unsigned long	m, min_m, max_m;
};

static u64 ccu_nm_calc_rate_frac(u64 parent, u64 n, u64 m)
{
	return div64_u64(parent, m) * n;
}

static bool ccu_nm_is_better_rate_frac(struct ccu_common *common,
			u64 target_rate,
			u64 current_rate,
			u64 best_rate)
{
	unsigned long min_rate_hw, max_rate_hw;
	u64 min_rate, max_rate;

	clk_hw_get_rate_range(&common->hw, &min_rate_hw, &max_rate_hw);
	min_rate = frac_create(min_rate_hw, 0);
	max_rate = frac_create(max_rate_hw, 0);

	if (current_rate > max_rate)
		return false;

	if (current_rate < min_rate)
		return false;

	if (common->features & CCU_FEATURE_CLOSEST_RATE)
		return abs(current_rate - target_rate) < abs(best_rate - target_rate);

	return current_rate <= target_rate && current_rate > best_rate;
}

static u64 ccu_nm_find_best_frac(struct ccu_common *common, u64 parent,
				      u64 rate, struct _ccu_nm *nm,
				      int precision_bits)
{
	u64 best_rate = 0;
	u64 best_n = 0, best_m = 0;
	int m_int;

	for (m_int = nm->min_m; m_int <= nm->max_m; m_int++) {
		unsigned long n_int;
		u64 n, m, n_size, tmp_rate;

		m = frac_create(m_int, 0);
		n_size = div64_u64(parent, m);
		n = div64_u64(rate, n_size);
		if (common->features & CCU_FEATURE_CLOSEST_RATE)
			n = frac_round_bits(n, precision_bits);
		else
			n = frac_floor_bits(n, precision_bits);

		n_int = frac_floor(n);
		if (n_int < nm->min_n)
			continue;
		if (nm->max_n < n_int)
			break;

		tmp_rate = ccu_nm_calc_rate_frac(parent, n, m);

		if (ccu_nm_is_better_rate_frac(common, rate,
					  tmp_rate, best_rate)) {
			best_rate = tmp_rate;
			best_n = n;
			best_m = m;
		}
	}

	nm->n_frac = best_n;
	nm->m = frac_floor(best_m);

	return best_rate;
}

static void ccu_nm_disable(struct clk_hw *hw)
{
	struct ccu_nm *nm = hw_to_ccu_nm(hw);

	return ccu_gate_helper_disable(&nm->common, nm->enable);
}

static int ccu_nm_enable(struct clk_hw *hw)
{
	struct ccu_nm *nm = hw_to_ccu_nm(hw);

	return ccu_gate_helper_enable(&nm->common, nm->enable);
}

static int ccu_nm_is_enabled(struct clk_hw *hw)
{
	struct ccu_nm *nm = hw_to_ccu_nm(hw);

	return ccu_gate_helper_is_enabled(&nm->common, nm->enable);
}

static unsigned long ccu_nm_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct ccu_nm *nm = hw_to_ccu_nm(hw);
	u64 parent_frac, n_frac, m_frac, rate_frac;
	unsigned long rate;
	unsigned long n, m;
	u32 sdm_frac;
	u32 reg;

	if (ccu_frac_helper_is_enabled(&nm->common, &nm->frac)) {
		rate = ccu_frac_helper_read_rate(&nm->common, &nm->frac);

		if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
			rate /= nm->fixed_post_div;

		return rate;
	}

	reg = readl(nm->common.base + nm->common.reg);

	n = reg >> nm->n.shift;
	n &= (1 << nm->n.width) - 1;
	n += nm->n.offset;
	if (!n)
		n++;

	m = reg >> nm->m.shift;
	m &= (1 << nm->m.width) - 1;
	m += nm->m.offset;
	if (!m)
		m++;

	sdm_frac = ccu_sdm_helper_get(&nm->common, &nm->sdm, parent_rate);
	parent_frac = frac_create(parent_rate, 0);
	n_frac = frac_create(n, sdm_frac);
	m_frac = frac_create(m, 0);
	rate_frac = ccu_nm_calc_rate_frac(parent_frac, n_frac, m_frac);
	rate = frac_round(rate_frac);

	if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
		rate /= nm->fixed_post_div;

	return rate;
}

static long ccu_nm_round_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long *parent_rate)
{
	struct ccu_nm *nm = hw_to_ccu_nm(hw);
	struct _ccu_nm _nm;
	u64 parent_frac, rate_frac, best_frac;
	int frac_precision;

	if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
		rate *= nm->fixed_post_div;

	if (rate < nm->min_rate) {
		rate = nm->min_rate;
		if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
			rate /= nm->fixed_post_div;
		return rate;
	}

	if (nm->max_rate && rate > nm->max_rate) {
		rate = nm->max_rate;
		if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
			rate /= nm->fixed_post_div;
		return rate;
	}

	if (ccu_frac_helper_has_rate(&nm->common, &nm->frac, rate)) {
		if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
			rate /= nm->fixed_post_div;
		return rate;
	}

	_nm.min_n = nm->n.min ?: 1;
	_nm.max_n = nm->n.max ?: 1 << nm->n.width;
	_nm.min_m = 1;
	_nm.max_m = nm->m.max ?: 1 << nm->m.width;

	parent_frac = frac_create(*parent_rate, 0);
	rate_frac = frac_create(rate, 0);
	frac_precision = ccu_sdm_helper_precision(&nm->common, &nm->sdm);
	best_frac = ccu_nm_find_best_frac(&nm->common, parent_frac,
					  rate_frac, &_nm, frac_precision);
	rate = frac_round(best_frac);

	if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
		rate /= nm->fixed_post_div;

	return rate;
}

static int ccu_nm_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct ccu_nm *nm = hw_to_ccu_nm(hw);
	u64 parent_frac, rate_frac;
	struct _ccu_nm _nm;
	unsigned long flags, n_int;
	int frac_precision;
	u32 reg;

	/* Adjust target rate according to post-dividers */
	if (nm->common.features & CCU_FEATURE_FIXED_POSTDIV)
		rate = rate * nm->fixed_post_div;

	if (ccu_frac_helper_has_rate(&nm->common, &nm->frac, rate)) {
		spin_lock_irqsave(nm->common.lock, flags);

		/* most SoCs require M to be 0 if fractional mode is used */
		reg = readl(nm->common.base + nm->common.reg);
		reg &= ~GENMASK(nm->m.width + nm->m.shift - 1, nm->m.shift);
		writel(reg, nm->common.base + nm->common.reg);

		spin_unlock_irqrestore(nm->common.lock, flags);

		ccu_frac_helper_enable(&nm->common, &nm->frac);

		return ccu_frac_helper_set_rate(&nm->common, &nm->frac,
						rate, nm->lock);
	} else {
		ccu_frac_helper_disable(&nm->common, &nm->frac);
	}

	_nm.min_n = nm->n.min ?: 1;
	_nm.max_n = nm->n.max ?: 1 << nm->n.width;
	_nm.min_m = 1;
	_nm.max_m = nm->m.max ?: 1 << nm->m.width;

	parent_frac = frac_create(parent_rate, 0);
	rate_frac = frac_create(rate, 0);
	frac_precision = ccu_sdm_helper_precision(&nm->common, &nm->sdm);

	ccu_nm_find_best_frac(&nm->common, parent_frac, rate_frac,
			      &_nm, frac_precision);
	ccu_sdm_helper_set(&nm->common, &nm->sdm, parent_rate,
			   frac_frac(_nm.n_frac));
	n_int = frac_floor(_nm.n_frac);

	spin_lock_irqsave(nm->common.lock, flags);

	reg = readl(nm->common.base + nm->common.reg);
	reg &= ~GENMASK(nm->n.width + nm->n.shift - 1, nm->n.shift);
	reg &= ~GENMASK(nm->m.width + nm->m.shift - 1, nm->m.shift);

	reg |= (n_int - nm->n.offset) << nm->n.shift;
	reg |= (_nm.m - nm->m.offset) << nm->m.shift;
	writel(reg, nm->common.base + nm->common.reg);

	spin_unlock_irqrestore(nm->common.lock, flags);

	ccu_helper_wait_for_lock(&nm->common, nm->lock);

	return 0;
}

const struct clk_ops ccu_nm_ops = {
	.disable	= ccu_nm_disable,
	.enable		= ccu_nm_enable,
	.is_enabled	= ccu_nm_is_enabled,

	.recalc_rate	= ccu_nm_recalc_rate,
	.round_rate	= ccu_nm_round_rate,
	.set_rate	= ccu_nm_set_rate,
};
EXPORT_SYMBOL_NS_GPL(ccu_nm_ops, SUNXI_CCU);
