Copyright 2025 John Watts <contact@jookia.org>.
Licensed under the CC0.

Allwinner SDM clocking
----------------------

Recently I've been working on recording odd sample rates using an Allwinner
T113-S3 board. After a lot of modification I managed to get the I2S audio
driver to ask the kernel for a clock, but it wasn't as precise as I'd like. 

Allwinner chips support spread spectrum clocking to aid in reducing
electromagnetic interference. I managed to use this with a fixed frequency to
generate extremely precise clocks for recording at arbitrary sample rates.

This article attempts to explain how this works as it's not immediately obvious
just from reading the code or kernel patches. It's kind of spiralled out of
control and become a deep dive in to how to implement fractional PLL clocking in
the Linux kernel, so if you're trying to do that for other hardware this might
save you some time and headache.

As a bit of a meta note, I wrote this explainer to double check the
understanding and correctness of my work. During writing I found many, many
mistakes that I'm certain wouldn't have been picked up just from checking
whether the output looked correct. If you're ever in a situation where you're
dealing with a complex system you don't fully understand I would highly
recommend writing up something like this and making a model to play with.

Regular clocking
----------------

The Allwinner T113-S3's audio subsystem a clock tree close to this. This is
simplified but should work well enough for discussion:

- osc24_clk (24 MHz fixed frequency crystal oscillator)
- audio_pll (Generated from the oscillator)
- i2s_clk (Divided from audio_pll)
- bclk (Divided from i2s_clk)

The desired BCLK can be found using the following formula:

	bclk = sample_rate * channels * bit_depth

For all my tests I'm using 32-bit 8 channel audio, so for the formulas I'll use
those. To record 32-bit 8 channel 48 kHz audko it would look like this:

	bclk = 48 kHz * 8 * 32 = 12.288 MHz

The mainline kernel works by running the I2S clock at a fixed frequency then
changing BCLK divider to get the desired frequency.

For my recording example the clocks would look like this:

	audio_pll = 614.4 MHz
	i2s_clk = 24.576 MHz (divides audio_pll by 25)
	bclk = 12.288 MHz (divides i2s_clk by 2)

The next bclk divider is not 3, but 4. Assuming we don't change the channels
or bit depth we can calculate the sample rate for this divider this way:

	i2s_clk = 24.576 MHz (divides audio_pll by 25)
	bclk = 6.144 MHz (divides i2s_clk by 4)
	sample_rate = bclk / channels / bit_depth
	sample_rate = 6.144 MHz / 8 / 32 = 24 kHz

This means any sample rate between 24 kHz and 48 kHz cannot be recorded using
the same channels and bit depth. If we wanted to record 40 kHz audio we could
have to record 48 kHz. This would be a problem if we wanted to avoid sampling
frequencies higher than 40 kHz.

Flexible clocking
-----------------

Instead of dividing a fixed clock by a variable bclk I modified my kernel to
use a bclk of 1 and asked for the I2S clock to be changed to the sample rate.

Asking for the previous 48 kHz example gives this clock tree:

	audio_pll = 614.4 MHz
	i2s_clk = 12.288 MHz (divides audio_pll by 50)
	sample_rate = 12.288 MHz / 8 / 32 = 48 kHz

The bclk divisor is gone and its division is now factored in to the I2S clock.
Asking for a 40 kHz clock which was previously impossible gives this tree:

	audio_pll = 30.72 MHz
	i2s_clk = 10.24 MHz (divides audio_pll by 3)
	sample_rate = 10.24 MHz / 8 / 32 = 40 kHz

I wrote a script (sweep_clocks.sh) that enumerates a wide amount of sample
rates and reports which ones are not clocked correctly under this method.
I've included the output of this script in sweep_results.txt if you're curious.

The first interesting thing is that while we can ask for any sample rate, it
doesn't mean we will get it. The kernel may give a rate close to the desired
sample rate instead. For example, asking for 212.8 kHz gives 210.937 kHz.
This is troublesome as this inaccuracy is not reported to ALSA and userspace
can't do anything about it.

The second interesting thing is that some rates fail to record at all. Asking
for 158.300 kHz should give a 158.299 kHz clock. After a lot of testing I've
found that using a particular clock source can fail to output a clock signal
correctly, even when configured correctly. My guess is this is due to a
hardware bug of some sort affecting the use of audio0_1x.
More details are in the (sweep_failures.txt) file.

This solution is better than stock mainline, but we can do better. But first we
need to learn more about clocks.

Integer PLL clocks
------------------

