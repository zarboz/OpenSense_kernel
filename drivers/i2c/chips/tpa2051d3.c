/* driver/i2c/chip/tap2051d3.c
 *
 * TI tpa2051d3 Speaker Amp
 *
 * Copyright (C) 2010 HTC Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <mach/tpa2051d3.h>
#include <linux/mutex.h>

#include <linux/gpio.h>
#include <linux/mfd/pm8xxx/pm8921.h>


#ifdef CONFIG_AMP_TPA2051D3_ON_GPIO
#define DEBUG (1)
#else
#define DEBUG (0)
#endif
#define AMP_ON_CMD_LEN 7
#define RETRY_CNT 5
static struct i2c_client *this_client;
static struct tpa2051d3_platform_data *pdata;
static char *config_data;
static int tpa2051_mode_cnt;
struct mutex spk_amp_lock;
static int tpa2051d3_opened;
static int last_spkamp_state;
static char SPK_AMP_ON[] =
			{0x00, 0x82, 0x25, 0x57, 0x2D, 0xCD, 0x0D};
static char HEADSET_AMP_ON[] =
			{0x00, 0x8C, 0x25, 0x57, 0x73, 0x4D, 0x0D};
static char RING_AMP_ON[] =
			{0x00, 0x8E, 0x25, 0x57, 0x8D, 0xCD, 0x0D};
static char HANDSET_AMP_ON[] =
			{0x00, 0x82, 0x25, 0x57, 0x13, 0xCD, 0x0D};
static char BEATS_AMP_ON[] =
			{0x00, 0x8C, 0x25, 0x57, 0x73, 0x4D, 0x0D};
static char BEATS_AMP_OFF[] =
			{0x00, 0x8C, 0x25, 0x57, 0x73, 0x4D, 0x0D};
static char LINEOUT_AMP_ON[] =
			{0x00, 0x8C, 0x25, 0x57, 0x73, 0x4D, 0x0D};
static char AMP_0FF[] = {0x00, 0x90};

static int tpa2051_write_reg(u8 reg, u8 val)
{
	int err;
	struct i2c_msg msg[1];
	unsigned char data[2];

	msg->addr = this_client->addr;
	msg->flags = 0;
	msg->len = 2;
	msg->buf = data;
	data[0] = reg;
	data[1] = val;

	err = i2c_transfer(this_client->adapter, msg, 1);
	if (err >= 0)
		return 0;

	return err;
}

static int tpa2051_i2c_write(char *txData, int length)
{
	int i, retry, pass = 0;
	char buf[2];
	struct i2c_msg msg[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = 2,
		 .buf = buf,
		},
	};
	for (i = 0; i < length; i++) {
		if (i == 2)  /* According to tpa2051d3 Spec */
			mdelay(1);
		buf[0] = i;
		buf[1] = txData[i];
#if DEBUG
		pr_info("i2c_write %d=%x\n", i, buf[1]);
#endif
		msg->buf = buf;
		retry = RETRY_CNT;
		pass = 0;
		while (retry--) {
			if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
				pr_err("%s: I2C transfer error %d retry %d\n",
						__func__, i, retry);
				msleep(20);
			} else {
				pass = 1;
				break;
			}
		}
		if (pass == 0) {
			pr_err("I2C transfer error, retry fail\n");
			return -EIO;
		}
	}
	return 0;
}

static int tpa2051_i2c_write_for_read(char *txData, int length)
{
	int i, retry, pass = 0;
	char buf[2];
	struct i2c_msg msg[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = 2,
		 .buf = buf,
		},
	};
	for (i = 0; i < length; i++) {
		if (i == 2)  /* According to tpa2051 Spec */
			mdelay(1);
		buf[0] = i;
		buf[1] = txData[i];
#if DEBUG
		pr_info("i2c_write %d=%x\n", i, buf[1]);
#endif
		msg->buf = buf;
		retry = RETRY_CNT;
		pass = 0;
		while (retry--) {
			if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
				pr_err("%s: I2C transfer error %d retry %d\n",
						__func__, i, retry);
				msleep(20);
			} else {
				pass = 1;
				break;
			}
		}
		if (pass == 0) {
			pr_err("I2C transfer error, retry fail\n");
			return -EIO;
		}
	}
	return 0;
}

