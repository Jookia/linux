// SPDX-License-Identifier: GPL-2.0
// Copyright 2026 John Watts <contact@jookia.org>

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fwnode.h>
#include <linux/gpio/driver.h>
#include <linux/iio/iio.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

/* TODO:
 * - ti,rdac-ohm might need to be optional
 * - ti,vref-millivot might need to be a regulator
 * - ti,vref-millivot should be ti,vref-mv
 * - ti,osc-hz should be replaced with a clock input
 */

/* USER NOTES:
 * - Do not use spi-gpio with this driver without patching it to limit
 *   speed! The mainline spi-gpio driver will ignore any maximum SPI
 *   speed and instead go as fast as possible. Even on a slow Linux chip
 *   like the Allwinner T113 this goes faster than the datasheet allows.
 *
 * - DOUT cannot be immediately read when SCLK changes, instead you must
 *   wait at least 50ns (t7). Make sure your SPI controller and logic
 *   analyzers account for this, such as by sampling a half cycle late.
 *
 * - The DRDY pulse must be sampled at a high frequency (around 66kHz or
 *   better) to detect the falling edge. You may need to configure your
 *   chip's interrupt system to use a faster clock than its default.
 *   For example, the Allwinner T113 uses a 32kHz clock by default which
 *   will miss DRDY. Setting it to 24MHz using PIO_INT_CLK_SELECT can
 *   reliably detect the DRDY falling edge on this chip.
 *
 * - CS can be tied low without any impact on functionality.
 *
 * - DIN and DOUT can be tied together.
 *
 * - If SCLK timing is violated (such as by having cycles last too long)
 *   the chip may forget it's in an SPI transaction and start a new one.
 *   This can lead to hung transactions or corrupted commands that overwrite
 *   unintended registers. The DRDY period resetting back to its default value
 *   is one sign of this.
 *   It is not possible to send a RESET command in this state as it will be
 *   read as another command, the reset GPIO must be used instead.
 *   I don't know how this broken SPI behaviour manifests with 4-wire
 *   SPI or a shared SPI bus.
 *
 * - Reset without a GPIO only supports the case where the device has
 *   been freshly powered on or managed by this driver before a reboot.
 *   It can't handle the case of the chip misbehaving, asleep, in
 *   continuous mode or the being mid-calibration.
 *
 * - Do not leave chip inputs floating, especially DSYNC. This can
 *   result in the chip exhibiting similar behaviour as above.
 *
 * - The IIO interface uses microvolts. If you need higher precision the
 *   raw reading is provided too.
 *
 * - This driver does not provide multiple channels or automatic
 *   calibration. Instead it exposes the functionality of the ADC
 *   directly by allowing you to set or get various values.
 *
 * - Reading or writing values in sysfs may fail but still show the written
 *   values. You should either consider retrying the request or failing and
 *   giving up on using the chip until you can debug the problem proper.
 *
 * - To calibrate the offset or gain, write 'selfcal' or 'syscal' to the
 *   offset or gain calibration file. This will perform a calibration.
 *   You can then read back the calibration and store it for next time.
 *
 * - The IDAC value is specified in nanoamps with an error up to 1024nA.
 *
 * - After setting an IDAC value you must wait for the value to physically
 *   settle. The datasheet doesn't list how long this will take, so you will
 *   have to experiment yourself.
 */

/* DEVELOPER NOTES:
 * - DRDY goes low to indicate that it's unsafe to read data, that it's
 *   unsafe to send any commands until calibration is finished, or that
 *   it's safe to send RESET or STOPC in continuous mode.
 *
 * - DRDY may not go low if the input voltages are out of range.
 *
 * - I interpret 'read data' to mean a full SPI transaction as that
 *   matches up with Figure 30 (RDATA Command Sequence) in the datasheet.
 *   It's unclear what happens if you do this while DRDY is high.
 *
 * - The datasheet provides two confusing figures: t17A and t17B which
 *   indicate the time around the falling edge where the DOR (data
 *   output register) contents are invalid. It specifies minimums with
 *   no maximums. You could interpret this to mean it's safe to have a
 *   SPI transaction run past the start of the DRDY rising edge as long
 *   as it doesn't clock out data during the falling edge, or you could
 *   interpret this to mean the data output register is always invalid.
 *   This driver chooses to discard any SPI transactions that overlap with
 *   t17A or t17B.
 *
 * - Tracking DRDY using interrupts can be flaky: Ideally we would
 *   have an interrupt for the edge changing then sample the GPIO value
 *   on each interrupt to see which state of the DRDY signal we're in.
 *   Then we can easily wait for DRDY to go low, send a SPI transaction,
 *   then check if DRDY went high. I tried this approach using two
 *   completion events. Waiting for DRDY going low worked fine, but it
 *   seemed the high check wouldn't trigger consistently.
 *
 * - The DRDY status is available in the MDEC1 register but this doesn't
 *   unclear how this is sampled in relation to the SPI transaction.
 *   The DRDY signal is comparatively small to a SPI transaction so
 *   repeated register reads risk missing the signal status. Then we
 *   would have to wait for the next opportunity and add considerable
 *   latency in the response to userspace.
 *
 * - The design I've gone with to ensure SPI transactions stay in the
 *   DRDY low period is to track falling DRDY edges and use dead
 *   reckoning to decide if a SPI transaction was viable or overtime.
 *   See the driver timing graph below for more details.
 *
 * - Using a regmap to handle registers is tricky as it would require sharing
 *   the ADC lock with any other program using the regmap, such as debugfs.
 *   I've opted to just manually write registers and provide an IIO debugfs
 *   endpoint to aid in debugging.
 *
 * - We assume writes succeed and cache the correct values instead of
 *   re-reading them. This avoids the headache of dealing with values split
 *   across registers that have failed to write.
 *
 * - Changes to settings do not apply until the next DRDY cycle, so to ensure
 *   userspace gets a sample consistent with their settings we discard the
 *   current sample when changing settings.
 */

/* DRIVER TIMING GRAPH:
 *
 * /-----------\____________________________/-----------\_________ DRDY
 *             ^----------------------------------------^ drdy_ns
 *             ^----^ latency_ns
 *                                       ^--------------^ pulse_ns
 *                   ^------------------^ drdy_window_ns
 *                             ^--------^ nowait_ns
 *                             x nowait_overtime_ktime
 *             <----> drdy_ktime
 *                      x start_ktime
 *                                 <----> overtime_ktime
 *              end_ktime <-----GOOD-----|--------OVERTIME--------
 *
 * - drdy_ns is the DRDY period. This is same as tDATA in the datasheet,
 *   which is (tOSC * mod_div * decimation). By default on a 4.9152MHz
 *   oscillator this is ((1 / 4.9152MHz) * 128 * 1920) = 50ms
 *
 * - latency_ns is the maximum latency we expect to have between the
 *   actual DRDY falling edge and the value in drdy_ktime. This may be
 *   longer than the actual latency.
 *
 * - pulse_ns is the maximum time we expect DRDY to be high for at the
 *   end of a DRDY period. This may be longer than the actual pulse.
 *
 * - drdy_window_ns is the period of time it is safe to perform an SPI
 *   transaction.
 *
 * - nowait_ns is the period of time where it is unacceptable to start
 *   a SPI transaction without waiting for the next DRDY period.
 *
 * - nowait_overtime_ktime is the latest time a SPI transaction can
 *   start without waiting for the next DRDY period.
 *
 * - drdy_ktime is the time measured when Linux handles the interrupt
 *   for DRDY going low. The time between the signal going low and the
 *   time being marked should be up to maximum latency_ns.
 *
 * - start_ktime is the time the SPI transaction is sent to the controller.
 *
 * - overtime_ktime is the latest time a SPI transaction can end. It is
 *   determined by adding drdy_window_ns to drdy_ktime and inherits its
 *   error based on latency_ns.
 *
 * - end_ktime is the time measured after the SPI transaction.
 *   If it is before overtime_ktime then it is considered good,
 *   otherwise the transaction is considered overtime and discarded.
 */

