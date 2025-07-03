#!/usr/bin/env python
# Copyright 2025 John Watts <contact@jookia.org>
# Licensed under CC0

# This was originally going to be a small file that just shows you how to
# calculate a SDM clock for the Allwinner T113. As you can see it has spiralled
# out of scope in to a model for calculating audio clocks and experimenting with
# possible improvements. But having a somewhat complete model is probably for
# the best anyway as it means you too can perform tests and try and understand
# how things work.
#
# I don't guarantee this model to be accurate to the kernel or hardware's
# clocking code, but it should match in spirit and be useful enough for
# optimizing PLL generation.
#
# This program supports three modes:
# 1. Finding the best rate for an audio rate
# 2. Finding the best rate for a PLL rate
# 3. Sweeping across multiple audio rates and calculating total errors
# 4. Comparing against the kernel clock constants
# Run the program with no arguments to see the usage
#
# The clock formulas we will be iterating over are:
#   audio0_4x_pll = 24 MHz * N1 / P1 / 4
#   audio1_div_pll_clk = 24 MHz * N2 / M1 / P2
#   i2s_clk = pll_clk / N3 / M2
#   and 12 <= N1 <= 127
#   and 12 <= N2 <= 128
#   and 1 <= P1 <= 64
#   and 1 <= M1 <= 2
#   and 1 <= P2 <= 8
#   and N3 in [1, 2, 4, 8]
#   and 1 <= M2 <= 32
# To mimic kernel code we have this in three stages:
# - The I2S clock
# - The PLL post-divider (P2 or 4)
# - The PLL itself
#
# Clock generation works by taking the desired target I2S clock, finding a PLL
# post-divider clock, then finding a PLL clock. Dividers are added by
# multiplying the clock in each step above then dividing it back down to check
# if the rate is accurate.
#
# I hope the above documentation answers most your questions about this program,
# I'm going to spend most my time documenting the PLL generation rather than
# code for generating other clocks or testing the clocks. It's not too
# complicated (in comparison), so I hope you can figure it out.
#
# For this Python code we will assume integers are 64-bit but when passing
# between kernel functions the rate can only be 32-bit. The kernel calculations
# will be done using fixed point math, I'll do my best to explain.

import math # math.floor
import sys # sys.argv, sys.exit

# I've decided to only allow specifying the rate on the command line. Any other
# settings have to changed here in the source code. It would probably be better
# to pass them as arguments or environmental variables of some sort, but this is
# good enough for now for the use case of sweeping and inquiring about specific
# audio or PLL rates.
# As a quick note, if you're trying to compare changes it can be useful to
# disable the I2S and P2 divisors as well as the audio1_div PLL. This turns the
# program in to a generator for a single PLL and can help narrow down bugs.
settings = {
	# General settings related to the program calculation
	'channels': 8, # Audio channels, used for the I2S clock
	'bits': 32, # Audio bits, used for the I2S clock
	'sweep_start': 8000, # Start Hz for sweeping
	'sweep_end': 216000, # End Hz for sweeping
	'sweep_step': 100, # Increment for each sweep step

	# Settings you would change in the kernel code
	'plls': ['audio0_4x', 'audio1_div'], # Which PLLs I2S are usable
	'audio1_max_n': 128, # Maximum AUDIO1 PLL N factor (up to 145?)
	'osc_rate': 24000000, # Oscillator rate, can be 24 MHz or 12 MHz
	'floor': True, # Whether to floor or round ALL the clocks
	'float_rates': False, # Whether to represent rates as floats

	# Settings related to testing PLL generation code
	'equal_duty_cycle': True, # Require P be divisible by 2
	'skip_final_round': False, # Skip correctly rounding the final rate
	'no_divs_i2s': False, # Disable all I2S divisors 
	'no_divs_p2': False, # Disable extra P2 divisors
	'fixed_bits': 17, # Fraction bits. 17 floors, 18+ rounds
	'fixed_size': 64, # Number of bits for a fixed point integer
	'n_multiply': False, # Multiply rate by P (requires bigger fixed_size)
	'avoid_spurs': False, # Avoid fractional PLL spurs
}