High performance chips like the T113-S3 provide several PLL (phase-locked
loops) clocks that can take the input oscillator at 24 MHz and multiply it to a
user's desired frequency. These clocks are then fed to subsystem clocks which
can further divide the clock by integer values.

The T113-S3 features two PLL clocks used for the I2S subsystem. The first clock
is the audio0 PLL which uses the following formula to derive three clocks:

	osc24_clk = 24 MHz
	audio0_pll = (osc24_clk / DIV2) * N / M1 / P
	audio0_pll_4x_clk = audio0_pll
	audio0_pll_2x_clk = audio0_pll / 2
	audio0_pll_1x_clk = audio0_pll / 4
	where 1 <= DIV2 <= 2 (also known as M0)
	and 7.5 <= N / M0 / M1 <= 125
	and 12 <= N
	and 1 <= M1 <= 2
	and 1 <= M0 <= 2
	and 1 <= P <= 64

The second clock is the audio1 PLL that derives three clocks:

	osc24_clk = 24 MHz
	audio1_pll = (osc24_clk / DIV2) * N / M
	audio1_pll_div2_clk = audio1_pll / P0
	audio1_pll_div5_clk = audio1_pll / P1
	where 1 <= DIV2 <= 2
	and 7.5 <= N <= 145
	and 12 <= N
	and 1 <= M <= 2
	and 1 <= P0 <= 8
	and 1 <= P1 <= 8

For our purposes we can ignore DIV2 and M1 as the kernel sets these to 1.

Each factor here is represented by a physical component such a multiplier or
divider circuit. The PLL multiplier has a frequency range between 180 MHz and
3000 Mhz. This manifests as the constraints above that keep the output passed to
the P or M dividers within that frequency range. Violating these constraints can
cause strange things like the frequency appearing lower than expected.

After division by P we may get a frequency that lower than the multiplier's
frequency range. This isn't a problem, the operating range affects the
multiplier and M0 and M1 dividers, not P. The P divisor has its own set of
properties, such as an odd value giving an unequal duty cycle of the clock.

The T113-S3 user manual doesn't specify constraints for audio1_pll's N, so I
calculated them here based on its supported operating frequency range. I haven't
tested these constraints, instead I decided to keep N <= 128 as 128 is listed in
the data sheet as a supported N useful for various audio frequencies.

I wrote a script (pll_frequencies.py) to enumerate all possible frequencies
available from these PLL clocks. Running it gives this line among others:

	614400000.0=24000000*128/5/1 pll=audio1

This shows the 614.4 MHz frequency (in Hz) is made by multiplying N to 128 then
dividing by 5. This frequency is useful enough for common frequencies that
Allwinner suggests using these exact settings for the an audio clock.

We can also see these start and end lines:

	1125000.0=24000000*12/64/4 pll=audio0
	1142857.142857143=24000000*12/63/4 pll=audio0
	1161290.322580645=24000000*12/62/4 pll=audio0
	1180327.8688524591=24000000*12/61/4 pll=audio0
	...
	3000000000.0=24000000*125/1/1 pll=audio0
	3024000000.0=24000000*126/1/1 pll=audio1
	3048000000.0=24000000*127/1/1 pll=audio1
	3072000000.0=24000000*128/1/1 pll=audio1

At the lower end of the frequencies we have gaps of around 20 kHz while at the
higher end of the frequencies we have gaps of 24 MHz. This is because while we
multiply by units of our 24 MHz oscillator we divide by regular decimal units.
This mismatch gives us a range of frequencies that get less dense at the far end.

These clocks are further divided down by the I2S clock which helps achieve a
little more precision but as we've seen we still don't get an acceptable level
of precision to drive audio devices at arbitrary sample rates.

Kernel clocking
---------------

In the previous section I showed how to enumerate all possible PLL frequencies
for the audio0 and audio1 PLLs that the kernel could use. These have to be
further divided by the I2S clock to get a clock usable for audio rates.
Enumerating through all these then picking the closest matching clock is a slow
process. Taking minutes or hours to find a clock configuration to record audio
is unacceptable.

Instead of an exhaustive search the kernel uses an algorithm to recursively
calculate a clock rate by looping through its divisors and asking for multiplied
rates from its parents. It will then divide these back down to get a rate close
to the desired rate. This (unreviewed) psuedocode explains the general process:

    function round_rate(clock, wanted_rate)
    	closest_rate = 0
    	for divider in clock.dividers
		multiplied_rate = wanted_rate * divider;
    		for parent in clock.parents
    			parent_rate = round_rate(parent, multiplied_rate)
    			real_rate = parent_rate / divider
    			if rate_is_closer(real_rate, closest_rate, wanted_rate))
    				closest_rate = real_rate
    			if closest_rate == wanted_rate:
    				return closest_rate
    	return closest_rate