#define ADS1216_CMD_RDATA 0x01
#define ADS1216_CMD_RREG 0x10
#define ADS1216_CMD_WREG 0x50
#define ADS1216_CMD_SELFOCAL 0xF1
#define ADS1216_CMD_SELFGCAL 0xF2
#define ADS1216_CMD_SYSOCAL 0xF3
#define ADS1216_CMD_SYSGCAL 0xF4
#define ADS1216_CMD_RESET 0xFE

#define ADS1216_REG_SETUP 0x00
#define ADS1216_REG_MUX 0x01
#define ADS1216_REG_ACR 0x02
#define ADS1216_REG_IDAC1 0x03
#define ADS1216_REG_IDAC2 0x04
#define ADS1216_REG_DIO 0x06
#define ADS1216_REG_DIR 0x07
#define ADS1216_REG_DEC0 0x08
#define ADS1216_REG_MDEC1 0x09
#define ADS1216_REG_OCR0 0x0A
#define ADS1216_REG_OCR1 0x0B
#define ADS1216_REG_OCR2 0x0C
#define ADS1216_REG_FSR0 0x0D
#define ADS1216_REG_FSR1 0x0E
#define ADS1216_REG_FSR2 0x0F
#define ADS1216_REG_MAX ADS1216_REG_FSR2

#define ADS1216_SETUP_ID GENMASK(7, 5)
#define ADS1216_SETUP_SPEED BIT(4)
#define ADS1216_SETUP_REF_EN BIT(3)
#define ADS1216_SETUP_REF_HI BIT(2)
#define ADS1216_SETUP_BUF_EN BIT(1)
#define ADS1216_SETUP_BIT_ORDER BIT(0)

#define ADS1216_MUX_PSEL GENMASK(7, 4)
#define ADS1216_MUX_NSEL GENMASK(3, 0)
#define ADS1216_MUX_AIN(x) x
#define ADS1216_MUX_AINCOM 8
#define ADS1216_MUX_TEMP 15

static const char * const ads1216_iio_sels_str[] = {
	// Make sure these map to the register order above
	"ain0",
	"ain1",
	"ain2",
	"ain3",
	"ain4",
	"ain5",
	"ain6",
	"ain7",
	"aincom",
	"temp",
};

#define ADS1216_IIO_SELS_TEMP 9 // The position of "temp" in the list above

#define ADS1216_ACR_BOCS BIT(7)
#define ADS1216_ACR_IDAC2R GENMASK(6, 5)
#define ADS1216_ACR_IDAC1R GENMASK(4, 3)
#define ADS1216_ACR_PGA GENMASK(2, 0)
#define ADS1216_ACR_PGA_MAX 128
#define ADS1216_ACR_IDACR_MAX 3

#define ADS1216_IIO_IDAC1 0
#define ADS1216_IIO_IDAC2 1

#define ADS1216_SETUP_SPEED BIT(4)
#define ADS1216_SETUP_REF_EN BIT(3)
#define ADS1216_SETUP_REF_HI BIT(2)
#define ADS1216_SETUP_BUF_EN BIT(1)
#define ADS1216_SETUP_BIT_ORDER BIT(0)

#define ADS1216_MDEC1_DATA_DRDY BIT(7)
#define ADS1216_MDEC1_DATA_FORMAT BIT(6)
#define ADS1216_MDEC1_SMODE GENMASK(5, 4)
#define ADS1216_MDEC1_RSVD BIT(3)
#define ADS1216_MDEC1_DEC GENMASK(2, 0)

#define ADS1216_MDEC1_DATA_FORMAT_BIPOLAR 0
#define ADS1216_MDEC1_DATA_FORMAT_UNIPOLAR 1

#define ADS1216_DECIMATION_MIN 20
#define ADS1216_DECIMATION_MAX 2047

#define ADS1216_FILTER_AUTO 0x00
#define ADS1216_FILTER_FAST 0x01
#define ADS1216_FILTER_SINC2 0x02
#define ADS1216_FILTER_SINC3 0x03

// Settings to use when initializing the chip.
// These are set to the defaults from the datasheet.
#define ADS1216_INIT_DECIMATION 1920
#define ADS1216_INIT_FILTER ADS1216_FILTER_AUTO
#define ADS1216_INIT_PSEL ADS1216_MUX_AIN(0)
#define ADS1216_INIT_NSEL ADS1216_MUX_AIN(1)
#define ADS1216_INIT_GPIO_DIR 0xFF // All input

static const char * const ads1216_iio_filters_str[] = {
	// Make sure these map to the register order above
	"auto",
	"fast",
	"sinc2",
	"sinc3",
};

// These list of timings are taken from the datasheet
#define ADS1216_TIMING_CMD_RDATA_TOSC 50 // t6
#define ADS1216_TIMING_CMD_RREG_TOSC 50 // t6
#define ADS1216_TIMING_CMD_WREG_TOSC 50 // t6
#define ADS1216_TIMING_CS_HOLD_TOSC 16 // t10
#define ADS1216_TIMING_DRDY_INVALID_TOSC 12 // t17B
#define ADS1216_TIMING_POST_RDATA_TOSC 4 // t11
#define ADS1216_TIMING_POST_RREG_TOSC 4 // t11
#define ADS1216_TIMING_POST_WREG_TOSC 4 // t11
#define ADS1216_TIMING_POST_RESET_TOSC 16 // t11
#define ADS1216_TIMING_POST_CALIB_MAX_DRDY 14 // t11, max for all calibration
#define ADS1216_TIMING_DRDY_DELAY_TOSC 12 // t17B

// The minimum time the reset GPIO should go low then go high.
// This time should be at least as long as t16 and t11 at 1MHz.
// t16 is 4 tOSC periods, t11 is 16 tOSC periods, so we will pick 16.
// At the minimum speed of 1MHz tOSC is 16us, so use that.
#define ADS1216_TIMING_RESET_GPIO_US 16

// The SPI speed to use for transactions.
// 128kHz seems to be the minimum needed to safely handle the lowest decimation
// value. We can't go higher than (fOSC / 4), which at 1MHz is 250kHz.
#define ADS1216_TIMING_SPI_SPEED_HZ 128000 // t1

// Some very low decimation values have too fast a DRDY period to
// reasonably read from. Periods under this length will be doubled
// by increasing the modulator clock division from 128 to 256.
// Warning: This will cause periods that are higher than half this value
// to exceed this value. For instance, 13ms becomes 26ms and 24ms
// becomes 48ms.
// I picked 25ms because it seems to me like the maximum unexpected
// additional latency you may want for making ADC measurements.
#define ADS1216_TIMING_DRDY_MOD256_NS 25000000

// The maximum amount of time we should assume has passed between the
// DRDY falling edge happening and the kernel setting drdy_ktime.
// Ideally this should account for the interrupt sampling speed,
// scheduling jitter and ktime precision.
// I picked 100us because it seems safe on my setup.
#define ADS1216_TIMING_DRDY_LATENCY_NS 100000

