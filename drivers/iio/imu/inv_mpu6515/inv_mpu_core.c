/*
* Copyright (C) 2012 Invensense, Inc.
*
* This software is licensed under the terms of the GNU General Public
* License version 2, as published by the Free Software Foundation, and
* may be copied, distributed, and modified under those terms.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR P URPOSE.  See the
* GNU General Public License for more details.
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/jiffies.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/of_gpio.h>
#include <linux/alarmtimer.h>

#include "inv_mpu_iio.h"
#ifdef INV_KERNEL_3_10
#include <linux/iio/sysfs.h>
#else
#include "sysfs.h"
#endif
#include "inv_test/inv_counters.h"

#ifdef CONFIG_DTS_INV_MPU_IIO
#include "inv_mpu_dts.h"
#endif

#if defined(CONFIG_SENSORS)
#include <linux/sensor/sensors_core.h>
#include "mpu6500_selftest.h"

#define MPU6500_ACCEL_CAL_PATH	"/efs/calibration_data"
#define MPU6500_GYRO_CAL_PATH	"/efs/gyro_cal_data"
#define MODEL_NAME	"MPU6515"
#define VENDOR_NAME	"INVENSENSE"
#endif

#define SHEALTH_VERSION	"29-May-2014 Rev 01"
#define MAX_BOARD_REV	0x1

static __s8 orientation_map[8][9] = {
	{-1,  0,  0, 0, -1,  0, 0,  0,  1},
	{ 0, -1,  0, 1,  0,  0, 0,  0,  1},
	{ 1,  0,  0, 0,  1,  0, 0,  0,  1},
	{ 0,  1,  0, -1, 0,  0, 0,  0,  1},
	{ 1,  0,  0, 0, -1,  0, 0,  0, -1},
	{ 0,  1,  0, 1,  0,  0, 0,  0, -1},
	{ -1, 0,  0, 0,  1,  0, 0,  0, -1},
	{ 0, -1,  0, -1, 0,  0, 0,  0, -1},
};

s64 get_time_ns(void)
{
	struct timespec ts;

	get_monotonic_boottime(&ts);

	return timespec_to_ns(&ts);
}

s64 get_time_timeofday(void)
{
	struct timeval tv;
	s64 nsec;

	do_gettimeofday(&tv);
	nsec = tv.tv_sec * 1000000000LL + tv.tv_usec * 1000;

	pr_info("[INV] %s time = %lld\n", __func__, nsec);

	return nsec;
}

/* This is for compatibility for power state. Should remove once HAL
   does not use power_state sysfs entry */
static bool fake_asleep;

static const struct inv_hw_s hw_info[INV_NUM_PARTS] = {
	{119, "ITG3500"},
	{ 63, "MPU3050"},
	{117, "MPU6050"},
	{118, "MPU9150"},
	{128, "MPU6500"},
	{128, "MPU9250"},
	{128, "MPU9255"},
	{128, "MPU9350"},
	{128, "MPU6515"},
};

static const u8 reg_gyro_offset[] = {REG_XG_OFFS_USRH,
					REG_XG_OFFS_USRH + 2,
					REG_XG_OFFS_USRH + 4};

const u8 reg_6050_accel_offset[] = {REG_XA_OFFS_H,
					REG_XA_OFFS_H + 2,
					REG_XA_OFFS_H + 4};

const u8 reg_6500_accel_offset[] = {REG_6500_XA_OFFS_H,
					REG_6500_YA_OFFS_H,
					REG_6500_ZA_OFFS_H};
#ifdef CONFIG_INV_TESTING
static bool suspend_state;
static int inv_mpu_suspend(struct device *dev);
static int inv_mpu_resume(struct device *dev);
struct test_data_out {
	bool gyro;
	bool accel;
	bool compass;
	bool pressure;
	bool LPQ;
	bool SIXQ;
	bool PEDQ;
};
static struct test_data_out data_out_control;
#endif

static void inv_setup_reg(struct inv_reg_map_s *reg)
{
	reg->sample_rate_div	= REG_SAMPLE_RATE_DIV;
	reg->lpf		= REG_CONFIG;
	reg->bank_sel		= REG_BANK_SEL;
	reg->user_ctrl		= REG_USER_CTRL;
	reg->fifo_en		= REG_FIFO_EN;
	reg->gyro_config	= REG_GYRO_CONFIG;
	reg->accel_config	= REG_ACCEL_CONFIG;
	reg->accel_config2	= REG_6500_ACCEL_CONFIG2;
	reg->fifo_count_h	= REG_FIFO_COUNT_H;
	reg->fifo_r_w		= REG_FIFO_R_W;
	reg->raw_accel		= REG_RAW_ACCEL;
	reg->temperature	= REG_TEMPERATURE;
	reg->int_enable		= REG_INT_ENABLE;
	reg->int_status		= REG_INT_STATUS;
	reg->pwr_mgmt_1		= REG_PWR_MGMT_1;
	reg->pwr_mgmt_2		= REG_PWR_MGMT_2;
	reg->mem_start_addr	= REG_MEM_START_ADDR;
	reg->mem_r_w		= REG_MEM_RW;
	reg->prgm_strt_addrh	= REG_PRGM_STRT_ADDRH;
};

/**
 *  inv_i2c_read_base() - Read one or more bytes from the device registers.
 *  @st:	Device driver instance.
 *  @i2c_addr:  i2c address of device.
 *  @reg:	First device register to be read from.
 *  @length:	Number of bytes to read.
 *  @data:	Data read from device.
 *  NOTE:This is not re-implementation of i2c_smbus_read because i2c
 *       address could be specified in this case. We could have two different
 *       i2c address due to secondary i2c interface.
 */
int inv_i2c_read_base(struct inv_mpu_state *st, u16 i2c_addr,
	u8 reg, u16 length, u8 *data)
{
	struct i2c_msg msgs[2];
	int res;

	if (!data)
		return -EINVAL;

	msgs[0].addr = i2c_addr;
	msgs[0].flags = 0;	/* write */
	msgs[0].buf = &reg;
	msgs[0].len = 1;

	msgs[1].addr = i2c_addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].buf = data;
	msgs[1].len = length;

	res = i2c_transfer(st->sl_handle, msgs, 2);

	if (res < 2) {
		if (res >= 0)
			res = -EIO;
	} else
		res = 0;

	INV_I2C_INC_MPUWRITE(3);
	INV_I2C_INC_MPUREAD(length);
#ifdef CONFIG_DYNAMIC_DEBUG
	{
		char *read = 0;
		pr_debug("%s RD%02X%02X%02X -> %s%s\n", st->hw->name,
			 i2c_addr, reg, length,
			 wr_pr_debug_begin(data, length, read),
			 wr_pr_debug_end(read));
	}
#endif
	return res;
}

/**
 *  inv_i2c_single_write_base() - Write a byte to a device register.
 *  @st:	Device driver instance.
 *  @i2c_addr:  I2C address of the device.
 *  @reg:	Device register to be written to.
 *  @data:	Byte to write to device.
 *  NOTE:This is not re-implementation of i2c_smbus_write because i2c
 *       address could be specified in this case. We could have two different
 *       i2c address due to secondary i2c interface.
 */
int inv_i2c_single_write_base(struct inv_mpu_state *st,
	u16 i2c_addr, u8 reg, u8 data)
{
	u8 tmp[2];
	struct i2c_msg msg;
	int res;
	tmp[0] = reg;
	tmp[1] = data;

	msg.addr = i2c_addr;
	msg.flags = 0;	/* write */
	msg.buf = tmp;
	msg.len = 2;

	pr_debug("%s WR%02X%02X%02X\n", st->hw->name, i2c_addr, reg, data);
	INV_I2C_INC_MPUWRITE(3);

	res = i2c_transfer(st->sl_handle, &msg, 1);
	if (res < 1) {
		if (res == 0)
			res = -EIO;
		return res;
	} else
		return 0;
}

static int accel_open_calibration(struct inv_mpu_state *st)
{
	struct file *cal_filp = NULL;
	int err = 0;
	mm_segment_t old_fs = {0};

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(MPU6500_ACCEL_CAL_PATH, O_RDONLY, 0);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		goto done;
	}

	err = cal_filp->f_op->read(cal_filp,
		(char *)&st->cal_data,
			3 * sizeof(s16), &cal_filp->f_pos);
	if (err != 3 * sizeof(s16)) {
		pr_err("%s: Can't read the cal data from file\n", __func__);
		err = -EIO;
	}

	pr_info("%s: (%d,%d,%d)\n", __func__,
		st->cal_data[0], st->cal_data[1],
			st->cal_data[2]);

	filp_close(cal_filp, current->files);
done:
	set_fs(old_fs);
	return err;
}

static int gyro_open_calibration(struct inv_mpu_state *st)
{
	struct file *cal_filp = NULL;
	int err = 0;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(MPU6500_GYRO_CAL_PATH, O_RDONLY, 0);
	if (IS_ERR(cal_filp)) {
		pr_err("[SENSOR] %s: - Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		goto done;
	}

	err = cal_filp->f_op->read(cal_filp,
		(char *)&st->gyro_bias, 3 * sizeof(int),
			&cal_filp->f_pos);
	if (err != 3 * sizeof(int)) {
		pr_err("[SENSOR] %s: - Can't read the cal data from file\n", __func__);
		err = -EIO;
	}

	pr_info("[SENSOR] %s: - (%d,%d,%d)\n", __func__,
		st->gyro_bias[0], st->gyro_bias[1],	st->gyro_bias[2]);

	filp_close(cal_filp, current->files);
done:
	set_fs(old_fs);
	return err;
}

static int gyro_do_calibrate(struct inv_mpu_state *st)
{
	struct file *cal_filp;
	int err;
	mm_segment_t old_fs;

	/* selftest was doing 2000dps condition, change to 500dps */
	st->gyro_bias[0] = st->gyro_bias[0] << 2;
	st->gyro_bias[1] = st->gyro_bias[1] << 2;
	st->gyro_bias[2] = st->gyro_bias[2] << 2;

	pr_info("[SENSOR] %s: - cal data (%d,%d,%d)\n", __func__,
		st->gyro_bias[0], st->gyro_bias[1], st->gyro_bias[2]);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(MPU6500_GYRO_CAL_PATH,
			O_CREAT | O_TRUNC | O_WRONLY, 0660);
	if (IS_ERR(cal_filp)) {
		pr_err("[SENSOR] %s: - Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		goto done;
	}

	err = cal_filp->f_op->write(cal_filp,
		(char *)&st->gyro_bias, 3 * sizeof(int),
			&cal_filp->f_pos);
	if (err != 3 * sizeof(int)) {
		pr_err("[SENSOR] %s: - Can't write the cal data to file\n", __func__);
		err = -EIO;
	}

	filp_close(cal_filp, current->files);
done:
	set_fs(old_fs);
	return err;
}


static int inv_switch_engine(struct inv_mpu_state *st, bool en, u32 mask)
{
	struct inv_reg_map_s *reg;
	u8 data, mgmt_1;
	int result;

	reg = &st->reg;
	/* Only when gyro is on, can
	   clock source be switched to gyro. Otherwise, it must be set to
	   internal clock */
	if (BIT_PWR_GYRO_STBY == mask) {
		result = inv_i2c_read(st, reg->pwr_mgmt_1, 1, &mgmt_1);
		if (result)
			return result;

		mgmt_1 &= ~BIT_CLK_MASK;
	}

	if ((BIT_PWR_GYRO_STBY == mask) && (!en)) {
		/* turning off gyro requires switch to internal clock first.
		   Then turn off gyro engine */
		mgmt_1 |= INV_CLK_INTERNAL;
		result = inv_i2c_single_write(st, reg->pwr_mgmt_1,
						mgmt_1);
		if (result)
			return result;
	}

	result = inv_i2c_read(st, reg->pwr_mgmt_2, 1, &data);
	if (result)
		return result;
	if (en)
		data &= (~mask);
	else
		data |= mask;
	result = inv_i2c_single_write(st, reg->pwr_mgmt_2, data);
	if (result)
		return result;

	if ((BIT_PWR_GYRO_STBY == mask) && en) {
		/* after gyro is on & stable, switch internal clock to PLL */
		mgmt_1 |= INV_CLK_PLL;
		result = inv_i2c_single_write(st, reg->pwr_mgmt_1,
						mgmt_1);
		if (result)
			return result;
		/* only gyro on needs sensor up time */
		msleep(50);
	}
	if ((BIT_PWR_ACCEL_STBY == mask) && en)
		usleep_range(REG_UP_TIME, REG_UP_TIME + 100);

	return 0;
}

/*
 *  inv_lpa_freq() - store current low power frequency setting.
 */
static int inv_lpa_freq(struct inv_mpu_state *st, int lpa_freq)
{
	unsigned long result;
	u8 d;
	/* 2, 4, 6, 7 corresponds to 0.98, 3.91, 15.63, 31.25 */
	const u8 mpu6500_lpa_mapping[] = {2, 4, 6, 7};

	if (lpa_freq > MAX_LPA_FREQ_PARAM)
		return -EINVAL;

	if (INV_MPU6500 == st->chip_type) {
		d = mpu6500_lpa_mapping[lpa_freq];
		result = inv_i2c_single_write(st, REG_6500_LP_ACCEL_ODR, d);
		if (result)
			return result;
	}
	st->chip_config.lpa_freq = lpa_freq;

	return 0;
}

static int set_power_itg(struct inv_mpu_state *st, bool power_on)
{
	struct inv_reg_map_s *reg;
	u8 data;
	int result;

	if ((!power_on) == st->chip_config.is_asleep)
		return 0;
	reg = &st->reg;
	if (power_on)
		data = 0;
	else
		data = BIT_SLEEP;

	if (!st->reactive_enable) {
		result = inv_i2c_single_write(st, reg->pwr_mgmt_1, data);
	} else {
		if (power_on)
			result = inv_i2c_single_write(st, reg->pwr_mgmt_1, data);
		else
			result = 0;
	}

	if (result)
		return result;

	if (power_on)
		mdelay(5);

	st->chip_config.is_asleep = !power_on;

	return 0;
}

/**
 *  inv_init_config() - Initialize hardware, disable FIFO.
 *  @indio_dev:	Device driver instance.
 *  Initial configuration:
 *  FSR: +/- 2000DPS
 *  DLPF: 42Hz
 *  FIFO rate: 50Hz
 */
static int inv_init_config(struct iio_dev *indio_dev)
{
	struct inv_reg_map_s *reg;
	int result, i;
	struct inv_mpu_state *st = iio_priv(indio_dev);
	const u8 *ch;
	u8 d[2];

	reg = &st->reg;

	result = inv_i2c_single_write(st, reg->gyro_config,
				INV_FSR_2000DPS << GYRO_CONFIG_FSR_SHIFT);
	if (result)
		return result;

	st->chip_config.fsr = INV_FSR_2000DPS;

	result = inv_i2c_single_write(st, reg->lpf, INV_FILTER_42HZ);
	if (result)
		return result;
	st->chip_config.lpf = INV_FILTER_42HZ;

	result = inv_i2c_single_write(st, reg->sample_rate_div,
					ONE_K_HZ / INIT_FIFO_RATE - 1);
	if (result)
		return result;
	st->chip_config.fifo_rate = INIT_FIFO_RATE;
	st->irq_dur_ns            = INIT_DUR_TIME;
	st->chip_config.prog_start_addr = DMP_START_ADDR;
	if (INV_MPU6050 == st->chip_type)
		st->self_test.samples = INIT_ST_MPU6050_SAMPLES;
	else
		st->self_test.samples = INIT_ST_SAMPLES;
	st->self_test.threshold = INIT_ST_THRESHOLD;
	st->batch.wake_fifo_on = true;
	st->suspend_state = false;
	if (INV_ITG3500 != st->chip_type) {
		st->chip_config.accel_fs = INV_FS_04G;
		result = inv_i2c_single_write(st, reg->accel_config,
			(INV_FS_04G << ACCEL_CONFIG_FSR_SHIFT));
		if (result)
			return result;

		result = inv_i2c_single_write(st, reg->accel_config2,INV_FILTER_42HZ);
		if (result)
			return result;

		st->tap.time = INIT_TAP_TIME;
		st->tap.thresh = INIT_TAP_THRESHOLD;
		st->tap.min_count = INIT_TAP_MIN_COUNT;
		st->sample_divider = INIT_SAMPLE_DIVIDER;
		st->smd.threshold = MPU_INIT_SMD_THLD;
		st->smd.delay     = MPU_INIT_SMD_DELAY_THLD;
		st->smd.delay2    = MPU_INIT_SMD_DELAY2_THLD;
		st->ped.int_thresh = INIT_PED_INT_THRESH;
		st->ped.step_thresh = INIT_PED_THRESH;
		st->sensor[SENSOR_STEP].rate = MAX_DMP_OUTPUT_RATE;

		st->lcd_pos.en = false;
		st->lcd_pos.time = 240;
		st->lcd_pos.up_x = 153;
		st->lcd_pos.up_y = 153;
		st->lcd_pos.up_z = 867;
		st->lcd_pos.down_x = 153;
		st->lcd_pos.down_y = 153;
		st->lcd_pos.down_z = -867;

		st->qshot.start_angle = INIT_QSHOT_START_ANGLE;
		st->qshot.finish_angle = INIT_QSHOT_FINISH_ANGLE;
		result = inv_i2c_single_write(st, REG_ACCEL_MOT_DUR,
						INIT_MOT_DUR);
		if (result)
			return result;
		st->mot_int.mot_dur = INIT_MOT_DUR;

		result = inv_i2c_single_write(st, REG_ACCEL_MOT_THR,
						INIT_MOT_THR);
		if (result)
			return result;
		st->mot_int.mot_thr = INIT_MOT_THR;

		for (i = 0; i < 3; i++) {
			result = inv_i2c_read(st, reg_gyro_offset[i], 2, d);
			if (result)
				return result;
			st->rom_gyro_offset[i] =
					(short)be16_to_cpup((__be16 *)(d));
			st->input_gyro_offset[i] = 0;
			st->input_gyro_dmp_bias[i] = 0;
		}
		if (INV_MPU6050 == st->chip_type)
			ch = reg_6050_accel_offset;
		else
			ch = reg_6500_accel_offset;
		for (i = 0; i < 3; i++) {
			result = inv_i2c_read(st, ch[i], 2, d);
			if (result)
				return result;
			st->rom_accel_offset[i] =
					(short)be16_to_cpup((__be16 *)(d));
			st->input_accel_offset[i] = 0;
			st->input_accel_dmp_bias[i] = 0;
		}
		st->ped.step = 0;
		st->ped.time = 0;
	}

	return 0;
}

/*
 *  inv_write_fsr() - Configure the gyro's scale range.
 */
static int inv_write_fsr(struct inv_mpu_state *st, int fsr)
{
	struct inv_reg_map_s *reg;
	int result;

	reg = &st->reg;
	if ((fsr < 0) || (fsr > MAX_GYRO_FS_PARAM))
		return -EINVAL;
	if (fsr == st->chip_config.fsr)
		return 0;

	if (INV_MPU3050 == st->chip_type)
		result = inv_i2c_single_write(st, reg->lpf,
			(fsr << GYRO_CONFIG_FSR_SHIFT) | st->chip_config.lpf);
	else
		result = inv_i2c_single_write(st, reg->gyro_config,
			fsr << GYRO_CONFIG_FSR_SHIFT);

	if (result)
		return result;
	st->chip_config.fsr = fsr;

	return 0;
}

/*
 *  inv_write_accel_fs() - Configure the accelerometer's scale range.
 */
static int inv_write_accel_fs(struct inv_mpu_state *st, int fs)
{
	int result;
	struct inv_reg_map_s *reg;

	reg = &st->reg;
	if (fs < 0 || fs > MAX_ACCEL_FS_PARAM)
		return -EINVAL;
	if (fs == st->chip_config.accel_fs)
		return 0;
	if (INV_MPU3050 == st->chip_type)
		result = st->slave_accel->set_fs(st, fs);
	else
		result = inv_i2c_single_write(st, reg->accel_config,
				(fs << ACCEL_CONFIG_FSR_SHIFT));
	if (result)
		return result;

	st->chip_config.accel_fs = fs;

	return 0;
}

static int inv_set_offset_reg(struct inv_mpu_state *st, int reg, int val)
{
	int result;
	u8 d;

	d = ((val >> 8) & 0xff);
	result = inv_i2c_single_write(st, reg, d);
	if (result)
		return result;

	d = (val & 0xff);
	result = inv_i2c_single_write(st, reg + 1, d);

	return result;
}

int inv_reset_offset_reg(struct inv_mpu_state *st, bool en)
{
	const u8 *ch;
	int i, result;
	s16 gyro[3], accel[3];

	if (en) {
		for (i = 0; i < 3; i++) {
			gyro[i] = st->rom_gyro_offset[i];
			accel[i] = st->rom_accel_offset[i];
		}
	} else {
		for (i = 0; i < 3; i++) {
			gyro[i] = st->rom_gyro_offset[i] +
						st->input_gyro_offset[i];
			accel[i] = st->rom_accel_offset[i] +
					(st->input_accel_offset[i] << 1);
		}
	}
	if (INV_MPU6050 == st->chip_type)
		ch = reg_6050_accel_offset;
	else
		ch = reg_6500_accel_offset;

	for (i = 0; i < 3; i++) {
		result = inv_set_offset_reg(st, reg_gyro_offset[i], gyro[i]);
		if (result)
			return result;
		result = inv_set_offset_reg(st, ch[i], accel[i]);
		if (result)
			return result;
	}

	return 0;
}
/*
 *  inv_fifo_rate_store() - Set fifo rate.
 */
static int inv_fifo_rate_store(struct inv_mpu_state *st, int fifo_rate)
{
	if ((fifo_rate < MIN_FIFO_RATE) || (fifo_rate > MAX_FIFO_RATE))
		return -EINVAL;
	if (fifo_rate == st->chip_config.fifo_rate)
		return 0;

	st->irq_dur_ns = NSEC_PER_SEC / fifo_rate;
	st->chip_config.fifo_rate = fifo_rate;

	return 0;
}

/*
 *  inv_reg_dump_show() - Register dump for testing.
 */
static ssize_t inv_reg_dump_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int ii;
	char data;
	ssize_t bytes_printed = 0;
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);

	mutex_lock(&indio_dev->mlock);
	for (ii = 0; ii < st->hw->num_reg; ii++) {
		/* don't read fifo r/w register */
		if (ii == st->reg.fifo_r_w)
			data = 0;
		else
			inv_i2c_read(st, ii, 1, &data);
		bytes_printed += sprintf(buf + bytes_printed, "%#2x: %#2x\n",
					 ii, data);
	}
	mutex_unlock(&indio_dev->mlock);

	return bytes_printed;
}