# Checks if a rate is better in comparison to a best and desired rate
def better_rate(new, best, desired):
	if settings['floor']:
		return new <= desired and new > best
	else:
		best_delta = abs(best - desired)
		new_delta = abs(new - desired)
		return new_delta <= best_delta

# Checks if an N value is better in comparison to a best N value
def better_N(new, best):
	if not settings['avoid_spurs']:
		return True
	# This code mainly checks if an N value is favorable compared to another
	# N value. This is only really relevant if some N values are objectively
	# bad, such as when testing for spurs. In our case we just try to get an
	# N value that is as close to 0.5 as possible
	fix = settings['fixed_bits']
	frac_bits = (1 << fix) - 1
	new_frac = new & frac_bits
	best_frac = best & frac_bits
	middle_N = 1 << (fix - 1) # Half a fraction
	best_delta = abs(middle_N - new_frac)
	new_delta = abs(middle_N - best_frac)
	return new_delta < best_delta
	
# Divides a rate, possibly keeping it a float
# This is intended to simulate what it would be like if we didn't need to round
# rates due to the kernel API. Surprisingly it doesn't seem to have that big of
# an impact from what I can see.
def rate_div(rate, div):
	if settings['float_rates']:
		return rate / div
	else:
		return rate // div

# Generates a PLL result for a desired rate before P2 division
def gen_pll(n_range, p_range, rate_hz):
	# Because we can't use floating point in the kernel we use fixed point
	# math to represent a fractional number. This is done by having a 64 bit
	# integer use the last 'fix' bits (18 or more) represent the fraction.
	# For example, to represent 12.34 with a 16-bit integer we do this:
	#   0000 1100   0101 0111
	#          12   87
	# Here the 87 is actually a fraction, 87/(2**8), or 87/256. We can find
	# out is decimal value by putting that in a calculator. This gives us:
	#   0000 1100 + (0101 0111 / 256)
	#          12 + (87 / 256) = 12.33984375
	# This is close to our input of 12.34. We can also just divide our
	# entire number by our fixed point fraction to get the decimal value:
	#   0000 1100 0101 0111 / 256 = 12.33984375
	# Regular mathematical operations work fine on a fixed point fraction as
	# long as you scale the inputs too when required. The only thing to note
	# is that the intermediate calculations are floored rather than rounded.
	#
	# Because we are using powers of two as our fixed point fraction, we can
	# do use some bit manipulation to do some common tasks.
	# To floor our fraction and get just the higher number:
	#   0000 1100 0101 0111 >> 8 = 0000 1100 = 12
	# To get just the fraction part:
	#   0000 1100 0101 0111 & 1111 1111 = 0101 0111
	# To floor our fraction to a certain amount of bits:
	#   (0000 1100 0101 0111 >> 3) << 3 = 0000 1100 0101 0100 = 12.328125
	# To round our fraction to a certain amount of bits we floor the
	# fraction but add the bit after it as that bit represents a half.
	#   (0000 1100 0101 0111 >> 3) +
	#   (0000 1100 0101 0111 >> 2) & 1) << 3
	#  = 0000 1100 0101 1000 = 12.34375
	#
	# Fixed point math has imprecision for the fraction. In this case the
	# fraction is a part of 256, which means we can't store anything more
	# precise than a 1/256th (or 0.00390625). This means converting to and
	# form a fixed point fraction must be done by floor or rounding.
	#
	# Rounding is ideal and reduces the error between representations of
	# fixed point and integer or floating point maths. You should always
	# round unless you intentionally want to introduce a loss of precision.
	#
	# For what it's worth, it doesn't seem like precision above 17 bits
	# helps at all if you're flooring the clocks. 18 helps if you want to
	# round clocks, but anything above that seems useless.

	# We must convert some values for use with our fixed point math. We use
	# the fixed_bits settings as the number of bits for the fraction.
	fix = settings['fixed_bits']
	osc_rate = settings['osc_rate'] << fix
	rate = rate_hz << fix
	min_n = n_range.start << fix
	max_n = (n_range.stop - 1) << fix

	best_result = None
	best_n = 0
	best_rate = 0
	for p_div in p_range:
		# If P is even we get an equal duty cycle. This is ideal but
		# it's worth seeing what happens if we turn it off.
		if settings['equal_duty_cycle'] and (p_div % 2) == 1:
			continue

		# Convert P to fixed point for future math
		p = p_div << fix

		# As far as I know there's two easy ways to find N here:
		# The first is: n = rate / (osc_rate / P)
		# For example:
		#   rate = 54.4768 MHz
		#   osc_rate = 24 MHz
		#   P = 54
		#   (osc_rate / P) = 444444.4444
		#   rate / 444444.4444 = 122.5728
		# If we do this with decimal fixed point math we get:
		#   rate = 54476800.000000 Hz
		#   osc_rate = 24000000.000000 Hz
		#   P = 54.000000
		#   (osc_rate // P) = 444444
		#   rate // 122.5728000 = 122.572922
		# Because (osc_rate // P) is a ratio it doesn't use our fixed
		# point precision and instead floors down. Dividing then
		# multiplying by this loses some precision in the final
		# computation and allows rates to slightly overshoot.
		#
		# The second way to find N is: (rate * P) / osc_rate
		# For example:
		#   rate = 54.4768 MHz
		#   osc_rate = 24 MHz
		#   P = 54
		#   (rate * P) = 2.9417472 GHz
		#   2.9417472 GHz / osc_rate = 122.5728
		# The problem here is that we don't have enough bits to
		# do this calculation, even with 64-bit integers. Take the
		# following calculation:
		#   (rate << 17) = 7140383129600
		#   (P << 17) = 7077888
		#   7140383129600 * 7077888 = 50538832068398284800
		# The result requires 66 bits, which we don't have.
		# If we use a 128 bit integer, we will find a new problem. The
		# result is 122.5727997, not 122.5768. To get the proper
		# precision we need at least a 24 bit fraction.
		#
		# I've implemented both of these algorithms below and did some
		# testing on them. Changing n_multiply to True, fixed_size to 64
		# and fixed_bits to 128 had barely any impact at all.
		if settings['n_multiply']:
			n_mul = rate * p
			if n_mul > (1 << settings['fixed_size']): # Overflow
				# Just for reference, in C you would use a
				# function to do multiplication with overflow
				# checking as checking AFTER an overflow is
				# undefined behavior.
				print(f"OVERFLOW: n_mul {rate} {p} {n_mul}")
				continue
			n = n_mul // osc_rate
		else:
			n_size = osc_rate // p
			n = rate // n_size

		# It's time to prepare the fraction used by the PLL!
		# On Allwinner chips this is a 17 bit fraction, so we must take
		# 17 bits out of fraction. Our fraction is larger so we must
		# round to a specific amount of bits, or floor if we don't want
		# to go over the desired rate.
		# We do this by modifying N directly so we can later use it to
		# find our final, modified rate.
		# If fix only has 17 bits we skip this check
		n_round_bit = 0
		if fix != 17:
			n = n >> (fix - 18)
			n_round_bit = n & 1
			if settings['floor']:
				n_round_bit = 0
			n = ((n >> 1) + n_round_bit) << (fix - 17)

		# Grab the wave bottom bits. We are lucky that we don't have to
		# convert this to some other fraction, but it seems like most
		# fractional PLLs use a power of two for their divisor.
		# Though I did see the i.MX6 uses a ratio of two numbers. That
		# would be a little more complicated to implement.
		wave_bottom = (n >> (fix - 17)) & ((1 << 17) - 1)

		# It's now time to get the final rate we'll get with our our
		# modified N value. This is done using the inverse of our
		# original function to get N.
		if settings['n_multiply']:
			osc_mul = osc_rate * n
			if osc_mul > (1 << settings['fixed_size']): # Overflow
				print(f"OVERFLOW: osc_mul {osc_rate} {p} {n}")
				continue
			final_rate = osc_mul // p
		else:
			n_size = osc_rate // p
			final_rate = n_size * n
		if final_rate > (1 << settings['fixed_size']): # Overflow
			print(f"OVERFLOW: final_rate {final_rate} {osc_rate} {p} {n}")

		if n < min_n:
			continue
		if max_n < n:
			break
		if not better_N(best_n, n):
			continue
		if not better_rate(final_rate, best_rate, rate):
			continue

		# It's time to convert our fractional clock value to an integer
		# one for use with kernel APIs. We do this by rounding the rate
		# we have and returning that. The clock subsystem then will use
		# this rate to compare rates.
		# This rounding is specifically to convert our fixed point value
		# back to an integer rate correctly without artifacts. It won't
		# cause any problems related to flooring or rounding rates.
		# This only seems to be useful when rounding is enabled.
		rounded_rate_bit = (final_rate >> (fix - 1)) & 1
		if settings['skip_final_round']:
			rounded_rate_bit = 0
		rounded_rate = (final_rate >> fix) + rounded_rate_bit

		# The real rate and error are not known by the clock system,
		# instead we have to track them for this model.
		real_rate = final_rate / (1 << fix)
		error = abs(rate_hz - real_rate)

		# Build the result for our caller.
		# Each clock contains somewhat similar details and are appended
		# to this data structure throughout generation. Only the
		# pll_rate and other friends are used for clock generation, the 
		# rest is informational.
		result = {}
		result['osc_rate'] = osc_rate >> fix
		result['pll_rate_real'] = real_rate
		result['pll_rate'] = rounded_rate
		result['pll_error'] = error
		result['pll_wanted'] = rate_hz
		result['pll_wave_bottom'] = wave_bottom
		result['pll_n'] = n / (1 << fix)
		result['pll_p1'] = p_div

		if settings['float_rates']:
			result['pll_rate'] = real_rate

		best_rate = final_rate
		best_result = result
		best_n = n

	return best_result