// The length of a DRDY pulse when the chip is idle (not calibrating or
// resetting or some other state). This isn't specified in the
// datasheet, but it seems to be around 30us on my chip with a 4.9152MHz
// oscillator, so maybe it would last around ~150 tOSC cycles in the
// idle case. This seems independent of modulator or decimation speed. I
// picked 200 tOSC periods to give some headroom. This ends up being
// 40us for my case which seems good enough.
#define ADS1216_TIMING_DRDY_PULSE_TOSC 200

// The minimum time left in a DRDY window deemed safe to do a SPI
// transaction without waiting for the next DRDY period. This should be
// the maximum time a SPI transaction will take. This has the effect of
// being the maximum latency for a read.
// I picked 25ms because I've seen my spi-gpio transactions take up to
// 20ms on my setup.
#define ADS1216_TIMING_DRDY_NOREAD_NS 25000000

struct ads1216_private {
	struct completion drdy_complete;
	ktime_t drdy_ktime; // Synchronized by drdy_complete

	struct mutex lock; // Protects entire structure below

	// Devices
	struct device *dev;
	struct spi_device *spi_dev;
	struct gpio_chip gpio;

	// fwnode settings
	struct gpio_desc *reset_gpio;
	u32 rdac_ohm;
	bool vref_en;
	u32 vref_uv;
	bool buf_en;
	u32 osc_hz;

	// Device state
	bool discard_sample;
	bool buffer_on;
	u16 decimation;
	u16 mod_div;
	u8 filter;
	u8 idac1r;
	u8 idac2r;
	u8 idac1;
	u8 idac2;
	u8 psel;
	u8 nsel;
	u8 pga;
	u8 dio;
	u8 dir;

	// Calculations
	u32 idac_base_na; // (VREF / (8 * rDAC))
	u32 osc_ns; // tOSC
	u32 drdy_ns; // tDATA, DRDY periods
};

static irqreturn_t ads1216_drdy_interrupt(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = (struct iio_dev *)dev_id;
	struct ads1216_private *priv = iio_priv(indio_dev);

	if (!completion_done(&priv->drdy_complete)) {
		// Only set drdy_ktime when we do a completion,
		// otherwise we could overwrite it mid-read
		priv->drdy_ktime = ktime_get();
		complete(&priv->drdy_complete);
	}

	return IRQ_HANDLED;
};

static int ads1216_drdy_wait_timeout(struct ads1216_private *priv,
	u32 timeout_ns)
{
	struct device *dev = priv->dev;
	int ret = 0;

	reinit_completion(&priv->drdy_complete);

	if (timeout_ns == 0) {
		wait_for_completion(&priv->drdy_complete);
	} else {
		u32 timeout_jiffies = nsecs_to_jiffies(timeout_ns);

		ret = wait_for_completion_timeout(&priv->drdy_complete,
						  timeout_jiffies);
		if (ret == 0) {
			dev_err(dev, "DRDY didn't activate?\n");
			return -EIO;
		}
	}

	return 0;
}

static int ads1216_drdy_wait(struct ads1216_private *priv)
{
	u32 timeout_ns = priv->drdy_ns * 2; // Allow missed one edge

	return ads1216_drdy_wait_timeout(priv, timeout_ns);
}

// DRDY sync modes for transfers
#define DRDY_SYNC_NONE (1 << 0) // Don't wait for DRDY at all
#define DRDY_SYNC_NEXT (1 << 1) // Wait for the next falling edge
#define DRDY_SYNC_LOW (1 << 2) // Finish before DRDY goes high

struct ads1216_xfer {
	u8 drdy_sync;
	u8 command_buf[8];
	u8 response_buf[8];
	int command_len;
	int response_len;
	int command_delay_tosc; // t6
	int post_delay_tosc; // t11
};

static int ads1216_drdy_sync(struct ads1216_private *priv,
			     u8 drdy_sync, ktime_t *overtime_ktime,
			     bool *check_overtime)
{
	bool drdy_next = drdy_sync & DRDY_SYNC_NEXT;
	bool drdy_low = drdy_sync & DRDY_SYNC_LOW;

	*check_overtime = drdy_low;

	if (!drdy_next && !drdy_low)
		return 0;

	// This code below calculates the driver timing graph based on the
	// previous DRDY time we've observed. This way we can check if we have
	// to wait for the next DRDY pulse or we can ask for a result right now

	u32 latency_ns = ADS1216_TIMING_DRDY_LATENCY_NS;
	u32 pulse_ns = priv->osc_ns * ADS1216_TIMING_DRDY_PULSE_TOSC;
	u32 drdy_window_ns = priv->drdy_ns - latency_ns - pulse_ns;
	ktime_t drdy_window_ktime = ns_to_ktime(drdy_window_ns);
	*overtime_ktime = ktime_add(priv->drdy_ktime, drdy_window_ktime);

	u32 nowait_ns = ADS1216_TIMING_DRDY_NOREAD_NS;
	ktime_t nowait_ktime = ns_to_ktime(nowait_ns);
	ktime_t nowait_overtime_ktime = ktime_sub(*overtime_ktime, nowait_ktime);
	ktime_t start_ktime = ktime_get();
	bool nowait_overtime = ktime_after(start_ktime, nowait_overtime_ktime);

	if (drdy_next || nowait_overtime) {
		int ret = ads1216_drdy_wait(priv);

		if (ret < 0)
			return ret;

		// Recalculate overtime_ktime for our new drdy_ktime
		*overtime_ktime = ktime_add(priv->drdy_ktime, drdy_window_ktime);
	}

	// Delay just to make sure the data output register is valid
	u32 drdy_invalid_ns = priv->osc_ns * ADS1216_TIMING_DRDY_INVALID_TOSC;

	ndelay(drdy_invalid_ns);

	return 0;
}

static int ads1216_transfer(struct ads1216_private *priv,
			    struct ads1216_xfer *xfer)
{
	struct spi_device *spi_dev = priv->spi_dev;
	struct device *dev = priv->dev;
	int ret = 0;

	dev_dbg(dev, "command %*phN", xfer->command_len, xfer->command_buf);

	int command_delay_ns = priv->osc_ns * xfer->command_delay_tosc;
	int cs_delay_ns = priv->osc_ns * ADS1216_TIMING_CS_HOLD_TOSC;
	int xfer_count = (xfer->response_len != 0) ? 2 : 1;

	if (xfer_count == 1) {
		// When we're only writing but not reading DOUT we
		// shouldn't ever need to wait on command_delay_ns and
		// instead just set the delay to cs_delay_ns.
		// But commands like WREG and WRAM seem to require a
		// delay anyway at t6 according to the datasheet?
		command_delay_ns += cs_delay_ns;
	}

	struct spi_transfer xfers[2] = {
		{
			.tx_buf = &xfer->command_buf,
			.len = xfer->command_len,
			.speed_hz = ADS1216_TIMING_SPI_SPEED_HZ,
			.delay = {
				.value = command_delay_ns,
				.unit = SPI_DELAY_UNIT_NSECS,
			},
		},
		{
			// This xfer is ignored if no response is expected
			.rx_buf = &xfer->response_buf,
			.len = xfer->response_len,
			.speed_hz = ADS1216_TIMING_SPI_SPEED_HZ,
			.delay = {
				.value = cs_delay_ns,
				.unit = SPI_DELAY_UNIT_NSECS,
			},
		},
	};