int write_be32_key_to_mem(struct inv_mpu_state *st,
					u32 data, int key)
{
	cpu_to_be32s(&data);
	return mem_w_key(key, sizeof(data), (u8 *)&data);
}

int inv_write_2bytes(struct inv_mpu_state *st, int k, int data)
{
	u8 d[2];

	if (data < 0 || data > USHRT_MAX)
		return -EINVAL;

	d[0] = (u8)((data >> 8) & 0xff);
	d[1] = (u8)(data & 0xff);

	return mem_w_key(k, ARRAY_SIZE(d), d);
}

static ssize_t _dmp_bias_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result, data, tmp;

	if (!st->chip_config.firmware_loaded)
		return -EINVAL;

	if (!st->chip_config.enable) {
		result = st->set_power_state(st, true);
		if (result)
			return result;
	}

	result = kstrtoint(buf, 10, &data);
	if (result)
		goto dmp_bias_store_fail;
	switch (this_attr->address) {
	case ATTR_DMP_ACCEL_X_DMP_BIAS:
		tmp = st->input_accel_dmp_bias[0];
		st->input_accel_dmp_bias[0] = data;
		result = inv_set_accel_bias_dmp(st);
		if (result)
			st->input_accel_dmp_bias[0] = tmp;
		break;
	case ATTR_DMP_ACCEL_Y_DMP_BIAS:
		tmp = st->input_accel_dmp_bias[1];
		st->input_accel_dmp_bias[1] = data;
		result = inv_set_accel_bias_dmp(st);
		if (result)
			st->input_accel_dmp_bias[1] = tmp;
		break;
	case ATTR_DMP_ACCEL_Z_DMP_BIAS:
		tmp = st->input_accel_dmp_bias[2];
		st->input_accel_dmp_bias[2] = data;
		result = inv_set_accel_bias_dmp(st);
		if (result)
			st->input_accel_dmp_bias[2] = tmp;
		break;
	case ATTR_DMP_GYRO_X_DMP_BIAS:
		result = write_be32_key_to_mem(st, data,
					KEY_CFG_EXT_GYRO_BIAS_X);
		if (result)
			goto dmp_bias_store_fail;
		st->input_gyro_dmp_bias[0] = data;
		break;
	case ATTR_DMP_GYRO_Y_DMP_BIAS:
		result = write_be32_key_to_mem(st, data,
					KEY_CFG_EXT_GYRO_BIAS_Y);
		if (result)
			goto dmp_bias_store_fail;
		st->input_gyro_dmp_bias[1] = data;
		break;
	case ATTR_DMP_GYRO_Z_DMP_BIAS:
		result = write_be32_key_to_mem(st, data,
					KEY_CFG_EXT_GYRO_BIAS_Z);
		if (result)
			goto dmp_bias_store_fail;
		st->input_gyro_dmp_bias[2] = data;
		break;
	default:
		break;
	}

dmp_bias_store_fail:
	if (!st->chip_config.enable)
		result |= st->set_power_state(st, false);
	if (result)
		return result;

	return count;
}

static ssize_t inv_dmp_bias_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	int result;

	mutex_lock(&indio_dev->mlock);
	result = _dmp_bias_store(dev, attr, buf, count);
	mutex_unlock(&indio_dev->mlock);

	return result;
}

static ssize_t _dmp_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result, data;

	if (st->chip_config.enable)
		return -EBUSY;
	result = kstrtoint(buf, 10, &data);
	if (result)
		return -EINVAL;
	switch (this_attr->address) {
	/* power of chip is not turned on */
	case ATTR_DMP_ON:
		st->chip_config.dmp_on = !!data;
		break;
	case ATTR_DMP_INT_ON:
		st->chip_config.dmp_int_on = !!data;
		break;
	case ATTR_DMP_EVENT_INT_ON:
		st->chip_config.dmp_event_int_on = !!data;
		break;
	case ATTR_DMP_STEP_INDICATOR_ON:
		st->chip_config.step_indicator_on = !!data;
		break;
	case ATTR_DMP_BATCHMODE_TIMEOUT:
		if (data < 0 || data > INT_MAX)
			return -EINVAL;
		st->batch.timeout = data;
		break;
	case ATTR_DMP_BATCHMODE_WAKE_FIFO_FULL:
		st->batch.wake_fifo_on = !!data;
		st->batch.overflow_on = 0;
		break;
	case ATTR_DMP_SIX_Q_ON:
		st->sensor[SENSOR_SIXQ].on = !!data;
		break;
	case ATTR_DMP_SIX_Q_RATE:
		if (data > MPU_DEFAULT_DMP_FREQ || data < 0)
			return -EINVAL;
		st->sensor[SENSOR_SIXQ].rate = data;
		st->sensor[SENSOR_SIXQ].dur = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_SIXQ].dur *= DMP_INTERVAL_INIT;
		break;
	case ATTR_DMP_LPQ_ON:
		st->sensor[SENSOR_LPQ].on = !!data;
		break;
	case ATTR_DMP_LPQ_RATE:
		if (data > MPU_DEFAULT_DMP_FREQ || data < 0)
			return -EINVAL;
		st->sensor[SENSOR_LPQ].rate = data;
		st->sensor[SENSOR_LPQ].dur = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_LPQ].dur *= DMP_INTERVAL_INIT;
		break;
	case ATTR_DMP_PED_Q_ON:
		st->sensor[SENSOR_PEDQ].on = !!data;
		break;
	case ATTR_DMP_PED_Q_RATE:
		if (data > MPU_DEFAULT_DMP_FREQ || data < 0)
			return -EINVAL;
		st->sensor[SENSOR_PEDQ].rate = data;
		st->sensor[SENSOR_PEDQ].dur = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_PEDQ].dur *= DMP_INTERVAL_INIT;
		break;
	case ATTR_DMP_STEP_DETECTOR_ON:
		st->sensor[SENSOR_STEP].on = !!data;
		break;
	default:
		return -EINVAL;
	}

	return count;
}

/*
 * inv_dmp_attr_store() -  calling this function will store DMP attributes
 */
static ssize_t inv_dmp_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	int result;

	mutex_lock(&indio_dev->mlock);
	result = _dmp_attr_store(dev, attr, buf, count);
	mutex_unlock(&indio_dev->mlock);

	return result;
}

static int inv_write_mg_shift_15(struct inv_mpu_state *st, int data, int k)
{
	int d;

	d = data * 16384 / 1000;
	d *= (1 << 15);

	return write_be32_key_to_mem(st, d, k);
}

static int inv_enable_lcd_pos(struct inv_mpu_state *st, bool on)
{
	u16 r_int_on = 0xf441;
	u16 r_int_off = 0xf1f1;
	int d, result;

	if (on)
		d = r_int_on;
	else
		d = r_int_off;

	result = inv_write_2bytes(st, KEY_CFG_LCD_UP_DOWN_INT, d);
	if (result)
		return result;
	result = inv_write_2bytes(st, KEY_LCD_UP_DOWN_ENABLE, on);

	return result;
}


static ssize_t _dmp_shealth_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result, data;

	if (!st->chip_config.firmware_loaded)
		return -EINVAL;

	result = kstrtoint(buf, 10, &data);
	if (result)
		goto dmp_mem_store_fail;

	switch (this_attr->address) {
	case ATTR_DMP_SHEALTH_ENABLE:
	{
		if(!!st->ped.on)
			result = inv_enable_shealth(st, !!data, true);

		if (result)
			goto dmp_mem_store_fail;
	}
	break;

	case ATTR_DMP_SHEALTH_INTERRUPT_PERIOD:
	{
		result = inv_set_shealth_interrupt_period(st, (s16)data);
		if (result)
			goto dmp_mem_store_fail;

		st->shealth.interrupt_duration = (s16) data;
	}
	break;

	case ATTR_DMP_SHEALTH_FREQ_THRESHOLD:
	{
		result = inv_set_shealth_walk_run_thresh(st, data);
		if (result)
			goto dmp_mem_store_fail;
	}
	break;

	case ATTR_DMP_SHEALTH_TIMER:
		result = inv_set_shealth_update_timer(st, (s16)data);
		break;

	default:
		result = -EINVAL;
		goto dmp_mem_store_fail;
	}

dmp_mem_store_fail:
	if (result)
		return result;

	return count;
}

