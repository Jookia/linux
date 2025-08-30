/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017 Chen-Yu Tsai. All rights reserved.
 */

#ifndef _CCU_SDM_H
#define _CCU_SDM_H

#include <linux/clk-provider.h>

#include "ccu_common.h"

struct ccu_sdm_internal {
	/* early SoCs don't have the SDM enable bit in the PLL register */
	u32		enable;
	/* second enable bit in tuning register */
	u32		tuning_enable;
	u16		tuning_reg;
};

#define _SUNXI_CCU_SDM(_enable, _reg, _reg_enable)	\
	{						\
		.enable		= _enable,		\
		.tuning_enable	= _reg_enable,		\
		.tuning_reg	= _reg,			\
	}

int ccu_sdm_helper_precision(struct ccu_common *common,
			     struct ccu_sdm_internal *sdm);
int ccu_sdm_helper_set(struct ccu_common *common,
		       struct ccu_sdm_internal *sdm,
		       unsigned long parent_rate, u32 frac);
u32 ccu_sdm_helper_get(struct ccu_common *common,
		       struct ccu_sdm_internal *sdm,
		       unsigned long parent_rate);

#endif