	ktime_t overtime_ktime;
	bool check_overtime;

	ret = ads1216_drdy_sync(priv, xfer->drdy_sync,
				&overtime_ktime, &check_overtime);
	if (ret < 0) {
		dev_err(dev, "ads1216_drdy_sync failed: %d\n", ret);
		return ret;
	}

	ret = spi_sync_transfer(spi_dev, xfers, xfer_count);
	if (ret < 0) {
		dev_err(dev, "spi_sync_transfer failed: %d\n", ret);
		return ret;
	}

	ktime_t end_ktime = ktime_get();
	bool overtime = ktime_after(end_ktime, overtime_ktime);

	if (check_overtime && overtime) {
		dev_err(dev, "SPI transaction took too long!\n");
		return -EIO;
	}

	int post_delay_ns = priv->osc_ns * xfer->post_delay_tosc;

	// post_delay_ns overlaps with cs_delay_ns so skip it if we can
	if (post_delay_ns > cs_delay_ns)
		ndelay(post_delay_ns - cs_delay_ns);

	if (xfer->response_len)
		dev_dbg(dev, "response %*phN",
			xfer->response_len, xfer->response_buf);

	return 0;
}

static int ads1216_reset(struct ads1216_private *priv)
{
	// This command is invalid in certain circumstances, such as
	// calibration, sleep or continuous mode with DRDY being high.
	// We don't know the state the chip is in when we run this command
	// so we can't do much here to avoid sending an invalid command

	struct ads1216_xfer xfer = {0};

	xfer.drdy_sync = DRDY_SYNC_NONE;
	xfer.command_buf[0] = ADS1216_CMD_RESET;
	xfer.command_len = 1;
	xfer.post_delay_tosc = ADS1216_TIMING_POST_RESET_TOSC;
	int ret = 0;

	priv->discard_sample = true;

	ret = ads1216_transfer(priv, &xfer);
	if (ret < 0)
		return ret;

	return 0;
}

static int ads1216_reg_read(struct ads1216_private *priv,
	u8 reg, u8 *out, size_t count)
{
	struct ads1216_xfer xfer = {0};
	int ret = 0;

	if (reg > 0xF)
		return -EINVAL;

	if (sizeof(xfer.response_buf) < count)
		return -EINVAL;

	xfer.drdy_sync = DRDY_SYNC_NONE;
	xfer.command_buf[0] = ADS1216_CMD_RREG | reg;
	xfer.command_buf[1] = count - 1;
	xfer.command_len = 2;
	xfer.response_len = count;
	xfer.command_delay_tosc = ADS1216_TIMING_CMD_RREG_TOSC;
	xfer.post_delay_tosc = ADS1216_TIMING_POST_RREG_TOSC;

	ret = ads1216_transfer(priv, &xfer);

	if (ret == 0)
		memcpy(out, xfer.response_buf, count);

	return ret;
}

static int ads1216_reg_write(struct ads1216_private *priv,
	u8 reg, u8 *val, size_t count)
{
	struct ads1216_xfer xfer = {0};

	if (reg > ADS1216_REG_MAX)
		return -EINVAL;

	if ((sizeof(xfer.command_buf) - 2) < count)
		return -EINVAL;

	xfer.drdy_sync = DRDY_SYNC_NONE;
	xfer.command_buf[0] = ADS1216_CMD_WREG | reg;
	xfer.command_buf[1] = count - 1;
	memcpy(&xfer.command_buf[2], val, count);
	xfer.command_len = 2 + count;
	xfer.command_delay_tosc = ADS1216_TIMING_CMD_WREG_TOSC;
	xfer.post_delay_tosc = ADS1216_TIMING_POST_WREG_TOSC;

	return ads1216_transfer(priv, &xfer);
}

static int ads1216_update_setup(struct ads1216_private *priv)
{
	bool setup_speed = priv->mod_div == 256;
	bool setup_ref_en = priv->vref_en;
	bool setup_ref_hi = priv->vref_en && priv->vref_uv == 2500000;
	bool setup_buf_en = priv->buf_en && priv->buffer_on;
	bool setup_bit_order = 0;

	u8 val = FIELD_PREP(ADS1216_SETUP_ID, 0) |
	      FIELD_PREP(ADS1216_SETUP_SPEED, setup_speed) |
	      FIELD_PREP(ADS1216_SETUP_REF_EN, setup_ref_en) |
	      FIELD_PREP(ADS1216_SETUP_REF_HI, setup_ref_hi) |
	      FIELD_PREP(ADS1216_SETUP_BUF_EN, setup_buf_en) |
	      FIELD_PREP(ADS1216_SETUP_BIT_ORDER, setup_bit_order);

	return ads1216_reg_write(priv, ADS1216_REG_SETUP, &val, 1);
}

static int ads1216_update_dec(struct ads1216_private *priv)
{
	u8 val = priv->decimation & 0xFF;

	return ads1216_reg_write(priv, ADS1216_REG_DEC0, &val, 1);
}

static int ads1216_update_mdec1(struct ads1216_private *priv)
{
	u8 dec1 = (priv->decimation >> 8) & 0x7;
	u8 mdec1_smode = priv->filter;
	bool mdec1_data_format = ADS1216_MDEC1_DATA_FORMAT_BIPOLAR;

	u8 val = FIELD_PREP(ADS1216_MDEC1_DATA_DRDY, 0) |
	      FIELD_PREP(ADS1216_MDEC1_DATA_FORMAT, mdec1_data_format) |
	      FIELD_PREP(ADS1216_MDEC1_SMODE, mdec1_smode) |
	      FIELD_PREP(ADS1216_MDEC1_RSVD, 0) |
	      FIELD_PREP(ADS1216_MDEC1_DEC, dec1);

	return ads1216_reg_write(priv, ADS1216_REG_MDEC1, &val, 1);
}

static int ads1216_update_acr(struct ads1216_private *priv)
{
	u8 reg = FIELD_PREP(ADS1216_ACR_BOCS, 0) |
		FIELD_PREP(ADS1216_ACR_IDAC2R, priv->idac2r) |
		FIELD_PREP(ADS1216_ACR_IDAC1R, priv->idac1r) |
		FIELD_PREP(ADS1216_ACR_PGA, priv->pga);

	return ads1216_reg_write(priv, ADS1216_REG_ACR, &reg, 1);
}

static int ads1216_calibrate(struct ads1216_private *priv, u8 command)
{
	struct ads1216_xfer xfer = {0};

	xfer.drdy_sync = DRDY_SYNC_NEXT;
	xfer.command_buf[0] = command;
	xfer.command_len = 1;
	int ret = 0;

	priv->discard_sample = true;

	ret = ads1216_transfer(priv, &xfer);
	if (ret < 0)
		return ret;

	// Calibration commands supposedly take the timing in t11 to complete,
	// but DRDY doesn't go low for an additional period.
	// Allow two extra periods to compensate
	u32 timeout_drdy = ADS1216_TIMING_POST_CALIB_MAX_DRDY + 2;
	u32 timeout_ns = priv->drdy_ns * timeout_drdy;

	ret = ads1216_drdy_wait_timeout(priv, timeout_ns);
	if (ret < 0)
		return ret;

	return 0;
}