static ssize_t _dmp_mem_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result, data;
	u8 sc_buf[IIO_BUFFER_BYTES];
	u16 hdr;

	if (st->chip_config.enable)
		return -EBUSY;
	if (!st->chip_config.firmware_loaded)
		return -EINVAL;
	result = st->set_power_state(st, true);
	if (result)
		return result;

	result = kstrtoint(buf, 10, &data);
	if (result)
		goto dmp_mem_store_fail;

	switch (this_attr->address) {
	case ATTR_DMP_PED_INT_ON:
		result = inv_enable_pedometer_interrupt(st, !!data);
		if (result)
			goto dmp_mem_store_fail;
		st->ped.int_on = !!data;
		break;
	case ATTR_DMP_PED_ON:
	{
		result = inv_enable_pedometer(st, !!data);

		/*reset internal pedometer step buffer*/
		if(!!data) {
			result = inv_reset_pedometer_internal_timer(st);
			// Sending dummy data to begin polling at HAL
			hdr = STEP_COUNTER_HDR;
			memcpy(sc_buf, &hdr, sizeof(hdr));
			mutex_lock(&st->iio_buf_write_lock);
			iio_push_to_buffers(indio_dev, sc_buf);
			mutex_unlock(&st->iio_buf_write_lock);
			pr_info("step counter dummy data sent\n");
		} else
			st->shealth.interrupt_mask = 0;

		if (result)
			goto dmp_mem_store_fail;

		/*turn off as default*/
		result = inv_enable_shealth(st, false, false);
		if (result)
			goto dmp_mem_store_fail;

		st->ped.on = !!data;

		/*change threshold to 2.5Hz*/
		inv_set_shealth_walk_run_thresh(st, 2500);
		inv_set_pedometer_step_threshold(st, PEDO_INIT_STEP_THRESHOLD);
		break;
	}

	case ATTR_DMP_PED_PEAK_THRESH:
	{
		result = write_be32_key_to_mem(st, KEY_D_PEDSTD_PEAKTHRSH,data);
		if (result)
			goto dmp_mem_store_fail;
		st->ped.step_peak_thresh = data;
		break;
	}

	case ATTR_DMP_PED_STEP_THRESH_TIME:
	{
		result = inv_write_2bytes(st,  KEY_D_PEDSTD_SB_TIME, data);
		if (result)
			goto dmp_mem_store_fail;
		st->ped.step_thresh_time = data;
		break;
	}

	case ATTR_DMP_PED_STEP_THRESH:
	{
		result = inv_write_2bytes(st, KEY_D_PEDSTD_SB, data);
		if (result)
			goto dmp_mem_store_fail;
		st->ped.step_thresh = data;
		break;
	}
	case ATTR_DMP_PED_INT_THRESH:
	{
		result = inv_write_2bytes(st, KEY_D_PEDSTD_SB2, data);
		if (result)
			goto dmp_mem_store_fail;
		st->ped.int_thresh = data;
		break;
	}
	case ATTR_DMP_SMD_ENABLE:
		result = inv_write_2bytes(st, KEY_SMD_ENABLE, !!data);
		if (result)
			goto dmp_mem_store_fail;
		st->chip_config.smd_enable = !!data;
		break;
	case ATTR_DMP_SMD_THLD:
		if (data < 0 || data > SHRT_MAX)
			goto dmp_mem_store_fail;
		result = write_be32_key_to_mem(st, data << 16,
						KEY_SMD_ACCEL_THLD);
		if (result)
			goto dmp_mem_store_fail;
		st->smd.threshold = data;
		break;
	case ATTR_DMP_SMD_DELAY_THLD:
		if (data < 0 || data > INT_MAX / MPU_DEFAULT_DMP_FREQ)
			goto dmp_mem_store_fail;
		result = write_be32_key_to_mem(st, data * MPU_DEFAULT_DMP_FREQ,
						KEY_SMD_DELAY_THLD);
		if (result)
			goto dmp_mem_store_fail;
		st->smd.delay = data;
		break;
	case ATTR_DMP_SMD_DELAY_THLD2:
		if (data < 0 || data > INT_MAX / MPU_DEFAULT_DMP_FREQ)
			goto dmp_mem_store_fail;
		result = write_be32_key_to_mem(st, data * MPU_DEFAULT_DMP_FREQ,
						KEY_SMD_DELAY2_THLD);
		if (result)
			goto dmp_mem_store_fail;
		st->smd.delay2 = data;
		break;

	case ATTR_DMP_LCD_POS_ENABLE:
		result = inv_enable_lcd_pos(st, !!data);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.en = !!data;
		break;
	case ATTR_DMP_LCD_POS_TIME_THRESH:
		result = write_be32_key_to_mem(st, data / DMP_TICK_DUR,
						KEY_LCD_TIME_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.time = data;
		break;
	case ATTR_DMP_LCD_POS_UP_X_THRESH:
		result = inv_write_mg_shift_15(st, data, KEY_LCD_UP_X_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.up_x = data;
		break;
	case ATTR_DMP_LCD_POS_UP_Y_THRESH:
		result = inv_write_mg_shift_15(st, data, KEY_LCD_UP_Y_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.up_y = data;
		break;
	case ATTR_DMP_LCD_POS_UP_Z_THRESH:
		result = inv_write_mg_shift_15(st, data, KEY_LCD_UP_Z_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.up_z = data;
		break;
	case ATTR_DMP_LCD_POS_DOWN_X_THRESH:
		result = inv_write_mg_shift_15(st, data, KEY_LCD_DOWN_X_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.down_x = data;
		break;
	case ATTR_DMP_LCD_POS_DOWN_Y_THRESH:
		result = inv_write_mg_shift_15(st, data, KEY_LCD_DOWN_Y_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.down_y = data;
		break;
	case ATTR_DMP_LCD_POS_DOWN_Z_THRESH:
		result = inv_write_mg_shift_15(st, data, KEY_LCD_DOWN_Z_THRESH);
		if (result)
			goto dmp_mem_store_fail;
		st->lcd_pos.down_z = data;
		break;
	case ATTR_DMP_QSHOT_START_ANGLE:
		result = inv_set_Qshot_start_angle(st, data);
		if (result)
			goto dmp_mem_store_fail;
		st->qshot.start_angle = data;
		break;
	case ATTR_DMP_QSHOT_FINISH_ANGLE:
		result = inv_set_Qshot_finish_angle(st, data);
		if (result)
			goto dmp_mem_store_fail;
		st->qshot.finish_angle = data;
		break;
	case ATTR_DMP_QSHOT_START_INT_ENABLE:
		result = inv_enable_Qshot_start_interrupt_klp(st, !!data);
		if (result)
			goto dmp_mem_store_fail;

		st->qshot.start_int_enable = !!data;
		break;
	case ATTR_DMP_QSHOT_FINISH_INT_ENABLE:
		result = inv_enable_Qshot_finish_interrupt_klp(st, !!data);
		if (result)
			goto dmp_mem_store_fail;
		st->qshot.finish_int_enable = !!data;
		break;
	case ATTR_DMP_DISPLAY_ORIENTATION_ON:
		result = inv_set_display_orient_interrupt_dmp(st, !!data);
		if (result)
			goto dmp_mem_store_fail;
		st->chip_config.display_orient_on = !!data;
		break;
#ifdef CONFIG_INV_TESTING
	case ATTR_DEBUG_SMD_ENABLE_TESTP1:
	{
		u8 d[] = {0x42};
		result = st->set_power_state(st, true);
		if (result)
			goto dmp_mem_store_fail;
		result = mem_w_key(KEY_SMD_ENABLE_TESTPT1, ARRAY_SIZE(d), d);
		if (result)
			goto dmp_mem_store_fail;
	}
		break;
	case ATTR_DEBUG_SMD_ENABLE_TESTP2:
	{
		u8 d[] = {0x42};
		result = st->set_power_state(st, true);
		if (result)
			goto dmp_mem_store_fail;
		result = mem_w_key(KEY_SMD_ENABLE_TESTPT2, ARRAY_SIZE(d), d);
		if (result)
			goto dmp_mem_store_fail;
	}
		break;
#endif
	default:
		result = -EINVAL;
		goto dmp_mem_store_fail;
	}

dmp_mem_store_fail:
	result |= st->set_power_state(st, false);
	if (result)
		return result;

	return count;
}

static ssize_t inv_dmp_shealth_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	int result;

	mutex_lock(&indio_dev->mlock);
	result = _dmp_shealth_store(dev, attr, buf, count);
	mutex_unlock(&indio_dev->mlock);

	return result;
}

/*
 * inv_dmp_mem_store() -  calling this function will store DMP memory data
 */
static ssize_t inv_dmp_mem_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	int result;

	mutex_lock(&indio_dev->mlock);
	result = _dmp_mem_store(dev, attr, buf, count);
	mutex_unlock(&indio_dev->mlock);

	return result;
}

static void inv_shealth_timer_func(unsigned long data)
{
	struct inv_mpu_state *st = (struct inv_mpu_state *)data;

	pr_info("%s\n", __func__);

	schedule_work(&st->shealth.work);
}


static void inv_shealth_sched_work(struct work_struct *data)
{
	struct inv_mpu_state *st =
		(struct inv_mpu_state *)container_of(data,
		struct inv_mpu_state, shealth.work);
	struct iio_dev *indio_dev = (struct iio_dev *)
				i2c_get_clientdata(st->client);

	pr_info("%s\n", __func__);

	mutex_lock(&indio_dev->mlock);
	if ((st->shealth.state == SHEALTH_STAT_WALK) &&
						(!st->shealth.enabled)) {
		st->shealth.state = SHEALTH_STAT_STOP;
		st->shealth.interrupt_mask |= SHEALTH_INT_STOP_WALKING;
		complete(&st->shealth.wait);
	}
	mutex_unlock(&indio_dev->mlock);
}

static ssize_t inv_attr64_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result;
	u64 tmp;
	u32 ped;

	mutex_lock(&indio_dev->mlock);
	if (!st->chip_config.enable || !st->chip_config.dmp_on) {
		mutex_unlock(&indio_dev->mlock);
		return -EINVAL;
	}
	result = 0;
	switch (this_attr->address) {
	case ATTR_DMP_PEDOMETER_STEPS:
		result = inv_get_pedometer_steps(st, &ped);
		result |= inv_read_pedometer_counter(st);
		tmp = st->ped.step + ped;

		if (tmp != st->shealth.step_count) {
			bool needs_to_complete = false;

			pr_info("shealth step counter = %lld\n", tmp);
			if (!st->shealth.enabled) {
				st->shealth.interrupt_mask |=
						SHEALTH_INT_STEP_COUNTER;

				/*enable stop counter*/
				del_timer(&st->shealth.timer);
				st->shealth.timer.expires = jiffies + 2 * HZ;
				add_timer(&st->shealth.timer);

				needs_to_complete = true;
			}

			st->shealth.step_count = tmp;

			if (st->shealth.state == SHEALTH_STAT_STOP) {
				st->shealth.state = SHEALTH_STAT_WALK;
				if (!st->shealth.enabled) {
					st->shealth.interrupt_mask |=
						SHEALTH_INT_START_WALKING;
					needs_to_complete = true;
				}
			}

			if (needs_to_complete)
				complete(&st->shealth.wait);


		}
		break;
	case ATTR_DMP_PEDOMETER_TIME:
		result = inv_get_pedometer_time(st, &ped);
		tmp = (u64)st->ped.time + ((u64)ped) * MS_PER_PED_TICKS;
		break;
	case ATTR_DMP_PEDOMETER_COUNTER:
		tmp = st->ped.last_step_time;
		break;
	default:
		result = -EINVAL;
		break;
	}
	mutex_unlock(&indio_dev->mlock);
	if (result)
		return -EINVAL;
	return sprintf(buf, "%lld\n", tmp);
}

static ssize_t inv_attr64_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result;
	u64 data;

	mutex_lock(&indio_dev->mlock);
	if (st->chip_config.enable || (!st->chip_config.firmware_loaded)) {
		mutex_unlock(&indio_dev->mlock);
		return -EINVAL;
	}
	result = st->set_power_state(st, true);
	if (result) {
		mutex_unlock(&indio_dev->mlock);
		return result;
	}
	result = kstrtoull(buf, 10, &data);
	if (result)
		goto attr64_store_fail;
	switch (this_attr->address) {
	case ATTR_DMP_PEDOMETER_STEPS:
		result = write_be32_key_to_mem(st, 0, KEY_D_PEDSTD_STEPCTR);
		if (result)
			goto attr64_store_fail;
		st->ped.step = data;
		break;
	case ATTR_DMP_PEDOMETER_TIME:
		result = write_be32_key_to_mem(st, 0, KEY_D_PEDSTD_TIMECTR);
		if (result)
			goto attr64_store_fail;
		st->ped.time = data;
		break;
	default:
		result = -EINVAL;
		break;
	}
attr64_store_fail:
	mutex_unlock(&indio_dev->mlock);
	result = st->set_power_state(st, false);
	if (result)
		return result;

	return count;
}
/*
 * inv_attr_show() -  calling this function will show current
 *                        dmp parameters.
 */
static ssize_t inv_attr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int result, axis;
	s8 *m;

	switch (this_attr->address) {
	case ATTR_GYRO_SCALE:
	{
		const s16 gyro_scale[] = {250, 500, 1000, 2000};

		return sprintf(buf, "%d\n", gyro_scale[st->chip_config.fsr]);
	}
	case ATTR_ACCEL_SCALE:
	{
		const s16 accel_scale[] = {2, 4, 8, 16};
		return sprintf(buf, "%d\n",
					accel_scale[st->chip_config.accel_fs] *
					st->chip_info.multi);
	}
	case ATTR_COMPASS_SCALE:
		st->slave_compass->get_scale(st, &result);

		return sprintf(buf, "%d\n", result);
	case ATTR_ACCEL_X_CALIBBIAS:
	case ATTR_ACCEL_Y_CALIBBIAS:
	case ATTR_ACCEL_Z_CALIBBIAS:
		axis = this_attr->address - ATTR_ACCEL_X_CALIBBIAS;
		return sprintf(buf, "%d\n", st->accel_bias[axis] *
						st->chip_info.multi);
	case ATTR_GYRO_X_CALIBBIAS:
	case ATTR_GYRO_Y_CALIBBIAS:
	case ATTR_GYRO_Z_CALIBBIAS:
		axis = this_attr->address - ATTR_GYRO_X_CALIBBIAS;
		return sprintf(buf, "%d\n", st->gyro_bias[axis]);
	case ATTR_SELF_TEST_GYRO_SCALE:
		return sprintf(buf, "%d\n", SELF_TEST_GYRO_FULL_SCALE);
	case ATTR_SELF_TEST_ACCEL_SCALE:
		if (INV_MPU6500 == st->chip_type)
			return sprintf(buf, "%d\n", SELF_TEST_ACCEL_6500_SCALE);
		else
			return sprintf(buf, "%d\n", SELF_TEST_ACCEL_FULL_SCALE);
	case ATTR_GYRO_X_OFFSET:
	case ATTR_GYRO_Y_OFFSET:
	case ATTR_GYRO_Z_OFFSET:
		axis = this_attr->address - ATTR_GYRO_X_OFFSET;
		return sprintf(buf, "%d\n", st->input_gyro_offset[axis]);
	case ATTR_ACCEL_X_OFFSET:
	case ATTR_ACCEL_Y_OFFSET:
	case ATTR_ACCEL_Z_OFFSET:
		axis = this_attr->address - ATTR_ACCEL_X_OFFSET;
		return sprintf(buf, "%d\n", st->input_accel_offset[axis]);
	case ATTR_DMP_ACCEL_X_DMP_BIAS:
		return sprintf(buf, "%d\n", st->input_accel_dmp_bias[0]);
	case ATTR_DMP_ACCEL_Y_DMP_BIAS:
		return sprintf(buf, "%d\n", st->input_accel_dmp_bias[1]);
	case ATTR_DMP_ACCEL_Z_DMP_BIAS:
		return sprintf(buf, "%d\n", st->input_accel_dmp_bias[2]);
	case ATTR_DMP_GYRO_X_DMP_BIAS:
		return sprintf(buf, "%d\n", st->input_gyro_dmp_bias[0]);
	case ATTR_DMP_GYRO_Y_DMP_BIAS:
		return sprintf(buf, "%d\n", st->input_gyro_dmp_bias[1]);
	case ATTR_DMP_GYRO_Z_DMP_BIAS:
		return sprintf(buf, "%d\n", st->input_gyro_dmp_bias[2]);
	case ATTR_DMP_PED_INT_ON:
		return sprintf(buf, "%d\n", st->ped.int_on);
	case ATTR_DMP_PED_ON:
		return sprintf(buf, "%d\n", st->ped.on);
	case ATTR_DMP_PED_PEAK_THRESH:
		return sprintf(buf, "%d\n", st->ped.step_peak_thresh);
	case ATTR_DMP_PED_STEP_THRESH_TIME:
		return sprintf(buf, "%d\n", st->ped.step_thresh_time);
	case ATTR_DMP_PED_STEP_THRESH:
		return sprintf(buf, "%d\n", st->ped.step_thresh);
	case ATTR_DMP_PED_INT_THRESH:
		return sprintf(buf, "%d\n", st->ped.int_thresh);
	case ATTR_DMP_SMD_ENABLE:
		return sprintf(buf, "%d\n", st->chip_config.smd_enable);
	case ATTR_DMP_SMD_THLD:
		return sprintf(buf, "%d\n", st->smd.threshold);
	case ATTR_DMP_SMD_DELAY_THLD:
		return sprintf(buf, "%d\n", st->smd.delay);
	case ATTR_DMP_SMD_DELAY_THLD2:
		return sprintf(buf, "%d\n", st->smd.delay2);
	case ATTR_DMP_TAP_ON:
		return sprintf(buf, "%d\n", st->tap.on);
	case ATTR_DMP_TAP_THRESHOLD:
		return sprintf(buf, "%d\n", st->tap.thresh);
	case ATTR_DMP_TAP_MIN_COUNT:
		return sprintf(buf, "%d\n", st->tap.min_count);
	case ATTR_DMP_TAP_TIME:
		return sprintf(buf, "%d\n", st->tap.time);
	case ATTR_DMP_LCD_POS_ENABLE:
		return sprintf(buf, "%d\n", st->lcd_pos.en);
	case ATTR_DMP_LCD_POS_TIME_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.time);
	case ATTR_DMP_LCD_POS_UP_X_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.up_x);
	case ATTR_DMP_LCD_POS_UP_Y_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.up_y);
	case ATTR_DMP_LCD_POS_UP_Z_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.up_z);
	case ATTR_DMP_LCD_POS_DOWN_X_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.down_x);
	case ATTR_DMP_LCD_POS_DOWN_Y_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.down_y);
	case ATTR_DMP_LCD_POS_DOWN_Z_THRESH:
		return sprintf(buf, "%d\n", st->lcd_pos.down_z);
	case ATTR_DMP_QSHOT_START_ANGLE:
		return sprintf(buf, "%d\n", st->qshot.start_angle);
	case ATTR_DMP_QSHOT_FINISH_ANGLE:
		return sprintf(buf, "%d\n", st->qshot.finish_angle);
	case ATTR_DMP_QSHOT_START_INT_ENABLE:
		return sprintf(buf, "%d\n", st->qshot.start_int_enable);
	case ATTR_DMP_QSHOT_FINISH_INT_ENABLE:
		return sprintf(buf, "%d\n", st->qshot.finish_int_enable);
	case ATTR_DMP_DISPLAY_ORIENTATION_ON:
		return sprintf(buf, "%d\n",
			st->chip_config.display_orient_on);
	case ATTR_DMP_ON:
		return sprintf(buf, "%d\n", st->chip_config.dmp_on);
	case ATTR_DMP_INT_ON:
		return sprintf(buf, "%d\n", st->chip_config.dmp_int_on);
	case ATTR_DMP_EVENT_INT_ON:
		return sprintf(buf, "%d\n", st->chip_config.dmp_event_int_on);
	case ATTR_DMP_STEP_INDICATOR_ON:
		return sprintf(buf, "%d\n", st->chip_config.step_indicator_on);
	case ATTR_DMP_BATCHMODE_TIMEOUT:
		return sprintf(buf, "%d\n",
				st->batch.timeout);
	case ATTR_DMP_BATCHMODE_WAKE_FIFO_FULL:
		return sprintf(buf, "%d\n",
				st->batch.wake_fifo_on);
	case ATTR_DMP_SIX_Q_ON:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_SIXQ].on);
	case ATTR_DMP_SIX_Q_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_SIXQ].rate);
	case ATTR_DMP_LPQ_ON:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_LPQ].on);
	case ATTR_DMP_LPQ_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_LPQ].rate);
	case ATTR_DMP_PED_Q_ON:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_PEDQ].on);
	case ATTR_DMP_PED_Q_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_PEDQ].rate);
	case ATTR_DMP_STEP_DETECTOR_ON:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_STEP].on);
	case ATTR_MOTION_LPA_ON:
		return sprintf(buf, "%d\n", st->mot_int.mot_on);
	case ATTR_MOTION_LPA_FREQ:{
		const char *f[] = {"1.25", "5", "20", "40"};
		return sprintf(buf, "%s\n", f[st->chip_config.lpa_freq]);
	}
	case ATTR_MOTION_LPA_THRESHOLD:
		return sprintf(buf, "%d\n", st->mot_int.mot_thr);

	case ATTR_SELF_TEST_SAMPLES:
		return sprintf(buf, "%d\n", st->self_test.samples);
	case ATTR_SELF_TEST_THRESHOLD:
		return sprintf(buf, "%d\n", st->self_test.threshold);
	case ATTR_GYRO_ENABLE:
		return sprintf(buf, "%d\n", st->chip_config.gyro_enable);
	case ATTR_GYRO_FIFO_ENABLE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_GYRO].on);
	case ATTR_GYRO_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_GYRO].rate);
	case ATTR_ACCEL_ENABLE:
		return sprintf(buf, "%d\n", st->chip_config.accel_enable);
	case ATTR_ACCEL_FIFO_ENABLE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_ACCEL].on);
	case ATTR_ACCEL_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_ACCEL].rate);
	case ATTR_COMPASS_ENABLE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_COMPASS].on);
	case ATTR_COMPASS_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_COMPASS].rate);
	case ATTR_PRESSURE_ENABLE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_PRESSURE].on);
	case ATTR_PRESSURE_RATE:
		return sprintf(buf, "%d\n", st->sensor[SENSOR_PRESSURE].rate);
	case ATTR_POWER_STATE:
		return sprintf(buf, "%d\n", !fake_asleep);
	case ATTR_FIRMWARE_LOADED:
		return sprintf(buf, "%d\n", st->chip_config.firmware_loaded);
	case ATTR_SAMPLING_FREQ:
		return sprintf(buf, "%d\n", st->chip_config.fifo_rate);
	case ATTR_SELF_TEST:
		mutex_lock(&indio_dev->mlock);
		if (st->chip_config.enable) {
			mutex_unlock(&indio_dev->mlock);
			return -EBUSY;
		}
		if (INV_MPU3050 == st->chip_type)
			result = 1;
		else
			result = inv_hw_self_test(st);
		mutex_unlock(&indio_dev->mlock);
		return sprintf(buf, "%d\n", result);
	case ATTR_GYRO_MATRIX:
		m = st->plat_data.orientation;
		return sprintf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
	case ATTR_ACCEL_MATRIX:
		if (st->plat_data.sec_slave_type ==
						SECONDARY_SLAVE_TYPE_ACCEL)
			m =
			st->plat_data.secondary_orientation;
		else
			m = st->plat_data.orientation;
		return sprintf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
	case ATTR_COMPASS_MATRIX:
		if (st->plat_data.sec_slave_type ==
				SECONDARY_SLAVE_TYPE_COMPASS)
			m =
			st->plat_data.secondary_orientation;
		else
			return -ENODEV;
		return sprintf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
	case ATTR_SECONDARY_NAME:
	{
		const char *n[] = {"NULL", "AK8975", "AK8972", "AK8963",
					"BMA250", "MLX90399",
					"AK09911", "AK09912"};
		switch (st->plat_data.sec_slave_id) {
		case COMPASS_ID_AK8975:
			return sprintf(buf, "%s\n", n[1]);
		case COMPASS_ID_AK8972:
			return sprintf(buf, "%s\n", n[2]);
		case COMPASS_ID_AK8963:
			return sprintf(buf, "%s\n", n[3]);
		case ACCEL_ID_BMA250:
			return sprintf(buf, "%s\n", n[4]);
		case COMPASS_ID_MLX90399:
			return sprintf(buf, "%s\n", n[5]);
		case COMPASS_ID_AK09911:
			return sprintf(buf, "%s\n", n[6]);
		case COMPASS_ID_AK09912:
			return sprintf(buf, "%s\n", n[7]);
		default:
			return sprintf(buf, "%s\n", n[0]);
		}
	}
#ifdef CONFIG_INV_TESTING
	case ATTR_REG_WRITE:
		return sprintf(buf, "1\n");
	case ATTR_COMPASS_SENS:
	{
		/* these 2 conditions should never be met, since the
		   'compass_sens' sysfs entry should be hidden if the compass
		   is not an AKM */
		if (st->plat_data.sec_slave_type !=
					SECONDARY_SLAVE_TYPE_COMPASS)
			return -ENODEV;
		if (st->plat_data.sec_slave_id != COMPASS_ID_AK8975 &&
		    st->plat_data.sec_slave_id != COMPASS_ID_AK8972 &&
		    st->plat_data.sec_slave_id != COMPASS_ID_AK8963)
			return -ENODEV;
		m = st->chip_info.compass_sens;
		return sprintf(buf, "%d,%d,%d\n", m[0], m[1], m[2]);
	}
	case ATTR_DEBUG_SMD_EXE_STATE:
	{
		u8 d[2];

		result = st->set_power_state(st, true);
		mpu_memory_read(st, st->i2c_addr,
				inv_dmp_get_address(KEY_SMD_EXE_STATE), 2, d);
		return sprintf(buf, "%d\n", (short)be16_to_cpup((__be16 *)(d)));
	}
	case ATTR_DEBUG_SMD_DELAY_CNTR:
	{
		u8 d[4];

		result = st->set_power_state(st, true);
		mpu_memory_read(st, st->i2c_addr,
				inv_dmp_get_address(KEY_SMD_DELAY_CNTR), 4, d);
		return sprintf(buf, "%d\n", (int)be32_to_cpup((__be32 *)(d)));
	}
