/* tc300k.c -- Linux driver for coreriver chip as touchkey
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Author: Junkyeong Kim <jk0430.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/i2c/tc300k.h>
#include <plat/gpio-cfg.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/regulator/consumer.h>
#include <linux/sec_sysfs.h>
#ifdef CONFIG_BATTERY_SAMSUNG
#include <linux/sec_batt.h>
#endif

#ifdef CONFIG_FB
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

/* TSK IC */
#define TC300K_TSK_IC	0x00
#define TC350K_TSK_IC	0x01

/* registers */
#define TC300K_KEYCODE		0x00
#define TC300K_FWVER		0x01
#define TC300K_MDVER		0x02
#define TC300K_MODE			0x03
#define TC300K_CHECKS_H		0x04
#define TC300K_CHECKS_L		0x05
#define TC300K_THRES_H		0x06
#define TC300K_THRES_L		0x07
#define TC300K_1KEY_DATA	0x08
#define TC300K_2KEY_DATA	0x0E
#define TC300K_3KEY_DATA	0x14
#define TC300K_4KEY_DATA	0x1A
#define TC300K_5KEY_DATA	0x20
#define TC300K_6KEY_DATA	0x26

#define TC300K_CH_PCK_H_OFFSET	0x00
#define TC300K_CH_PCK_L_OFFSET	0x01
#define TC300K_DIFF_H_OFFSET	0x02
#define TC300K_DIFF_L_OFFSET	0x03
#define TC300K_RAW_H_OFFSET		0x04
#define TC300K_RAW_L_OFFSET		0x05

/* registers for tabs2(tc350k) */
#define TC350K_1KEY		0x10	// recent inner
#define TC350K_2KEY		0x18	// back inner
#define TC350K_3KEY		0x20	// recent outer
#define TC350K_4KEY		0x28	// back outer

#define TC350K_THRES_DATA_OFFSET	0x00
#define TC350K_CH_PER_DATA_OFFSET	0x02
#define TC350K_CH_DIFF_DATA_OFFSET	0x04
#define TC350K_CH_RAW_DATA_OFFSET	0x06

#define TC350K_DATA_SIZE		0x02
#define TC350K_DATA_H_OFFSET	0x00
#define TC350K_DATA_L_OFFSET	0x01

/* command */
#define TC300K_CMD_ADDR			0x00
#define TC300K_CMD_LED_ON		0x10
#define TC300K_CMD_LED_OFF		0x20
#define TC300K_CMD_GLOVE_ON		0x30
#define TC300K_CMD_GLOVE_OFF	0x40
#define TC300K_CMD_FAC_ON		0x50
#define TC300K_CMD_FAC_OFF		0x60
#define TC300K_CMD_CAL_CHECKSUM	0x70
#define TC300K_CMD_FLIP_ON		0xB0
#define TC300K_CMD_FLIP_OFF	0xC0

#define TC300K_CMD_DELAY		50

/* connecter check */
#define SUB_DET_DISABLE			0
#define SUB_DET_ENABLE_CON_OFF	1
#define SUB_DET_ENABLE_CON_ON	2

/* firmware */
#define TC300K_FW_PATH_SDCARD	"/sdcard/tc300k.bin"

#define TK_UPDATE_PASS		0
#define TK_UPDATE_DOWN		1
#define TK_UPDATE_FAIL		2

/* ISP command */
#define TC300K_CSYNC1			0xA3
#define TC300K_CSYNC2			0xAC
#define TC300K_CSYNC3			0xA5
#define TC300K_CCFG				0x92
#define TC300K_PRDATA			0x81
#define TC300K_PEDATA			0x82
#define TC300K_PWDATA			0x83
#define TC300K_PECHIP			0x8A
#define TC300K_PEDISC			0xB0
#define TC300K_LDDATA			0xB2
#define TC300K_LDMODE			0xB8
#define TC300K_RDDATA			0xB9
#define TC300K_PCRST			0xB4
#define TC300K_PCRED			0xB5
#define TC300K_PCINC			0xB6
#define TC300K_RDPCH			0xBD

/* ISP delay */
#define TC300K_TSYNC1			300	/* us */
#define TC300K_TSYNC2			50	/* 1ms~50ms */
#define TC300K_TSYNC3			100	/* us */
#define TC300K_TDLY1			1	/* us */
#define TC300K_TDLY2			2	/* us */
#define TC300K_TFERASE			10	/* ms */
#define TC300K_TPROG			20	/* us */

#define TC300K_CHECKSUM_DELAY	500

enum {
	FW_INKERNEL,
	FW_SDCARD,
};

enum {
	NORMAL_MODE,
	FACTORY_MODE,
};

struct fw_image {
	u8 hdr_ver;
	u8 hdr_len;
	u16 first_fw_ver;
	u16 second_fw_ver;
	u16 third_ver;
	u32 fw_len;
	u16 checksum;
	u16 alignment_dummy;
	u8 data[0];
} __attribute__ ((packed));

#define	TSK_RELEASE	0x00
#define	TSK_PRESS	0x01

struct tsk_event_val {
	u16	tsk_bitmap;
	u8	tsk_status;
	int	tsk_keycode;
	char*	tsk_keyname;
};

struct tsk_event_val tsk_ev_old[8] =
{
	{0x01, TSK_PRESS, KEY_BACK, "back"},
	{0x02, TSK_PRESS, KEY_RECENT, "recent"},
	{0x03, TSK_PRESS, KEY_DUMMY_BACK, "dummy_back"},
	{0x04, TSK_PRESS, KEY_DUMMY_MENU, "dummy_menu"},
	{0x09, TSK_RELEASE, KEY_BACK, "back"},
	{0x0A, TSK_RELEASE, KEY_RECENT, "recent"},
	{0x0B, TSK_RELEASE, KEY_DUMMY_BACK, "dummy_back"},
	{0x0C, TSK_RELEASE, KEY_DUMMY_MENU, "dummy_menu"}
};

struct tsk_event_val tsk_ev[4] =
{
	{0x01 << 0, TSK_PRESS, KEY_RECENT, "recent"},
	{0x01 << 1, TSK_PRESS, KEY_BACK, "back"},
	{0x01 << 4, TSK_RELEASE, KEY_RECENT, "recent"},
	{0x01 << 5, TSK_RELEASE, KEY_BACK, "back"}
};

struct tsk_event_val tsk_ev_swap[4] =
{
	{0x01 << 0, TSK_PRESS, KEY_BACK, "back"},
	{0x01 << 1, TSK_PRESS, KEY_RECENT, "recent"},
	{0x01 << 4, TSK_RELEASE, KEY_BACK, "back"},
	{0x01 << 5, TSK_RELEASE, KEY_RECENT, "recent"}
};

struct tc300k_data {
	struct device *sec_touchkey;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct tc300k_platform_data *pdata;
	struct mutex lock;
	struct mutex lock_fac;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	struct fw_image *fw_img;
	const struct firmware *fw;
	char phys[32];
	int irq;
	u16 checksum;
	u16 threhold;
	int mode;
	int (*power) (bool on);
	u8 fw_ver;
	u8 fw_ver_bin;
	u8 md_ver;
	u8 fw_update_status;
	bool enabled;
	bool fw_downloding;
	bool glove_mode;
	bool factory_mode;
	bool led_on;
	bool flip_mode;
	int key_num;
	struct tsk_event_val *tsk_ev_val;

	struct pinctrl *pinctrl_i2c;
	struct pinctrl *pinctrl_irq;
	struct pinctrl_state *pin_state[4];

#ifdef CONFIG_FB
	struct notifier_block fb_notif;
#endif
};

extern struct class *sec_class;

char *str_states[] = {"on_irq", "off_irq", "on_i2c", "off_i2c"};
enum {
	I_STATE_ON_IRQ = 0,
	I_STATE_OFF_IRQ,
	I_STATE_ON_I2C,
	I_STATE_OFF_I2C,
};