static int ads1216_data_read(struct ads1216_private *priv, u32 *out)
{
	struct ads1216_xfer xfer = {0};

	xfer.drdy_sync = DRDY_SYNC_LOW;
	xfer.command_buf[0] = ADS1216_CMD_RDATA;
	xfer.command_len = 1;
	xfer.response_len = 3;
	xfer.command_delay_tosc = ADS1216_TIMING_CMD_RDATA_TOSC;
	xfer.post_delay_tosc = ADS1216_TIMING_POST_RDATA_TOSC;

	// Wait until the next period if we have discard_sample set
	// This will discard a valid sample if the discard and read
	// happen in different periods.
	if (priv->discard_sample) {
		xfer.drdy_sync |= DRDY_SYNC_NEXT;
		priv->discard_sample = false;
	}

	int ret = ads1216_transfer(priv, &xfer);

	if (ret == 0)
		*out = xfer.response_buf[0] << 16 |
		       xfer.response_buf[1] << 8 |
		       xfer.response_buf[2] << 0;

	return ret;
}

static int ads1216_decimation_set(struct ads1216_private *priv, u16 decimation)
{
	u32 old_drdy_timeout_ns = priv->drdy_ns * 2; // Allow one missed edge

	priv->decimation = decimation;
	priv->mod_div = 128;
	priv->drdy_ns = priv->osc_ns * priv->mod_div * priv->decimation;

	if (priv->drdy_ns < ADS1216_TIMING_DRDY_MOD256_NS) {
		priv->mod_div = 256;
		priv->drdy_ns *= 2;
	}

	dev_dbg(priv->dev, "decimation: %d", priv->decimation);
	dev_dbg(priv->dev, "mod_div: %d", priv->mod_div);
	dev_dbg(priv->dev, "drdy_ns: %d", priv->drdy_ns);

	int ret = ads1216_update_setup(priv);

	if (ret < 0)
		return ret;

	ret = ads1216_update_dec(priv);
	if (ret < 0)
		return ret;

	ret = ads1216_update_mdec1(priv);
	if (ret < 0)
		return ret;

	// discard_sample is not needed as we wait for DRDY

	return ads1216_drdy_wait_timeout(priv, old_drdy_timeout_ns);
}

static int ads1216_scale_get(struct ads1216_private *priv)
{
	return priv->vref_uv >> priv->pga;
}

static int ads1216_scale_set(struct ads1216_private *priv, int val)
{
	if (val <= 0 || priv->vref_uv < val)
		return -EINVAL;

	// The highest value we can read for a given PGA value is:
	//   max_value = priv->vref_uv / (2 ** priv->pga)
	// We can find a given PGA value for a max_value this way:
	//   (2 ** priv->pga) = priv->vref_uv / max_value
	//   priv->pga = log2(priv->vref_uv / max_value)
	// If instead of passing max_value we pass val it's likely the result
	// won't cleanly divide. By flooring at each step that returns a
	// fraction we end up with a PGA value that is smaller than wanted.
	// This is exactly what we want to avoid setting the gain too high and
	// being unable to read the signal.

	// Make sure to clamp the divisor to be within 1 - ADS1216_ACR_PGA_MAX
	priv->pga = ilog2(min(priv->vref_uv / val, ADS1216_ACR_PGA_MAX));

	priv->discard_sample = true;

	return ads1216_update_acr(priv);
}

static int ads1216_sel_get(struct ads1216_private *priv, bool psel)
{
	int val = 0;

	if (psel)
		val = priv->psel;
	else
		val = priv->nsel;

	if (val == ADS1216_MUX_TEMP)
		val = ADS1216_IIO_SELS_TEMP;

	return val;
}

static int ads1216_sel_set(struct ads1216_private *priv, bool psel, int val)
{
	if (val == ADS1216_IIO_SELS_TEMP)
		val = ADS1216_MUX_TEMP;

	if (psel)
		priv->psel = val;
	else
		priv->nsel = val;

	priv->discard_sample = true;

	u8 reg = FIELD_PREP(ADS1216_MUX_PSEL, priv->psel) |
		FIELD_PREP(ADS1216_MUX_NSEL, priv->nsel);

	return ads1216_reg_write(priv, ADS1216_REG_MUX, &reg, 1);
}

static int ads1216_filter_set(struct ads1216_private *priv, int val)
{
	priv->filter = val;
	priv->discard_sample = true;

	return ads1216_update_mdec1(priv);
}

static int ads1216_buffer_set(struct ads1216_private *priv, bool val)
{
	priv->buffer_on = val;
	priv->discard_sample = true;

	return ads1216_update_setup(priv);
}

static int ads1216_idac_get(struct ads1216_private *priv, bool is_idac1)
{
	// The formula for IDAC current is:
	//   (VREF / (8 * rDAC)) * (2 ** (RANGE - 1)) * (DAC CODE)
	// We have already calculated the first term and stored it as
	// priv->idac_base_na, so our equation looks like this:
	//   priv->idac_base_na * (2 ** (RANGE - 1)) * (DAC CODE)
	// RANGE is a register value, we can store it as priv->idac1r:
	//   priv->idac_base_na * (2 ** (priv->idac1r - 1)) * (DAC CODE)
	// DAC CODE is a register value, we can store it as priv->idac1:
	//   priv->idac_base_na * (2 ** (priv->idac1r - 1)) * (priv->idac1)
	// We can re-arrange this equation to use a left shift:
	//   priv->idac_base_na * (priv->idac1r << (priv->idac1 - 1))
	// For IDAC2 we use priv->idac2 and priv->idac2r

	if (is_idac1)
		return priv->idac_base_na * (priv->idac1 << (priv->idac1r - 1));
	else
		return priv->idac_base_na * (priv->idac2 << (priv->idac2r - 1));
}

static int ads1216_idac_set(struct ads1216_private *priv, bool is_idac1,
			    u32 current_na)
{
	// We must calculate the RANGE and DAC CODE values such that:
	//   CURRENT = (VREF / (8 * rDAC)) * (2 ** (RANGE - 1)) * (DAC CODE)
	// If we look at our formula from ads1216_idac_get:
	//   priv->idac_base_na * (priv->idac1r << (priv->idac1 - 1))
	// We can see that priv->idac1r is a kind of 8-bit floating point value
	// with priv->idac1 specifying the mantissa.
	// All we need to do is to find our divisor and convert it to this format

	// Get the divisor as a regular non-fixed value
	u32 divisor = 0;

	if (current_na != 0)
		divisor = current_na / priv->idac_base_na;

	// Find the mantissa and shift down the value to fit in 8 bits
	u8 dac_mantissa = max(fls(divisor) - 8, 0);
	u8 dac_value = divisor >> dac_mantissa;

	// Add 1 to the range as 0 is reserved for disabling the DAC
	u8 dac_range = dac_mantissa + 1;

	if (dac_value == 0) // Disable output if there's no value
		dac_range = 0;

	// If we've overflowed, use the maximum value
	if (dac_range > ADS1216_ACR_IDACR_MAX) {
		dac_value = 0xFF;
		dac_range = ADS1216_ACR_IDACR_MAX;
	}

	dev_dbg(priv->dev, "current %dnA divisor %d range %d value %d",
		current_na, (u32)divisor, dac_range, dac_value);

	int ret = 0;
	u8 reg = 0;
	u8 val = dac_value;

	if (is_idac1) {
		reg = ADS1216_REG_IDAC1;
		priv->idac1r = dac_range;
		priv->idac1 = dac_value;
	} else {
		reg = ADS1216_REG_IDAC2;
		priv->idac2r = dac_range;
		priv->idac2 = dac_value;
	}

	priv->discard_sample = true;

	ret = ads1216_update_acr(priv);
	if (ret < 0)
		return ret;

	return ads1216_reg_write(priv, reg, &val, 1);
}