The real kernel code is a lot more complicated: Each clock has its own specific
implementation of round_rate that works a little differently. Depending on the
clock there may be multiple dividers, such as with PLLs. It may also be
unacceptable to get a clock higher than wanted_rate so it may always pick a
lower clock even if it isn't closer.

Because rates are specified in Hz, any dividers that produces a fraction may be
rounded (or floored) and not represented. This may doesn't happen in hardware,
so the clock you get back may have a small error compared to the software clock.

The 48kHz example from before gives this clock tree:

	osc24_clk = 24 MHz
	audio_pll = 614.4 MHz
	i2s_clk = 12.288 MHz (divides audio_pll by 50)
	sample_rate = 12.288 MHz / 8 / 32 = 48 kHz

Assuming a clock tree like this:

	- pll_audio0
	- pll_audio1
	- i2s_clk (parents pll_audio0 or pll_audio1)

Searching for the i2s_clk using two audio_plls could have a call tree like this:

	- round_rate(i2s_clk, 12.288 MHz)
		- round_rate(pll_audio0, 12.288 MHz) = 12.285714 MHz
			- Divides by 1 back down to 12.285714 MHz
			- New closest match: 12.285714 MHz
		- round_rate(pll_audio1, 12.288 MHz) = 0 MHz
			- No match, PLL can't go that low
		- round_rate(pll_audio0, 24.576 MHz) = 24.571428 MHz
			- Divides by 2 back down to 12.285714 MHz
			- Not a new closest match
		- round_rate(pll_audio1, 24.576 MHz) = 24 MHz
			- Divides by 2 back down to 12 MHz
			- Not a new closest match
		- (many other attempts omitted)
		- round_rate(pll_audio0, 614.4 MHz) = 614.4 MHz
			- Divides by 50 back down to 12.288 MHz
			- Perfect match found, return it

In reality we have a clock tree that looks more like this:

	- pll_audio0_4x
		- pll_audio0_2x
		- pll_audio0
	- pll_audio1
		- pll_audio1_div2
		- pll_audio1_div5
	- i2s_clk (parents pll_audio0_4x, pll_audio0, pll_audio1_div2, pll_audio1_div5)

The i2s_clk calc rate would search through parents rates for pll_audio0 which
would search through parent rates for pll_audio_4x. The same happens with
pll_audio1_div2 looking for parent rates for pll_audio1.
Note that there's significant overlap between pll_audio0 and pll_audio0_4x when
rates are divisible by 4.

Clock flooring gotcha
---------------------

When the kernel is told to set a clock to a rate it will internally round the
rate. This means code that looks like this can end up with some strange results:

	wanted_rate = sample_rate * channels * bit_depth
	real_rate = round_rate(i2s_clk, wanted_rate)
	if real_rate == 0:
		error("Unable to find clock")
	clk_set_rate(real_rate)

For example, let's look at sample rate of 8.2 kHz. For this rate we need the I2S
clock to be 2.099200 MHz. Asking the kernel for this rate gives us this clock tree:

	audio_pll = 14.689655 MHz
	i2s_clk = 2.098522 MHz (divides audio_pll by 7)
	sample_rate = 2.098522 MHz / 8 / 32 = 8.197 kHz

The kernel has found a rate close to and below the one we want. So what happens
if we ask for the new floored rate directly?
Doing so gives us this clock tree:

	audio_pll = 6.292682 MHz
	i2s_clk = 2.097560 MHz (divides audio_pll by 3)
	sample_rate = 2.097560 MHz / 8 / 32 = 8.193 kHz

We've asked the kernel for a rate it can clearly give us but we haven't received
it. This is because the correct audio_pll doesn't cleanly divide down to the
sample rate. Integer math truncates the remainder and gets us in a situation
where we can divide the correct sample rate from the correct audio_pll but not
vice versa.

	14689655 / 7 = 2098522
	2098522 * 7 = 14689654

When we ask for the rounded the value the kernel deems the correct audio PLL
value too large by 1 Hz and discards it.

This means round_rate and clk_set_rate given a value N may not give the value N
back even if the clock system is able to supply it.
To avoid this issue we must never pass a rounded rate to clk_set_rate, instead
the original must be preserved.

Clock composition gotcha
------------------------

When asking for for our 8.2 kHz sample rate we can actually do better than the
kernel gives us. We can have the following hypothetical clock tree:
For example, let's look at sample rate of 8.2 kHz. For this rate we need the I2S
clock to be 2.099200 MHz. Asking the kernel for this rate gives us this clock tree:

	audio_pll = 14.693877 MHz
	i2s_clk = 2.099125 MHz (divides audio_pll by 7)
	sample_rate = 2.099125 MHz / 8 / 32 = 8.199 kHz