#endif
	case ATTR_DMP_SHEALTH_CADENCE:
	{
		int i = 0;
		char concat[256];
		s32 cadence = 0;
		s64 start_timestamp = 0, end_timestamp = 0;
		s64 start_time_timeofday = 0, end_time_timeofday = 0;
		bool is_data_ready = false;

		if (st->shealth.enabled || (st->shealth.stop_timestamp > 0))
			is_data_ready = true;

		if (is_data_ready) {
			if (st->shealth.start_timestamp > 0) {
				start_timestamp = st->shealth.start_timestamp;
				start_time_timeofday =
					st->shealth.start_time_timeofday;
			}

			if (st->shealth.stop_timestamp > 0) {
				end_timestamp = st->shealth.stop_timestamp;
				end_time_timeofday =
					st->shealth.stop_time_timeofday;
			} else {
				end_timestamp =
					inv_get_shealth_timestamp(st, false);
				end_time_timeofday = get_time_timeofday();
			}
		}

		/*start timestamp*/
		sprintf(concat, "%lld,", start_timestamp);
		strcat(buf, concat);

		/*interrupt or stop timestap*/
		sprintf(concat, "%lld,", end_timestamp);
		strcat(buf, concat);

		/*valid count of cadence*/
		sprintf(concat, "%d,", st->shealth.valid_count);
		strcat(buf, concat);

		for (i = 0; i < SHEALTH_CADENCE_LEN; i++) {
			if (is_data_ready)
				cadence = st->shealth.cadence[i];
			sprintf(concat, "%u,", cadence);
			strcat(buf, concat);
		}

		strcat(buf, "\n");

		pr_info("[INV] Cadence Read : %s\n", buf);

		/*set start_timestamp to interrupt_timestamp
							for next cadence data*/
		if (st->shealth.interrupt_timestamp > 0) {
			st->shealth.start_timestamp =
					st->shealth.interrupt_timestamp;
			st->shealth.start_time_timeofday =
					st->shealth.interrupt_time_timeofday;
		}

		return strlen(buf);
	}

	case ATTR_DMP_SHEALTH_ENABLE:
	{
		return sprintf(buf, "%d\n", st->shealth.enabled);
	}

	case ATTR_DMP_SHEALTH_INTERRUPT_PERIOD:
	{
		return sprintf(buf, "%d\n", st->shealth.interrupt_duration);
	}

	case ATTR_DMP_SHEALTH_INSTANT_CADENCE:
	{
		return inv_get_shealth_instant_cadence(st, buf);
	}
	case ATTR_DMP_SHEALTH_FLUSH_CADENCE:
	{
		inv_get_shealth_instant_cadence(st, buf);
		pr_info("[INV] Cadence Flush : %s\n", buf);
		inv_clear_shealth_cadence(st);
		st->shealth.start_timestamp =
					inv_get_shealth_timestamp(st, true);
		st->shealth.stop_timestamp = -1;
		st->shealth.interrupt_timestamp = -1;

		st->shealth.start_time_timeofday = get_time_timeofday();
		st->shealth.stop_time_timeofday = -1;
		st->shealth.interrupt_time_timeofday = -1;

		/* reset interrupt period */
		inv_set_shealth_interrupt_period(st,
						st->shealth.interrupt_duration);
		/* reset cadence update timer */
		inv_reset_shealth_update_timer(st);

		return strlen(buf);
	}

	case ATTR_DMP_SHEALTH_FREQ_THRESHOLD:
	{
		inv_get_shealth_walk_run_thresh(st, buf);
		pr_info("[SHEALTH:%s] Frequency threshold : %s", __func__, buf);
		return strlen(buf);
	}

	case ATTR_DMP_SHEALTH_TIMER:
	{
		inv_get_shealth_update_timer(st, buf);
		pr_info("[SHEALTH:%s] Update Timer : %s", __func__, buf);
		return strlen(buf);
	}
	default:
		return -EPERM;
	}
}

/*
 * inv_dmp_display_orient_show() -  calling this function will
 *			show orientation This event must use poll.
 */
static ssize_t inv_dmp_display_orient_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", st->display_orient_data);
}

/*
 * inv_accel_motion_show() -  calling this function showes motion interrupt.
 *                         This event must use poll.
 */
static ssize_t inv_accel_motion_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

/*
 * inv_smd_show() -  calling this function showes smd interrupt.
 *                         This event must use poll.
 */
static ssize_t inv_smd_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

/*
 * inv_ped_show() -  calling this function showes pedometer interrupt.
 *                         This event must use poll.
 */
static ssize_t inv_ped_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

/*
 * inv_lcd_pos_show() -  calling this function showes lcd position interrupt.
 *                         This event must use poll.
 */
static ssize_t inv_lcd_pos_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", st->lcd_pos.data);
}
/*
 * inv_qshot_start_show() - calling this function showes qshot start interrupt.
 *                         This event must use poll.
 */
static ssize_t inv_qshot_start_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

/*
 * inv_qshot_finish_show() - this function showes qshot finish interrupt.
 *                         This event must use poll.
 */
static ssize_t inv_qshot_finish_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "1\n");
}

static ssize_t inv_shealth_int_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st = iio_priv(dev_get_drvdata(dev));
	u16 interrupt_mask = 0;

	if(st->ped.on) {
		pr_info("%s enter\n", __func__);
		wait_for_completion_interruptible(&st->shealth.wait);

		interrupt_mask = st->shealth.interrupt_mask;
		st->shealth.interrupt_mask = 0;

		pr_info("%s exit. interrupt_mask:%d\n", __func__, interrupt_mask);
	}

	return sprintf(buf, "%d\n", interrupt_mask);
}

/*
 * inv_dmp_tap_show() -  calling this function will show tap
 *                         This event must use poll.
 */
static ssize_t inv_dmp_tap_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st = iio_priv(dev_get_drvdata(dev));
	return sprintf(buf, "%d\n", st->tap_data);
}

/*
 *  inv_temperature_show() - Read temperature data directly from registers.
 */
static ssize_t inv_temperature_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{

	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct inv_reg_map_s *reg;
	int result, cur_scale, cur_off;
	short temp;
	long scale_t;
	u8 data[2];
	const long scale[] = {3834792L, 3158064L, 3340827L};
	const long offset[] = {5383314L, 2394184L, 1376256L};

	reg = &st->reg;
	mutex_lock(&indio_dev->mlock);
	if (!st->chip_config.enable)
		result = st->set_power_state(st, true);
	else
		result = 0;
	if (result) {
		mutex_unlock(&indio_dev->mlock);
		return result;
	}
	result = inv_i2c_read(st, reg->temperature, 2, data);
	if (!st->chip_config.enable)
		result |= st->set_power_state(st, false);
	mutex_unlock(&indio_dev->mlock);
	if (result) {
		pr_err("Could not read temperature register.\n");
		return result;
	}
	temp = (signed short)(be16_to_cpup((short *)&data[0]));
	switch (st->chip_type) {
	case INV_MPU3050:
		cur_scale = scale[0];
		cur_off   = offset[0];
		break;
	case INV_MPU6050:
		cur_scale = scale[1];
		cur_off   = offset[1];
		break;
	case INV_MPU6500:
		cur_scale = scale[2];
		cur_off   = offset[2];
		break;
	default:
		return -EINVAL;
	};
	scale_t = cur_off +
		inv_q30_mult((int)temp << MPU_TEMP_SHIFT, cur_scale);

	INV_I2C_INC_TEMPREAD(1);

	return sprintf(buf, "%ld %lld\n", scale_t, get_time_ns());
}
static ssize_t inv_timestamp_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n", get_time_ns());
}

static ssize_t inv_flush_batch_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	int result;
	bool has_data = false;

	mutex_lock(&indio_dev->mlock);
	result = inv_flush_batch_data(indio_dev, &has_data);
	mutex_unlock(&indio_dev->mlock);

	if (result)
		return sprintf(buf, "%d\n", result);
	else
		return sprintf(buf, "%d\n", has_data);
}

/*
 * inv_firmware_loaded() -  calling this function will change
 *                        firmware load
 */
static int inv_firmware_loaded(struct inv_mpu_state *st, int data)
{
	if (data)
		return -EINVAL;
	st->chip_config.firmware_loaded = 0;

	return 0;
}

static int inv_switch_gyro_engine(struct inv_mpu_state *st, bool en)
{
	return inv_switch_engine(st, en, BIT_PWR_GYRO_STBY);
}

static int inv_switch_accel_engine(struct inv_mpu_state *st, bool en)
{
	return inv_switch_engine(st, en, BIT_PWR_ACCEL_STBY);
}

static ssize_t _attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int data;
	u8 d, axis;
	int result;

	result = 0;
	if (st->chip_config.enable)
		return -EBUSY;
	if (this_attr->address <= ATTR_MOTION_LPA_THRESHOLD) {
		result = st->set_power_state(st, true);
		if (result)
			return result;
	}

	/* check the input and validate it's format */
	switch (this_attr->address) {
#ifdef CONFIG_INV_TESTING
	/* these inputs are strings */
	case ATTR_COMPASS_MATRIX:
	case ATTR_COMPASS_SENS:
		break;
#endif
	/* these inputs are integers */
	default:
		result = kstrtoint(buf, 10, &data);
		if (result)
			goto attr_store_fail;
		break;
	}

	switch (this_attr->address) {
	case ATTR_GYRO_X_OFFSET:
	case ATTR_GYRO_Y_OFFSET:
	case ATTR_GYRO_Z_OFFSET:
		if ((data > MPU_MAX_G_OFFSET_VALUE) ||
				(data < MPU_MIN_G_OFFSET_VALUE))
			return -EINVAL;
		axis = this_attr->address - ATTR_GYRO_X_OFFSET;
		result = inv_set_offset_reg(st,
				reg_gyro_offset[axis],
				st->rom_gyro_offset[axis] + data);

		if (result)
			goto attr_store_fail;
		st->input_gyro_offset[axis] = data;
		break;
	case ATTR_ACCEL_X_OFFSET:
	case ATTR_ACCEL_Y_OFFSET:
	case ATTR_ACCEL_Z_OFFSET:
	{
		const u8 *ch;

		if ((data > MPU_MAX_4G_OFFSET_VALUE) ||
			(data < MPU_MIN_4G_OFFSET_VALUE))
			return -EINVAL;

		axis = this_attr->address - ATTR_ACCEL_X_OFFSET;
		if (INV_MPU6050 == st->chip_type)
			ch = reg_6050_accel_offset;
		else
			ch = reg_6500_accel_offset;

		result = inv_set_offset_reg(st, ch[axis],
			st->rom_accel_offset[axis] + (data << 1));
		if (result)
			goto attr_store_fail;
		st->input_accel_offset[axis] = data;
		break;
	}
	case ATTR_GYRO_SCALE:
		result = inv_write_fsr(st, data);
		break;
	case ATTR_ACCEL_SCALE:
		result = inv_write_accel_fs(st, data);
		break;
	case ATTR_COMPASS_SCALE:
		result = st->slave_compass->set_scale(st, data);
		break;
	case ATTR_MOTION_LPA_ON:
		if (INV_MPU6500 == st->chip_type) {
			if (data)
				/* enable and put in MPU6500 mode */
				d = BIT_ACCEL_INTEL_ENABLE
					| BIT_ACCEL_INTEL_MODE;
			else
				d = 0;
			result = inv_i2c_single_write(st,
						REG_6500_ACCEL_INTEL_CTRL, d);
			if (result)
				goto attr_store_fail;
		}
		st->mot_int.mot_on = !!data;
		st->chip_config.lpa_mode = !!data;
		break;
	case ATTR_MOTION_LPA_FREQ:
		result = inv_lpa_freq(st, data);
		break;
	case ATTR_MOTION_LPA_THRESHOLD:
		if ((data > MPU6XXX_MAX_MOTION_THRESH) || (data < 0)) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		if (INV_MPU6500 == st->chip_type) {
			d = (u8)(data >> MPU6500_MOTION_THRESH_SHIFT);
			data = (d << MPU6500_MOTION_THRESH_SHIFT);
		} else {
			d = (u8)(data >> MPU6050_MOTION_THRESH_SHIFT);
			data = (d << MPU6050_MOTION_THRESH_SHIFT);
		}

		result = inv_i2c_single_write(st, REG_ACCEL_MOT_THR, d);
		if (result)
			goto attr_store_fail;
		st->mot_int.mot_thr = data;
		break;
	/* from now on, power is not turned on */
	case ATTR_SELF_TEST_SAMPLES:
		if (data > ST_MAX_SAMPLES || data < 0) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		st->self_test.samples = data;
		break;
	case ATTR_SELF_TEST_THRESHOLD:
		if (data > ST_MAX_THRESHOLD || data < 0) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		st->self_test.threshold = data;
	case ATTR_GYRO_ENABLE:
		st->chip_config.gyro_enable = !!data;
		if (st->chip_config.gyro_enable)
			gyro_open_calibration(st);
		break;
	case ATTR_GYRO_FIFO_ENABLE:
		st->sensor[SENSOR_GYRO].on = !!data;
		break;
	case ATTR_GYRO_RATE:
		st->sensor[SENSOR_GYRO].rate = data;
		st->sensor[SENSOR_GYRO].dur  = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_GYRO].dur  *= DMP_INTERVAL_INIT;
		break;
	case ATTR_ACCEL_ENABLE:
		st->chip_config.accel_enable = !!data;
		if (st->chip_config.accel_enable)
			accel_open_calibration(st);
		break;
	case ATTR_ACCEL_FIFO_ENABLE:
		st->sensor[SENSOR_ACCEL].on = !!data;
		break;
	case ATTR_ACCEL_RATE:
		st->sensor[SENSOR_ACCEL].rate = data;
		st->sensor[SENSOR_ACCEL].dur  = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_ACCEL].dur  *= DMP_INTERVAL_INIT;
		break;
	case ATTR_COMPASS_ENABLE:
		st->sensor[SENSOR_COMPASS].on = !!data;
		break;
	case ATTR_COMPASS_RATE:
		if (data <= 0) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		if ((MSEC_PER_SEC / st->slave_compass->rate_scale) < data)
			data = MSEC_PER_SEC / st->slave_compass->rate_scale;

		st->sensor[SENSOR_COMPASS].rate = data;
		st->sensor[SENSOR_COMPASS].dur  = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_COMPASS].dur  *= DMP_INTERVAL_INIT;
		break;
	case ATTR_PRESSURE_ENABLE:
		st->sensor[SENSOR_PRESSURE].on = !!data;
		break;
	case ATTR_PRESSURE_RATE:
		if (data <= 0) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		if ((MSEC_PER_SEC / st->slave_pressure->rate_scale) < data)
			data = MSEC_PER_SEC / st->slave_pressure->rate_scale;

		st->sensor[SENSOR_PRESSURE].rate = data;
		st->sensor[SENSOR_PRESSURE].dur  = MPU_DEFAULT_DMP_FREQ / data;
		st->sensor[SENSOR_PRESSURE].dur  *= DMP_INTERVAL_INIT;
		break;
	case ATTR_POWER_STATE:
		fake_asleep = !data;
		break;
	case ATTR_FIRMWARE_LOADED:
		result = inv_firmware_loaded(st, data);
		break;
	case ATTR_SAMPLING_FREQ:
		result = inv_fifo_rate_store(st, data);
		break;
#ifdef CONFIG_INV_TESTING
	case ATTR_COMPASS_MATRIX:
	{
		char *str;
		__s8 m[9];
		d = 0;
		if (st->plat_data.sec_slave_type == SECONDARY_SLAVE_TYPE_NONE)
			return -ENODEV;
		while ((str = strsep((char **)&buf, ","))) {
			if (d >= 9) {
				result = -EINVAL;
				goto attr_store_fail;
			}
			result = kstrtoint(str, 10, &data);
			if (result)
				goto attr_store_fail;
			if (data < -1 || data > 1) {
				result = -EINVAL;
				goto attr_store_fail;
			}
			m[d] = data;
			d++;
		}
		if (d < 9) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		memcpy(st->plat_data.secondary_orientation, m, sizeof(m));
		pr_debug(KERN_INFO
			 "compass_matrix: %d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
		break;
	}
	case ATTR_COMPASS_SENS:
	{
		char *str;
		__s8 s[3];
		d = 0;
		/* these 2 conditions should never be met, since the
		   'compass_sens' sysfs entry should be hidden if the compass
		   is not an AKM */
		if (st->plat_data.sec_slave_type == SECONDARY_SLAVE_TYPE_NONE)
			return -ENODEV;
		if (st->plat_data.sec_slave_id != COMPASS_ID_AK8975 &&
		    st->plat_data.sec_slave_id != COMPASS_ID_AK8972 &&
		    st->plat_data.sec_slave_id != COMPASS_ID_AK8963)
			return -ENODEV;
		/* read the input data, expecting 3 comma separated values */
		while ((str = strsep((char **)&buf, ","))) {
			if (d >= 3) {
				result = -EINVAL;
				goto attr_store_fail;
			}
			result = kstrtoint(str, 10, &data);
			if (result)
				goto attr_store_fail;
			if (data < 0 || data > 255) {
				result = -EINVAL;
				goto attr_store_fail;
			}
			s[d] = data;
			d++;
		}
		if (d < 3) {
			result = -EINVAL;
			goto attr_store_fail;
		}
		/* store the new compass sensitivity adjustment */
		memcpy(st->chip_info.compass_sens, s, sizeof(s));
		pr_debug(KERN_INFO
			 "compass_sens: %d,%d,%d\n", s[0], s[1], s[2]);
		break;
	}
#endif
	default:
		result = -EINVAL;
		goto attr_store_fail;
	};

attr_store_fail:
	if (this_attr->address <= ATTR_MOTION_LPA_THRESHOLD)
		result |= st->set_power_state(st, false);
	if (result)
		return result;

	return count;
}

/*
 * inv_attr_store() -  calling this function will store current
 *                        non-dmp parameter settings
 */
static ssize_t inv_attr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	int result;

	mutex_lock(&indio_dev->mlock);
	result = _attr_store(dev, attr, buf, count);
	mutex_unlock(&indio_dev->mlock);

	return result;
}

static ssize_t inv_master_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	int data;
	int result;

	result = kstrtoint(buf, 10, &data);
	if (result)
		return result;

	mutex_lock(&indio_dev->mlock);
	if (st->chip_config.enable == (!!data)) {
		result = count;
		goto end_enable;
	}
	if (!!data) {
		if (st->reactive_enable) {
			st->reactive_accel_on_time = jiffies;
		}
		result = st->set_power_state(st, true);
		if (result)
			goto end_enable;
	}
	result = set_inv_enable(indio_dev, !!data);
	if (result)
		goto end_enable;
	if (!data) {
		result = st->set_power_state(st, false);
		if (result)
			goto end_enable;
	}
	result = count;

end_enable:
	mutex_unlock(&indio_dev->mlock);

	return result;
}

static ssize_t inv_master_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st = iio_priv(dev_get_drvdata(dev));

	return sprintf(buf, "%d\n", st->chip_config.enable);
}