static int ads1216_read_raw(struct ads1216_private *priv, int *val)
{
	u32 raw_value = 0;
	int ret = 0;

	ret = ads1216_data_read(priv, &raw_value);
	if (ret < 0)
		return ret;

	*val = raw_value;

	return IIO_VAL_INT;
}

static int ads1216_read_processed(struct ads1216_private *priv, int *val)
{
	u32 raw_value = 0;
	int ret = 0;

	ret = ads1216_data_read(priv, &raw_value);
	if (ret < 0)
		return ret;

	// In bipolar mode the 24-bit data value is signed, sign extend it
	// to 32 bits so we can do signed arithmetic on it
	raw_value = sign_extend32(raw_value, 23);

	// Convert to signed 60.24 fixed point value.
	// This will give us a fixed point value between -1.0 and 1.0
	s64 fixed24 = raw_value;

	// The data value is a fraction of (VREF * 2). Multiply it to undo the
	// fraction and get the total voltage
	fixed24 *= priv->vref_uv * 2;

	// Discard the remaining fractional value and return the total voltage
	*val = fixed24 >> 24;

	return IIO_VAL_INT;
}

static int ads1216_read_scale(struct ads1216_private *priv, int *val)
{
	int ret = ads1216_scale_get(priv);

	if (ret < 0)
		return ret;

	*val = ret;

	return IIO_VAL_INT;
}

static int ads1216_iio_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	if (mask == IIO_CHAN_INFO_RAW)
		return ads1216_read_raw(priv, val);
	else if (mask == IIO_CHAN_INFO_PROCESSED)
		return ads1216_read_processed(priv, val);
	else if (mask == IIO_CHAN_INFO_SCALE)
		return ads1216_read_scale(priv, val);
	else
		return -EINVAL;
}

static int ads1216_iio_write_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int val, int val2, long mask)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	if (mask == IIO_CHAN_INFO_SCALE)
		return ads1216_scale_set(priv, val);
	else
		return -EINVAL;
}

static int ads1216_iio_debugfs_reg_access(struct iio_dev *indio_dev,
					  unsigned int reg,
					  unsigned int writeval,
					  unsigned int *readval)
{
	struct ads1216_private *priv = iio_priv(indio_dev);
	u8 val = writeval;
	int ret = 0;

	guard(mutex)(&priv->lock);

	if (reg < 0 || reg > 0xF)
		return -EINVAL;

	if (writeval < 0 || writeval > 0xFF)
		return -EINVAL;

	if (readval) {
		ret = ads1216_reg_read(priv, reg, &val, 1);
		if (ret >= 0)
			*readval = val;
	} else {
		ret = ads1216_reg_write(priv, reg, &val, 1);
	}

	return ret;
}

static int ads1216_iio_get_filter(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return priv->filter;
}

static int ads1216_iio_set_filter(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       unsigned int val)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return ads1216_filter_set(priv, val);
}

static int ads1216_iio_get_psel(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return ads1216_sel_get(priv, true);
}

static int ads1216_iio_set_psel(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       unsigned int val)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return ads1216_sel_set(priv, true, val);
}

static int ads1216_iio_get_nsel(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return ads1216_sel_get(priv, false);
}

static int ads1216_iio_set_nsel(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       unsigned int val)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return ads1216_sel_set(priv, false, val);
}

static int ads1216_iio_calibrate(struct ads1216_private *priv,
				 bool self, u8 reg, size_t len)
{
	u8 command = 0;
	int ret = -EINVAL;

	if (self && reg == ADS1216_REG_OCR0)
		command = ADS1216_CMD_SELFOCAL;
	else if (self && reg == ADS1216_REG_FSR0)
		command = ADS1216_CMD_SELFGCAL;
	else if (!self && reg == ADS1216_REG_OCR0)
		command = ADS1216_CMD_SYSOCAL;
	else if (!self && reg == ADS1216_REG_FSR0)
		command = ADS1216_CMD_SYSGCAL;

	if (command)
		ret = ads1216_calibrate(priv, command);

	if (ret < 0)
		return ret;

	return len;
}

static ssize_t ads1216_iio_read_ocrfsr(struct iio_dev *indio_dev,
				       uintptr_t private,
				       const struct iio_chan_spec *chan,
				       char *buf)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	u8 reg_val[3];
	int ret = ads1216_reg_read(priv, private, reg_val, ARRAY_SIZE(reg_val));

	if (ret < 0)
		return ret;

	s32 val = reg_val[2] << 16 |
		reg_val[1] << 8 |
		reg_val[0] << 0;

	return sysfs_emit(buf, "%d\n", val);
}

static ssize_t ads1216_iio_write_ocrfsr(struct iio_dev *indio_dev,
					uintptr_t private,
					const struct iio_chan_spec *chan,
					const char *buf, size_t len)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);
	int ret = 0;

	guard(mutex)(&priv->lock);

	// NOTE: sysfs passes a buffer big enough to avoid read overflows

	// Special case if calibration is requested
	if (strncmp(buf, "selfcal", strlen("selfcal")) == 0)
		return ads1216_iio_calibrate(priv, true, private, len);
	else if (strncmp(buf, "syscal", strlen("syscal")) == 0)
		return ads1216_iio_calibrate(priv, false, private, len);

	u32 val = 0;

	ret = kstrtou32(buf, 10, &val);
	if (ret < 0)
		return ret;

	// The write must be an unsigned 24-bit value
	if ((1 << 24) <= val)
		return -EINVAL;

	u8 reg_val[3];

	reg_val[0] = (val >> 0) & 0xFF;
	reg_val[1] = (val >> 8) & 0xFF;
	reg_val[2] = (val >> 16) & 0xFF;

	ret = ads1216_reg_write(priv, private, reg_val, ARRAY_SIZE(reg_val));
	if (ret < 0)
		return ret;

	return len;
}

static ssize_t ads1216_iio_read_decimation(struct iio_dev *indio_dev,
					   uintptr_t private,
					   const struct iio_chan_spec *chan,
					   char *buf)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	return sysfs_emit(buf, "%d\n", priv->decimation);
}

static ssize_t ads1216_iio_write_decimation(struct iio_dev *indio_dev,
					    uintptr_t private,
					    const struct iio_chan_spec *chan,
					    const char *buf, size_t len)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);
	int ret = 0;

	guard(mutex)(&priv->lock);

	u32 val = 0;

	ret = kstrtou32(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val < ADS1216_DECIMATION_MIN || val > ADS1216_DECIMATION_MAX)
		return -EINVAL;

	ret = ads1216_decimation_set(priv, val);
	if (ret < 0)
		return ret;

	return len;
}

static ssize_t ads1216_iio_read_buffer(struct iio_dev *indio_dev,
				       uintptr_t private,
				       const struct iio_chan_spec *chan,
				       char *buf)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	if (priv->buffer_on)
		return sysfs_emit(buf, "on\n");
	else
		return sysfs_emit(buf, "off\n");
}

static ssize_t ads1216_iio_write_buffer(struct iio_dev *indio_dev,
					uintptr_t private,
					const struct iio_chan_spec *chan,
					const char *buf, size_t len)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);
	int ret = 0;

	guard(mutex)(&priv->lock);

	bool val;

	if (strncmp(buf, "on", strlen("on")) == 0)
		val = true;
	else if (strncmp(buf, "off", strlen("off")) == 0)
		val = false;
	else
		return -EINVAL;

	ret = ads1216_buffer_set(priv, val);
	if (ret < 0)
		return ret;

	return len;
}