However Linux will never give us this value. This is in part for the same reason
as the flooring gotcha: The audio_pll rate we multiply from the i2s_clk is
14.693875 MHz, while the actual PLL we could have is is 2 Hz too high.

However unlike the flooring problem, this is not a result of flooring happening
twice. This happens because the decision to discard and floor rates happens
locally on a per-clock basis rather than within the whole request for the final
rate. The audio PLL has no idea its clock is being further divided down.

This has another side effect: Clocks are unaware of the error in a rate and
assume all clocks they are for are perfect. Despite the I2S clock being able to
divide audio_pll multiple times it doesn't have a preference as to what
dividers it uses. It can't pick a divider that minimizes error from the PLL.

Clock rounding gotcha
---------------------

It's possible to specify that clocks should round to the closest rate requested
rather than floor the value down. This is done per clock rather than per clock
request. This is a little strange to me: Wouldn't the clock user know best about
the tolerance of the clock? Not the clock itself?

I decided to set the audio PLLs to round to see what would happen. It helped
with the two previous gotchas but introduced a new gotcha entirely: When mixing
clocks that floor and clocks that round you can get some strange results.

I wrote a script to help find the ideal clock setups for I2S rates
(best_i2s_rate.py). Using it we can find the best rates for 93.4 kHz rounded and
not rounded:

	$ ./best_i2s_rate.py 93400 8 32 1
	best freq is 93750.0 24000000.0=24000000*12/12/1 pll=audio0 N=1 M=1 rate=24000000.0 round=True
	$ ./best_i2s_rate.py 93400 8 32 0
	best freq is 93017.578125 190500000.0=24000000*127/8/2 pll=audio1 N=8 M=1 rate=23812500.0 round=False

From the results we can see that for a simple rate of 93.4 kHz we would expect
these clocks when flooring:

	audio1_pll = 24 MHz * 127 / 8 = 381 MHz
	audio1_pll_div2_clk = audio1_pll / 2 = 190.5 MHz
	i2s_clk = audio1_pll_div_clk / 8 = 23.8125 MHz
	sample_rate = 23.8125 MHz / 8 / 32 = 93.0 kHz

We would expect these clocks when rounding:

	audio1_pll = 24 MHz * 16 / 1 = 381 MHz
	audio1_pll_div2_clk = audio1_pll / 2 = 192 MHz
	i2s_clk = audio1_pll_div_clk / 8 = 24 MHz
	sample_rate = 24 MHz / 8 / 32 = 93.75 kHz

If all clocks are set to round or floor then either of these results should
work. But if only audio1_pll is set to round still we have a new problem:
audio1_pll_div2_clk asks for 191.2832 MHz but gets 192 MHz, a value larger than
expected. The clock Linux ends up giving me is now 92.307 kHz, a much less
accurate value than either of those.

Allwinner SDM PLL clocks
------------------------

Electronics signals really like radiate, even without antennas. Unintentional
radiation of high speed signals as electromagnetic interference is often a core
design problem when designing electronic circuits. There's many different ways
to minimize the radiated noise by careful hardware design, but removing the
noise source in the first place is can go a great way to helping.

One way to do this is to take a fixed clock signal and add intentional jitter to
it, spreading the signal over a spectrum. The less time spent at a particular
frequency the less energy is less noise is produced. The trade-off here is that
the signal is less accurate and uses more space in the electromagnetic spectrum.

Allwinner implements delta-sigma fractional-N PLL clocks for its chips. These
allow specifying two fractional frequencies and a rate to spread between these
frequencies. Allwinner and Linux call these SDM clocks or sigma-delta modulated
(modulation?) clocks. As of writing the best documentation for Allwinner's SDM
clocks are the T113-S3 user manual, though there is some less clear
documentation in the H616 and A113 manuals.

The calculations for the PLLs remain identical, however a new variable is added:
X. This represents an additional fractional value between 0 and 1 added to N.

As a simple example, say we want to program audio0_pll to operate at 600 MHz.
The formulas for calculating the PLL from before are a little complex, so let's
assume every variable we could supply is 1 except N. This gives us this formula:

	audio0_pll = 24 MHz * N
	audio0_pll = 24 MHz * 25 = 600 MHz

Now let's add X, a fractional value between 0 and 1. We can use that to
calculate two values (X1 and X2) for operating at 606 MHz and 616 MHz:

	audio0_pll = 24 MHz * (N + X)
	audio0_pll_x1 = 24 MHz * (25 + X1) = 606 MHz
	audio0_pll_x2 = 24 MHz * (25 + X2) = 618 MHz
	where X1 = 0.25
	where X2 = 0.75