# Generates the PLL clock with P2 division
def gen_pll_clock(pll, rate):
	if pll == "audio0_4x":
		pll_name = "audio0_4x"
		n_range = range(12, 128)
		p1_range = range(1, 65)
		p2_range = range(4, 5)
	else:
		pll_name = "audio1_div"
		n_range = range(12, settings['audio1_max_n'] + 1)
		p1_range = range(1, 3)
		p2_range = range(1, 9)
		if settings['no_divs_p2']:
			p2_range = [1]
	best_result = None
	best_rate = 0
	postdivs = list(p2_range)
	postdivs.reverse()
	for p2 in postdivs:
		new_rate = rate * p2
		if new_rate > (1 << 32): # Overflow
			continue
		result = gen_pll(n_range, p1_range, new_rate)
		if result is None:
			continue
		final_rate = rate_div(result['pll_rate'], p2)
		if not better_rate(final_rate, best_rate, rate):
			continue
		best_rate = final_rate
		best_result = result
		best_result['pll_p2'] = p2
		best_result['pll_rate_p2'] = final_rate
		best_result['pll_wanted_p2'] = rate
		best_result['pll_error_p2'] = result['pll_error'] / p2
		best_result['pll_name'] = pll_name
		best_result['divided_rate'] = final_rate
		best_result['divided_wanted'] = rate
		best_result['divided_error'] = result['pll_error_p2']
		if final_rate == rate: # Exact match
			return best_result
	return best_result