static ssize_t ads1216_iio_read_idac(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     char *buf)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);

	guard(mutex)(&priv->lock);

	bool is_rdac1 = private == ADS1216_IIO_IDAC1;
	u32 val = ads1216_idac_get(priv, is_rdac1);

	return sysfs_emit(buf, "%d\n", val);
}

static ssize_t ads1216_iio_write_idac(struct iio_dev *indio_dev,
				      uintptr_t private,
				      const struct iio_chan_spec *chan,
				      const char *buf, size_t len)
{
	(void)chan;
	struct ads1216_private *priv = iio_priv(indio_dev);
	int ret = 0;

	guard(mutex)(&priv->lock);

	u32 val = 0;

	ret = kstrtou32(buf, 10, &val);
	if (ret < 0)
		return ret;

	bool is_rdac1 = private == ADS1216_IIO_IDAC1;

	ret = ads1216_idac_set(priv, is_rdac1, val);
	if (ret < 0)
		return ret;

	return len;
}

static const struct iio_enum ads1216_iio_psel_enum = {
	.items = ads1216_iio_sels_str,
	.num_items = ARRAY_SIZE(ads1216_iio_sels_str),
	.get = ads1216_iio_get_psel,
	.set = ads1216_iio_set_psel,
};

static const struct iio_enum ads1216_iio_nsel_enum = {
	.items = ads1216_iio_sels_str,
	.num_items = ARRAY_SIZE(ads1216_iio_sels_str),
	.get = ads1216_iio_get_nsel,
	.set = ads1216_iio_set_nsel,
};

static const struct iio_enum ads1216_iio_filter_enum = {
	.items = ads1216_iio_filters_str,
	.num_items = ARRAY_SIZE(ads1216_iio_filters_str),
	.get = ads1216_iio_get_filter,
	.set = ads1216_iio_set_filter,
};

static const struct iio_chan_spec_ext_info ads1216_iio_ext_info[] = {
	IIO_ENUM("psel", IIO_SHARED_BY_ALL,
		 &ads1216_iio_psel_enum),
	IIO_ENUM_AVAILABLE("psel", IIO_SHARED_BY_ALL,
			   &ads1216_iio_psel_enum),
	IIO_ENUM("nsel", IIO_SHARED_BY_ALL,
		 &ads1216_iio_nsel_enum),
	IIO_ENUM_AVAILABLE("nsel", IIO_SHARED_BY_ALL,
			   &ads1216_iio_nsel_enum),
	IIO_ENUM("filter", IIO_SHARED_BY_ALL,
		 &ads1216_iio_filter_enum),
	IIO_ENUM_AVAILABLE("filter", IIO_SHARED_BY_ALL,
			   &ads1216_iio_filter_enum),
	{
		.name = "calibration_offset",
		.shared = IIO_SHARED_BY_ALL,
		.read = ads1216_iio_read_ocrfsr,
		.write = ads1216_iio_write_ocrfsr,
		.private = ADS1216_REG_OCR0,
	},
	{
		.name = "calibration_gain",
		.shared = IIO_SHARED_BY_ALL,
		.read = ads1216_iio_read_ocrfsr,
		.write = ads1216_iio_write_ocrfsr,
		.private = ADS1216_REG_FSR0,
	},
	{
		.name = "decimation",
		.shared = IIO_SHARED_BY_ALL,
		.read = ads1216_iio_read_decimation,
		.write = ads1216_iio_write_decimation,
	},
	{
		.name = "buffer",
		.shared = IIO_SHARED_BY_ALL,
		.read = ads1216_iio_read_buffer,
		.write = ads1216_iio_write_buffer,
	},
	{
		.name = "idac1",
		.shared = IIO_SHARED_BY_ALL,
		.read = ads1216_iio_read_idac,
		.write = ads1216_iio_write_idac,
		.private = ADS1216_IIO_IDAC1,
	},
	{
		.name = "idac2",
		.shared = IIO_SHARED_BY_ALL,
		.read = ads1216_iio_read_idac,
		.write = ads1216_iio_write_idac,
		.private = ADS1216_IIO_IDAC2,
	},
	{ }
};

static const struct iio_chan_spec ads1216_iio_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			BIT(IIO_CHAN_INFO_PROCESSED) |
			BIT(IIO_CHAN_INFO_SCALE),
		.ext_info = ads1216_iio_ext_info,
	}
};

static const struct iio_info ads1216_iio_info = {
	.read_raw = &ads1216_iio_read_raw,
	.write_raw = &ads1216_iio_write_raw,
	.debugfs_reg_access = ads1216_iio_debugfs_reg_access,
};

static int ads1216_gpio_get_direction(struct gpio_chip *gc,
				      unsigned int offset)
{
	struct ads1216_private *priv = gpiochip_get_data(gc);

	guard(mutex)(&priv->lock);

	bool input = (priv->dir & BIT(offset)) != 0;

	if (input)
		return GPIO_LINE_DIRECTION_IN;
	else
		return GPIO_LINE_DIRECTION_OUT;
}

static int ads1216_gpio_direction_input(struct gpio_chip *gc,
					unsigned int offset)
{
	struct ads1216_private *priv = gpiochip_get_data(gc);

	guard(mutex)(&priv->lock);

	priv->dir |= BIT(offset); // Set input

	return ads1216_reg_write(priv, ADS1216_REG_DIR, &priv->dir, 1);
}

static int ads1216_gpio_direction_output(struct gpio_chip *gc,
					 unsigned int offset,
					 int value)
{
	struct ads1216_private *priv = gpiochip_get_data(gc);
	int ret = 0;

	guard(mutex)(&priv->lock);

	priv->dir &= ~BIT(offset); // Set output

	ret = ads1216_reg_write(priv, ADS1216_REG_DIR, &priv->dir, 1);
	if (ret < 0)
		return ret;

	if (value)
		priv->dio |= BIT(offset); // Set high
	else
		priv->dio &= ~BIT(offset); // Set low

	return ads1216_reg_write(priv, ADS1216_REG_DIO, &priv->dio, 1);
}

static int ads1216_gpio_get(struct gpio_chip *gc,
			    unsigned int offset)
{
	struct ads1216_private *priv = gpiochip_get_data(gc);
	int ret = 0;
	u8 val = 0;

	guard(mutex)(&priv->lock);

	ret = ads1216_reg_read(priv, ADS1216_REG_DIO, &val, 1);
	if (ret < 0)
		return ret;

	return (val & BIT(offset)) != 0;
}

static void ads1216_gpio_set(struct gpio_chip *gc,
			     unsigned int offset,
			     int value)
{
	struct ads1216_private *priv = gpiochip_get_data(gc);

	guard(mutex)(&priv->lock);

	if (value)
		priv->dio |= BIT(offset); // Set high
	else
		priv->dio &= ~BIT(offset); // Set low

	ads1216_reg_write(priv, ADS1216_REG_DIO, &priv->dio, 1);
}

static const char *const supply_names[] = {
	"avdd",
	"dvdd",
	"vref",
};