static bool tc300k_power_enabled;
static bool tc300k_keyled_enabled;
const char *regulator_ic;
const char *regulator_led;
/*temporary*/
int get_tsp_status(void)
{
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void tc300k_early_suspend(struct early_suspend *h);
static void tc300k_late_resume(struct early_suspend *h);
#endif

static void tc300k_input_close(struct input_dev *dev);
static int tc300k_input_open(struct input_dev *dev);
static int tc300_pinctrl_init(struct tc300k_data *data);
static void tc300_config_gpio_i2c(struct tc300k_data *data, int onoff);
static int tc300_pinctrl(struct tc300k_data *data, int status);
static int read_tc350k_register_data(struct tc300k_data *data, int read_key_num, int read_offset);

static void tc300k_release_all_fingers(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	int i;

	dev_dbg(&client->dev, "[TK] %s\n", __func__);

	for (i = 0; i < data->key_num ; i++)
		input_report_key(data->input_dev,
			data->tsk_ev_val[i].tsk_keycode, 0);

	input_sync(data->input_dev);
}

static void tc300k_reset(struct tc300k_data *data)
{
	tc300k_release_all_fingers(data);

	data->pdata->keyled(false);
	data->pdata->power(false);

	msleep(50);

	data->pdata->power(true);
	data->pdata->keyled(true);
	msleep(120);
}

static void tc300k_reset_probe(struct tc300k_data *data)
{
	data->pdata->keyled(false);
	data->pdata->power(false);

	msleep(50);

	data->pdata->power(true);
	data->pdata->keyled(true);
	msleep(120);
}

int tc300k_get_fw_version(struct tc300k_data *data, bool probe)
{
	struct i2c_client *client = data->client;
	int retry = 3;
	int buf;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK]can't excute %s\n", __func__);
		return -1;
	}

	buf = i2c_smbus_read_byte_data(client, TC300K_FWVER);
	if (buf < 0) {
		while (retry--) {
			dev_err(&client->dev, "[TK]%s read fail(%d)\n",
				__func__, retry);
			if (probe)
				tc300k_reset_probe(data);
			else
				tc300k_reset(data);
			buf = i2c_smbus_read_byte_data(client, TC300K_FWVER);
			if (buf > 0)
				break;
		}
		if (retry <= 0) {
			dev_err(&client->dev, "[TK]%s read fail\n", __func__);
			data->fw_ver = 0;
			return -1;
		}
	}
	data->fw_ver = (u8)buf;
	dev_info(&client->dev, "[TK]fw_ver : 0x%x\n", data->fw_ver);

	return 0;
}
static void tc300k_gpio_request(struct tc300k_data *data)
{
	int ret = 0;
	dev_info(&data->client->dev, "%s: enter \n",__func__);

	if (!data->pdata->i2c_gpio) {
		ret = gpio_request(data->pdata->gpio_scl, "touchkey_scl");
		if (ret) {
			dev_err(&data->client->dev, "%s: unable to request touchkey_scl [%d]\n",
					__func__, data->pdata->gpio_scl);
		}

		ret = gpio_request(data->pdata->gpio_sda, "touchkey_sda");
		if (ret) {
			dev_err(&data->client->dev, "%s: unable to request touchkey_sda [%d]\n",
					__func__, data->pdata->gpio_sda);
		}
	}

	ret = gpio_request(data->pdata->gpio_int, "touchkey_irq");
	if (ret) {
		dev_err(&data->client->dev, "%s: unable to request touchkey_irq [%d]\n",
				__func__, data->pdata->gpio_int);
	}
}

#ifdef CONFIG_OF
static int tc300k_parse_dt(struct device *dev,
			struct tc300k_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

	of_property_read_u32(np, "coreriver,use_bitmap", &pdata->use_bitmap);

	pr_info("[TK] %s : %s protocol.\n",
				__func__, pdata->use_bitmap ? "Use Bit-map" : "Use OLD");

	pdata->gpio_scl = of_get_named_gpio_flags(np, "coreriver,scl-gpio", 0, &pdata->scl_gpio_flags);
	pdata->gpio_sda = of_get_named_gpio_flags(np, "coreriver,sda-gpio", 0, &pdata->sda_gpio_flags);
	pdata->gpio_int = of_get_named_gpio_flags(np, "coreriver,irq-gpio", 0, &pdata->irq_gpio_flags);

	pdata->boot_on_ldo = of_property_read_bool(np, "coreriver,boot-on-ldo");

	pdata->gpio_sub_det = of_get_named_gpio_flags(np, "coreriver,sub-det-gpio", 0, &pdata->irq_gpio_flags);
	if (pdata->gpio_sub_det  < 0 ) {
		pr_info("[TK] %s Failed to get sub-det-gpio[%d] property\n", __func__, pdata->gpio_sub_det);
		pdata->gpio_sub_det = 0;
	}

	pdata->i2c_gpio = of_property_read_bool(np, "coreriver,i2c-gpio");

	if (of_property_read_string(np, "coreriver,regulator_ic", &pdata->regulator_ic)) {
		pr_err("[TK] %s Failed to get regulator_ic name property\n",__func__);
		return -EINVAL;
	}
	regulator_ic = pdata->regulator_ic;

	if (of_property_read_string(np, "coreriver,regulator_led", &pdata->regulator_led)) {
		pr_err("[TK] %s Failed to get regulator_led name property\n",__func__);
		pdata->regulator_led = NULL;
		//return -EINVAL;  /* Optional parmeters do not return error value */
	}
	regulator_led = pdata->regulator_led;

	if (of_property_read_string(np, "coreriver,fw_name", &pdata->fw_name)) {
		pr_err("[TK] %s Failed to get fw_name property\n",__func__);
		return -EINVAL;
	} else {
		pr_info("[TK] %s fw_name %s\n", __func__, pdata->fw_name);
	}

	if (of_property_read_u32(np, "coreriver,sensing_ch_num", &pdata->sensing_ch_num) < 0){
		pr_err("[TK] %s Failed to get sensing_ch_num property\n",__func__);
		return -EINVAL;
	}

	if (of_property_read_u32(np, "coreriver,tsk_ic_num", &pdata->tsk_ic_num) < 0){
		pr_info("[TK] %s Failed to get tsk_ic_num, TSK IC is TC300K\n", __func__);
	} else {
		if (pdata->tsk_ic_num == TC350K_TSK_IC)
			pr_info("[TK] %s TSK IC is TC350K_TSK_IC[%d]\n", __func__, pdata->tsk_ic_num);
		else
			pr_err("[TK] %s TSK IC is unknown![%d]\n", __func__, pdata->tsk_ic_num);
	}

	return 0;
}
#else
static int tc300k_parse_dt(struct device *dev,
			struct tc300k_platform_data *pdata)
{
	return -ENODEV;
}
#endif


int tc300k_touchkey_power(bool on)
{
	struct regulator *regulator;
	int ret = 0;

	if (tc300k_power_enabled == on)
		return ret;

	printk(KERN_DEBUG "[TK] %s %s\n",
		__func__, on ? "on" : "off");

	regulator = regulator_get(NULL, regulator_ic);
	if (IS_ERR(regulator)){
		pr_err("[TK] %s: regulator_ic get failed\n", __func__);
		ret = -EIO;
		goto out;
	}
	if (on) {
		ret = regulator_enable(regulator);
		if (ret) {
			pr_err("[TK] %s: regulator_ic enable failed\n", __func__);
			goto out;
		}
	} else {
		if (regulator_is_enabled(regulator))
			regulator_disable(regulator);
	}

	tc300k_power_enabled = on;

out:
	regulator_put(regulator);
	return ret;
}

int tc300k_touchkey_led_control(bool on)
{
	struct regulator *regulator;
	int ret = 0;

	if (tc300k_keyled_enabled == on)
		return ret;

	if (!regulator_led)
		return ret;

	printk(KERN_DEBUG "[TK] %s %s\n",
		__func__, on ? "on" : "off");

	regulator = regulator_get(NULL, regulator_led);
	if (IS_ERR(regulator)){
		pr_err("[TK] %s: regulator_led get failed\n", __func__);
		ret = -EIO;
		goto out;
	}
	if (on) {
		ret = regulator_enable(regulator);
		if (ret) {
			pr_err("[TK] %s: regulator_led enable failed\n", __func__);
			goto out;
		}
	} else {
		if (regulator_is_enabled(regulator))
			regulator_disable(regulator);
	}

	tc300k_keyled_enabled = on;

out:
	regulator_put(regulator);
	return 0;
}

static irqreturn_t tc300k_interrupt(int irq, void *dev_id)
{
	struct tc300k_data *data = dev_id;
	struct i2c_client *client = data->client;
	int ret, retry;
	u8 key_val;
	int i = 0;
	bool key_handle_flag;

	dev_dbg(&client->dev, "[TK] %s\n",__func__);//okga

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return IRQ_HANDLED;
	}

	ret = i2c_smbus_read_byte_data(client, TC300K_KEYCODE);
	if (ret < 0) {
		retry = 3;
		while (retry--) {
			dev_err(&client->dev, "[TK] %s read fail ret=%d(retry:%d)\n",
				__func__, ret, retry);
			msleep(10);
			ret = i2c_smbus_read_byte_data(client, TC300K_KEYCODE);
			if (ret > 0)
				break;
		}
		if (retry <= 0) {
			tc300k_reset(data);
			return IRQ_HANDLED;
		}
	}
	key_val = (u8)ret;

	for (i = 0 ; i < data->key_num * 2 ; i++){
		if (data->pdata->use_bitmap)
			key_handle_flag = (key_val & data->tsk_ev_val[i].tsk_bitmap);
		else
			key_handle_flag = (key_val == data->tsk_ev_val[i].tsk_bitmap);

		if (key_handle_flag){
			input_report_key(data->input_dev,
				data->tsk_ev_val[i].tsk_keycode, data->tsk_ev_val[i].tsk_status);
#ifdef CONFIG_SAMSUNG_PRODUCT_SHIP
			dev_notice(&client->dev, "[TK] key %s\n",
				data->tsk_ev_val[i].tsk_status? "P" : "R");
#else
			dev_notice(&client->dev,
				"[TK] key %s : %s(0x%02X)\n",
				data->tsk_ev_val[i].tsk_status? "P" : "R",
				data->tsk_ev_val[i].tsk_keyname, key_val);
#endif
		}
	}

	input_sync(data->input_dev);
	return IRQ_HANDLED;
}