#ifdef CONFIG_INV_TESTING
static ssize_t inv_test_suspend_resume_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int data;
	int result;

	result = kstrtoint(buf, 10, &data);
	if (result)
		return result;
	if (data)
		inv_mpu_suspend(dev);
	else
		inv_mpu_resume(dev);
	suspend_state = !!data;

	return count;
}

static ssize_t inv_test_suspend_resume_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{

	return sprintf(buf, "%d\n", suspend_state);
}

/*
 * inv_reg_write_store() - register write command for testing.
 *                         Format: WSRRDD, where RR is the register in hex,
 *                                         and DD is the data in hex.
 */
static ssize_t inv_reg_write_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	u32 result;
	u8 wreg, wval;
	int temp;
	char local_buf[10];

	if ((buf[0] != 'W' && buf[0] != 'w') ||
	    (buf[1] != 'S' && buf[1] != 's'))
		return -EINVAL;
	if (strlen(buf) < 6)
		return -EINVAL;

	strncpy(local_buf, buf, 7);
	local_buf[6] = 0;
	result = sscanf(&local_buf[4], "%x", &temp);
	if (result == 0)
		return -EINVAL;
	wval = temp;
	local_buf[4] = 0;
	sscanf(&local_buf[2], "%x", &temp);
	if (result == 0)
		return -EINVAL;
	wreg = temp;

	result = inv_i2c_single_write(st, wreg, wval);
	if (result)
		return result;

	return count;
}

static ssize_t inv_test_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int data;
	u8 *m;
	int result;

	if (st->chip_config.enable)
		return -EBUSY;
	result = kstrtoint(buf, 10, &data);
	if (result)
		return -EINVAL;

	result = st->set_power_state(st, true);
	if (result)
		return result;

	switch (this_attr->address) {
	case ATTR_DEBUG_ACCEL_COUNTER:
	{
		u8 D[6] = {0xf2, 0xb0, 0x80, 0xc0, 0xc8, 0xc2};
		u8 E[6] = {0xf3, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_01, ARRAY_SIZE(D), m);
		data_out_control.accel = !!data;
		break;
	}
	case ATTR_DEBUG_GYRO_COUNTER:
	{
		u8 D[6] = {0xf2, 0xb0, 0x80, 0xc4, 0xcc, 0xc6};
		u8 E[6] = {0xf3, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_02, ARRAY_SIZE(D), m);
		data_out_control.gyro = !!data;
		break;
	}
	case ATTR_DEBUG_COMPASS_COUNTER:
	{
		u8 D[6] = {0xf2, 0xb0, 0x81, 0xc0, 0xc8, 0xc2};
		u8 E[6] = {0xf3, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_03, ARRAY_SIZE(D), m);
		data_out_control.compass = !!data;
		break;
	}
	case ATTR_DEBUG_PRESSURE_COUNTER:
	{
		u8 D[6] = {0xf2, 0xb0, 0x81, 0xc4, 0xcc, 0xc6};
		u8 E[6] = {0xf3, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_04, ARRAY_SIZE(D), m);
		data_out_control.pressure = !!data;
		break;
	}
	case ATTR_DEBUG_LPQ_COUNTER:
	{
		u8 D[6] = {0xf1, 0xb1, 0x83, 0xc2, 0xc4, 0xc6};
		u8 E[6] = {0xf1, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_05, ARRAY_SIZE(D), m);
		data_out_control.LPQ = !!data;
		break;
	}
	case ATTR_DEBUG_SIXQ_COUNTER:
	{
		u8 D[6] = {0xf1, 0xb1, 0x89, 0xc2, 0xc4, 0xc6};
		u8 E[6] = {0xf1, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_06, ARRAY_SIZE(D), m);
		data_out_control.SIXQ = !!data;
		break;
	}
	case ATTR_DEBUG_PEDQ_COUNTER:
	{
		u8 D[6] = {0xf2, 0xf2, 0x88, 0xc2, 0xc4, 0xc6};
		u8 E[6] = {0xf3, 0xb1, 0x88, 0xc0, 0xc0, 0xc0};

		if (data)
			m = E;
		else
			m = D;
		result = mem_w_key(KEY_TEST_07, ARRAY_SIZE(D), m);
		data_out_control.PEDQ = !!data;
		break;
	}
	default:
		return -EINVAL;
	}

	return count;
}

static ssize_t inv_test_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);

	switch (this_attr->address) {
	case ATTR_DEBUG_ACCEL_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.accel);
	case ATTR_DEBUG_GYRO_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.gyro);
	case ATTR_DEBUG_COMPASS_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.compass);
	case ATTR_DEBUG_PRESSURE_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.pressure);
	case ATTR_DEBUG_LPQ_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.LPQ);
	case ATTR_DEBUG_SIXQ_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.SIXQ);
	case ATTR_DEBUG_PEDQ_COUNTER:
		return sprintf(buf, "%d\n", data_out_control.PEDQ);
	default:
		return -EINVAL;
	}
}

#endif /* CONFIG_INV_TESTING */

static const struct iio_chan_spec inv_mpu_channels[] = {
	IIO_CHAN_SOFT_TIMESTAMP(INV_MPU_SCAN_TIMESTAMP),
};

/*constant IIO attribute */
static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("10 20 50 100 200 500");

/* special sysfs */
static DEVICE_ATTR(reg_dump, S_IRUGO, inv_reg_dump_show, NULL);
static DEVICE_ATTR(temperature, S_IRUGO, inv_temperature_show, NULL);
static DEVICE_ATTR(timestamp, S_IRUGO, inv_timestamp_show, NULL);
/* event based sysfs, needs poll to read */
static DEVICE_ATTR(event_tap, S_IRUGO, inv_dmp_tap_show, NULL);
static DEVICE_ATTR(event_display_orientation, S_IRUGO,
	inv_dmp_display_orient_show, NULL);
static DEVICE_ATTR(event_accel_motion, S_IRUGO, inv_accel_motion_show, NULL);
static DEVICE_ATTR(event_smd, S_IRUGO, inv_smd_show, NULL);
static DEVICE_ATTR(event_pedometer, S_IRUGO, inv_ped_show, NULL);
static DEVICE_ATTR(event_lcd_position, S_IRUGO, inv_lcd_pos_show, NULL);
static DEVICE_ATTR(event_qshot_start, S_IRUGO, inv_qshot_start_show, NULL);
static DEVICE_ATTR(event_qshot_finish, S_IRUGO, inv_qshot_finish_show, NULL);
static DEVICE_ATTR(event_shealth_int, S_IRUGO, inv_shealth_int_show, NULL);

/* master enable method */
static DEVICE_ATTR(master_enable, S_IRUGO | S_IWUSR, inv_master_enable_show,
					inv_master_enable_store);

/* special run time sysfs entry, read only */
static DEVICE_ATTR(flush_batch, S_IRUGO, inv_flush_batch_show, NULL);

/* DMP sysfs with power on/off */
static IIO_DEVICE_ATTR(in_accel_x_dmp_bias, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_bias_store, ATTR_DMP_ACCEL_X_DMP_BIAS);
static IIO_DEVICE_ATTR(in_accel_y_dmp_bias, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_bias_store, ATTR_DMP_ACCEL_Y_DMP_BIAS);
static IIO_DEVICE_ATTR(in_accel_z_dmp_bias, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_bias_store, ATTR_DMP_ACCEL_Z_DMP_BIAS);

static IIO_DEVICE_ATTR(in_anglvel_x_dmp_bias, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_bias_store, ATTR_DMP_GYRO_X_DMP_BIAS);
static IIO_DEVICE_ATTR(in_anglvel_y_dmp_bias, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_bias_store, ATTR_DMP_GYRO_Y_DMP_BIAS);
static IIO_DEVICE_ATTR(in_anglvel_z_dmp_bias, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_bias_store, ATTR_DMP_GYRO_Z_DMP_BIAS);

static IIO_DEVICE_ATTR(pedometer_int_on, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_PED_INT_ON);
static IIO_DEVICE_ATTR(pedometer_on, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_PED_ON);

static IIO_DEVICE_ATTR(pedometer_peak_thresh, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_PED_PEAK_THRESH);
static IIO_DEVICE_ATTR(pedometer_step_thresh_time, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_PED_STEP_THRESH_TIME);


static IIO_DEVICE_ATTR(pedometer_step_thresh, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_PED_STEP_THRESH);
static IIO_DEVICE_ATTR(pedometer_int_thresh, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_PED_INT_THRESH);

static IIO_DEVICE_ATTR(smd_enable, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_SMD_ENABLE);
static IIO_DEVICE_ATTR(smd_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_SMD_THLD);
static IIO_DEVICE_ATTR(smd_delay_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_SMD_DELAY_THLD);
static IIO_DEVICE_ATTR(smd_delay_threshold2, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_SMD_DELAY_THLD2);

static IIO_DEVICE_ATTR(pedometer_steps, S_IRUGO | S_IWUSR, inv_attr64_show,
	inv_attr64_store, ATTR_DMP_PEDOMETER_STEPS);
static IIO_DEVICE_ATTR(pedometer_time, S_IRUGO | S_IWUSR, inv_attr64_show,
	inv_attr64_store, ATTR_DMP_PEDOMETER_TIME);
static IIO_DEVICE_ATTR(pedometer_counter, S_IRUGO | S_IWUSR, inv_attr64_show,
	NULL, ATTR_DMP_PEDOMETER_COUNTER);

static IIO_DEVICE_ATTR(shealth_cadence, 0660, inv_attr_show,
	NULL, ATTR_DMP_SHEALTH_CADENCE);
static IIO_DEVICE_ATTR(shealth_cadence_enable, 0660, inv_attr_show,
	inv_dmp_shealth_store, ATTR_DMP_SHEALTH_ENABLE);
static IIO_DEVICE_ATTR(shealth_int_period, 0660, inv_attr_show,
	inv_dmp_shealth_store, ATTR_DMP_SHEALTH_INTERRUPT_PERIOD);
static IIO_DEVICE_ATTR(shealth_instant_cadence, 0660,
		inv_attr_show, NULL, ATTR_DMP_SHEALTH_INSTANT_CADENCE);
static IIO_DEVICE_ATTR(shealth_flush_cadence, 0660, inv_attr_show,
	NULL, ATTR_DMP_SHEALTH_FLUSH_CADENCE);
static IIO_DEVICE_ATTR(shealth_freq_threshold, 0660, inv_attr_show,
	inv_dmp_shealth_store, ATTR_DMP_SHEALTH_FREQ_THRESHOLD);
static IIO_DEVICE_ATTR(shealth_timer, 0660, inv_attr_show,
	inv_dmp_shealth_store, ATTR_DMP_SHEALTH_TIMER);


static IIO_DEVICE_ATTR(tap_on, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_TAP_ON);
static IIO_DEVICE_ATTR(tap_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_TAP_THRESHOLD);
static IIO_DEVICE_ATTR(tap_min_count, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_TAP_MIN_COUNT);
static IIO_DEVICE_ATTR(tap_time, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_TAP_TIME);
static IIO_DEVICE_ATTR(display_orientation_on, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_DISPLAY_ORIENTATION_ON);

static IIO_DEVICE_ATTR(lcd_pos_enable, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_ENABLE);
static IIO_DEVICE_ATTR(lcd_pos_time_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_TIME_THRESH);
static IIO_DEVICE_ATTR(lcd_pos_up_x_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_UP_X_THRESH);
static IIO_DEVICE_ATTR(lcd_pos_up_y_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_UP_Y_THRESH);
static IIO_DEVICE_ATTR(lcd_pos_up_z_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_UP_Z_THRESH);
static IIO_DEVICE_ATTR(lcd_pos_down_x_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_DOWN_X_THRESH);
static IIO_DEVICE_ATTR(lcd_pos_down_y_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_DOWN_Y_THRESH);
static IIO_DEVICE_ATTR(lcd_pos_down_z_threshold, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_LCD_POS_DOWN_Z_THRESH);

static IIO_DEVICE_ATTR(qshot_start_angle, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_QSHOT_START_ANGLE);
static IIO_DEVICE_ATTR(qshot_finish_angle, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_QSHOT_FINISH_ANGLE);
static IIO_DEVICE_ATTR(qshot_start_int_enable, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_QSHOT_START_INT_ENABLE);
static IIO_DEVICE_ATTR(qshot_finish_int_enable, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_mem_store, ATTR_DMP_QSHOT_FINISH_INT_ENABLE);

/* DMP sysfs without power on/off */
static IIO_DEVICE_ATTR(dmp_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_ON);
static IIO_DEVICE_ATTR(dmp_int_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_INT_ON);
static IIO_DEVICE_ATTR(dmp_event_int_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_EVENT_INT_ON);
static IIO_DEVICE_ATTR(step_indicator_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_STEP_INDICATOR_ON);
static IIO_DEVICE_ATTR(batchmode_timeout, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_attr_store, ATTR_DMP_BATCHMODE_TIMEOUT);
static IIO_DEVICE_ATTR(batchmode_wake_fifo_full_on, S_IRUGO | S_IWUSR,
	inv_attr_show, inv_dmp_attr_store, ATTR_DMP_BATCHMODE_WAKE_FIFO_FULL);

static IIO_DEVICE_ATTR(six_axes_q_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_SIX_Q_ON);
static IIO_DEVICE_ATTR(six_axes_q_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_SIX_Q_RATE);

static IIO_DEVICE_ATTR(three_axes_q_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_LPQ_ON);
static IIO_DEVICE_ATTR(three_axes_q_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_LPQ_RATE);

static IIO_DEVICE_ATTR(ped_q_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_PED_Q_ON);
static IIO_DEVICE_ATTR(ped_q_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_PED_Q_RATE);

static IIO_DEVICE_ATTR(step_detector_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_dmp_attr_store, ATTR_DMP_STEP_DETECTOR_ON);

/* non DMP sysfs with power on/off */
static IIO_DEVICE_ATTR(motion_lpa_on, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_MOTION_LPA_ON);
static IIO_DEVICE_ATTR(motion_lpa_freq, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_MOTION_LPA_FREQ);
static IIO_DEVICE_ATTR(motion_lpa_threshold, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_MOTION_LPA_THRESHOLD);

static IIO_DEVICE_ATTR(in_accel_scale, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_SCALE);
static IIO_DEVICE_ATTR(in_anglvel_scale, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_SCALE);
static IIO_DEVICE_ATTR(in_magn_scale, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_COMPASS_SCALE);

static IIO_DEVICE_ATTR(in_anglvel_x_offset, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_X_OFFSET);
static IIO_DEVICE_ATTR(in_anglvel_y_offset, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_Y_OFFSET);
static IIO_DEVICE_ATTR(in_anglvel_z_offset, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_Z_OFFSET);

static IIO_DEVICE_ATTR(in_accel_x_offset, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_X_OFFSET);
static IIO_DEVICE_ATTR(in_accel_y_offset, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_Y_OFFSET);
static IIO_DEVICE_ATTR(in_accel_z_offset, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_Z_OFFSET);

/* non DMP sysfs without power on/off */
static IIO_DEVICE_ATTR(self_test_samples, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_SELF_TEST_SAMPLES);
static IIO_DEVICE_ATTR(self_test_threshold, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_SELF_TEST_THRESHOLD);

static IIO_DEVICE_ATTR(gyro_enable, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_ENABLE);
static IIO_DEVICE_ATTR(gyro_fifo_enable, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_FIFO_ENABLE);
static IIO_DEVICE_ATTR(gyro_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_GYRO_RATE);

static IIO_DEVICE_ATTR(accel_enable, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_ENABLE);
static IIO_DEVICE_ATTR(accel_fifo_enable, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_FIFO_ENABLE);
static IIO_DEVICE_ATTR(accel_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_ACCEL_RATE);

static IIO_DEVICE_ATTR(compass_enable, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_COMPASS_ENABLE);
static IIO_DEVICE_ATTR(compass_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_COMPASS_RATE);

static IIO_DEVICE_ATTR(pressure_enable, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_PRESSURE_ENABLE);
static IIO_DEVICE_ATTR(pressure_rate, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_PRESSURE_RATE);

static IIO_DEVICE_ATTR(power_state, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_POWER_STATE);
static IIO_DEVICE_ATTR(firmware_loaded, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_FIRMWARE_LOADED);
static IIO_DEVICE_ATTR(sampling_frequency, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_SAMPLING_FREQ);

/* show method only sysfs but with power on/off */
static IIO_DEVICE_ATTR(self_test, S_IRUGO, inv_attr_show, NULL,
	ATTR_SELF_TEST);

/* show method only sysfs */
static IIO_DEVICE_ATTR(in_accel_x_calibbias, S_IRUGO, inv_attr_show,
	NULL, ATTR_ACCEL_X_CALIBBIAS);
static IIO_DEVICE_ATTR(in_accel_y_calibbias, S_IRUGO, inv_attr_show,
	NULL, ATTR_ACCEL_Y_CALIBBIAS);
static IIO_DEVICE_ATTR(in_accel_z_calibbias, S_IRUGO, inv_attr_show,
	NULL, ATTR_ACCEL_Z_CALIBBIAS);

static IIO_DEVICE_ATTR(in_anglvel_x_calibbias, S_IRUGO,
		inv_attr_show, NULL, ATTR_GYRO_X_CALIBBIAS);
static IIO_DEVICE_ATTR(in_anglvel_y_calibbias, S_IRUGO,
		inv_attr_show, NULL, ATTR_GYRO_Y_CALIBBIAS);
static IIO_DEVICE_ATTR(in_anglvel_z_calibbias, S_IRUGO,
		inv_attr_show, NULL, ATTR_GYRO_Z_CALIBBIAS);

static IIO_DEVICE_ATTR(in_anglvel_self_test_scale, S_IRUGO,
		inv_attr_show, NULL, ATTR_SELF_TEST_GYRO_SCALE);
static IIO_DEVICE_ATTR(in_accel_self_test_scale, S_IRUGO,
		inv_attr_show, NULL, ATTR_SELF_TEST_ACCEL_SCALE);

static IIO_DEVICE_ATTR(gyro_matrix, S_IRUGO, inv_attr_show, NULL,
	ATTR_GYRO_MATRIX);
static IIO_DEVICE_ATTR(accel_matrix, S_IRUGO, inv_attr_show, NULL,
	ATTR_ACCEL_MATRIX);
#ifdef CONFIG_INV_TESTING /* read/write in test mode */
static IIO_DEVICE_ATTR(compass_matrix, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_COMPASS_MATRIX);
static IIO_DEVICE_ATTR(compass_sens, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_attr_store, ATTR_COMPASS_SENS);
#else
static IIO_DEVICE_ATTR(compass_matrix, S_IRUGO, inv_attr_show, NULL,
	ATTR_COMPASS_MATRIX);
#endif
static IIO_DEVICE_ATTR(secondary_name, S_IRUGO, inv_attr_show, NULL,
	ATTR_SECONDARY_NAME);

#ifdef CONFIG_INV_TESTING
static IIO_DEVICE_ATTR(reg_write, S_IRUGO | S_IWUSR, inv_attr_show,
	inv_reg_write_store, ATTR_REG_WRITE);
