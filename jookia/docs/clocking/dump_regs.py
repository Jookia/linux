#!/usr/bin/env python
# Copyright 2025 John Watts <contact@jookia.org>
# Licensed under CC0

import sys

regmap_ctrl_shared = {
	"pll_en": (31, 31),
	"pll_ld_en": (30, 30),
	"lock_enable": (29, 29),
	"lock": (28, 28),
	"pll_output_gate": (27, 27),
	"sdm_enabled": (24, 24),
	"pll_unlock_mdsel": (7, 6),
	"pll_lock_mdsel": (5, 5),
}

regmap_ctrl_audio0 = regmap_ctrl_shared | {
	"pll_p": (21, 16),
	"pll_n": (15, 8),
	"pll_input_div2": (1, 1),
	"pll_output_div2": (0, 0),
}

regmap_ctrl_audio1 = regmap_ctrl_shared | {
	"pll_p1": (22, 20),
	"pll_p0": (18, 16),
	"pll_n": (15, 8),
	"pll_input_div2": (1, 1),
}

regmap_pattern = {
	"sig_delt_pat_en": (31, 31),
	"spr_freq_mode": (30, 29),
	"wave_step": (28, 20),
	"sdm_clk_sel": (19, 19),
	"freq": (18, 17),
	"wave_bottom": (16, 0),
}

def read_bits(register, top, bottom):
	mask = (1 << (top + 1)) - 1
	return (register & mask) >> bottom

def write_bits(field, top, bottom):
	size = (top - bottom)
	mask = (1 << (size + 1)) - 1
	return (field & mask) << bottom

def read_reg(regmap, reg):
	regs = {}
	for (name, bits) in list(regmap.items()):
		(start, end) = bits
		regs[name] = read_bits(reg, start, end)
	return regs

def write_reg(regmap, fields):
	reg = 0
	for (name, bits) in list(regmap.items()):
		field = fields[name]
		(start, end) = bits
		reg |= write_bits(field, start, end)
	return reg

def print_reg(name, regmap, reg):
	print(f"--- Register dump for {name} register ---")
	print(f"Hex value: 0x{reg:08x}")
	fields = read_reg(regmap, reg)
	for (name, value) in list(fields.items()):
		print(f"\t{name}: 0x{value:x} ({value})")

def sdm_fractions(input_div2, sdm_freq, wave_bottom, wave_step):
	sdm_freqs = {
		0: 31500,
		1: 32000,
		2: 32500,
		3: 33000,
	}
	sdm_freq = sdm_freqs[sdm_freq]
	sdm_frac = 2**17
	step_div = 24000000 / input_div2 / sdm_freq / 2

	step_diff = wave_step * step_div
	frac1 = wave_bottom / sdm_frac
	frac2 = (wave_bottom + step_diff) / sdm_frac

	return (frac1, frac2)

def pll_rate(input_div2, output_div2, n, frac):
	rate = 24000000
	rate /= input_div2
	rate *= (n + frac)
	rate /= output_div2
	return rate

def calc_pll_clock(ctrl_fields, pattern_fields):
	input_div2 = (ctrl_fields["pll_input_div2"] + 1)
	output_div2 = (ctrl_fields.get("pll_output_div2", 0) + 1) # audio0 only
	n = (ctrl_fields["pll_n"] + 1)
	sdm_freq = pattern_fields["freq"]
	wave_bottom = pattern_fields["wave_bottom"]
	wave_step = pattern_fields["wave_step"]

	fracs = sdm_fractions(input_div2, sdm_freq, wave_bottom, wave_step)
	start_pll = pll_rate(input_div2, output_div2, n, fracs[0])
	end_pll = pll_rate(input_div2, output_div2, n, fracs[1])

	return (start_pll, end_pll)

def divide_clock(tuple, factor):
	return (tuple[0] / factor, tuple[1] / factor)

def calc_clocks_audio0(ctrl_fields, pattern_fields):
	p = (ctrl_fields["pll_p"] + 1)

	pll_clock = calc_pll_clock(ctrl_fields, pattern_fields)
	audio0_4x_clock = divide_clock(pll_clock, p)
	audio0_2x_clock = divide_clock(audio0_4x_clock, 2)
	audio0_1x_clock = divide_clock(audio0_4x_clock, 4)

	clocks = {}
	clocks["pll"] = pll_clock
	clocks["audio0_4x"] = audio0_4x_clock
	clocks["audio0_2x"] = audio0_2x_clock
	clocks["audio0_1x"] = audio0_1x_clock

	return clocks

def calc_clocks_audio1(ctrl_fields, pattern_fields):
	p0 = (ctrl_fields["pll_p0"] + 1)
	p1 = (ctrl_fields["pll_p1"] + 1)

	pll_clock = calc_pll_clock(ctrl_fields, pattern_fields)
	audio1_clock = pll_clock
	div2_clock = divide_clock(audio1_clock, p0)
	div5_clock = divide_clock(audio1_clock, p1)

	clocks = {}
	clocks["pll"] = pll_clock
	clocks["audio1"] = audio1_clock
	clocks["audio1_div2"] = div2_clock
	clocks["audio1_div5"] = div5_clock

	return clocks

def print_clocks(clocks):
	print("--- Clock dump ---")
	for (name, value) in list(clocks.items()):
		start_mhz = (value[0]) / 1000000
		end_mhz = (value[1]) / 1000000
		if start_mhz == end_mhz:
			print(f"\t{name}: {start_mhz} MHz")
		else:
			print(f"\t{name}: {start_mhz}-{end_mhz} MHz")
	
def dump_regs(argv):
	if len(argv) not in [3, 4]:
		print("Usage: ./dump_regs.py PLL PLL_CTRL_REG [PLL_PAT_REG]")
		print("Example: ./dump_regs.py audio0 0xA9001800 0xE3F07111")
		return 1

	pll = argv[1]
	ctrl_reg = int(argv[2], base=16)
	if len(argv) == 4:
		pat_reg = int(argv[3], base=16)
	else:
		pat_reg = 0x0

	if pll == "audio0":
		regmap_ctrl = regmap_ctrl_audio0
		calc_func = calc_clocks_audio0
	elif pll == "audio1":
		regmap_ctrl = regmap_ctrl_audio1
		calc_func = calc_clocks_audio1
	else:
		print("PLL must be audio0 or audio1")
		return 1

	ctrl_fields = read_reg(regmap_ctrl, ctrl_reg)
	pattern_fields = read_reg(regmap_pattern, pat_reg)
	clocks = calc_func(ctrl_fields, pattern_fields)

	print_reg("control", regmap_ctrl, ctrl_reg)
	if pat_reg != 0x0:
		print_reg("pattern", regmap_pattern, pat_reg)
	print_clocks(clocks)

	if ctrl_fields["sdm_enabled"] != pattern_fields["sig_delt_pat_en"]:
		print("WARNING: sdm_enabled != sig_delt_pat_en")
	if ctrl_fields["pll_input_div2"] != pattern_fields["sdm_clk_sel"]:
		# In practice this only matters if SDM is enabled
		if ctrl_fields["sdm_enabled"]:
			print("WARNING: pll_input_div2 != sdm_clk_sel")

	return 0

if __name__ == '__main__':
	sys.exit(dump_regs(sys.argv))