static ssize_t tc300k_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	int value;
	u8 threshold_h, threshold_l;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -EPERM;
	}

	if (data->pdata->tsk_ic_num == TC350K_TSK_IC) {
		mutex_lock(&data->lock_fac);
		value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_THRES_DATA_OFFSET);
		mutex_unlock(&data->lock_fac);
		return sprintf(buf, "%d\n", value);
	} else {
		ret = i2c_smbus_read_byte_data(client, TC300K_THRES_H);
		if (ret < 0) {
			dev_err(&client->dev, "[TK] %s: failed to read threshold_h (%d)\n",
				__func__, ret);
			return ret;
		}
		threshold_h = ret;

		ret = i2c_smbus_read_byte_data(client, TC300K_THRES_L);
		if (ret < 0) {
			dev_err(&client->dev, "[TK] %s: failed to read threshold_l (%d)\n",
				__func__, ret);
			return ret;
		}
		threshold_l = ret;

		data->threhold = (threshold_h << 8) | threshold_l;
		return sprintf(buf, "%d\n", data->threhold);
	}
}

static ssize_t tc300k_led_control(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		dev_err(&client->dev, "[TK] %s: cmd read err\n", __func__);
		return count;
	}

	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		dev_err(&client->dev, "[TK] %s: wrong command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		if (scan_buffer == 1)
			data->led_on = true;
		return count;
	}

	if (scan_buffer == 1) {
		dev_notice(&client->dev, "[TK] led on\n");
		cmd = TC300K_CMD_LED_ON;
	} else {
		dev_notice(&client->dev, "[TK] led off\n");
		cmd = TC300K_CMD_LED_OFF;
	}
	ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, cmd);
	if (ret < 0)
		dev_err(&client->dev, "[TK] %s fail(%d)\n", __func__, ret);

	msleep(TC300K_CMD_DELAY);

	return count;
}

static int load_fw_in_kernel(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	ret = request_firmware(&data->fw, data->pdata->fw_name, &client->dev);
	if (ret) {
		dev_err(&client->dev, "[TK] %s fail(%d)\n", __func__, ret);
		return -1;
	}
	data->fw_img = (struct fw_image *)data->fw->data;

	dev_info(&client->dev, "[TK] 0x%x firm (size=%d)\n",
		data->fw_img->first_fw_ver, data->fw_img->fw_len);
	dev_info(&client->dev, "[TK] %s done\n", __func__);

	return 0;
}

static int load_fw_sdcard(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	struct file *fp;
	mm_segment_t old_fs;
	long fsize, nread;
	int ret = 0;

	old_fs = get_fs();
	set_fs(get_ds());
	fp = filp_open(TC300K_FW_PATH_SDCARD, O_RDONLY, S_IRUSR);
	if (IS_ERR(fp)) {
		dev_err(&client->dev, "[TK] %s %s open error\n",
			__func__, TC300K_FW_PATH_SDCARD);
		ret = -ENOENT;
		goto fail_sdcard_open;
	}

	fsize = fp->f_path.dentry->d_inode->i_size;

	data->fw_img = kzalloc((size_t)fsize, GFP_KERNEL);
	if (!data->fw_img) {
		dev_err(&client->dev, "[TK] %s fail to kzalloc for fw\n", __func__);
		filp_close(fp, current->files);
		ret = -ENOMEM;
		goto fail_sdcard_kzalloc;
	}

	nread = vfs_read(fp, (char __user *)data->fw_img, fsize, &fp->f_pos);
	if (nread != fsize) {
		dev_err(&client->dev,
				"[TK] %s fail to vfs_read file\n", __func__);
		ret = -EINVAL;
		goto fail_sdcard_size;
	}
	filp_close(fp, current->files);
	set_fs(old_fs);

	dev_info(&client->dev, "[TK] fw_size : %lu\n", nread);
	dev_info(&client->dev, "[TK] %s done\n", __func__);

	return ret;

fail_sdcard_size:
	kfree(&data->fw_img);
fail_sdcard_kzalloc:
	filp_close(fp, current->files);
fail_sdcard_open:
	set_fs(old_fs);

	return ret;
}

static inline void setsda(struct tc300k_data *data, int state)
{
	if (state)
		gpio_direction_output(data->pdata->gpio_sda, 1);
	else
		gpio_direction_output(data->pdata->gpio_sda, 0);
}

static inline void setscl(struct tc300k_data *data, int state)
{
	if (state)
		gpio_direction_output(data->pdata->gpio_scl, 1);
	else
		gpio_direction_output(data->pdata->gpio_scl, 0);
}

static inline int getsda(struct tc300k_data *data)
{
	return gpio_get_value(data->pdata->gpio_sda);
}

static inline int getscl(struct tc300k_data *data)
{
	return gpio_get_value(data->pdata->gpio_scl);
}

static void send_9bit(struct tc300k_data *data, u8 buff)
{
	int i;

	setscl(data, 1);
	ndelay(20);
	setsda(data, 0);
	ndelay(20);
	setscl(data, 0);
	ndelay(20);

	for (i = 0; i < 8; i++) {
		setscl(data, 1);
		ndelay(20);
		setsda(data, (buff >> i) & 0x01);
		ndelay(20);
		setscl(data, 0);
		ndelay(20);
	}

	setsda(data, 0);
}

static u8 wait_9bit(struct tc300k_data *data)
{
	int i;
	int buf;
	u8 send_buf = 0;

	gpio_direction_input(data->pdata->gpio_sda);

	getsda(data);
	ndelay(10);
	setscl(data, 1);
	ndelay(40);
	setscl(data, 0);
	ndelay(20);

	for (i = 0; i < 8; i++) {
		setscl(data, 1);
		ndelay(20);
		buf = getsda(data);
		ndelay(20);
		setscl(data, 0);
		ndelay(20);
		send_buf |= (buf & 0x01) << i;
	}
	setsda(data, 0);

	return send_buf;
}

static void tc300k_reset_for_isp(struct tc300k_data *data, bool start)
{
	if (start) {
		setscl(data, 0);
		setsda(data, 0);
		data->pdata->keyled(false);
		data->pdata->power(false);

		msleep(100);

		data->pdata->power(true);

		usleep_range(5000, 6000);
	} else {
		data->pdata->keyled(false);
		data->pdata->power(false);
		msleep(100);

		data->pdata->power(true);
		data->pdata->keyled(true);
		msleep(120);

		gpio_direction_input(data->pdata->gpio_sda);
		gpio_direction_input(data->pdata->gpio_scl);
	}
}

static void load(struct tc300k_data *data, u8 buff)
{
    send_9bit(data, TC300K_LDDATA);
    udelay(1);
    send_9bit(data, buff);
    udelay(1);
}

static void step(struct tc300k_data *data, u8 buff)
{
    send_9bit(data, TC300K_CCFG);
    udelay(1);
    send_9bit(data, buff);
    udelay(2);
}

static void setpc(struct tc300k_data *data, u16 addr)
{
    u8 buf[4];
    int i;

    buf[0] = 0x02;
    buf[1] = addr >> 8;
    buf[2] = addr & 0xff;
    buf[3] = 0x00;

    for (i = 0; i < 4; i++)
        step(data, buf[i]);
}

static void configure_isp(struct tc300k_data *data)
{
    u8 buf[7];
    int i;

    buf[0] = 0x75;    buf[1] = 0xFC;    buf[2] = 0xAC;
    buf[3] = 0x75;    buf[4] = 0xFC;    buf[5] = 0x35;
    buf[6] = 0x00;

    /* Step(cmd) */
    for (i = 0; i < 7; i++)
        step(data, buf[i]);
}

static int tc300k_erase_fw(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	int i;
	u8 state = 0;

	tc300k_reset_for_isp(data, true);

	/* isp_enable_condition */
	send_9bit(data, TC300K_CSYNC1);
	udelay(9);
	send_9bit(data, TC300K_CSYNC2);
	udelay(9);
	send_9bit(data, TC300K_CSYNC3);
	usleep_range(150, 160);

	state = wait_9bit(data);
	if (state != 0x01) {
		dev_err(&client->dev, "[TK] %s isp enable error %d\n", __func__, state);
		return -1;
	}

	configure_isp(data);

	/* Full Chip Erase */
	send_9bit(data, TC300K_PCRST);
	udelay(1);
	send_9bit(data, TC300K_PECHIP);
	usleep_range(15000, 15500);


	state = 0;
	for (i = 0; i < 100; i++) {
		udelay(2);
		send_9bit(data, TC300K_CSYNC3);
		udelay(1);

		state = wait_9bit(data);
		if ((state & 0x04) == 0x00)
			break;
	}

	if (i == 100) {
		dev_err(&client->dev, "[TK] %s fail\n", __func__);
		return -1;
	}

	dev_info(&client->dev, "[TK] %s success\n", __func__);
	return 0;
}