# Generates an I2S clock
def gen_i2s_clock(rate):
	best_rate = 0
	best_result = None
	if settings['no_divs_i2s']:
		N_range = [1]
		M_range = [1]
	else:
		N_range = [1, 2, 4, 8]
		M_range = range(1, 33)
	for pll in settings['plls']:
		for N in N_range:
			for M in M_range:
				mul = N * M
				new_rate = rate * mul
				if new_rate > (1 << 32): # Overflow
					continue
				result = gen_pll_clock(pll, new_rate)
				if result is None:
					continue
				real_rate = rate_div(result['pll_rate_p2'], mul)
				if not better_rate(real_rate, best_rate, rate):
					continue
				best_result = result
				best_rate = real_rate
				result['i2s_rate'] = real_rate
				result['i2s_wanted'] = rate
				result['i2s_error'] = result['pll_error_p2'] / mul
				result['i2s_n'] = N
				result['i2s_m'] = M
				if real_rate == rate: # Exact match
					return best_result
	return best_result

# Generates an 'audio' clock, not a real clock but subdivides to our target rate
def gen_audio_clock(rate):
	khz_mult = settings['channels'] * settings['bits']
	wanted_rate = rate * khz_mult
	result = gen_i2s_clock(wanted_rate)
	if result is None:
		return None
	result['audio_rate'] = rate_div(result['i2s_rate'], khz_mult)
	result['audio_error'] = result['i2s_error'] / khz_mult
	result['audio_wanted'] = rate
	return result