static int ads1216_read_fwnode(struct ads1216_private *priv)
{
	struct device *dev = priv->dev;
	u32 vref_mv = 0;
	int ret = 0;

	struct fwnode_handle *fwnode = dev_fwnode(dev);

	if (!fwnode) {
		dev_err(dev, "no fwnode attached to device?\n");
		return -EINVAL;
	}

	priv->buf_en = fwnode_property_read_bool(fwnode, "ti,buf-en");
	priv->vref_en = fwnode_property_read_bool(fwnode, "ti,vref-en");

	ret = fwnode_property_read_u32(fwnode, "ti,vref-millivolt", &vref_mv);
	if (ret != 0) {
		dev_err(dev, "reading ti,vref-millivolt failed: %d\n", ret);
		return ret;
	}

	if (priv->vref_en && vref_mv != 1250 && vref_mv != 2500) {
		dev_err(dev, "invalid ti,vref-millivolt value\n");
		return ret;
	}

	ret = fwnode_property_read_u32(fwnode, "ti,osc-hz", &priv->osc_hz);
	if (ret != 0) {
		dev_err(dev, "reading ti,osc-hz failed: %d\n", ret);
		return ret;
	}

	ret = fwnode_property_read_u32(fwnode, "ti,rdac-ohm", &priv->rdac_ohm);
	if (ret != 0) {
		dev_err(dev, "reading ti,rdac-ohm failed: %d\n", ret);
		return ret;
	}

	if (priv->osc_hz < 1000000 || priv->osc_hz > 5000000)  {
		dev_err(dev, "oscillator clock out of range");
		return -EINVAL;
	}

	priv->vref_uv = vref_mv * 1000;
	priv->osc_ns = 1000000000 / priv->osc_hz;
	priv->buffer_on = priv->buf_en;

	// Precalculate idac_base_na (VREF / (8 * rDAC)
	if (priv->rdac_ohm == 0) {
		priv->idac_base_na = 0;
	} else {
		u32 vref_nv = priv->vref_uv * 1000;

		priv->idac_base_na = vref_nv / (8 *  priv->rdac_ohm);
	}

	dev_dbg(dev, "fwnode: %pfw", fwnode);
	dev_dbg(dev, "idac_base_na: %d", priv->idac_base_na);
	dev_dbg(dev, "rdac_ohm: %d", priv->rdac_ohm);
	dev_dbg(dev, "vref_en: %d", priv->vref_en);
	dev_dbg(dev, "vref_uv: %d", priv->vref_uv);
	dev_dbg(dev, "buf_en: %d", priv->buf_en);
	dev_dbg(dev, "osc_hz: %d", priv->osc_hz);
	dev_dbg(dev, "osc_ns: %d", priv->osc_ns);

	return 0;
}

static int ads1216_init(struct ads1216_private *priv)
{
	struct device *dev = priv->dev;
	int ret = 0;

	if (priv->reset_gpio) {
		gpiod_set_value_cansleep(priv->reset_gpio, 1);
		fsleep(ADS1216_TIMING_RESET_GPIO_US);

		gpiod_set_value_cansleep(priv->reset_gpio, 0);
		fsleep(ADS1216_TIMING_RESET_GPIO_US);
	} else {
		ret = ads1216_reset(priv);
		if (ret < 0) {
			dev_err(dev, "ads1216_reset failed: %d\n", ret);
			return -EIO;
		}
	}

	ret = ads1216_decimation_set(priv, ADS1216_INIT_DECIMATION);
	if (ret < 0) {
		dev_err(dev, "ads1216_update_decimation failed: %d\n", ret);
		return -EIO;
	}

	ret = ads1216_filter_set(priv, ADS1216_INIT_FILTER);
	if (ret < 0) {
		dev_err(dev, "ads1216_filter_set failed: %d\n", ret);
		return -EIO;
	}

	ret = ads1216_sel_set(priv, ADS1216_INIT_PSEL, true);
	if (ret < 0) {
		dev_err(dev, "ads1216_sel_set psel failed: %d\n", ret);
		return -EIO;
	}

	ret = ads1216_sel_set(priv, ADS1216_INIT_NSEL, false);
	if (ret < 0) {
		dev_err(dev, "ads1216_sel_set nsel failed: %d\n", ret);
		return -EIO;
	}

	ret = ads1216_scale_set(priv, priv->vref_uv);
	if (ret < 0) {
		dev_err(dev, "ads1216_scale_set failed: %d\n", ret);
		return -EIO;
	}

	// We don't need to write this register as we use the default
	priv->dir = ADS1216_INIT_GPIO_DIR;

	return 0;
}

static int ads1216_probe(struct spi_device *spi)
{
	struct ads1216_private *priv = NULL;
	struct iio_dev *indio_dev = NULL;
	struct device *dev = &spi->dev;
	int ret = 0;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	// Enable real-time mode for the SPI controller in hopes it will
	// keep our transactions within the DRDY period
	spi->rt = true;

	// We don't lock priv, instead we assume the kernel protects our
	// incomplete state during probe
	priv = iio_priv(indio_dev);

	init_completion(&priv->drdy_complete);
	priv->spi_dev = spi;
	priv->dev = dev;

	ret = ads1216_read_fwnode(priv);
	if (ret != 0) {
		dev_err(dev, "ads1216_read_fwnode failed: %d\n", ret);
		return ret;
	}

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(supply_names),
					     supply_names);
	if (ret != 0) {
		dev_err(dev, "regulator_bulk_get_enable failed: %d\n", ret);
		return ret;
	}

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio)) {
		ret = PTR_ERR(priv->reset_gpio);
		dev_err(dev, "devm_gpiod_get_optional reset failed: %d\n", ret);
		return ret;
	}

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ads1216_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(ads1216_iio_channels);
	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->info = &ads1216_iio_info;

	ret = devm_request_irq(dev, spi->irq, &ads1216_drdy_interrupt,
			       IRQF_TRIGGER_FALLING,
			       indio_dev->name, indio_dev);
	if (ret != 0) {
		dev_err(dev, "request_irq failed: %d\n", ret);
		return ret;
	}

	struct gpio_chip *gpio = &priv->gpio;

	gpio->label = "ads1216";
	gpio->parent = dev;
	gpio->owner = THIS_MODULE;
	gpio->get_direction = ads1216_gpio_get_direction;
	gpio->direction_input = ads1216_gpio_direction_input;
	gpio->direction_output = ads1216_gpio_direction_output;
	gpio->get = ads1216_gpio_get;
	gpio->set = ads1216_gpio_set;
	gpio->base = -1;
	gpio->ngpio = 8;
	gpio->can_sleep = true;

	ret = devm_gpiochip_add_data(dev, gpio, priv);
	if (ret < 0) {
		dev_err(dev, "ads1216_init failed: %d\n", ret);
		return ret;
	}

	ret = ads1216_init(priv);
	if (ret < 0) {
		dev_err(dev, "ads1216_init failed: %d\n", ret);
		return -EIO;
	}

	return devm_iio_device_register(dev, indio_dev);
}

static const struct spi_device_id ads1216_id[] = {
	{ "ads1216" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads1216_id);

static const struct of_device_id ads1216_of_table[] = {
	{ .compatible = "ti,ads1216" },
	{ }
};
MODULE_DEVICE_TABLE(of, ads1216_of_table);

static struct spi_driver ads1216_driver = {
	.driver = {
		.name = "ads1216",
		.of_match_table = ads1216_of_table,
	},
	.probe = ads1216_probe,
	.id_table = ads1216_id,
};
module_spi_driver(ads1216_driver);

MODULE_AUTHOR("John Watts <contact@jookia.org>");
MODULE_DESCRIPTION("TI ADS1216 ADC");
MODULE_LICENSE("GPL");