The user manual doesn't address this, but I suspect (N + X) must be within the
range of N, such as (N + X) <= 125 rather than N <= 125 on its own with X added.
Otherwise the multiplier may go outside its supported frequency range.

The SDM PLLs represent X as a 17-bit fixed point number, a fraction of 2^17.
Our values of X converted to these numbers are:

	x1_fixed = (2**17) * 0.25 = 32768
	x2_fixed = (2**17) * 0.75 = 98304

There's a few interesting things to note here. The first is that because X is
added to N, it can't go below our original 600 MHz value or over 624 MHz. Any
two frequencies used for spread spectrum must exist in the same 24 MHz N space.

The second thing is if X1 equal to X2 we get just a fixed fractional clock. In
this case having X1 or X2 be 0 or 1 is a little useless.

The third thing to note is that the X fraction is in increments of 24 MHz /
(2**17), or 183 Hz. Because this is a fixed error you can create a high
frequency clock with the fixed error size then divide it down to reduce the
significance of the error in your final clock. Higher dividers will reduce the
maximum error in your final clock. But lower dividers may happen to divide in
such a way that creates a clock closer to your desired clock.

Implementing spread spectrum requires picking a spread frequency listed in the
datasheet then calculating the wave bottom and wave step like this:

	wave_bottom = x1_fixed = 32768
	step_diff = x2_fixed - x1_fixed = 65536
	step_div = 24 MHz / FREQ / 2 = 380.9523810
	wave_step = step_diff / step_div = 172.032
	where FREQ = 31.5 kHz

We can work backwards from these calculations to find frequencies from an
existing set of PLL configuration values. The T113-S3 user manual gives an
example, so let's try reverse engineering these variables:

	N = 25
	FREQ = 31.5 kHz
	wave_bottom = 28945
	wave_step = 63

Here's how I did it:

	x1_fixed = wave_bottom
	step_div = 24 MHz / FREQ / 2 = 380.9523810
	step_diff = wave_step * step_div = 24000
	x2_fixed = x1_fixed + step_diff = 52945
	x1 = x1_fixed / (2**17) = 0.2208328247
	x2 = x2_fixed / (2**17) = 0.4039382935
	freq1 = 24 MHz * (N + x1) = 605.2999878 MHz
	freq2 = 24 MHz * (N + x2) = 609.6945190 MHz

These are the same frequencies used in the user manual: 605.3 MHz and 609.7 MHz.
The formulas I've written here are not the official formulas but instead what
I've found convenient for writing and separating values.

The H616 user manual as well as various source code dumps specify fixed register
values the PLLs for audio PLLs. For example, this quote:

	When PLL_AUDIO(1X) is 24.576 MHz,
	PLL_AUDIO_CTRL_REG is recommended to set to 0xA8010F01,
	PLL_AUDIO_PAR0_CTRL_REG is recommended to set to 0xE000C49B.

After a little decoding of these registers we get these values:

	P = 2
	N = 16
	M1 = 1
	M0 = 2
	FREQ = 31.5 kHz
	wave_bottom = 50331
	wave_step = 0

A wave_step of 0 means the clock doesn't spread over a range of frequencies,
instead it stays fixed at a single frequency. This allows using the SDM PLL as a
static fractional clock with higher precision than the regular integer clock.

The calculation for a fixed frequency is simpler:

	X = wave_bottom / (2**17) = 0.3839950562
	audio0_pll = 24MHz * (16 + X) / 1 / 2 / 2 = 98.30397034 MHz
	audio0_pll_1x_clk = audio0_pll / 4 = 24.57599258 MHz

The last thing to note is that when using the SDM clock you must specify a
spread frequency mode. There's four modes, but from what I can tell only 1-bit
triangular spread frequency works with the calculations above. The rest produce
incorrect rates as well as significant jitter, at least for fixed clocks. Maybe
spread spectrum clocks require another mode.

Now that we understand the SDM clocks, let's look at real examples.

Dumping T113-S3 PLL clocks
--------------------------

I wrote a script to dump the clocks and their assosciated registers on the
T113-S3. Using it we can grab the control and pattern registers for each clock:

	$ ./dump_clocks.sh
	PLL_AUDIO0 0xF0142A00
	PLL_AUDIO0 PAT0 0x00000000
	PLL_AUDIO0 PAT1 0x00000000
	PLL_AUDIO1 0xF0417E02
	PLL_AUDIO1 PAT0 0x00000000
	PLL_AUDIO1 PAT1 0x00000000