# Dumps all the settings
def dump_settings():
	print("settings:", end='')
	for (key, value) in settings.items():
		print(f" {key}={value}", end='')
	print("")

# Dumps all the result data from clock generation
def dump_result(result):
	mhz = 1000000
	khz = 1000

	pll_rate = result['pll_rate'] / mhz
	pll_wanted = result['pll_wanted'] / mhz
	pll_error = result['pll_error']
	pll_n = result['pll_n']
	pll_p1 = result['pll_p1']
	pll_name = result['pll_name']
	pll_wave_bottom = result['pll_wave_bottom']

	print(f"--- Clock result ({pll_name}) ---")
	print(f"OSC rate: {result['osc_rate'] / mhz} MHz")
	print(f"PLL: {pll_rate} MHz", end='')
	print(f" (wanted {pll_wanted} MHz,", end='')
	print(f" error {pll_error:.9f} Hz)", end='')
	print(f" N={pll_n:.9f} P1={pll_p1}", end='')
	print(f" WAVE_BOTTOM={pll_wave_bottom}")

	if "divided_rate" in result:
		pll_p2 = result['pll_p2']
		divided_rate = result['divided_rate'] / mhz
		divided_wanted = result['divided_wanted'] / mhz
		divided_error = result['divided_error']
		print(f"P2: {divided_rate} MHz", end='')
		print(f" (wanted {divided_wanted} MHz,", end='')
		print(f" error {divided_error:.9f} Hz) P2={pll_p2}")

	if "i2s_rate" in result:
		i2s_rate = result['i2s_rate'] / mhz
		i2s_wanted = result['i2s_wanted'] / mhz
		i2s_error = result['i2s_error']
		i2s_n = result['i2s_n']
		i2s_m = result['i2s_m']
		print(f"I2S: {i2s_rate} MHz", end='')
		print(f" (wanted {i2s_wanted} MHz,", end='')
		print(f" error {i2s_error:.9f} Hz)", end='')
		print(f" N={i2s_n} M={i2s_m}", end='')
		print(f" parent={pll_name}")

	if "audio_rate" in result:
		audio_rate = result['audio_rate'] / khz
		audio_wanted = result['audio_wanted'] / khz
		audio_error = result['audio_error']
		print(f"Audio: {audio_rate} kHz", end='')
		print(f" (wanted {audio_wanted} kHz,", end='')
		print(f" error {audio_error:.9f} Hz)")

# Handles the 'audio' program mode
def main_audio(argv):
	if len(argv) != 3:
		return -1
	rate = int(argv[2])
	result = gen_audio_clock(rate)
	dump_settings()
	if result is None:
		print("Unable to generate rate")
		return 1
	dump_result(result)
	return 0

# Handles the 'pll' program mode
def main_pll(argv):
	if len(argv) != 3:
		return -1
	rate = int(argv[2])
	dump_settings()
	for pll in settings['plls']:
		result = gen_pll_clock(pll, rate)
		if result is None:
			print(f"Unable to generate rate using PLL {pll}")
			continue
		dump_result(result)
	return 0

# Handles the 'sweep' program mode
def main_sweep(argv):
	if len(argv) != 2:
		return -1
	dump_settings()
	print(f"--- Sweep results ---")
	pll_errors = []
	audio_errors = []
	worst_pll_error = (0, 0)
	worst_audio_error = (0, 0)
	sweep_rate = settings['sweep_start']
	while sweep_rate != settings['sweep_end']:
		rate = sweep_rate
		sweep_rate += settings['sweep_step']
		result = gen_audio_clock(rate)
		if result is None:
			print(f"Unable to generate clock for {rate} Hz")
			return 1
		pll_error = result['pll_error']
		audio_error = result['audio_error']
		pll_errors.append(pll_error)
		audio_errors.append(audio_error)
		if audio_error > worst_audio_error[0]:
			worst_audio_error = (audio_error, rate)
		if pll_error > worst_pll_error[0]:
			worst_pll_error = (pll_error, rate)
	pll_error_avg = sum(pll_errors) / len(pll_errors)
	audio_error_avg = sum(audio_errors) / len(audio_errors)
	print(f"average PLL error {pll_error_avg:.9f} Hz")
	print(f"average audio error {audio_error_avg:.9f} Hz")
	print(f"worst PLL error {worst_pll_error[0]:.9f} Hz", end='')
	print(f" ({worst_pll_error[1]:.9f} Hz rate)")
	print(f"worst audio error {worst_audio_error[0]:.9f} Hz", end='')
	print(f" ({worst_audio_error[1]:.9f} Hz rate)")
	return 0