static int tc300k_write_fw(struct tc300k_data *data)
{
	u16 addr = 0;
	u8 code_data;

	setpc(data, addr);
	load(data, TC300K_PWDATA);
	send_9bit(data, TC300K_LDMODE);
	udelay(1);

	while (addr < data->fw_img->fw_len) {
		code_data = data->fw_img->data[addr++];
		load(data, code_data);
		usleep_range(20, 21);
	}

	send_9bit(data, TC300K_PEDISC);
	udelay(1);

	return 0;
}

static int tc300k_verify_fw(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	u16 addr = 0;
	u8 code_data;

	setpc(data, addr);

	dev_info(&client->dev, "[TK] fw code size = %#x (%u)",
		data->fw_img->fw_len, data->fw_img->fw_len);
	while (addr < data->fw_img->fw_len) {
		if ((addr % 0x40) == 0)
			dev_dbg(&client->dev, "[TK] fw verify addr = %#x\n", addr);

		send_9bit(data, TC300K_PRDATA);
		udelay(2);
		code_data = wait_9bit(data);
		udelay(1);

		if (code_data != data->fw_img->data[addr++]) {
			dev_err(&client->dev,
				"%s addr : %#x data error (0x%2x)\n",
				__func__, addr - 1, code_data );
			return -1;
		}
	}
	dev_info(&client->dev, "[TK] %s success\n", __func__);

	return 0;
}

static void t300k_release_fw(struct tc300k_data *data, u8 fw_path)
{
	if (fw_path == FW_INKERNEL)
		release_firmware(data->fw);
	else if (fw_path == FW_SDCARD)
		kfree(data->fw_img);
}

static int tc300k_flash_fw(struct tc300k_data *data, u8 fw_path)
{
	struct i2c_client *client = data->client;
	int retry = 5;
	int ret;
	tc300_config_gpio_i2c(data, 0);
	do {
		ret = tc300k_erase_fw(data);
		if (ret)
			dev_err(&client->dev, "[TK] %s erase fail(retry=%d)\n",
				__func__, retry);
		else
			break;
	} while (retry-- > 0);
	if (retry < 0)
		goto err_tc300k_flash_fw;

	retry = 5;
	do {
		tc300k_write_fw(data);

		ret = tc300k_verify_fw(data);
		if (ret)
			dev_err(&client->dev, "[TK] %s verify fail(retry=%d)\n",
				__func__, retry);
		else
			break;
	} while (retry-- > 0);

	tc300k_reset_for_isp(data, false);
	tc300_config_gpio_i2c(data, 1);
	if (retry < 0)
		goto err_tc300k_flash_fw;

	return 0;

err_tc300k_flash_fw:

	return -1;
}

static int tc300k_crc_check(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	u16 checksum;
	u8 cmd;
	u8 checksum_h, checksum_l;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] %s can't excute\n", __func__);
		return -1;
	}

	cmd = TC300K_CMD_CAL_CHECKSUM;
	ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, cmd);
	if (ret) {
		dev_err(&client->dev, "[TK] %s command fail (%d)\n", __func__, ret);
		return ret;
	}

	msleep(TC300K_CHECKSUM_DELAY);

	ret = i2c_smbus_read_byte_data(client, TC300K_CHECKS_H);
	if (ret < 0) {
		dev_err(&client->dev, "[TK] %s: failed to read checksum_h (%d)\n",
			__func__, ret);
		return ret;
	}
	checksum_h = ret;

	ret = i2c_smbus_read_byte_data(client, TC300K_CHECKS_L);
	if (ret < 0) {
		dev_err(&client->dev, "[TK] %s: failed to read checksum_l (%d)\n",
			__func__, ret);
		return ret;
	}
	checksum_l = ret;

	checksum = (checksum_h << 8) | checksum_l;

	if (data->fw_img->checksum != checksum) {
		dev_err(&client->dev,
			"%s checksum fail - firm checksum(%d), compute checksum(%d)\n",
			__func__, data->fw_img->checksum, checksum);
		return -1;
	}

	dev_info(&client->dev, "[TK] %s success (%d)\n", __func__, checksum);

	return 0;
}

static int tc300k_fw_update(struct tc300k_data *data, u8 fw_path, bool force)
{
	struct i2c_client *client = data->client;
	int retry = 4;
	int ret;

	if (fw_path == FW_INKERNEL) {
		if (!force) {
			ret = tc300k_get_fw_version(data, false);
			if (ret)
				return -1;
		}

		ret = load_fw_in_kernel(data);
		if (ret)
			return -1;

		data->fw_ver_bin = data->fw_img->first_fw_ver;
		if (!force && (data->fw_ver >= data->fw_ver_bin)) {
			dev_notice(&client->dev, "[TK] do not need firm update (IC:0x%x, BIN:0x%x)\n",
				data->fw_ver, data->fw_ver_bin);
			t300k_release_fw(data, fw_path);
			return 0;
		}
	} else if (fw_path == FW_SDCARD) {
		ret = load_fw_sdcard(data);
		if (ret)
			return -1;
	}

	while (retry--) {
		data->fw_downloding = true;
		ret = tc300k_flash_fw(data, fw_path);
		data->fw_downloding = false;
		if (ret) {
			dev_err(&client->dev, "[TK] %s tc300k_flash_fw fail (%d)\n",
				__func__, retry);
			continue;
		}

		ret = tc300k_get_fw_version(data, false);
		if (ret) {
			dev_err(&client->dev, "[TK] %s tc300k_get_fw_version fail (%d)\n",
				__func__, retry);
			continue;
		}
		if (data->fw_ver != data->fw_img->first_fw_ver) {
			dev_err(&client->dev, "[TK] %s fw version fail (0x%x, 0x%x)(%d)\n",
				__func__, data->fw_ver, data->fw_img->first_fw_ver, retry);
			continue;
		}

		ret = tc300k_crc_check(data);
		if (ret) {
			dev_err(&client->dev, "[TK] %s crc check fail (%d)\n",
				__func__, retry);
			continue;
		}
		break;
	}

	if (retry > 0)
		dev_info(&client->dev, "%s success\n", __func__);

	t300k_release_fw(data, fw_path);

	return ret;
}

/*
 * Fw update by parameters:
 * s | S = TSK FW from kernel binary and compare fw version.
 * i | I = TSK FW from SD Card and Not compare fw version.
 * f | F = TSK FW from kernel binary and Not compare fw version.
 */
static ssize_t tc300k_update_store(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 fw_path;
	bool fw_update_force = false;

	switch(*buf) {
	case 's':
	case 'S':
		fw_path = FW_INKERNEL;
		break;
	case 'i':
	case 'I':
		fw_path = FW_SDCARD;
		break;
	case 'f':
	case 'F':
		fw_path = FW_INKERNEL;
		fw_update_force = true;
		break;
	default:
		dev_err(&client->dev, "[TK] %s wrong command fail\n", __func__);
		data->fw_update_status = TK_UPDATE_FAIL;
		return count;
	}

	data->fw_update_status = TK_UPDATE_DOWN;

	disable_irq(client->irq);
	ret = tc300k_fw_update(data, fw_path, fw_update_force);
	enable_irq(client->irq);
	if (ret < 0) {
		dev_err(&client->dev, "[TK] %s fail\n", __func__);
		data->fw_update_status = TK_UPDATE_FAIL;
	} else
		data->fw_update_status = TK_UPDATE_PASS;

	return count;
}

static ssize_t tc300k_firm_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int ret;

	if (data->fw_update_status == TK_UPDATE_PASS)
		ret = sprintf(buf, "PASS\n");
	else if (data->fw_update_status == TK_UPDATE_DOWN)
		ret = sprintf(buf, "DOWNLOADING\n");
	else if (data->fw_update_status == TK_UPDATE_FAIL)
		ret = sprintf(buf, "FAIL\n");
	else
		ret = sprintf(buf, "NG\n");

	return ret;
}

static ssize_t tc300k_firm_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "0x%02x\n", data->fw_ver_bin);
}

static ssize_t tc300k_firm_version_read_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;

	ret = tc300k_get_fw_version(data, false);
	if (ret < 0)
		dev_err(&client->dev, "[TK] %s: failed to read firmware version (%d)\n",
			__func__, ret);

	return sprintf(buf, "0x%02x\n", data->fw_ver);
}