/* smd debug related sysfs */
static IIO_DEVICE_ATTR(debug_smd_enable_testp1, S_IWUSR, NULL,
	inv_dmp_attr_store, ATTR_DEBUG_SMD_ENABLE_TESTP1);
static IIO_DEVICE_ATTR(debug_smd_enable_testp2, S_IWUSR, NULL,
	inv_dmp_attr_store, ATTR_DEBUG_SMD_ENABLE_TESTP2);
static IIO_DEVICE_ATTR(debug_smd_exe_state, S_IRUGO, inv_attr_show,
	NULL, ATTR_DEBUG_SMD_EXE_STATE);
static IIO_DEVICE_ATTR(debug_smd_delay_cntr, S_IRUGO, inv_attr_show,
	NULL, ATTR_DEBUG_SMD_DELAY_CNTR);
static DEVICE_ATTR(test_suspend_resume, S_IRUGO | S_IWUSR,
		inv_test_suspend_resume_show, inv_test_suspend_resume_store);

static IIO_DEVICE_ATTR(test_gyro_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_GYRO_COUNTER);
static IIO_DEVICE_ATTR(test_accel_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_ACCEL_COUNTER);
static IIO_DEVICE_ATTR(test_compass_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_COMPASS_COUNTER);
static IIO_DEVICE_ATTR(test_pressure_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_PRESSURE_COUNTER);
static IIO_DEVICE_ATTR(test_LPQ_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_LPQ_COUNTER);
static IIO_DEVICE_ATTR(test_SIXQ_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_SIXQ_COUNTER);
static IIO_DEVICE_ATTR(test_PEDQ_counter, S_IRUGO | S_IWUSR, inv_test_show,
	inv_test_store, ATTR_DEBUG_PEDQ_COUNTER);
#endif

static const struct attribute *inv_gyro_attributes[] = {
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	&dev_attr_reg_dump.attr,
	&dev_attr_temperature.attr,
	&dev_attr_timestamp.attr,
	&dev_attr_master_enable.attr,
	&iio_dev_attr_in_anglvel_scale.dev_attr.attr,
	&iio_dev_attr_in_anglvel_x_calibbias.dev_attr.attr,
	&iio_dev_attr_in_anglvel_y_calibbias.dev_attr.attr,
	&iio_dev_attr_in_anglvel_z_calibbias.dev_attr.attr,
	&iio_dev_attr_in_anglvel_x_offset.dev_attr.attr,
	&iio_dev_attr_in_anglvel_y_offset.dev_attr.attr,
	&iio_dev_attr_in_anglvel_z_offset.dev_attr.attr,
	&iio_dev_attr_in_anglvel_self_test_scale.dev_attr.attr,
	&iio_dev_attr_self_test_samples.dev_attr.attr,
	&iio_dev_attr_self_test_threshold.dev_attr.attr,
	&iio_dev_attr_gyro_enable.dev_attr.attr,
	&iio_dev_attr_gyro_fifo_enable.dev_attr.attr,
	&iio_dev_attr_gyro_rate.dev_attr.attr,
	&iio_dev_attr_power_state.dev_attr.attr,
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_self_test.dev_attr.attr,
	&iio_dev_attr_gyro_matrix.dev_attr.attr,
	&iio_dev_attr_secondary_name.dev_attr.attr,
#ifdef CONFIG_INV_TESTING
	&iio_dev_attr_reg_write.dev_attr.attr,
	&iio_dev_attr_debug_smd_enable_testp1.dev_attr.attr,
	&iio_dev_attr_debug_smd_enable_testp2.dev_attr.attr,
	&iio_dev_attr_debug_smd_exe_state.dev_attr.attr,
	&iio_dev_attr_debug_smd_delay_cntr.dev_attr.attr,
	&dev_attr_test_suspend_resume.attr,
	&iio_dev_attr_test_gyro_counter.dev_attr.attr,
	&iio_dev_attr_test_accel_counter.dev_attr.attr,
	&iio_dev_attr_test_compass_counter.dev_attr.attr,
	&iio_dev_attr_test_pressure_counter.dev_attr.attr,
	&iio_dev_attr_test_LPQ_counter.dev_attr.attr,
	&iio_dev_attr_test_SIXQ_counter.dev_attr.attr,
	&iio_dev_attr_test_PEDQ_counter.dev_attr.attr,
#endif
};

static const struct attribute *inv_mpu6xxx_attributes[] = {
	&dev_attr_event_accel_motion.attr,
	&dev_attr_event_smd.attr,
	&dev_attr_event_pedometer.attr,
	&dev_attr_event_shealth_int.attr,
	&dev_attr_flush_batch.attr,
	&iio_dev_attr_in_accel_scale.dev_attr.attr,
	&iio_dev_attr_in_accel_x_calibbias.dev_attr.attr,
	&iio_dev_attr_in_accel_y_calibbias.dev_attr.attr,
	&iio_dev_attr_in_accel_z_calibbias.dev_attr.attr,
	&iio_dev_attr_in_accel_self_test_scale.dev_attr.attr,
	&iio_dev_attr_in_accel_x_offset.dev_attr.attr,
	&iio_dev_attr_in_accel_y_offset.dev_attr.attr,
	&iio_dev_attr_in_accel_z_offset.dev_attr.attr,
	&iio_dev_attr_in_accel_x_dmp_bias.dev_attr.attr,
	&iio_dev_attr_in_accel_y_dmp_bias.dev_attr.attr,
	&iio_dev_attr_in_accel_z_dmp_bias.dev_attr.attr,
	&iio_dev_attr_in_anglvel_x_dmp_bias.dev_attr.attr,
	&iio_dev_attr_in_anglvel_y_dmp_bias.dev_attr.attr,
	&iio_dev_attr_in_anglvel_z_dmp_bias.dev_attr.attr,
	&iio_dev_attr_pedometer_int_on.dev_attr.attr,
	&iio_dev_attr_pedometer_on.dev_attr.attr,
	&iio_dev_attr_pedometer_steps.dev_attr.attr,
	&iio_dev_attr_pedometer_time.dev_attr.attr,
	&iio_dev_attr_pedometer_counter.dev_attr.attr,
	&iio_dev_attr_shealth_cadence.dev_attr.attr,
	&iio_dev_attr_shealth_cadence_enable.dev_attr.attr,
	&iio_dev_attr_shealth_int_period.dev_attr.attr,
	&iio_dev_attr_shealth_instant_cadence.dev_attr.attr,
	&iio_dev_attr_shealth_flush_cadence.dev_attr.attr,
	&iio_dev_attr_shealth_freq_threshold.dev_attr.attr,
	&iio_dev_attr_shealth_timer.dev_attr.attr,
	&iio_dev_attr_pedometer_peak_thresh.dev_attr.attr,
	&iio_dev_attr_pedometer_step_thresh_time.dev_attr.attr,
	&iio_dev_attr_pedometer_step_thresh.dev_attr.attr,
	&iio_dev_attr_pedometer_int_thresh.dev_attr.attr,
	&iio_dev_attr_smd_enable.dev_attr.attr,
	&iio_dev_attr_smd_threshold.dev_attr.attr,
	&iio_dev_attr_smd_delay_threshold.dev_attr.attr,
	&iio_dev_attr_smd_delay_threshold2.dev_attr.attr,
	&iio_dev_attr_dmp_on.dev_attr.attr,
	&iio_dev_attr_dmp_int_on.dev_attr.attr,
	&iio_dev_attr_dmp_event_int_on.dev_attr.attr,
	&iio_dev_attr_step_indicator_on.dev_attr.attr,
	&iio_dev_attr_batchmode_timeout.dev_attr.attr,
	&iio_dev_attr_batchmode_wake_fifo_full_on.dev_attr.attr,
	&iio_dev_attr_six_axes_q_on.dev_attr.attr,
	&iio_dev_attr_six_axes_q_rate.dev_attr.attr,
	&iio_dev_attr_three_axes_q_on.dev_attr.attr,
	&iio_dev_attr_three_axes_q_rate.dev_attr.attr,
	&iio_dev_attr_ped_q_on.dev_attr.attr,
	&iio_dev_attr_ped_q_rate.dev_attr.attr,
	&iio_dev_attr_step_detector_on.dev_attr.attr,
	&iio_dev_attr_accel_enable.dev_attr.attr,
	&iio_dev_attr_accel_fifo_enable.dev_attr.attr,
	&iio_dev_attr_accel_rate.dev_attr.attr,
	&iio_dev_attr_firmware_loaded.dev_attr.attr,
	&iio_dev_attr_accel_matrix.dev_attr.attr,
};

static const struct attribute *inv_mpu6500_attributes[] = {
	&iio_dev_attr_motion_lpa_on.dev_attr.attr,
	&iio_dev_attr_motion_lpa_freq.dev_attr.attr,
	&iio_dev_attr_motion_lpa_threshold.dev_attr.attr,
};

static const struct attribute *inv_tap_attributes[] = {
	&dev_attr_event_tap.attr,
	&iio_dev_attr_tap_on.dev_attr.attr,
	&iio_dev_attr_tap_threshold.dev_attr.attr,
	&iio_dev_attr_tap_min_count.dev_attr.attr,
	&iio_dev_attr_tap_time.dev_attr.attr,
};

static const struct attribute *inv_lcd_pos_attributes[] = {
	&dev_attr_event_lcd_position.attr,
	&iio_dev_attr_lcd_pos_enable.dev_attr.attr,
	&iio_dev_attr_lcd_pos_time_threshold.dev_attr.attr,
	&iio_dev_attr_lcd_pos_up_x_threshold.dev_attr.attr,
	&iio_dev_attr_lcd_pos_up_y_threshold.dev_attr.attr,
	&iio_dev_attr_lcd_pos_up_z_threshold.dev_attr.attr,
	&iio_dev_attr_lcd_pos_down_x_threshold.dev_attr.attr,
	&iio_dev_attr_lcd_pos_down_y_threshold.dev_attr.attr,
	&iio_dev_attr_lcd_pos_down_z_threshold.dev_attr.attr,
};

static const struct attribute *inv_qshot_attributes[] = {
	&dev_attr_event_qshot_start.attr,
	&dev_attr_event_qshot_finish.attr,
	&iio_dev_attr_qshot_start_angle.dev_attr.attr,
	&iio_dev_attr_qshot_finish_angle.dev_attr.attr,
	&iio_dev_attr_qshot_start_int_enable.dev_attr.attr,
	&iio_dev_attr_qshot_finish_int_enable.dev_attr.attr,
};


static const struct attribute *inv_display_orient_attributes[] = {
	&dev_attr_event_display_orientation.attr,
	&iio_dev_attr_display_orientation_on.dev_attr.attr,
};

static const struct attribute *inv_compass_attributes[] = {
	&iio_dev_attr_in_magn_scale.dev_attr.attr,
	&iio_dev_attr_compass_enable.dev_attr.attr,
	&iio_dev_attr_compass_rate.dev_attr.attr,
	&iio_dev_attr_compass_matrix.dev_attr.attr,
};

static const struct attribute *inv_akxxxx_attributes[] = {
#ifdef CONFIG_INV_TESTING
	&iio_dev_attr_compass_sens.dev_attr.attr,
#endif
};

static const struct attribute *inv_pressure_attributes[] = {
	&iio_dev_attr_pressure_enable.dev_attr.attr,
	&iio_dev_attr_pressure_rate.dev_attr.attr,
};

static const struct attribute *inv_mpu3050_attributes[] = {
	&iio_dev_attr_in_accel_scale.dev_attr.attr,
	&iio_dev_attr_accel_enable.dev_attr.attr,
	&iio_dev_attr_accel_fifo_enable.dev_attr.attr,
	&iio_dev_attr_accel_matrix.dev_attr.attr,
};

static struct attribute *inv_attributes[
	ARRAY_SIZE(inv_gyro_attributes) +
	ARRAY_SIZE(inv_mpu6xxx_attributes) +
	ARRAY_SIZE(inv_mpu6500_attributes) +
	ARRAY_SIZE(inv_compass_attributes) +
	ARRAY_SIZE(inv_akxxxx_attributes) +
	ARRAY_SIZE(inv_pressure_attributes) +
	ARRAY_SIZE(inv_tap_attributes) +
	ARRAY_SIZE(inv_lcd_pos_attributes) +
	ARRAY_SIZE(inv_display_orient_attributes) +
	ARRAY_SIZE(inv_qshot_attributes) +
	1
];

static const struct attribute_group inv_attribute_group = {
	.name = "mpu",
	.attrs = inv_attributes
};

static const struct iio_info mpu_info = {
	.driver_module = THIS_MODULE,
	.attrs = &inv_attribute_group,
};

#if defined(CONFIG_SENSORS)
static ssize_t inv_accel_cal_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	int err;
	st = dev_get_drvdata(dev);
	if (!st->cal_data[0] && !st->cal_data[1] && !st->cal_data[2])
		err = -1;
	else
		err = 1;
	return snprintf(buf, PAGE_SIZE, "%d, %d, %d, %d\n", err,
		st->cal_data[0], st->cal_data[1], st->cal_data[2]);
}

static int accel_do_calibrate(struct inv_mpu_state *st, int enable)
{
	signed short x, y, z;
	struct inv_reg_map_s *reg;
	int result, i;
	unsigned char data[6];
	int acc_enable;
	struct file *cal_filp;
	int sum[3] = { 0, };
	mm_segment_t old_fs = {0};

	reg = &(st->reg);

	if (enable) {
		if (!st->chip_config.enable) {
				result = st->set_power_state(st, true);
				if (result) {
					pr_err("%s,Could not chip enable fail.\n", __func__);
					return result;
				}
		}

		acc_enable = st->chip_config.accel_enable;
		if (!acc_enable) {
			st->chip_config.accel_enable = 1;

			result = st->switch_accel_engine(st, true);
			if (result) {
				pr_err("%s,Could not accel enable fail.\n", __func__);
				return result;
			}
		}

		for (i = 0; i < 10; i++) {
			result = inv_i2c_read(st, reg->raw_accel, BYTES_PER_SENSOR, data);
			if (result) {
				pr_err("%s,Could not accel enable fail.\n", __func__);
				return result;
			}

			x = (signed short)(((data[0] << 8) | data[1])*st->chip_info.multi);
			y = (signed short)(((data[2] << 8) | data[3])*st->chip_info.multi);
			z = (signed short)(((data[4] << 8) | data[5])*st->chip_info.multi);

			sum[0] += 0 - x;
			sum[1] += 0 - y;
			if (z > 0)
				sum[2] += MPU_MAX_4G_OFFSET_VALUE - z;
			else
				sum[2] += MPU_MIN_4G_OFFSET_VALUE - z;
			usleep_range(10000, 11000);
		}

		for (i = 0; i < 3 ; i++)
			st->cal_data[i] = sum[i] / 10;

		if (!acc_enable) {
			st->chip_config.accel_enable = acc_enable;

			result = st->switch_accel_engine(st, false);
			if (result) {
				pr_err("%s,Could not accel disable fail.\n", __func__);
				return result;
			}
		}

		if (!st->chip_config.enable) {
				result = st->set_power_state(st, false);
				if (result) {
					pr_err("%s,Could not chip disable fail.\n", __func__);
					return result;
				}
		}
	} else {
		for (i = 0; i < 3 ; i++) {
			sum[i] = st->cal_data[i];
			st->cal_data[i] = 0;
		}
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(MPU6500_ACCEL_CAL_PATH,
			O_CREAT | O_TRUNC | O_WRONLY, 0660);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		result = PTR_ERR(cal_filp);
		goto done;
	}

	result = cal_filp->f_op->write(cal_filp,
		(char *)&st->cal_data, 3 * sizeof(s16),
			&cal_filp->f_pos);
	if (result != 3 * sizeof(s16)) {
		pr_err("%s: Can't write the cal data to file\n", __func__);
		if (enable)
			for (i = 0; i < 3 ; i++)
				st->cal_data[i] = 0;
		else
			for (i = 0; i < 3 ; i++)
				st->cal_data[i] = sum[i];

		result = -EIO;
	}

	filp_close(cal_filp, current->files);
done:
	set_fs(old_fs);

	return result;
}

static ssize_t inv_accel_cal_store(struct device *dev,
				struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct inv_mpu_state *st;
	int err, enable;
	err = kstrtoint(buf, 10, &enable);

	if (err) {
		pr_err("%s, kstrtoint fail\n", __func__);
	} else {
		st = dev_get_drvdata(dev);
		err = accel_do_calibrate(st, enable);
		if (err) {
			pr_err("%s, accel calibration fail\n", __func__);
		}
	}

	return size;
}

static ssize_t inv_accel_raw_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	signed short cx, cy, cz;
	signed short x, y, z;
	signed short m[9];
	int i;

	struct inv_mpu_state *st;

	st = dev_get_drvdata(dev);

	x = (st->accel_data[0] + st->cal_data[0]);
	y = (st->accel_data[1] + st->cal_data[1]);
	z = (st->accel_data[2] + st->cal_data[2]);

	cx = cy = cz = 0;

	if (st->plat_data.orientation != NULL) {
		for (i = 0; i < 9; i++)
			m[i] = st->plat_data.orientation[i];

		cx = m[0]*x + m[1]*y + m[2]*z;
		cy = m[3]*x + m[4]*y + m[5]*z;
		cz = m[6]*x + m[7]*y + m[8]*z;
	}

	return snprintf(buf, PAGE_SIZE, "%d, %d, %d\n", cx, cy, cz);
}

static ssize_t inv_mpu_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);
}

static ssize_t inv_mpu_name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);
}

static ssize_t inv_reactive_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	st = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", st->reactive_state);
}

