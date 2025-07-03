#!/usr/bin/env python
# Copyright 2025 John Watts <contact@jookia.org>
# Licensed under CC0

from pll_frequencies import enumerate_pll_frequencies
import sys

def find_best_i2s_rate(argv):
	if len(argv) != 5:
		print("Usage: ./best_i2s_rate.py RATE CHANNELS BITS ROUND")
		print("Example: ./best_i2s_rate.py 93400 8 32 0")
		return 1
	round = bool(int(argv[4]) == 1)
	khz_mult = int(argv[2]) * int(argv[3])
	wanted_rate = int(argv[1]) * khz_mult
	best_rate = 0
	best_delta = wanted_rate
	best_str = ""
	frequencies = enumerate_pll_frequencies()
	freqs = sorted(list(frequencies.keys()))
	for freq in freqs:
		for N in [1, 2, 4, 8]:
			for M in range(1, 33):
				real_rate = freq / N / M
				if not round and real_rate > wanted_rate: # floor it
					break
				delta = abs(real_rate - wanted_rate)
				str = frequencies[freq]
				if delta < best_delta:
					best_rate = real_rate
					best_delta = delta
					freq_str = best_rate / khz_mult
					best_str = f"best freq is {freq_str} {str} N={N} M={M} rate={best_rate} round={round}" 
				if delta == 0:
					break
	print(best_str)
	return 0

if __name__ == '__main__':
	sys.exit(find_best_i2s_rate(sys.argv))
