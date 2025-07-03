#!/usr/bin/env python
# Copyright 2025 John Watts <contact@jookia.org>
# Licensed under CC0

def enumerate_pll_frequencies():
	frequencies = {}
	for PLL in [0, 1]:
		if PLL == 0: # audio0
			pll = "audio0"
			_N = range(12, 127)
			_P = range(1, 65)
			_postdiv = [1, 2, 4]
		elif PLL == 1: # audio1
			pll = "audio1"
			_N = range(12, 129)
			_P = range(1, 9)
			_postdiv = [1, 2]
		for postdiv in _postdiv:
			for P in _P:
				for N in _N:
					osc24 = 24000000
					freq = osc24 * N / P / postdiv
					str = f"{freq}={osc24}*{N}/{P}/{postdiv} pll={pll}"
					if freq not in frequencies:
						frequencies[freq] = str
	return frequencies

def list_all_pll_frequencies():
	frequencies = enumerate_pll_frequencies()
	freqs = sorted(list(frequencies.keys()))
	for freq in freqs:
		str = frequencies[freq]
		print(str)

if __name__ == '__main__':
	list_all_pll_frequencies()