static ssize_t inv_reactive_store(struct device *dev,
				struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct inv_mpu_state *st = dev_get_drvdata(dev);
	bool onoff = false;
	unsigned long enable = 0;
	int result;

	if (kstrtoul(buf, 10, &enable)) {
		pr_err("[SENSOR] %s, kstrtoint fail\n", __func__);
		return -EINVAL;
	}

	if (enable == 1) {
		onoff = true;
	} else if (enable == 0) {
		onoff = false;
	} else if (enable == 2) {
		onoff = true;
		st->factory_mode = true;
	} else {
		pr_err("[SENSOR] %s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	if (onoff) {	/* enable reactive_alert */
		st->mot_st_time = jiffies;
	} else {	/* disable reactive_alert */
		st->reactive_state = 0;
		if (st->factory_mode)
			st->factory_mode = false;
	}

	st->reactive_enable = onoff;

	if (!(st->sensor[SENSOR_ACCEL].on |
			st->sensor[SENSOR_GYRO].on |
			st->sensor[SENSOR_COMPASS].on |
			st->sensor[SENSOR_PRESSURE].on |
			st->chip_config.dmp_on |
			st->mot_int.mot_on)) {
		result = set_inv_enable(st->indio_dev, false);
		if (result)
			pr_err("[SENSOR] %s, set_inv_enable error\n", __func__);
	} else {
		result = set_inv_enable(st->indio_dev, true);
		if (result)
			pr_err("[SENSOR] %s, set_inv_enable error\n", __func__);
	}

	pr_info("[SENSOR] %s: onoff = %d, state =%d OUT\n", __func__,
			st->reactive_enable,
			st->reactive_state);

	return size;
}

static ssize_t inv_mpu_acc_selftest_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct inv_mpu_state *st;
	int gyro_ratio[3], accel_ratio[3];
	int result = 0;

	st = dev_get_drvdata(dev);

	result = mpu6500_hw_self_check(st, gyro_ratio, accel_ratio, MPU6500_HWST_ACCEL) + 1;
	if( result != 1)
	{
		result = mpu6500_hw_self_check(st, gyro_ratio, accel_ratio, MPU6500_HWST_ACCEL) + 1;
	}

	if(result == 1)
		pr_info("%s : selftest success. ret:%d\n", __func__, result);
	else if(result == 2)
		pr_info("%s : selftest(accel) failed. ret:%d\n", __func__, result);
	else if(result == 3)
		pr_info("%s : selftest(gyro) failed. ret:%d\n", __func__, result);

	pr_info("%s : %d.%01d,%d.%01d,%d.%01d\n", __func__,
		(int)abs(accel_ratio[0]/10),
		(int)abs(accel_ratio[0])%10,
		(int)abs(accel_ratio[1]/10),
		(int)abs(accel_ratio[1])%10,
		(int)abs(accel_ratio[2]/10),
		(int)abs(accel_ratio[2])%10);

	return sprintf(buf, "%d,"
		"%d.%01d,%d.%01d,%d.%01d\n",
		result,
		(int)abs(accel_ratio[0]/10),
		(int)abs(accel_ratio[0])%10,
		(int)abs(accel_ratio[1]/10),
		(int)abs(accel_ratio[1])%10,
		(int)abs(accel_ratio[2]/10),
		(int)abs(accel_ratio[2])%10);
}

static ssize_t inv_accel_lpf_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct inv_mpu_state *st = dev_get_drvdata(dev);
	unsigned long enable = 0;
	u8 reg = 0;

	if (kstrtoul(buf, 10, &enable)) {
		pr_err("[SENSOR] %s, kstrtoint fail\n", __func__);
		return -EINVAL;
	}

	if (enable == 1) {
		reg = INV_FILTER_42HZ;
		pr_info("[SENSOR] %s, INV-LPF On", __func__);
	} else if (enable == 0) {
		reg = 0x00;
		pr_info("[SENSOR] %s, INV-LPF Off", __func__);
	} else {
		pr_info("[SENSOR] %s, invalid value!\n", __func__);
		return size;
	}

	inv_i2c_single_write(st, REG_6500_ACCEL_CONFIG2, reg);

	return size;
}

static struct device_attribute dev_attr_acc_calibration =
	__ATTR(calibration, S_IRUGO | S_IWUSR | S_IWGRP,
		inv_accel_cal_show, inv_accel_cal_store);
static struct device_attribute dev_attr_acc_raw_data =
	__ATTR(raw_data, S_IRUSR | S_IRGRP,
	inv_accel_raw_data_show, NULL);
static struct device_attribute dev_attr_acc_vendor =
	__ATTR(vendor, S_IRUSR | S_IRGRP,
	inv_mpu_vendor_show, NULL);
static struct device_attribute dev_attr_acc_name =
	__ATTR(name, S_IRUSR | S_IRGRP,
	inv_mpu_name_show, NULL);
static struct device_attribute dev_attr_acc_reactive_alert =
	__ATTR(reactive_alert, S_IRUGO | S_IWUSR | S_IWGRP,
		inv_reactive_show, inv_reactive_store);
static struct device_attribute dev_attr_acc_selftest =
	__ATTR(selftest, S_IRUSR | S_IRGRP,
	inv_mpu_acc_selftest_show, NULL);
static struct device_attribute dev_attr_acc_lpf =
	__ATTR(lowpassfilter, S_IWUSR | S_IWGRP,
	NULL, inv_accel_lpf_store);

static struct device_attribute *accel_sensor_attrs[] = {
	&dev_attr_acc_calibration,
	&dev_attr_acc_raw_data,
	&dev_attr_acc_vendor,
	&dev_attr_acc_name,
	&dev_attr_acc_reactive_alert,
	&dev_attr_acc_selftest,
	&dev_attr_acc_lpf,
	NULL,
};

static ssize_t inv_mpu_power_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	st = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", st->chip_config.gyro_enable);
}

static ssize_t inv_mpu_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st = dev_get_drvdata(dev);
	short temperature = 0;
	unsigned char reg[2];
	int result;

	result = inv_i2c_read(st, MPUREG_TEMP_OUT_H, 2, reg);
	if (result) {
		pr_err("[SENSOR] %s: Could not read temperature register.\n", __func__);
		return result;
	}

	temperature = (short) (((reg[0]) << 8) | reg[1]);
	temperature = ((temperature / 334) + 21);

	pr_info("[SENSOR] %s: read temperature = %d\n", __func__, temperature);

	return sprintf(buf, "%d\n", temperature);
}

static ssize_t inv_mpu_gyro_selftest_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	int result, success;
	int hw_result = -1;
	int scaled_gyro_bias[3] = {0};
	int scaled_gyro_rms[3] = {0};
	int packet_count[3] = {0};
	int gyro_ratio[3] = {0};
	int accel_ratio[3];
	u8 state, retryCnt = 0;

	st = dev_get_drvdata(dev);

	result = inv_i2c_read(st, REG_INT_PIN_CFG, 1, &state);
	if (result)
		pr_err("%s : bypass state read error", __func__);

	if (state & BIT_BYPASS_EN) {
		pr_info("%s : set to non-bypass mode", __func__);
		/* set to non-bypass mode */
		result = inv_i2c_single_write(st, REG_INT_PIN_CFG, 0);
		if (result)
			pr_err("%s : non-bypass error", __func__);
	}

retry:
	success = mpu6500_selftest_run(st, packet_count,
					scaled_gyro_bias,
					scaled_gyro_rms,
					st->gyro_bias);

	if (success != 0) {
		if (retryCnt++ < 2) {
			pr_err("%s, returns wrong value. retry %d", __func__, retryCnt);
			goto retry;
		}
	} else {
		/* MPU6500_HWST_ALL   : gyro & accel */
		/* MPU6500_HWST_ACCEL : only accel   */
		/* MPU6500_HWST_GYRO  : only gyro    */
		hw_result = mpu6500_hw_self_check(st, gyro_ratio, accel_ratio, MPU6500_HWST_GYRO);
	}

	pr_info("%s, result = %d, hw_result = %d\n", __func__, success, hw_result);

	if (state & BIT_BYPASS_EN) {
		pr_info("%s : set to bypass mode", __func__);
		/* set to bypass mode */
		result = inv_i2c_single_write(st, REG_INT_PIN_CFG, state);
		if (result)
			pr_err("%s : bypass error", __func__);
	}

	if((result | hw_result) == 0) {
		pr_info("%s : selftest success. ret:%d\n", __func__, hw_result | hw_result);
		gyro_do_calibrate(st);
	} else {
		pr_info("%s : selftest failed. ret:%d\n", __func__, hw_result | hw_result);
		st->gyro_bias[0] = 0;
		st->gyro_bias[1] = 0;
		st->gyro_bias[2] = 0;
	}

	pr_info("%s : %d.%03d,%d.%03d,%d.%03d,\n", __func__,
		(int)abs(scaled_gyro_bias[0] / 1000),
		(int)abs(scaled_gyro_bias[0]) % 1000,
		(int)abs(scaled_gyro_bias[1] / 1000),
		(int)abs(scaled_gyro_bias[1]) % 1000,
		(int)abs(scaled_gyro_bias[2] / 1000),
		(int)abs(scaled_gyro_bias[2]) % 1000);
	pr_info("%s : %d.%03d,%d.%03d,%d.%03d\n", __func__,
		scaled_gyro_rms[0] / 1000,
		(int)abs(scaled_gyro_rms[0]) % 1000,
		scaled_gyro_rms[1] / 1000,
		(int)abs(scaled_gyro_rms[1]) % 1000,
		scaled_gyro_rms[2] / 1000,
		(int)abs(scaled_gyro_rms[2]) % 1000);
	pr_info("%s : %d.%01d,%d.%01d,%d.%01d\n", __func__,
		(int)abs(gyro_ratio[0]/10),
		(int)abs(gyro_ratio[0])%10,
		(int)abs(gyro_ratio[1]/10),
		(int)abs(gyro_ratio[1])%10,
		(int)abs(gyro_ratio[2]/10),
		(int)abs(gyro_ratio[2])%10);
	pr_info("%s : %d.%03d,%d.%03d,%d.%03d\n", __func__,
		(int)abs(packet_count[0] / 100),
		(int)abs(packet_count[0]) % 100,
		(int)abs(packet_count[1] / 100),
		(int)abs(packet_count[1]) % 100,
		(int)abs(packet_count[2] / 100),
		(int)abs(packet_count[2]) % 100);

	return snprintf(buf, PAGE_SIZE, "%d,"
			"%d.%03d,%d.%03d,%d.%03d,"
			"%d.%03d,%d.%03d,%d.%03d,"
			"%d.%01d,%d.%01d,%d.%01d,"
			"%d.%03d,%d.%03d,%d.%03d\n",
			success | hw_result,
			(int)abs(scaled_gyro_bias[0] / 1000),
			(int)abs(scaled_gyro_bias[0]) % 1000,
			(int)abs(scaled_gyro_bias[1] / 1000),
			(int)abs(scaled_gyro_bias[1]) % 1000,
			(int)abs(scaled_gyro_bias[2] / 1000),
			(int)abs(scaled_gyro_bias[2]) % 1000,
			scaled_gyro_rms[0] / 1000,
			(int)abs(scaled_gyro_rms[0]) % 1000,
			scaled_gyro_rms[1] / 1000,
			(int)abs(scaled_gyro_rms[1]) % 1000,
			scaled_gyro_rms[2] / 1000,
			(int)abs(scaled_gyro_rms[2]) % 1000,
			(int)abs(gyro_ratio[0]/10),
			(int)abs(gyro_ratio[0])%10,
			(int)abs(gyro_ratio[1]/10),
			(int)abs(gyro_ratio[1])%10,
			(int)abs(gyro_ratio[2]/10),
			(int)abs(gyro_ratio[2])%10,
			(int)abs(packet_count[0] / 100),
			(int)abs(packet_count[0]) % 100,
			(int)abs(packet_count[1] / 100),
			(int)abs(packet_count[1]) % 100,
			(int)abs(packet_count[2] / 100),
			(int)abs(packet_count[2]) % 100);
}

static struct device_attribute dev_attr_gyro_selftest =
	__ATTR(selftest, S_IRUSR | S_IRGRP,
	inv_mpu_gyro_selftest_show, NULL);
static struct device_attribute dev_attr_gyro_temperature =
	__ATTR(temperature, S_IRUSR | S_IRGRP,
	inv_mpu_temp_show, NULL);
static struct device_attribute dev_attr_gyro_power_on =
	__ATTR(power_on, S_IRUSR | S_IRGRP,
	inv_mpu_power_show, NULL);
static struct device_attribute dev_attr_gyro_vendor =
	__ATTR(vendor, S_IRUGO,
	inv_mpu_vendor_show, NULL);
static struct device_attribute dev_attr_gyro_name =
	__ATTR(name, S_IRUGO,
	inv_mpu_name_show, NULL);

static struct device_attribute *gyro_sensor_attrs[] = {
	&dev_attr_gyro_selftest,
	&dev_attr_gyro_temperature,
	&dev_attr_gyro_power_on,
	&dev_attr_gyro_vendor,
	&dev_attr_gyro_name,
	NULL,
};

#endif

static void inv_setup_func_ptr(struct inv_mpu_state *st)
{
	if (st->chip_type == INV_MPU3050) {
		st->set_power_state    = set_power_mpu3050;
		st->switch_gyro_engine = inv_switch_3050_gyro_engine;
		st->switch_accel_engine = inv_switch_3050_accel_engine;
		st->init_config        = inv_init_config_mpu3050;
		st->setup_reg          = inv_setup_reg_mpu3050;
	} else {
		st->set_power_state    = set_power_itg;
		st->switch_gyro_engine = inv_switch_gyro_engine;
		st->switch_accel_engine = inv_switch_accel_engine;
		st->init_config        = inv_init_config;
		st->setup_reg          = inv_setup_reg;
	}
}

static int inv_detect_6xxx(struct inv_mpu_state *st)
{
	int result;
	u8 d;

	result = inv_i2c_read(st, REG_WHOAMI, 1, &d);
	if (result)
		return result;
	if ((d == MPU6500_ID) || (d == MPU6515_ID)) {
		st->chip_type = INV_MPU6500;
		strcpy(st->name, "mpu6500");
	} else {
		strcpy(st->name, "mpu6050");
	}

	return 0;
}

static int inv_setup_vddio(struct inv_mpu_state *st)
{
	int result;
	u8 data[1];

	if (INV_MPU6050 == st->chip_type) {
		result = inv_i2c_read(st, REG_YGOFFS_TC, 1, data);
		if (result)
			return result;
		data[0] &= ~BIT_I2C_MST_VDDIO;
		if (st->plat_data.level_shifter)
			data[0] |= BIT_I2C_MST_VDDIO;
		/*set up VDDIO register */
		result = inv_i2c_single_write(st, REG_YGOFFS_TC, data[0]);
		if (result)
			return result;
	}

	return 0;
}

/*
 *  inv_check_chip_type() - check and setup chip type.
 */
static int inv_check_chip_type(struct inv_mpu_state *st,
		const struct i2c_device_id *id, bool reset_needed)
{
	struct inv_reg_map_s *reg;
	int result;
	int t_ind;
	struct inv_chip_config_s *conf;
	struct mpu_platform_data *plat;

	conf = &st->chip_config;
	plat = &st->plat_data;
	if (!strcmp(id->name, "itg3500")) {
		st->chip_type = INV_ITG3500;
	} else if (!strcmp(id->name, "mpu3050")) {
		st->chip_type = INV_MPU3050;
	} else if (!strcmp(id->name, "mpu6050")) {
		st->chip_type = INV_MPU6050;
	} else if (!strcmp(id->name, "mpu9150")) {
		st->chip_type = INV_MPU6050;
		plat->sec_slave_type = SECONDARY_SLAVE_TYPE_COMPASS;
		plat->sec_slave_id = COMPASS_ID_AK8975;
	} else if (!strcmp(id->name, "mpu6500")) {
		st->chip_type = INV_MPU6500;
	} else if (!strcmp(id->name, "mpu9250")) {
		st->chip_type = INV_MPU6500;
		plat->sec_slave_type = SECONDARY_SLAVE_TYPE_COMPASS;
		plat->sec_slave_id = COMPASS_ID_AK8963;
	} else if (!strcmp(id->name, "mpu9255")) {
		st->chip_type = INV_MPU6500;
		plat->sec_slave_type = SECONDARY_SLAVE_TYPE_COMPASS;
		plat->sec_slave_id = COMPASS_ID_AK8963;
	} else if (!strcmp(id->name, "mpu6xxx")) {
		st->chip_type = INV_MPU6050;
	} else if (!strcmp(id->name, "mpu9350")) {
		st->chip_type = INV_MPU6500;
		plat->sec_slave_type = SECONDARY_SLAVE_TYPE_COMPASS;
		plat->sec_slave_id = COMPASS_ID_MLX90399;
		/* block use of MPU9350 in this build
		   since it's not production ready */
		pr_err("MPU9350 support is not officially available yet.\n");
		return -EPERM;
	} else if (!strcmp(id->name, "mpu6515")) {
		st->chip_type = INV_MPU6500;
	} else {
		return -EPERM;
	}
	inv_setup_func_ptr(st);
	st->hw  = &hw_info[st->chip_type];
	reg = &st->reg;
	st->setup_reg(reg);

	if (reset_needed) {
		/* reset to make sure previous state are not there */
		result = inv_i2c_single_write(st, reg->pwr_mgmt_1, BIT_H_RESET);
		if (result)
			return result;
		msleep(POWER_UP_TIME);
	}
	/* toggle power state */
	result = st->set_power_state(st, false);
	if (result)
		return result;

	result = st->set_power_state(st, true);
	if (result)
		return result;

	if (!strcmp(id->name, "mpu6xxx")) {
		/* for MPU6500, reading register need more time */
		msleep(POWER_UP_TIME);
		result = inv_detect_6xxx(st);
		if (result)
			return result;
	}

	if ((plat->sec_slave_type != SECONDARY_SLAVE_TYPE_NONE) ||
		(plat->aux_slave_type != SECONDARY_SLAVE_TYPE_NONE)) {
		result = inv_setup_vddio(st);
		if (result)
			return result;
	}

	switch (st->chip_type) {
	case INV_ITG3500:
		break;
	case INV_MPU6050:
	case INV_MPU6500:
		if (SECONDARY_SLAVE_TYPE_COMPASS == plat->sec_slave_type)
			conf->has_compass = 1;
		else
			conf->has_compass = 0;
		if (SECONDARY_SLAVE_TYPE_PRESSURE == plat->aux_slave_type)
			conf->has_pressure = 1;
		else
			conf->has_pressure = 0;

		break;
	case INV_MPU3050:
		if (SECONDARY_SLAVE_TYPE_ACCEL == plat->sec_slave_type) {
			if (ACCEL_ID_BMA250 == plat->sec_slave_id)
				inv_register_mpu3050_slave(st);
		}
		break;
	default:
		result = st->set_power_state(st, false);
		return -ENODEV;
	}
	if (conf->has_compass && conf->has_pressure &&
			(COMPASS_ID_MLX90399 == plat->sec_slave_id)) {
		pr_err("MLX90399 can't share slaves with others\n");
		return -EINVAL;
	}
	switch (st->chip_type) {
	case INV_MPU6050:
		result = inv_get_silicon_rev_mpu6050(st);
		break;
	case INV_MPU6500:
		result = inv_get_silicon_rev_mpu6500(st);
		break;
	default:
		result = 0;
		break;
	}
	if (result) {
		pr_err("read silicon rev error\n");
		st->set_power_state(st, false);
		return result;
	}

	/* turn off the gyro engine after OTP reading */
	result = st->switch_gyro_engine(st, false);
	if (result)
		return result;
	result = st->switch_accel_engine(st, false);
	if (result)
		return result;

	if (conf->has_compass) {
		result = inv_mpu_setup_compass_slave(st);
		if (result) {
			pr_err("compass setup failed\n");
			st->set_power_state(st, false);
			return result;
		}
	}
	if (conf->has_pressure) {
		result = inv_mpu_setup_pressure_slave(st);
		if (result) {
			pr_err("pressure setup failed\n");
			st->set_power_state(st, false);
			return result;
		}
	}

	t_ind = 0;
	memcpy(&inv_attributes[t_ind], inv_gyro_attributes,
		sizeof(inv_gyro_attributes));
	t_ind += ARRAY_SIZE(inv_gyro_attributes);

	if (INV_MPU3050 == st->chip_type && st->slave_accel != NULL) {
		memcpy(&inv_attributes[t_ind], inv_mpu3050_attributes,
		       sizeof(inv_mpu3050_attributes));
		t_ind += ARRAY_SIZE(inv_mpu3050_attributes);
		inv_attributes[t_ind] = NULL;
		return 0;
	}

	/* all MPU6xxx based parts */
	if ((INV_MPU6050 == st->chip_type) || (INV_MPU6500 == st->chip_type)) {
		memcpy(&inv_attributes[t_ind], inv_mpu6xxx_attributes,
		       sizeof(inv_mpu6xxx_attributes));
		t_ind += ARRAY_SIZE(inv_mpu6xxx_attributes);
	}

	/* MPU6500 only */
	if (INV_MPU6500 == st->chip_type) {
		memcpy(&inv_attributes[t_ind], inv_mpu6500_attributes,
		       sizeof(inv_mpu6500_attributes));
		t_ind += ARRAY_SIZE(inv_mpu6500_attributes);

		memcpy(&inv_attributes[t_ind], inv_qshot_attributes,
		       sizeof(inv_qshot_attributes));
		t_ind += ARRAY_SIZE(inv_qshot_attributes);

	}

	if (conf->has_compass) {
		memcpy(&inv_attributes[t_ind], inv_compass_attributes,
		       sizeof(inv_compass_attributes));
		t_ind += ARRAY_SIZE(inv_compass_attributes);

		/* AKM only */
		if (st->plat_data.sec_slave_id == COMPASS_ID_AK8975 ||
		    st->plat_data.sec_slave_id == COMPASS_ID_AK8972 ||
		    st->plat_data.sec_slave_id == COMPASS_ID_AK09911 ||
		    st->plat_data.sec_slave_id == COMPASS_ID_AK09912 ||
		    st->plat_data.sec_slave_id == COMPASS_ID_AK8963) {
			memcpy(&inv_attributes[t_ind], inv_akxxxx_attributes,
			       sizeof(inv_akxxxx_attributes));
			t_ind += ARRAY_SIZE(inv_akxxxx_attributes);
		}
	}

	if (conf->has_pressure) {
		memcpy(&inv_attributes[t_ind], inv_pressure_attributes,
		       sizeof(inv_pressure_attributes));
		t_ind += ARRAY_SIZE(inv_pressure_attributes);
	}

	inv_attributes[t_ind] = NULL;

	return 0;
}