static int tpa2051_i2c_read(char *rxData, int length)
{
	int rc;
	struct i2c_msg msgs[] = {
		{
		 .addr = this_client->addr,
		 .flags = I2C_M_RD,
		 .len = length,
		 .buf = rxData,
		},
	};

	rc = i2c_transfer(this_client->adapter, msgs, 1);
	if (rc < 0) {
		pr_err("%s: transfer error %d\n", __func__, rc);
		return rc;
	}

#if DEBUG
	{
		int i = 0;
		for (i = 0; i < length; i++)
			pr_info("i2c_read %s: rx[%d] = %2x\n", __func__, i, \
				rxData[i]);
	}
#endif

	return 0;
}

static int tpa2051d3_open(struct inode *inode, struct file *file)
{
	int rc = 0;

	mutex_lock(&spk_amp_lock);

	if (tpa2051d3_opened) {
		pr_err("%s: busy\n", __func__);
		rc = -EBUSY;
		goto done;
	}
	tpa2051d3_opened = 1;
done:
	mutex_unlock(&spk_amp_lock);
	return rc;
}

static int tpa2051d3_release(struct inode *inode, struct file *file)
{
	mutex_lock(&spk_amp_lock);
	tpa2051d3_opened = 0;
	mutex_unlock(&spk_amp_lock);

	return 0;
}
void set_amp(int on, char *i2c_command)
{
	mutex_lock(&spk_amp_lock);
	if (on && !last_spkamp_state) {
		if (tpa2051_i2c_write(i2c_command, AMP_ON_CMD_LEN) == 0) {
			last_spkamp_state = 1;
			pr_info("%s: ON reg1=%x, reg2=%x\n",
				__func__, i2c_command[1], i2c_command[2]);
		}
	} else if (!on && last_spkamp_state) {
		if (tpa2051_i2c_write(AMP_0FF, sizeof(AMP_0FF)) == 0) {
			last_spkamp_state = 0;
			pr_debug("%s: OFF\n", __func__);
		}
	}
	mutex_unlock(&spk_amp_lock);
}

void set_speaker_amp(int on)
{
	set_amp(on, SPK_AMP_ON);
}

void set_headset_amp(int on)
{
	set_amp(on, HEADSET_AMP_ON);
}

void set_speaker_headset_amp(int on)
{
	set_amp(on, RING_AMP_ON);
}

void set_handset_amp(int on)
{
	set_amp(on, HANDSET_AMP_ON);
}

void set_usb_audio_amp(int on)
{
	set_amp(on, LINEOUT_AMP_ON);
}

void set_beats_on(int en)
{
	pr_aud_info("%s: %d\n", __func__, en);
	en = 1;
	pr_aud_info("BEATS HACK - %s: %d\n", __func__, en);
	mutex_lock(&spk_amp_lock);
	if (en) {
		tpa2051_i2c_write(BEATS_AMP_ON, AMP_ON_CMD_LEN);
		pr_info("%s: en(%d) reg_value[5]=%2x, reg_value[6]=%2x\n", __func__,  \
				en, BEATS_AMP_ON[5], BEATS_AMP_ON[6]);
	} else {
		tpa2051_i2c_write(BEATS_AMP_OFF, AMP_ON_CMD_LEN);
		pr_info("%s: en(%d)  reg_value[5]=%2x, reg_value[6]=%2x\n", __func__,  \
				en, BEATS_AMP_OFF[5], BEATS_AMP_OFF[6]);
	}
	mutex_unlock(&spk_amp_lock);
}