static ssize_t recent_key_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->tsk_ic_num == TC350K_TSK_IC) {
		mutex_lock(&data->lock_fac);
		value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_CH_PER_DATA_OFFSET);
		mutex_unlock(&data->lock_fac);
	} else {
		ret = i2c_smbus_read_i2c_block_data(client, TC300K_2KEY_DATA, 6, buff);
		if (ret != 6) {
			dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
			return -1;
		}
		value = (buff[TC300K_CH_PCK_H_OFFSET] << 8) |
			buff[TC300K_CH_PCK_L_OFFSET];
	}
	return sprintf(buf, "%d\n", value);
}

static ssize_t recent_key_ref_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->sensing_ch_num < 6)
		return sprintf(buf, "%d\n", 0);

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_6KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_CH_PCK_H_OFFSET] << 8) |
			buff[TC300K_CH_PCK_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t back_key_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->tsk_ic_num == TC350K_TSK_IC) {
		mutex_lock(&data->lock_fac);
		value = read_tc350k_register_data(data, TC350K_2KEY, TC350K_CH_PER_DATA_OFFSET);
		mutex_unlock(&data->lock_fac);
	} else {
		ret = i2c_smbus_read_i2c_block_data(client, TC300K_1KEY_DATA, 6, buff);
		if (ret != 6) {
			dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
			return -1;
		}
		value = (buff[TC300K_CH_PCK_H_OFFSET] << 8) |
			buff[TC300K_CH_PCK_L_OFFSET];
	}
	return sprintf(buf, "%d\n", value);
}

static ssize_t back_key_ref_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->sensing_ch_num < 6)
		return sprintf(buf, "%d\n", 0);

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_5KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_CH_PCK_H_OFFSET] << 8) |
		buff[TC300K_CH_PCK_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t dummy_recent_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[6];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_4KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_CH_PCK_H_OFFSET] << 8) |
		buff[TC300K_CH_PCK_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t dummy_back_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[6];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_3KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_CH_PCK_H_OFFSET] << 8) |
		buff[TC300K_CH_PCK_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t recent_key_raw(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	dev_info(&client->dev, "[TK] %s called!\n", __func__);

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->tsk_ic_num == TC350K_TSK_IC) {
		mutex_lock(&data->lock_fac);
		value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_CH_RAW_DATA_OFFSET);
		mutex_unlock(&data->lock_fac);
	} else {
		ret = i2c_smbus_read_i2c_block_data(client, TC300K_2KEY_DATA, 6, buff);

		if (ret != 6) {
			dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
			return -1;
		}
		value = (buff[TC300K_RAW_H_OFFSET] << 8) |
			buff[TC300K_RAW_L_OFFSET];
	}
	return sprintf(buf, "%d\n", value);
}

static ssize_t recent_key_raw_ref(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->sensing_ch_num < 6)
		return sprintf(buf, "%d\n", 0);

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_6KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_RAW_H_OFFSET] << 8) |
		buff[TC300K_RAW_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t back_key_raw(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	dev_info(&client->dev, "[TK] %s called!\n", __func__);

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	if (data->pdata->tsk_ic_num == TC350K_TSK_IC) {
		mutex_lock(&data->lock_fac);
		value = read_tc350k_register_data(data, TC350K_2KEY, TC350K_CH_RAW_DATA_OFFSET);
		mutex_unlock(&data->lock_fac);
	} else {
		ret = i2c_smbus_read_i2c_block_data(client, TC300K_1KEY_DATA, 6, buff);
		if (ret != 6) {
			dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
			return -1;
		}
		value = (buff[TC300K_RAW_H_OFFSET] << 8) |
			buff[TC300K_RAW_L_OFFSET];
	}
	return sprintf(buf, "%d\n", value);
}

static ssize_t back_key_raw_ref(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[8];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}


	if (data->pdata->sensing_ch_num < 6)
		return sprintf(buf, "%d\n", 0);

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_5KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_RAW_H_OFFSET] << 8) |
		buff[TC300K_RAW_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t dummy_recent_raw(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[6];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_4KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_RAW_H_OFFSET] << 8) |
		buff[TC300K_RAW_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static ssize_t dummy_back_raw(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[6];
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	ret = i2c_smbus_read_i2c_block_data(client, TC300K_3KEY_DATA, 6, buff);
	if (ret != 6) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		return -1;
	}
	value = (buff[TC300K_RAW_H_OFFSET] << 8) |
		buff[TC300K_RAW_L_OFFSET];

	return sprintf(buf, "%d\n", value);
}

static int read_tc350k_register_data(struct tc300k_data *data, int read_key_num, int read_offset)
{
	struct i2c_client *client = data->client;
	int ret;
	u8 buff[2];
	int value;

	ret = i2c_smbus_read_i2c_block_data(client, read_key_num + read_offset, TC350K_DATA_SIZE, buff);
	if (ret != TC350K_DATA_SIZE) {
		dev_err(&client->dev, "[TK] %s read fail(%d)\n", __func__, ret);
		value = 0;
		goto exit;
	}
	value = (buff[TC350K_DATA_H_OFFSET] << 8) | buff[TC350K_DATA_L_OFFSET];

	dev_dbg(&client->dev, "[TK] %s : read key num/offset = [0x%X/0x%X], value : [%d]\n",
								__func__, read_key_num, read_offset, value);

exit:
	return value;
}

static ssize_t back_raw_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_2KEY, TC350K_CH_RAW_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_raw_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_4KEY, TC350K_CH_RAW_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_raw_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_CH_RAW_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_raw_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_3KEY, TC350K_CH_RAW_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_idac_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_2KEY, TC350K_CH_DIFF_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_idac_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_4KEY, TC350K_CH_DIFF_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_idac_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_CH_DIFF_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_idac_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_3KEY, TC350K_CH_DIFF_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_2KEY, TC350K_CH_PER_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_4KEY, TC350K_CH_PER_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_CH_PER_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_3KEY, TC350K_CH_PER_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_threshold_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_2KEY, TC350K_THRES_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t back_threshold_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_4KEY, TC350K_THRES_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_threshold_inner(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_1KEY, TC350K_THRES_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}
static ssize_t recent_threshold_outer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	int value;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&data->client->dev, "[TK] can't excute %s\n", __func__);
		return -1;
	}

	mutex_lock(&data->lock_fac);
	value = read_tc350k_register_data(data, TC350K_3KEY, TC350K_THRES_DATA_OFFSET);
	mutex_unlock(&data->lock_fac);

	return sprintf(buf, "%d\n", value);
}


static int tc300k_factory_mode_enable(struct i2c_client *client, u8 cmd)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, cmd);
	msleep(TC300K_CMD_DELAY);

	return ret;
}

static ssize_t tc300k_factory_mode(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		dev_err(&client->dev, "[TK] %s: cmd read err\n", __func__);
		return count;
	}

	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		dev_err(&client->dev, "[TK] %s: wrong command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (data->factory_mode == (bool)scan_buffer) {
		dev_info(&client->dev, "[TK] %s same command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (scan_buffer == 1) {
		dev_notice(&client->dev, "[TK] factory mode\n");
		cmd = TC300K_CMD_FAC_ON;
	} else {
		dev_notice(&client->dev, "[TK] normal mode\n");
		cmd = TC300K_CMD_FAC_OFF;
	}

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		data->factory_mode = (bool)scan_buffer;
		return count;
	}

	ret = tc300k_factory_mode_enable(client, cmd);
	if (ret < 0)
		dev_err(&client->dev, "[TK] %s fail(%d)\n", __func__, ret);

	data->factory_mode = (bool)scan_buffer;

	return count;
}

static ssize_t tc300k_factory_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", data->factory_mode);
}

static int tc300k_glove_mode_enable(struct i2c_client *client, u8 cmd)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, cmd);
	msleep(TC300K_CMD_DELAY);

	return ret;
}

static ssize_t tc300k_glove_mode(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		dev_err(&client->dev, "[TK] %s: cmd read err\n", __func__);
		return count;
	}
	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		dev_err(&client->dev, "[TK] %s: wrong command(%d)\n",
			__func__, scan_buffer);
		return count;
	}
	if (data->glove_mode == (bool)scan_buffer) {
		dev_info(&client->dev, "[TK] %s same command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (scan_buffer == 1) {
		dev_notice(&client->dev, "[TK] glove mode\n");
		cmd = TC300K_CMD_GLOVE_ON;
	} else {
		dev_notice(&client->dev, "[TK] normal mode\n");
		cmd = TC300K_CMD_GLOVE_OFF;

	}
	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		data->glove_mode = (bool)scan_buffer;
		return count;
	}

	if (data->flip_mode) {
		dev_info(&client->dev, "%s skip glove mode %d by flip enable", __func__, scan_buffer);
		data->glove_mode = (bool)scan_buffer;
		return count;
	}

	ret = tc300k_glove_mode_enable(client, cmd);
	if (ret < 0)
		dev_err(&client->dev, "[TK] %s fail(%d)\n", __func__, ret);
	data->glove_mode = (bool)scan_buffer;

	return count;
}

