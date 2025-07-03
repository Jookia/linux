#!/bin/sh
# Copyright 2025 John Watts <contact@jookia.org>
# Licensed under CC0

echo "i2s2 rate is $(cat /sys/kernel/debug/clk/i2s2/clk_rate)"
echo "i2s2 parent is $(cat /sys/kernel/debug/clk/i2s2/clk_parent)"
echo "audio0 rate is $(cat /sys/kernel/debug/clk/pll-audio0/clk_rate)"
echo "audio0-4x rate is $(cat /sys/kernel/debug/clk/pll-audio0-4x/clk_rate)"
echo "audio1 rate is $(cat /sys/kernel/debug/clk/pll-audio1/clk_rate)"
echo "audio1-div2 rate is $(cat /sys/kernel/debug/clk/pll-audio1-div2/clk_rate)"
echo "audio1-div4 rate is $(cat /sys/kernel/debug/clk/pll-audio1-div5/clk_rate)"
echo "clk register dump: "
echo "  PLL_AUDIO0 $(devmem 0x02001078 32)"
echo "  PLL_AUDIO0 PAT0 $(devmem 0x02001178 32)"
echo "  PLL_AUDIO0 PAT1 $(devmem 0x0200117C 32)"
echo "  PLL_AUDIO1 $(devmem 0x02001080 32)"
echo "  PLL_AUDIO1 PAT0 $(devmem 0x02001180 32)"
echo "  PLL_AUDIO1 PAT1 $(devmem 0x02001184 32)"
echo "  I2S2 $(devmem 0x02001A18 32)"