int update_amp_parameter(int mode)
{
	if (mode > tpa2051_mode_cnt)
		return EINVAL;
	if (*(config_data + mode * MODE_CMD_LEM + 1) == SPKR_OUTPUT)
		memcpy(SPK_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(SPK_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == HEADSET_OUTPUT)
		memcpy(BEATS_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(BEATS_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == DUAL_OUTPUT)
		memcpy(RING_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(RING_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == HANDSET_OUTPUT)
		memcpy(HANDSET_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(HANDSET_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == BEATS_ON_OUTPUT)
		memcpy(BEATS_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(BEATS_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == BEATS_OFF_OUTPUT)
		memcpy(BEATS_AMP_OFF, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(BEATS_AMP_OFF));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == LINEOUT_OUTPUT)
		memcpy(LINEOUT_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(LINEOUT_AMP_ON));
	else {
		pr_aud_info("wrong mode id %d\n", mode);
		return -EINVAL;
	}
	return 0;
}

static long tpa2051d3_ioctl(struct file *file, unsigned int cmd,
	   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int rc = 0, modeid = 0;
	unsigned char tmp[7];
	unsigned char reg_idx[1] = {0x01};
	unsigned char spk_cfg[8];
	unsigned char reg_value[2];
	struct tpa2051_config_data cfg;

	switch (cmd) {
	case TPA2051_WRITE_REG:
		pr_info("%s: TPA2051_WRITE_REG\n", __func__);
		mutex_lock(&spk_amp_lock);
		if (!last_spkamp_state) {
			/* According to tpa2051d3 Spec */
			mdelay(30);
		}
		if (copy_from_user(reg_value, argp, sizeof(reg_value)))
			goto err1;
		pr_info("%s: reg_value[0]=%2x, reg_value[1]=%2x\n", __func__,  \
				reg_value[0], reg_value[1]);
		rc = tpa2051_write_reg(reg_value[0], reg_value[1]);

err1:
		mutex_unlock(&spk_amp_lock);
		break;
	case TPA2051_SET_CONFIG:
		if (copy_from_user(spk_cfg, argp, sizeof(spk_cfg)))
			return -EFAULT;
		if (spk_cfg[0] == SPKR_OUTPUT)
			memcpy(SPK_AMP_ON, spk_cfg + 1,
					sizeof(SPK_AMP_ON));
		else if (spk_cfg[0] == HEADSET_OUTPUT)
			memcpy(HEADSET_AMP_ON, spk_cfg + 1,
					sizeof(HEADSET_AMP_ON));
		else if (spk_cfg[0] == DUAL_OUTPUT)
			memcpy(RING_AMP_ON, spk_cfg + 1,
					sizeof(RING_AMP_ON));
		else if (spk_cfg[0] == LINEOUT_OUTPUT)
			memcpy(LINEOUT_AMP_ON, spk_cfg + 1,
					sizeof(LINEOUT_AMP_ON));
		else
			return -EINVAL;
		break;
	case TPA2051_READ_CONFIG:
		mutex_lock(&spk_amp_lock);
		if (!last_spkamp_state) {
			/* According to tpa2051d3 Spec */
			mdelay(30);
		}

		rc = tpa2051_i2c_write_for_read(reg_idx, sizeof(reg_idx));
		if (rc < 0)
			goto err2;

		rc = tpa2051_i2c_read(tmp, sizeof(tmp));
		if (rc < 0)
			goto err2;

		if (copy_to_user(argp, &tmp, sizeof(tmp)))
			rc = -EFAULT;
err2:
		mutex_unlock(&spk_amp_lock);
		break;
	case TPA2051_SET_MODE:
		if (copy_from_user(&modeid, argp, sizeof(modeid)))
			return -EFAULT;

		if (modeid > tpa2051_mode_cnt || modeid <= 0) {
			pr_err("unsupported tpa2051 mode %d\n", modeid);
			return -EINVAL;
		}
		rc = update_amp_parameter(modeid);
		pr_info("set tpa2051 mode to %d\n", modeid);
		break;
	case TPA2051_SET_PARAM:
		cfg.cmd_data = 0;
		tpa2051_mode_cnt = 0;
		if (copy_from_user(&cfg, argp, sizeof(cfg))) {
			pr_err("%s: copy from user failed.\n", __func__);
			return -EFAULT;
		}

		if (cfg.data_len <= 0) {
			pr_err("%s: invalid data length %d\n",
					__func__, cfg.data_len);
			return -EINVAL;
		}
		if (config_data == NULL)
			config_data = kmalloc(cfg.data_len, GFP_KERNEL);
		if (!config_data) {
			pr_err("%s: out of memory\n", __func__);
			return -ENOMEM;
		}
		if (copy_from_user(config_data, cfg.cmd_data, cfg.data_len)) {
			pr_err("%s: copy data from user failed.\n", __func__);
			kfree(config_data);
			config_data = NULL;
			return -EFAULT;
		}
		tpa2051_mode_cnt = cfg.mode_num;
		pr_info("%s: update tpa2051 i2c commands #%d success.\n",
				__func__, cfg.data_len);
		/* update default paramater from csv*/
		update_amp_parameter(TPA2051_MODE_PLAYBACK_SPKR);
		update_amp_parameter(TPA2051_MODE_PLAYBACK_HEADSET);
		update_amp_parameter(TPA2051_MODE_RING);
		update_amp_parameter(TPA2051_MODE_PLAYBACK_HEADSET_BEATS_ON);
		update_amp_parameter(TPA2051_MODE_PLAYBACK_HEADSET_BEATS_OFF);
		update_amp_parameter(TPA2051_MODE_LINEOUT);
		rc = 0;
		break;
	default:
		pr_err("%s: Invalid command\n", __func__);
		rc = -EINVAL;
		break;
	}
	return rc;
}

static struct file_operations tpa2051d3_fops = {
	.owner = THIS_MODULE,
	.open = tpa2051d3_open,
	.release = tpa2051d3_release,
	.unlocked_ioctl = tpa2051d3_ioctl,
};

static struct miscdevice tpa2051d3_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "tpa2051d3",
	.fops = &tpa2051d3_fops,
};

int tpa2051d3_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;

	pdata = client->dev.platform_data;

	if (pdata == NULL) {
		pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
		if (pdata == NULL) {
			ret = -ENOMEM;
			pr_err("%s: platform data is NULL\n", __func__);
			goto err_alloc_data_failed;
		}
	}

	this_client = client;

	if (ret < 0) {
		pr_err("%s: pmic request aud_spk_en pin failed\n", __func__);
		goto err_free_gpio_all;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s: i2c check functionality error\n", __func__);
		ret = -ENODEV;
		goto err_free_gpio_all;
	}

	ret = misc_register(&tpa2051d3_device);
	if (ret) {
		pr_err("%s: tpa2051d3_device register failed\n", __func__);
		goto err_free_gpio_all;
	}

	if (pdata->spkr_cmd[1] != 0)  /* path id != 0 */
		memcpy(SPK_AMP_ON, pdata->spkr_cmd, sizeof(SPK_AMP_ON));
	if (pdata->hsed_cmd[1] != 0)
		memcpy(HEADSET_AMP_ON, pdata->hsed_cmd, sizeof(HEADSET_AMP_ON));
	if (pdata->rece_cmd[1] != 0)
		memcpy(HANDSET_AMP_ON, pdata->rece_cmd, sizeof(HANDSET_AMP_ON));

	return 0;

err_free_gpio_all:
	return ret;
err_alloc_data_failed:
	return ret;
}

static int tpa2051d3_remove(struct i2c_client *client)
{
	struct tpa2051d3_platform_data *p2051data = i2c_get_clientdata(client);
	kfree(p2051data);

	return 0;
}

static int tpa2051d3_suspend(struct i2c_client *client, pm_message_t mesg)
{
	return 0;
}

static int tpa2051d3_resume(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id tpa2051d3_id[] = {
	{ TPA2051D3_I2C_NAME, 0 },
	{ }
};

static struct i2c_driver tpa2051d3_driver = {
	.probe = tpa2051d3_probe,
	.remove = tpa2051d3_remove,
	.suspend = tpa2051d3_suspend,
	.resume = tpa2051d3_resume,
	.id_table = tpa2051d3_id,
	.driver = {
		.name = TPA2051D3_I2C_NAME,
	},
};

static int __init tpa2051d3_init(void)
{
	pr_info("%s\n", __func__);
	mutex_init(&spk_amp_lock);
	return i2c_add_driver(&tpa2051d3_driver);
}

static void __exit tpa2051d3_exit(void)
{
	i2c_del_driver(&tpa2051d3_driver);
}

module_init(tpa2051d3_init);
module_exit(tpa2051d3_exit);

MODULE_DESCRIPTION("tpa2051d3 Speaker Amp driver");
MODULE_LICENSE("GPL");