/*
 *  inv_create_dmp_sysfs() - create binary sysfs dmp entry.
 */
static const struct bin_attribute dmp_firmware = {
	.attr = {
		.name = "dmp_firmware",
		.mode = S_IRUGO | S_IWUSR
	},
	.size = DMP_IMAGE_SIZE + 32,
	.read = inv_dmp_firmware_read,
	.write = inv_dmp_firmware_write,
};

static const struct bin_attribute six_q_value = {
	.attr = {
		.name = "six_axes_q_value",
		.mode = S_IWUSR
	},
	.size = QUATERNION_BYTES,
	.read = NULL,
	.write = inv_six_q_write,
};

static int inv_create_dmp_sysfs(struct iio_dev *ind)
{
	int result;

	result = sysfs_create_bin_file(&ind->dev.kobj, &dmp_firmware);
	if (result)
		return result;
	result = sysfs_create_bin_file(&ind->dev.kobj, &six_q_value);

	return result;
}
static int mpu_parse_dt(struct mpu_platform_data *pdata)
{
	struct device_node *np = pdata->client->dev.of_node;
	u32 ori_num;
	int retval = 0;
	int i;

	if (!of_property_read_u8(np, "invensense,int_config",
				&pdata->int_config))
		pr_info("int_config = 0x%X\n", pdata->int_config);

	if (!of_property_read_u8(np, "invensense,level_shifter",
				&pdata->level_shifter))
		pr_info("level_shifter = 0x%X\n", pdata->level_shifter);

	if (!of_property_read_u32(np, "invensense,orientation",
				&ori_num))
		pr_info("orientation = %d\n", ori_num);
	for(i=0;i<9;i++)
		pdata->orientation[i] = orientation_map[ori_num][i];

	pdata->client->irq = of_get_named_gpio(np, "invensense,irq-gpio",0);
	if (gpio_is_valid(pdata->client->irq)) {
		retval = gpio_request_one(pdata->client->irq, GPIOF_DIR_IN, "mpu_int");
		if (retval) {
			pr_info("%s, get failed\n", __func__);
		}
	}else {
		pr_info("%s, get failed\n", __func__);
	}
	return 0;
}

static int platform_init_data(struct i2c_client *client, struct mpu_platform_data *data)
{
	data->client = client;
	mpu_parse_dt(data);
	client->irq = gpio_to_irq(data->client->irq);

	return 0;
}

/*
 *  inv_mpu_probe() - probe function.
 */
static int inv_mpu_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct inv_mpu_state *st;
	struct iio_dev *indio_dev;
	int result;
	/*
	 * If we're not coming from a power-off condition, we need to
	 * reset the chip as we may have gotten here via a watchdog
	 * reboot, in which case the status of the chip is unknown
	 * (i.e. chip is not reset by hardware on a watchdog reboot).
	 */
	bool reset_needed = true;

	pr_info("[MPU6515] %s is called!!\n", __func__);
#ifdef CONFIG_DTS_INV_MPU_IIO
	enable_irq_wake(client->irq);
#endif

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		result = -ENOSYS;
		pr_err("I2c function error\n");
		goto out_no_free;
	}
#ifdef INV_KERNEL_3_10
	indio_dev = iio_device_alloc(sizeof(*st));
#else
	indio_dev = iio_allocate_device(sizeof(*st));
#endif
	if (indio_dev == NULL) {
		pr_err("memory allocation failed\n");
		result =  -ENOMEM;
		goto out_no_free;
	}
	st = iio_priv(indio_dev);
	st->indio_dev = indio_dev;
	st->client = client;
	st->sl_handle = client->adapter;
	st->i2c_addr = client->addr;
	init_completion(&st->shealth.wait);
	INIT_WORK(&st->shealth.work, inv_shealth_sched_work);
	init_timer(&st->shealth.timer);
	st->shealth.timer.data = (unsigned long)st;
	st->shealth.timer.function = inv_shealth_timer_func;
	st->shealth.step_count = 0;

#ifdef CONFIG_DTS_INV_MPU_IIO
	result = invensense_mpu_parse_dt(&client->dev, &st->plat_data);
	if (result)
		goto out_free;

	/*Power on device.*/
	if (st->plat_data.power_on) {
		result = st->plat_data.power_on(&st->plat_data);
		if (result < 0) {
			dev_err(&client->dev,
					"power_on failed: %d\n", result);
			return result;
		}
	}

mdelay(100);
#else
	result = platform_init_data(client, &st->plat_data);
	if (result) {
		pr_err("Could not initialize device.\n");
		goto out_free;
	}

#endif
	result = inv_check_chip_type(st, id, reset_needed);
	if (result)
		goto out_free;

	result = st->init_config(indio_dev);
	if (result) {
		dev_err(&client->adapter->dev,
			"Could not initialize device.\n");
		goto out_free;
	}

	/* Make state variables available to all _show and _store functions. */
	i2c_set_clientdata(client, indio_dev);
	indio_dev->dev.parent = &client->dev;
	if (!strcmp(id->name, "mpu6xxx"))
		indio_dev->name = st->name;
	else
		indio_dev->name = id->name;
	indio_dev->channels = inv_mpu_channels;
	indio_dev->num_channels = ARRAY_SIZE(inv_mpu_channels);

	indio_dev->info = &mpu_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->currentmode = INDIO_DIRECT_MODE;

	result = inv_mpu_configure_ring(indio_dev);
	if (result) {
		pr_err("configure ring buffer fail\n");
		goto out_free;
	}

	result = iio_buffer_register(indio_dev, indio_dev->channels,
					indio_dev->num_channels);
	if (result) {
		pr_err("ring buffer register fail\n");
		goto out_unreg_ring;
	}
	st->irq = client->irq;
	result = inv_mpu_probe_trigger(indio_dev);
	if (result) {
		pr_err("trigger probe fail\n");
		goto out_remove_ring;
	}

	/* Tell the i2c counter, we have an IRQ */
	INV_I2C_SETIRQ(IRQ_MPU, client->irq);

	result = iio_device_register(indio_dev);
	if (result) {
		pr_err("IIO device register fail\n");
		goto out_remove_trigger;
	}

	if (INV_MPU6050 == st->chip_type ||
		INV_MPU6500 == st->chip_type) {
		result = inv_create_dmp_sysfs(indio_dev);
		if (result) {
			pr_err("create dmp sysfs failed\n");
			goto out_unreg_iio;
		}
	}
	INIT_KFIFO(st->timestamps);
	spin_lock_init(&st->time_stamp_lock);
	mutex_init(&st->suspend_resume_lock);
	mutex_init(&st->iio_buf_write_lock);
	wake_lock_init(&st->shealth.wake_lock, WAKE_LOCK_SUSPEND, "inv_iio");

	result = st->set_power_state(st, false);
	if (result) {
		dev_err(&client->adapter->dev,
			"%s could not be turned off.\n", st->hw->name);
		goto out_remove_dmp_sysfs;
	}
	inv_init_sensor_struct(st);

#ifdef CONFIG_SENSORS
	wake_lock_init(&st->reactive_wake_lock, WAKE_LOCK_SUSPEND,
			"reactive_wake_lock");

	result = sensors_register(st->gyro_sensor_device,
		st, gyro_sensor_attrs, "gyro_sensor");
	if (result) {
		pr_err("%s: cound not register gyro sensor device(%d).\n",
		__func__, result);
		goto err_gyro_sensor_register_failed;
	}

	result = sensors_register(st->accel_sensor_device,
		st, accel_sensor_attrs, "accelerometer_sensor");
	if (result) {
		pr_err("%s: cound not register accel sensor device(%d).\n",
		__func__, result);
		goto err_accel_sensor_register_failed;
	}
#endif

	result = inv_mpu_configure_ring2(indio_dev);
	if (result) {
		pr_err("configure ring2 buffer fail\n");
		goto err_request_threaded_irq;
	}

	enable_irq(client->irq);
	enable_irq_wake(client->irq);

	dev_info(&client->dev, "%s is ready to go!\n", indio_dev->name);

	/* version info */
	pr_info("[SHEALTH] %s version probed successfully.\n", SHEALTH_VERSION);

	return 0;

err_request_threaded_irq:
	sensors_unregister(st->accel_sensor_device,accel_sensor_attrs);
#ifdef CONFIG_SENSORS
err_accel_sensor_register_failed:
	sensors_unregister(st->gyro_sensor_device,gyro_sensor_attrs);
err_gyro_sensor_register_failed:
	wake_lock_destroy(&st->reactive_wake_lock);
#endif

out_remove_dmp_sysfs:
	mutex_destroy(&st->iio_buf_write_lock);
	mutex_destroy(&st->suspend_resume_lock);
	kfifo_free(&st->timestamps);
out_unreg_iio:
	iio_device_unregister(indio_dev);
out_remove_trigger:
	if (indio_dev->modes & INDIO_BUFFER_TRIGGERED)
		inv_mpu_remove_trigger(indio_dev);
out_remove_ring:
	iio_buffer_unregister(indio_dev);
out_unreg_ring:
	inv_mpu_unconfigure_ring(indio_dev);
out_free:
#ifdef INV_KERNEL_3_10
	iio_device_free(indio_dev);
#else
	iio_free_device(indio_dev);
#endif
out_no_free:
	dev_err(&client->adapter->dev, "%s failed %d\n", __func__, result);
	return -EIO;
}

static void inv_mpu_shutdown(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	struct inv_reg_map_s *reg;
	int result;

	reg = &st->reg;
	mutex_lock(&indio_dev->mlock);
	dev_dbg(&client->adapter->dev, "Shutting down %s...\n", st->hw->name);

	/* reset to make sure previous state are not there */
	result = inv_i2c_single_write(st, reg->pwr_mgmt_1, BIT_H_RESET);
	if (result)
		dev_err(&client->adapter->dev, "Failed to reset %s\n",
			st->hw->name);
	mdelay(POWER_UP_TIME);
	/* turn off power to ensure gyro engine is off */
	result = st->set_power_state(st, false);
	if (result)
		dev_err(&client->adapter->dev, "Failed to turn off %s\n",
			st->hw->name);
	mutex_unlock(&indio_dev->mlock);
	pr_info("[SENSOR] %s is done\n", __func__);
}

/*
 *  inv_mpu_remove() - remove function.
 */
static int inv_mpu_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct inv_mpu_state *st = iio_priv(indio_dev);

	kfifo_free(&st->timestamps);
	iio_device_unregister(indio_dev);
	if (indio_dev->modes & INDIO_BUFFER_TRIGGERED)
		inv_mpu_remove_trigger(indio_dev);
	iio_buffer_unregister(indio_dev);
	inv_mpu_unconfigure_ring(indio_dev);
#ifdef INV_KERNEL_3_10
	iio_device_free(indio_dev);
#else
	iio_free_device(indio_dev);
#endif
	dev_info(&client->adapter->dev, "inv-mpu-iio module removed.\n");

	return 0;
}

static int inv_setup_suspend_batchmode(struct iio_dev *indio_dev, bool suspend)
{
	struct inv_mpu_state *st = iio_priv(indio_dev);
	int result;
	int counter;

	if (st->chip_config.dmp_on &&
		st->chip_config.enable &&
		st->batch.on &&
		(!st->chip_config.dmp_event_int_on)) {
		/* turn off data interrupt in suspend mode;turn on resume */
		result = inv_set_interrupt_on_gesture_event(st, suspend);
		if (result)
			return result;
		if (suspend)
			counter = INT_MAX;
		else
			counter = st->batch.counter;
		result = write_be32_key_to_mem(st, counter, KEY_BM_BATCH_THLD);
		if (result)
			return result;
	}

	return 0;
}

#ifdef CONFIG_PM
/*
 * inv_mpu_resume(): resume method for this driver.
 *    This method can be modified according to the request of different
 *    customers. It basically undo everything suspend_noirq is doing
 *    and recover the chip to what it was before suspend.
 */
static int inv_mpu_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct inv_mpu_state *st = iio_priv(indio_dev);
	int result;

	/* add code according to different request Start */
	pr_debug("%s inv_mpu_resume\n", st->hw->name);


	result = 0;
	if (st->chip_config.dmp_on && st->chip_config.enable) {
		result = st->set_power_state(st, true);
		result |= inv_read_time_and_ticks(st, true);
		if (st->ped.int_on)
			result |= inv_enable_pedometer_interrupt(st, true);
		if (st->chip_config.display_orient_on)
			result |= inv_set_display_orient_interrupt_dmp(st,
								true);
		result |= inv_setup_suspend_batchmode(indio_dev, false);
	} else if (st->chip_config.enable) {
		result = st->set_power_state(st, true);
	}

	/* add code according to different request End */
	enable_irq(st->client->irq);

	return result;
}

/*
 * inv_mpu_suspend(): suspend method for this driver.
 *    This method can be modified according to the request of different
 *    customers. If customer want some events, such as SMD to wake up the CPU,
 *    then data interrupt should be disabled in this interrupt to avoid
 *    unnecessary interrupts. If customer want pedometer running while CPU is
 *    asleep, then pedometer should be turned on while pedometer interrupt
 *    should be turned off.
 */
static int inv_mpu_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct inv_mpu_state *st = iio_priv(indio_dev);
	int result;
	u8 d =0;
	/* add code according to different request Start */
	pr_debug("%s inv_mpu_suspend\n", st->hw->name);
	disable_irq(st->client->irq);

	result = 0;
	if (st->chip_config.dmp_on && st->chip_config.enable) {
		/* turn off pedometer interrupt during suspend */
		if (st->ped.int_on)
			result |= inv_enable_pedometer_interrupt(st, false);
		/* turn off orientation interrupt during suspend */
		if (st->chip_config.display_orient_on)
			result |= inv_set_display_orient_interrupt_dmp(st,
								false);
		/* setup batch mode related during suspend */
		result = inv_setup_suspend_batchmode(indio_dev, true);

		if (st->sensor[SENSOR_ACCEL].on)
			st->sensor[SENSOR_ACCEL].send_data(st, false);
		if (st->sensor[SENSOR_GYRO].on)
			st->sensor[SENSOR_GYRO].send_data(st, false);
		if (st->sensor[SENSOR_SIXQ].on)
			st->sensor[SENSOR_SIXQ].send_data(st, false);
		if (st->sensor[SENSOR_LPQ].on)
			st->sensor[SENSOR_LPQ].send_data(st, false);

		/* only in DMP non-batch data mode, turn off the power */
		if ((!st->batch.on) && (!st->chip_config.smd_enable) &&
					(!st->ped.on))
			result |= st->set_power_state(st, false);
	} else if (st->chip_config.enable) {
		result = inv_i2c_read(st, REG_INT_ENABLE, 1, &d);
		if (!result){
			/* Unmask DRDY */
			d &= ~BIT_DATA_RDY_EN;
			inv_i2c_single_write(st, REG_INT_ENABLE, d);
		}
		/* in non DMP case, just turn off the power */
		result |= st->set_power_state(st, false);
	}

	/* add code according to different request End */

	return result;
}

static const struct dev_pm_ops inv_mpu_pmops = {
	.suspend       = inv_mpu_suspend,
	.resume        = inv_mpu_resume,
};
#define INV_MPU_PMOPS (&inv_mpu_pmops)
#else
#define INV_MPU_PMOPS NULL
#endif /* CONFIG_PM */

static const u16 normal_i2c[] = { I2C_CLIENT_END };
/* device id table is used to identify what device can be
 * supported by this driver
 */
static const struct i2c_device_id inv_mpu_id[] = {
	{"itg3500", INV_ITG3500},
	{"mpu3050", INV_MPU3050},
	{"mpu6050", INV_MPU6050},
	{"mpu9150", INV_MPU9150},
	{"mpu6500", INV_MPU6500},
	{"mpu9250", INV_MPU9250},
	{"mpu9255", INV_MPU9255},
	{"mpu6xxx", INV_MPU6XXX},
	{"mpu9350", INV_MPU9350},
	{"mpu6515", INV_MPU6515},
	{}
};

MODULE_DEVICE_TABLE(i2c, inv_mpu_id);

static struct i2c_driver inv_mpu_driver = {
	.class = I2C_CLASS_HWMON,
	.probe		=	inv_mpu_probe,
	.remove		=	inv_mpu_remove,
	.shutdown	=	inv_mpu_shutdown,
	.id_table	=	inv_mpu_id,
	.driver = {
		.owner	=	THIS_MODULE,
		.name	=	"inv-mpu-iio",
		.pm     =       INV_MPU_PMOPS,
	},
	.address_list = normal_i2c,
};

static int __init inv_mpu_init(void)
{
	int result = i2c_add_driver(&inv_mpu_driver);
	if (result) {
		pr_err("failed\n");
		return result;
	}
	return 0;
}

static void __exit inv_mpu_exit(void)
{
	i2c_del_driver(&inv_mpu_driver);
}

module_init(inv_mpu_init);
module_exit(inv_mpu_exit);

MODULE_AUTHOR("Invensense Corporation");
MODULE_DESCRIPTION("Invensense device driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("inv-mpu-iio");