static ssize_t tc300k_glove_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", data->glove_mode);
}

static ssize_t tc300k_flip_mode(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		dev_err(&client->dev, "[TK] %s: cmd read err\n", __func__);
		return count;
	}
	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		dev_err(&client->dev, "[TK] %s: wrong command(%d)\n",
			__func__, scan_buffer);
		return count;
	}
	if (data->flip_mode == (bool)scan_buffer) {
		dev_info(&client->dev, "[TK] %s same command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (scan_buffer == 1) {
		dev_notice(&client->dev, "[TK] flip mode\n");
		cmd = TC300K_CMD_FLIP_ON;
	} else {
		dev_notice(&client->dev, "[TK] normal mode\n");
		cmd = TC300K_CMD_FLIP_OFF;

	}
	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		data->flip_mode = (bool)scan_buffer;
		return count;
	}

	if (!data->flip_mode){
		if (data->glove_mode)
			tc300k_glove_mode_enable(client, TC300K_CMD_GLOVE_ON);
		else
			tc300k_glove_mode_enable(client, TC300K_CMD_GLOVE_OFF);
	}

	ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, cmd);
	msleep(TC300K_CMD_DELAY);

	if (ret < 0)
		dev_err(&client->dev, "[TK] %s fail(%d)\n", __func__, ret);
	data->flip_mode = (bool)scan_buffer;

	return count;
}

static ssize_t tc300k_modecheck_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct tc300k_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int ret;
	u8 mode, glove, factory;

	if ((!data->enabled) || data->fw_downloding) {
		dev_err(&client->dev, "[TK] can't excute %s\n", __func__);
		return -EPERM;
	}

	ret = i2c_smbus_read_byte_data(client, TC300K_MODE);
	if (ret < 0) {
		dev_err(&client->dev, "[TK] %s: failed to read threshold_h (%d)\n",
			__func__, ret);
		return ret;
	}
	mode = ret;

	glove = ((mode & 0xf0) >> 4);
	factory = mode & 0x0f;

	return sprintf(buf, "glove:%d, factory:%d\n", glove, factory);
}

static DEVICE_ATTR(touchkey_threshold, S_IRUGO, tc300k_threshold_show, NULL);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		tc300k_led_control);
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, tc300k_update_store);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO,
		tc300k_firm_status_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO,
		tc300k_firm_version_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO,
		tc300k_firm_version_read_show, NULL);
static DEVICE_ATTR(touchkey_recent, S_IRUGO, recent_key_show, NULL);
static DEVICE_ATTR(touchkey_recent_ref, S_IRUGO, recent_key_ref_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO, back_key_show, NULL);
static DEVICE_ATTR(touchkey_back_ref, S_IRUGO, back_key_ref_show, NULL);
static DEVICE_ATTR(touchkey_d_menu, S_IRUGO, dummy_recent_show, NULL);
static DEVICE_ATTR(touchkey_d_back, S_IRUGO, dummy_back_show, NULL);
static DEVICE_ATTR(touchkey_recent_raw, S_IRUGO, recent_key_raw, NULL);
static DEVICE_ATTR(touchkey_recent_raw_ref, S_IRUGO, recent_key_raw_ref, NULL);
static DEVICE_ATTR(touchkey_back_raw, S_IRUGO, back_key_raw, NULL);
static DEVICE_ATTR(touchkey_back_raw_ref, S_IRUGO, back_key_raw_ref, NULL);
static DEVICE_ATTR(touchkey_d_menu_raw, S_IRUGO, dummy_recent_raw, NULL);
static DEVICE_ATTR(touchkey_d_back_raw, S_IRUGO, dummy_back_raw, NULL);

/* for tc350k */
static DEVICE_ATTR(touchkey_back_raw_inner, S_IRUGO, back_raw_inner, NULL);
static DEVICE_ATTR(touchkey_back_raw_outer, S_IRUGO, back_raw_outer, NULL);
static DEVICE_ATTR(touchkey_recent_raw_inner, S_IRUGO, recent_raw_inner, NULL);
static DEVICE_ATTR(touchkey_recent_raw_outer, S_IRUGO, recent_raw_outer, NULL);

static DEVICE_ATTR(touchkey_back_idac_inner, S_IRUGO, back_idac_inner, NULL);
static DEVICE_ATTR(touchkey_back_idac_outer, S_IRUGO, back_idac_outer, NULL);
static DEVICE_ATTR(touchkey_recent_idac_inner, S_IRUGO, recent_idac_inner, NULL);
static DEVICE_ATTR(touchkey_recent_idac_outer, S_IRUGO, recent_idac_outer, NULL);

static DEVICE_ATTR(touchkey_back_idac, S_IRUGO, back_idac_inner, NULL);
static DEVICE_ATTR(touchkey_recent_idac, S_IRUGO, recent_idac_inner, NULL);

static DEVICE_ATTR(touchkey_back_inner, S_IRUGO, back_inner, NULL);
static DEVICE_ATTR(touchkey_back_outer, S_IRUGO, back_outer, NULL);
static DEVICE_ATTR(touchkey_recent_inner, S_IRUGO, recent_inner, NULL);
static DEVICE_ATTR(touchkey_recent_outer, S_IRUGO, recent_outer, NULL);

static DEVICE_ATTR(touchkey_recent_threshold_inner, S_IRUGO, recent_threshold_inner, NULL);
static DEVICE_ATTR(touchkey_back_threshold_inner, S_IRUGO, back_threshold_inner, NULL);
static DEVICE_ATTR(touchkey_recent_threshold_outer, S_IRUGO, recent_threshold_outer, NULL);
static DEVICE_ATTR(touchkey_back_threshold_outer, S_IRUGO, back_threshold_outer, NULL);
/* end 350k */

static DEVICE_ATTR(touchkey_factory_mode, S_IRUGO | S_IWUSR | S_IWGRP,
		tc300k_factory_mode_show, tc300k_factory_mode);
static DEVICE_ATTR(glove_mode, S_IRUGO | S_IWUSR | S_IWGRP,
		tc300k_glove_mode_show, tc300k_glove_mode);
static DEVICE_ATTR(flip_mode, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, tc300k_flip_mode);

static DEVICE_ATTR(modecheck, S_IRUGO, tc300k_modecheck_show, NULL);

static struct attribute *sec_touchkey_attributes[] = {
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_recent_ref.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_back_ref.attr,
	&dev_attr_touchkey_d_menu.attr,
	&dev_attr_touchkey_d_back.attr,
	&dev_attr_touchkey_recent_raw.attr,
	&dev_attr_touchkey_recent_raw_ref.attr,
	&dev_attr_touchkey_back_raw.attr,
	&dev_attr_touchkey_back_raw_ref.attr,
	&dev_attr_touchkey_d_menu_raw.attr,
	&dev_attr_touchkey_d_back_raw.attr,
	&dev_attr_touchkey_factory_mode.attr,
	&dev_attr_glove_mode.attr,
	&dev_attr_modecheck.attr,
	NULL,
};

static struct attribute_group sec_touchkey_attr_group = {
	.attrs = sec_touchkey_attributes,
};

static struct attribute *sec_touchkey_attributes_350k[] = {
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,

	&dev_attr_touchkey_back_raw_inner.attr,
	&dev_attr_touchkey_back_raw_outer.attr,
	&dev_attr_touchkey_recent_raw_inner.attr,
	&dev_attr_touchkey_recent_raw_outer.attr,

	&dev_attr_touchkey_back_idac_inner.attr,
	&dev_attr_touchkey_back_idac_outer.attr,
	&dev_attr_touchkey_recent_idac_inner.attr,
	&dev_attr_touchkey_recent_idac_outer.attr,

	&dev_attr_touchkey_back_inner.attr,
	&dev_attr_touchkey_back_outer.attr,
	&dev_attr_touchkey_recent_inner.attr,
	&dev_attr_touchkey_recent_outer.attr,

	&dev_attr_touchkey_recent_threshold_inner.attr,
	&dev_attr_touchkey_back_threshold_inner.attr,
	&dev_attr_touchkey_recent_threshold_outer.attr,
	&dev_attr_touchkey_back_threshold_outer.attr,

	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_recent_raw.attr,
	&dev_attr_touchkey_back_raw.attr,
	&dev_attr_touchkey_recent_idac.attr,
	&dev_attr_touchkey_back_idac.attr,
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_glove_mode.attr,
	&dev_attr_flip_mode.attr,

	&dev_attr_touchkey_factory_mode.attr,
	&dev_attr_modecheck.attr,
	NULL,
};

static struct attribute_group sec_touchkey_attr_group_350k = {
	.attrs = sec_touchkey_attributes_350k,
};