# Generated by running this in the kernel source tree:
# $ grep -h -R '\.pattern = 0x' drivers/clk/sunxi-ng/ | sort | uniq
kernel_clocks_str = """
        { .rate = 180633600, .pattern = 0xc001288d, .m = 3, .n = 22 },
        { .rate = 196608000, .pattern = 0xc001eb85, .m = 5, .n = 40 },
        { .rate = 22579200, .pattern = 0xc0010d84, .m = 8, .n = 7 },
        { .rate = 24576000, .pattern = 0xc000ac02, .m = 14, .n = 14 },
        { .rate = 451584000, .pattern = 0xc0014396, .m = 2, .n = 37 },
        { .rate = 45158400, .pattern = 0xc00121ff, .m = 29, .n = 54 },
        { .rate = 45158400, .pattern = 0xc001bcd3, .m = 18, .n = 33 },
        { .rate = 49152000, .pattern = 0xc000e147, .m = 30, .n = 61 },
        { .rate = 49152000, .pattern = 0xc001eb85, .m = 20, .n = 40 },
        { .rate = 541900800, .pattern = 0xc001288d, .m = 1, .n = 22 },
        { .rate = 589824000, .pattern = 0xc00126e9, .m = 1, .n = 24 },
        { .rate = 90316800, .pattern = 0xc001288d, .m = 6, .n = 22 },
"""

# Handles the 'kernel' program mode
def main_kernel(argv):
	if len(argv) != 2:
		return -1
	dump_settings()
	print(f"--- Kernel comparison ---")
	for clock_str in kernel_clocks_str.split("\n"):
		split = clock_str.split()
		if split == []:
			continue
		rate = int(split[3][:-1])
		pattern = int(split[6][:-1], 16)
		m = int(split[9][:-1])
		n = int(split[12])
		wave_bottom = pattern & ((1 << 17) - 1)
		wave_frac = wave_bottom / (1 << 17)
		real_rate = 24000000 * (n + wave_frac) / m
		their_error = abs(real_rate - rate)
		our_rate = 0
		our_error = rate
		our_pll = "none"
		delta_error = 0
		for pll in settings['plls']:
			result = gen_pll_clock(pll, rate)
			if not result:
				continue
			new_error = result['divided_error']
			if new_error < our_error:
				our_rate = result['divided_rate']
				our_pll = result['pll_name']
				our_error = new_error
				delta_error = abs(their_error - our_error)
		mhz = 1000000
		print(f"{(rate / mhz):.6f} MHz:", end='')
		print(f" real={(real_rate / mhz):.6f} MHz", end='')
		print(f" our_rate={(our_rate / mhz):.6f} MHz", end='')
		print(f" our_pll={our_pll}", end='')
		print(f" error={their_error:.6f} Hz", end='')
		print(f" our_error={our_error:.6f} Hz")
	return 0

# Handles the program and setting a mode
def main(argv):
	if len(argv) < 2:
		mode = None
	else:
		mode = argv[1]
	if mode == "audio":
		ret = main_audio(argv)
	elif mode == "pll":
		ret = main_pll(argv)
	elif mode == "sweep":
		ret = main_sweep(argv)
	elif mode == "kernel":
		ret = main_kernel(argv)
	else:
		ret = -1
	if ret != -1:
		return ret
	print("Usage 1: ./sdm_model.py audio RATE")
	print("Example: ./sdm_model.py audio 93400")
	print("")
	print("Usage 2: ./sdm_model.py pll RATE")
	print("Example: ./sdm_model.py pll 54476800")
	print("")
	print("Usage 3: ./sdm_model.py sweep")
	print("")
	print("Usage 3: ./sdm_model.py kernel TREE")
	print("Example: ./sdm_model.py kernel")
	return 1

if __name__ == '__main__':
	sys.exit(main(sys.argv))