Then we can pass them to another script to understand the clocks:

	$ ./dump_regs.py audio0 0xF0142A00 0x00000000
	--- Register dump for control register ---
	Hex value: 0xf0417e02
	        pll_en: 0x1 (1)
	        pll_ld_en: 0x1 (1)
	        lock_enable: 0x1 (1)
	        lock: 0x1 (1)
	        pll_output_gate: 0x0 (0)
	        sdm_enabled: 0x0 (0)
	        pll_unlock_mdsel: 0x0 (0)
	        pll_lock_mdsel: 0x0 (0)
	        pll_p1: 0x4 (4)
	        pll_p0: 0x1 (1)
	        pll_n: 0x7e (126)
	        pll_input_div2: 0x1 (1)
	--- Clock dump ---
	        pll: 1032.0 MHz
	        audio0_4x: 49.14285714285714 MHz
	        audio0_2x: 24.57142857142857 MHz
	        audio0_1x: 12.285714285714285 MHz
	
	$ ./dump_regs.py audio1 0xF0417E02 0x00000000
	--- Clock dump ---
	        pll: 1524.0 MHz
	        audio1: 1524.0 MHz
	        audio1_div2: 762.0 MHz
	        audio1_div5: 304.8 MHz
	
	$ ./dump_regs.py audio0 0xA9001800 0xE3F07111
	--- Register dump for pattern register ---
	Hex value: 0xe3f07111
	        sig_delt_pat_en: 0x1 (1)
	        spr_freq_mode: 0x3 (3)
	        wave_step: 0x3f (63)
	        sdm_clk_sel: 0x0 (0)
	        freq: 0x0 (0)
	        wave_bottom: 0x7111 (28945)
	--- Clock dump ---
	        pll: 605.2999877929688-609.6945190429688 MHz
	        audio0_4x: 605.2999877929688-609.6945190429688 MHz
	        audio0_2x: 302.6499938964844-304.8472595214844 MHz
	        audio0_1x: 151.3249969482422-152.4236297607422 MHz

I've omitted some output to save space here.

Manual SDM clock calculations
-----------------------------

When I did my frequency sweep I found that asking for a sample rate of 212.8 kHz
gave a real rate of 210.937 kHz, off by 1.863 kHz. Let's calculate a decent
fractional clock rate for this rate by hand. To do this we must:

1. Calculate the desired I2S clock rate
2. Find the largest post-divider P we can use to improve accuracy
3. Ensure it's even to ensure an equal duty cycle
4. Pre-divide N by the post-divider P to get the size of each N step
5. Divide the I2S clock by the pre-divided N
6. Convert the remainder X to a fraction of (2**17)
7. Round it and convert it to back to the fraction X
8. Calculate the clock as usual