static int tc300k_connecter_check(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;

	if (data->pdata->gpio_sub_det == 0) {
		dev_err(&client->dev, "%s: Not use sub_det pin\n", __func__);
		return SUB_DET_DISABLE;

	} else {
		if (gpio_get_value(data->pdata->gpio_sub_det)) {
			return SUB_DET_ENABLE_CON_OFF;
		} else {
			return SUB_DET_ENABLE_CON_ON;
		}

	}

}
static int tc300k_fw_check(struct tc300k_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	int tsk_connecter_status;

	tsk_connecter_status = tc300k_connecter_check(data);

	if (tsk_connecter_status == SUB_DET_ENABLE_CON_OFF) {
		dev_err(&client->dev, "%s : TSK IC is disconnected! skip probe(%d)\n",
						__func__, gpio_get_value(data->pdata->gpio_sub_det));
		return -1;
	}

	ret = tc300k_get_fw_version(data, true);

	if (ret < 0) {
		if (tsk_connecter_status == SUB_DET_ENABLE_CON_ON) {
			dev_err(&client->dev,
				"[TK] %s: i2c fail, But TSK IC is connected!\n", __func__);
			data->fw_ver = 0xFF;
		} else {
			dev_err(&client->dev,
				"[TK] %s: i2c fail...[%d], addr[%d]\n",
				__func__, ret, data->client->addr);
			dev_err(&client->dev,
				"[TK] %s: touchkey driver unload\n", __func__);
			return ret;
		}
	}

	if (data->fw_ver == 0xFF) {
		dev_notice(&client->dev,
			"[TK] fw version 0xFF, Excute firmware update!\n");
		ret = tc300k_fw_update(data, FW_INKERNEL, true);
		if (ret)
			return -1;
	} else {
		ret = tc300k_fw_update(data, FW_INKERNEL, false);
		if (ret)
			return -1;
	}

	return 0;
}

static int tc300_pinctrl_init(struct tc300k_data *data)
{
	struct device *dev = &data->client->dev;
	int i;
	dev_info(&data->client->dev, "%s\n",__func__);
	// IRQ
	data->pinctrl_irq = devm_pinctrl_get(dev);
	if (IS_ERR(data->pinctrl_irq)) {
		printk(KERN_DEBUG"%s: Failed to get irq pinctrl\n", __func__);
		data->pinctrl_irq = NULL;
		goto i2c_pinctrl_get;
	}
	for (i = 0; i < 2; ++i) {
		data->pin_state[i] = pinctrl_lookup_state(data->pinctrl_irq, str_states[i]);
		if (IS_ERR(data->pin_state[i])) {
			printk(KERN_DEBUG"%s: Failed to get irq pinctrl state\n", __func__);
			devm_pinctrl_put(data->pinctrl_irq);
			data->pinctrl_irq = NULL;
			goto i2c_pinctrl_get;
		}
	}

i2c_pinctrl_get:
	/* for h/w i2c */
	dev = data->client->dev.parent->parent;
	dev_err(&data->client->dev, "%s: use dev's parent\n", __func__);

	// I2C
	data->pinctrl_i2c = devm_pinctrl_get(dev);
	if (IS_ERR(data->pinctrl_i2c)) {
		dev_err(&data->client->dev, "%s: Failed to get i2c pinctrl\n", __func__);
		goto err_pinctrl_get_i2c;
	}
	for (i = 2; i < 4; ++i) {
		data->pin_state[i] = pinctrl_lookup_state(data->pinctrl_i2c, str_states[i]);
		if (IS_ERR(data->pin_state[i])) {
			dev_err(&data->client->dev, "%s: Failed to get i2c pinctrl state\n", __func__);
			goto err_pinctrl_get_state_i2c;
		}
	}
	return 0;

err_pinctrl_get_state_i2c:
	devm_pinctrl_put(data->pinctrl_i2c);
err_pinctrl_get_i2c:
	return -ENODEV;
}

static int tc300_pinctrl(struct tc300k_data *data, int state)
{
	struct pinctrl *pinctrl_i2c = data->pinctrl_i2c;
	struct pinctrl *pinctrl_irq = data->pinctrl_irq;
	int ret=0;

	switch (state) {
		case I_STATE_ON_IRQ:
		case I_STATE_OFF_IRQ:
			if (pinctrl_irq)
				ret = pinctrl_select_state(pinctrl_irq, data->pin_state[state]);
			break;
		case I_STATE_ON_I2C:
		case I_STATE_OFF_I2C:
			if (pinctrl_i2c)
				ret = pinctrl_select_state(pinctrl_i2c, data->pin_state[state]);
			break;
	}
	if (ret < 0) {
		dev_err(&data->client->dev,
		"%s: Failed to configure tc300_pinctrl state[%d]\n", __func__, state);
		return ret;
	}

	return 0;
}

static void tc300_config_gpio_i2c(struct tc300k_data *data, int onoff)
{
	printk(KERN_INFO "%s\n",__func__);

	tc300_pinctrl(data, onoff ? I_STATE_ON_I2C : I_STATE_OFF_I2C);
	mdelay(100);
}

#ifdef CONFIG_FB
static int fb_notifier_callback(struct notifier_block *self,
	unsigned long event, void *data);
#endif

static int __devinit tc300k_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct tc300k_platform_data *pdata;
	struct input_dev *input_dev;
	struct tc300k_data *data;
	int ret=0;
	int i=0;
	int err=0;
	printk(KERN_DEBUG "[TK] %s\n",__func__);

#ifdef CONFIG_BATTERY_SAMSUNG
	if (lpcharge == 1) {
		dev_err(&client->dev, "%s : Do not load driver due to : lpm %d\n",
			 __func__, lpcharge);
		return -ENODEV;
	}
#endif

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"[TK] i2c_check_functionality fail\n");
		return -EIO;
	}

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct tc300k_platform_data),
				GFP_KERNEL);
		if (!pdata) {
			dev_info(&client->dev, "[TK] Failed to allocate memory\n");
			goto err_alloc_data;
		}

		err = tc300k_parse_dt(&client->dev, pdata);
		if (err)
			goto err_alloc_data;
	}else
		pdata = client->dev.platform_data;

	data = kzalloc(sizeof(struct tc300k_data), GFP_KERNEL);
	if (!data) {
		dev_err(&client->dev, "[TK] Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_alloc_data;
	}

	data->pdata = pdata;

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&client->dev,
			"[TK] Failed to allocate memory for input device\n");
		ret = -ENOMEM;
		goto err_alloc_input;
	}

	data->client = client;
	data->input_dev = input_dev;

	if (data->pdata == NULL) {
		pr_err("[TK] failed to get platform data\n");
		ret = -EINVAL;
		goto err_platform_data;
	}
	data->irq = -1;
	mutex_init(&data->lock);
	mutex_init(&data->lock_fac);

	pdata->power = tc300k_touchkey_power;
	pdata->keyled = tc300k_touchkey_led_control;

	i2c_set_clientdata(client, data);
	tc300k_gpio_request(data);

	ret = tc300_pinctrl_init(data);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: Failed to init pinctrl: %d\n", __func__, ret);
		goto err_platform_data;
	}

	if(pdata->boot_on_ldo){
		data->pdata->power(true);
		data->pdata->keyled(true);
	} else {
		data->pdata->power(true);
		data->pdata->keyled(true);
		msleep(120);
	}
	data->enabled = true;

	client->irq=gpio_to_irq(pdata->gpio_int);

	ret = tc300k_fw_check(data);
	if (ret) {
		dev_err(&client->dev,
			"[TK] failed to firmware check(%d)\n", ret);
		goto err_fw_check;
	}

	snprintf(data->phys, sizeof(data->phys),
		"%s/input0", dev_name(&client->dev));
	input_dev->name = "sec_touchkey";
	input_dev->phys = data->phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;
	input_dev->open = tc300k_input_open;
	input_dev->close = tc300k_input_close;

	if (pdata->use_bitmap) {
		data->tsk_ev_val = tsk_ev;
		data->key_num = ARRAY_SIZE(tsk_ev)/2;
	} else {
		data->tsk_ev_val = tsk_ev_old;
		data->key_num = ARRAY_SIZE(tsk_ev_old)/2;
	}
	dev_info(&client->dev, "[TK] number of keys = %d\n", data->key_num);

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);
	for (i = 0; i < data->key_num; i++) {
		set_bit(data->tsk_ev_val[i].tsk_keycode, input_dev->keybit);
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
		dev_info(&client->dev, "[TK] keycode[%d]= %3d\n",
						i, data->tsk_ev_val[i].tsk_keycode);
#endif
	}
	input_set_drvdata(input_dev, data);

	ret = input_register_device(input_dev);
	if (ret) {
		dev_err(&client->dev, "[TK] fail to register input_dev (%d)\n",
			ret);
		goto err_register_input_dev;
	}

	ret = request_threaded_irq(client->irq, NULL, tc300k_interrupt,
				IRQF_DISABLED | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT, TC300K_NAME, data);
	if (ret < 0) {
		dev_err(&client->dev, "[TK] fail to request irq (%d).\n",
			pdata->gpio_int);
		goto err_request_irq;
	}
	data->irq = pdata->gpio_int;

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	data->early_suspend.suspend = tc300k_early_suspend;
	data->early_suspend.resume = tc300k_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

	data->sec_touchkey = sec_device_create(client, "sec_touchkey");
	if (IS_ERR(data->sec_touchkey))
		dev_err(&client->dev,
			"[TK] Failed to create device for the touchkey sysfs\n");

	if (data->pdata->tsk_ic_num == TC350K_TSK_IC) {
		ret = sysfs_create_group(&data->sec_touchkey->kobj,
			&sec_touchkey_attr_group_350k);
	} else {
		ret = sysfs_create_group(&data->sec_touchkey->kobj,
			&sec_touchkey_attr_group);
	}

	if (ret)
		dev_err(&client->dev, "[TK] Failed to create sysfs group\n");


	ret = sysfs_create_link(&data->sec_touchkey->kobj, &input_dev->dev.kobj, "input");
		if (ret < 0) {
			dev_err(&client->dev,
					"[TK] %s: Failed to create input symbolic link\n",
					__func__);
		}

