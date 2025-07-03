#!/bin/sh
# Copyright 2025 John Watts <contact@jookia.org>
# Licensed under CC0

for _freq in $(seq 80 2150); do
	freq="${_freq}00"
	arecord -Dsysdefault -c 8 -f s32 -r ${freq} -s1 -q >/dev/null
	realrate="$(($(cat /sys/kernel/debug/clk/i2s2/clk_rate) / 8 / 32))"
	if test "$realrate" -ne "${freq}"; then
		delta="$(($freq-$realrate))"
		echo "$freq realrate $realrate delta $delta"
	fi
done 2>&1 | tee sweep_results.txt