You can do this by hand like this:

	rate = 212.8 kHz
	channels = 8
	bit_depth = 32
	i2s_clk = rate * channels * bit_depth = 54.4768 MHz
	P = even(24 MHz * MAX_N // i2s_clk) = even(55) = 54
	n_size = 24 MHz / 54 = 444.4444444 kHz
	N = i2s_clk / n_size = 122.5728000
	X_frac = (2**17) * 0.5728000 = 75078.0416
	X_real = round(X_frac) / (2**17) = 0.5727996826
	audio0_pll = 24 MHz * 122.5727996826 / 54 = 54.47679986 MHz
	real_rate = audio0_pll / channels / bit_depth = 212.7999994 kHz
	where MIN_N = 12
	where MAX_N = 127

The PLL rate is now off by 0.14 Hz, and the divided I2S rate is now off by
0.0006 Hz. This is much better than the original 1863 Hz error.

By rearranging the dividers we can find the precision for a given set of
dividers and estimate the maximum error for an audio rate:

	scaled = 24 MHz / P / channels / bit_depth = 1.736111111 kHz
	X_size = 1 / (2**17) = 0.000007629394531
	X_scaled = X_size * scaled = 13.24547662 mHz
	X = X_scaled * round(X_frac) = 994.4438937 Hz
	rate = (scaled * N) + X = 212.7999994 kHz

Here X_scaled is the granularity, or the increments we can add to our scaled N.
The higher the N the smaller X is in comparison, making a smaller error.

For the use case of audio, we could try to find the minimum divider P that gives
us a maximum rate to simplify our calculations, assuming the channels and bit
depth division is handled by the peripheral.
Here's an example for 8 channel 32-bit audio:

	scaled = 24 MHz / channels / bit_depth = 24 MHz / 8 / 32 = 93.75 kHz
	X_size = 1 / (2**17) = 0.000007629394531
	X_scaled = X_size * scaled = 715.2557373 mHz
	min_error = 500 mHz
	min_M = ceil(X_scaled / 500 mHz) = 2
	real_error = X_scaled / min_M = 357.6278687 mHz
	max_rate = (scaled / min_M * MAX_N) = 5.953125 MHz

For traditional 2 channel 16-bit audio, the minimum divisor must be 12:

	scaled = 24 MHz / channels / bit_depth = 24 MHz / 2 / 16 = 750 kHz
	X_size = 1 / (2**17) = 0.000007629394531
	X_scaled = X_size * scaled = 5.722045898 Hz
	min_error = 500 mHz
	min_M = ceil(X_scaled / 500 mHz) = 12
	real_error = X_scaled / min_M = 476.8371582 mHz
	max_rate = (scaled / min_M * MAX_N) = 7.9375 MHz

This is surprising but makes sense: Less data means a lower frequency rate. This
requires a lower frequency clock which will be more affected by error. To reduce
the error proportion the clock must be multiplied up to a higher clock and then
divided back down to get the correct clock.

The above calculations seem to imply the audio1 PLL can't deliver on the
necessary precision we desire for lower frequencies: It has a divisor from 1 to
8, it wouldn't be able to set M to 12. However the I2S peripheral can just
increase its division factor by 2 and M can be set to 6 just fine.

A last note is that this method does not always create the best clocks as the
highest divider only reduces error, it may not be the best divider. The ideal
divider creates an n_size that our requested clock cleanly fits in to, or as
closely as possible. This may be much lower than expected.

For example, the PLL clock rate for 48khz 8 channel 32-bit audio is 12.288 MHz.
An M divisor of 64 gives a fairly accurate clock, only off by 0.84 Hz. However
a divider of 44 gives a rate off by 0.06 Hz.

Kernel calculations
-------------------

Calculating a rate in the kernel clocking code is a little more difficult. In
addition to the hardware constraints we have some additional problems.

The first problems are with math: We can't use floating point math in the
kernel, and we're on 32-bit hardware. The compiler and kernel support 64-bit
math so we'll have to use fixed point math instead to represent rates.

The second set of problems are related to representing rates precisely: The
kernel represents a rate in Hz, so we can't pass any information out about how
accurate the clock is out of our calculations. If we floor our resulting rates
we may end up with code asking for a clock of 1000 Hz and taking a clock that is
1000.9 Hz instead of a clock that is 999.9 Hz.

The third set of problems is that we no longer have a full picture of the clock
tree. We don't know if we're being asked for a clock that can be less accurate
as it is being divided down, and we can't signal that there are more accurate
clocks if the clock consumer is willing to divide a higher rate.

The fourth set of problems is we don't have full control over the clocks with
DIV2 and M1 being pre-set by the kernel, and we can't use audio0_1x due to a
hardware bug. We also don't know the correct limit for the audio1 PLL.

To aid in writing the kernel code I first created a model program (sdm_model.py)
that somewhat simulates the kernel clocking logic and allows calculating
individual audio rates, PLL rates, and sweeping a frequency range to see the
range of errors. I would highly, highly recommend toying with this program and
reading its source code to get in to the details on clock generation.
Here's the current output for a sweep as of writing:

	$ ./sdm_model.py sweep
	--- Sweep results ---
	average PLL error 1.769337691 Hz
	average audio error 0.001026851 Hz
	worst PLL error 57.861328125 Hz (190400.000000000 Hz rate)
	worst audio error 0.011658669 Hz (214100.000000000 Hz rate)

I implemented a lot of settings to see what would improve these metrics. I
couldn't really find anything that gives significant gains outside a naive
algorithm that has a lot of precision loss.

The kernel comes with various patterns we can compare against:

	$ ./sdm_mode.py kernel
	--- Kernel comparison ---
	24.576000 MHz: real=24.575919 MHz our_rate=24.576000 MHz our_pll=audio0_4x error=80.984933 Hz our_error=0.100424 Hz
	49.152000 MHz: real=49.151996 MHz our_rate=49.151999 MHz our_pll=audio0_4x error=4.150391 Hz our_error=1.098633 Hz
	90.316800 MHz: real=90.316803 MHz our_rate=90.316798 MHz our_pll=audio1_div error=2.978516 Hz our_error=1.381138 Hz

I've cut out quite a lot, but after going and examining each difference I've
found that the clocking algorithm I've implemented matches or does better
provided rounding is enabled and we can select between multiple PLLs.

I implemented SDM support code in the kernel by replacing the existing
calculation of clock parameters with a fixed point one. Then I added a way to
pass the fraction to the SDM hardware. I didn't bother to enable the audio1 PLL
or enforce an equal duty cycle by making P equally divisible.

The kernel mostly matches the model, but there's some extra rounding happening
somewhere causing the PLL rounded result to be used for clk_set_rate.
Using the sweep script shows all rates have a delta of 1 Hz, but measuring with
an oscilloscope shows the deviation for audio signals is more like 0.2 Hz.

Asking for an 8.123 kHz rate and dumping registers shows I should be getting
a 8.123008104 kHz clock, but my oscilloscope frequency counter shows 8.12328
kHz audio clock. Multiplying this by channels and bits gives an I2S clock of
2.07955968 MHz, but the I2S clock signal is 2.07949 MHz. The conclusion I have
from this is that my tools can't reliably measure signals that precisely.

Fractional spurs
----------------

One concern with fractional clocks is that specific configurations will result
in spurious peaks of clock noise. I'll be honest: I don't understand what they
are, what they impact, or how to measure them. Hopefully this section is useful
to someone that does. If you're that someone, I wouldn't mind if you reached
out and explained them a bit more to me.

These spurs happen in specific points on the frequency range and these points
depend on the input clock signal (the oscillator) and its harmonics.
For example, a clock may have spurs at integer multiplies of N, multiples of
N/2, N/3, and so on. Each harmonic has less with the spurs.

We can't do much about the spurs outside avoid them. This is done by picking a
different frequency for our PLL and using a divider to get the desired output
frequency. Dividing the input clock can help with this too.

If we know where the spurs are in the frequency range we can adjust our clock
generation algorithm to try finding a rate without hitting specific fractions.
For instance we could mark the fractions of 0.5, 0.25, 0.75, 0.125, 0.375,
0.625 and 0.875 as bad points and prioritize N values that aren't near them.

Allwinner doesn't provide any guidance on which values to avoid. Neither do any
other manuals for fractional PLLs on chips I can find. I can't find any kernel
code related to handling spurs in fractional clocks either.

Apparently one feature of a fractional PLL is filtering, so my best guess is
that the hardware works to filter out these spurs to the point they aren't
noticeable.

I did look at various clock signals on my entry-level oscilloscope. I don't see
any jitter using display persistence. FFT mode while running the sweep clocks
function just showed square wave harmonics.

I added a setting to the model to try and keep fractions close to 0.5. I read
this suggestion second hand during my research for Allwinner SDM clocks.
Unfortunately I can't contact the source and ask why this was suggested. I then
compared the genearted rates to the kernel supplied rates and saw no change. So
either these rates already hang around 0.5 or Allwinner doesn't do any compensation
on their provided patterns either. I don't know.

I made the decision to keep fractions close to 0.5 in my kernel calculator. It
can always be added later if there's more concrete information.

Debugging tips
--------------

Troubleshooting clocking problems in the kernel can be a struggle. I don't have
an actual debugger, only a serial port. Even if I did, clocking code is a
fairly sensitive area and stopping everything to step through it seems a bit
troublesome. Adding print statements can be trouble too given how many loops
are in the PLL code and how many clock division combinations there are.
To help with this there's a few things you can do.

First, enable CLOCK_ALLOW_WRITE_DEBUGFS. This allows you to set clock rates
using /sys/kernel/debug. This lets you bypass driver code and child clocks
going through a ton of divisors.

	$ echo 880640000 > /sys/kernel/debug/clk/pll-audio0-4x/clk_rate

Second, disable parent clocks. This is Allwinner specific, but by changing
clock parents to NULL I could avoid using that clock parent. For example,
here's how to limit I2S parents to just audio0:

	static const struct clk_hw *i2s_spdif_tx_parents[] = {
		&pll_audio0_clk.hw,
		NULL, // &pll_audio0_4x_clk.common.hw
		NULL, // &pll_audio1_div2_clk.common.hw,
		NULL, // &pll_audio1_div5_clk.common.hw,
	};

Third, use trace_printk. This works the same as regular printk() in the kernel
but logs to a fast buffer you can dump later. You can use this with the tracing
subsystem to retrieve messages selectively. Here's what I run:

	cd /sys/kernel/debug/tracing # You may have to mount this
	echo > trace # Clear the trace
	echo 1 > tracing_on # Enable tracing capture
	echo 55040000 > /sys/kernel/debug/clk/pll-audio0/clk_rate
	echo 0 > tracing_on # Disable tracing capture
	cat trace # Read the trace

This worked very well for troubleshooting the clock calculation code. I used it
over SSH which was nicer than serial.