#ifdef CONFIG_FB
	data->fb_notif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&data->fb_notif))
		pr_err("%s: could not create fb notifier\n", __func__);
#endif

	dev_set_drvdata(data->sec_touchkey, data);

	dev_info(&client->dev, "[TK] %s done\n", __func__);
	return 0;

err_request_irq:
	input_unregister_device(input_dev);
err_register_input_dev:
err_fw_check:
	mutex_destroy(&data->lock);
	mutex_destroy(&data->lock_fac);
	data->pdata->keyled(false);
	data->pdata->power(false);
err_platform_data:
	input_free_device(input_dev);
err_alloc_input:
	kfree(data);
err_alloc_data:
	return ret;
}

static int __devexit tc300k_remove(struct i2c_client *client)
{
	struct tc300k_data *data = i2c_get_clientdata(client);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif
	free_irq(client->irq, data);
	input_unregister_device(data->input_dev);
	input_free_device(data->input_dev);
#ifdef CONFIG_FB
	fb_unregister_client(&data->fb_notif);
#endif
	mutex_destroy(&data->lock);
	mutex_destroy(&data->lock_fac);
	data->pdata->keyled(false);
	data->pdata->power(false);
	gpio_free(data->pdata->gpio_int);
	gpio_free(data->pdata->gpio_sda);
	gpio_free(data->pdata->gpio_scl);
	kfree(data);

	return 0;
}

static void tc300k_shutdown(struct i2c_client *client)
{
	struct tc300k_data *data = i2c_get_clientdata(client);

	data->pdata->keyled(false);
	data->pdata->power(false);
}

#if defined(CONFIG_PM)
static int tc300k_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tc300k_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->lock);

	if (!data->enabled) {
		mutex_unlock(&data->lock);
		return 0;
	}

	dev_notice(&data->client->dev, "[TK] %s: users=%d\n",
		__func__, data->input_dev->users);

	disable_irq(client->irq);
	data->enabled = false;
	tc300k_release_all_fingers(data);
	data->pdata->keyled(false);
	data->pdata->power(false);
	data->led_on = false;

	mutex_unlock(&data->lock);

	return 0;
}

static int tc300k_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct tc300k_data *data = i2c_get_clientdata(client);
	int ret;
	u8 cmd;

	mutex_lock(&data->lock);
	if (data->enabled) {
		mutex_unlock(&data->lock);
		return 0;
	}
	dev_notice(&data->client->dev, "[TK] %s: users=%d\n", __func__, data->input_dev->users);

	data->pdata->power(true);
	data->pdata->keyled(true);
	msleep(120);
	enable_irq(client->irq);

	data->enabled = true;
	if (data->led_on == true) {
		data->led_on = false;
		dev_notice(&client->dev, "[TK] led on(resume)\n");
		cmd = TC300K_CMD_LED_ON;
		ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, cmd);
		if (ret < 0)
			dev_err(&client->dev, "%s led on fail(%d)\n", __func__, ret);
		else
			msleep(TC300K_CMD_DELAY);
	}

	if (data->glove_mode) {
		ret = tc300k_glove_mode_enable(client, TC300K_CMD_GLOVE_ON);
		if (ret < 0)
			dev_err(&client->dev, "[TK] %s glovemode fail(%d)\n", __func__, ret);
	}

	if (data->flip_mode) {
		ret = i2c_smbus_write_byte_data(client, TC300K_CMD_ADDR, TC300K_CMD_FLIP_ON);
		msleep(TC300K_CMD_DELAY);

		if (ret < 0)
			dev_err(&client->dev, "[TK] %s flipmode fail(%d)\n", __func__, ret);
	}

	if (data->factory_mode) {
		ret = tc300k_factory_mode_enable(client, TC300K_CMD_FAC_ON);
		if (ret < 0)
			dev_err(&client->dev, "[TK] %s factorymode fail(%d)\n", __func__, ret);
	}
	mutex_unlock(&data->lock);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void tc300k_early_suspend(struct early_suspend *h)
{
	struct tc300k_data *data;
	data = container_of(h, struct tc300k_data, early_suspend);
	tc300k_suspend(&data->client->dev);
}

static void tc300k_late_resume(struct early_suspend *h)
{
	struct tc300k_data *data;
	data = container_of(h, struct tc300k_data, early_suspend);
	tc300k_resume(&data->client->dev);
}
#endif

static void tc300k_input_close(struct input_dev *dev)
{
	struct tc300k_data *data = input_get_drvdata(dev);
	dev_info(&data->client->dev, "[TK] %s: users=%d\n", __func__,
		   data->input_dev->users);

	tc300k_suspend(&data->client->dev);
	tc300_pinctrl(data, I_STATE_OFF_IRQ);
}

static int tc300k_input_open(struct input_dev *dev)
{
	struct tc300k_data *data = input_get_drvdata(dev);

	dev_info(&data->client->dev, "[TK] %s: users=%d\n", __func__,
		   data->input_dev->users);

	tc300_pinctrl(data, I_STATE_ON_IRQ);
	tc300k_resume(&data->client->dev);

	return 0;
}
#endif /* CONFIG_PM */

#ifdef CONFIG_FB
static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	struct tc300k_data *tc_data = container_of(self, struct tc300k_data, fb_notif);

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		int *blank = evdata->data;
		switch (*blank) {
		case FB_BLANK_UNBLANK:
		case FB_BLANK_NORMAL:
		case FB_BLANK_VSYNC_SUSPEND:
		case FB_BLANK_HSYNC_SUSPEND:
			tc300k_input_open(tc_data->input_dev);
			break;
		case FB_BLANK_POWERDOWN:
			tc300k_input_close(tc_data->input_dev);
			break;
		default:
			/* Don't handle what we don't understand */
			break;
		}
	}

	return 0;
}
#endif

#if 0
#if defined(CONFIG_PM) || defined(CONFIG_HAS_EARLYSUSPEND)
static const struct dev_pm_ops tc300k_pm_ops = {
	.suspend = tc300k_suspend,
	.resume = tc300k_resume,
};
#endif
#endif

static const struct i2c_device_id tc300k_id[] = {
	{TC300K_NAME, 0},
	{ }
};

#ifdef CONFIG_OF
static struct of_device_id coreriver_match_table[] = {
	{ .compatible = "coreriver,tc300-keypad",},
	{ },
};
#else
#define coreriver_match_table	NULL
#endif
MODULE_DEVICE_TABLE(i2c, tc300k_id);

static struct i2c_driver tc300k_driver = {
	.probe = tc300k_probe,
	.remove = __devexit_p(tc300k_remove),
	.shutdown = tc300k_shutdown,
	.driver = {
		.name = TC300K_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(coreriver_match_table),
#endif
#if 0
#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
		.pm	= &tc370_pm_ops,
#endif
#endif
	},
	.id_table = tc300k_id,
};

static int __init tc300k_init(void)
{
	int ret = 0;

//#ifdef CONFIG_SAMSUNG_LPM_MODE
#if 0
	if (poweroff_charging) {
		pr_notice("%s : LPM Charging Mode!!\n", __func__);
		return 0;
	}
#endif

	ret = i2c_add_driver(&tc300k_driver);
	if (ret) {
		printk(KERN_ERR "[TK] coreriver touch keypad registration failed. ret= %d\n",
			ret);
	}
	printk(KERN_ERR "[TK] %s: init done %d\n", __func__, ret);

	return ret;
}

static void __exit tc300k_exit(void)
{
	i2c_del_driver(&tc300k_driver);
}

module_init(tc300k_init);
module_exit(tc300k_exit);

MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Touchkey driver for Coreriver TC300K");
MODULE_LICENSE("GPL");
