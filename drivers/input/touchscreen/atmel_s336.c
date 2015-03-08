/*
 * Atmel maXTouch Touchscreen driver
 *
 * Copyright (C) 2010 Samsung Electronics Co.Ltd
 * Copyright (C) 2011-2012 Atmel Corporation
 * Copyright (C) 2012 Google, Inc.
 * Copyright (C) 2013 LG Electronics, Inc.
 *
 * Author: <WX-BSP-TS@lge.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/time.h>
#include <linux/wakelock.h>
#include <linux/mutex.h>
#include <linux/mfd/pm8xxx/cradle.h>
#include <linux/uaccess.h>
#include <linux/time.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/async.h>
#include "atmel_s336.h"
#include "atmel_s336_patch.h"

static struct mutex i2c_suspend_lock;
static struct mutex irq_lock;

static bool is_probing;
static bool selftest_enable;
static bool selftest_show;

static struct wake_lock touch_wake_lock;
static struct mxt_data *global_mxt_data;
static struct sys_device lge_touch_sys_device;
static struct workqueue_struct	*touch_wq = NULL;

static bool touch_irq_mask = 1;
static bool touch_irq_wake_mask = 0;
unsigned char touched_finger_count = 0;
static unsigned char patchevent_mask = 0;
static unsigned char power_block_mask = 0;

struct lge_touch_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct mxt_data *ts, char *buf);
	ssize_t (*store)(struct mxt_data *ts, const char *buf, size_t count);
};

#define LGE_TOUCH_ATTR(_name, _mode, _show, _store)	\
struct lge_touch_attribute lge_touch_attr_##_name = __ATTR(_name, _mode, _show, _store)

#define jitter_abs(x)	(x > 0 ? x : -x)
#define jitter_sub(x, y)	(x > y ? x - y : y - x)
#define get_time_interval(a,b) a>=b ? a-b : 1000000+a-b
#define ghost_value	(data->pdata->ghost_detection_value)
static struct timeval t_ex_debug[TIME_EX_PROFILE_MAX];

static int last_reported_case = 0xFF;

static u16 retry_chk = 0;
static int ghost_detection_check_count = 0;

static u8 ime_status_value = 0;
static u8 half_err_cnt = 0;
static u8 set_rtc_failure = 0;

static int mxt_soft_reset(struct mxt_data *data);
static int mxt_hw_reset(struct mxt_data *data);
static int mxt_t6_command(struct mxt_data *data, u16 cmd_offset, u8 value, bool wait);
static void mxt_reset_slots(struct mxt_data *data);
static void mxt_start(struct mxt_data *data);
static int mxt_set_t7_power_cfg(struct mxt_data *data, u8 sleep);
static void mxt_regulator_enable(struct mxt_data *data);
static void mxt_stop(struct mxt_data *data);
static int mxt_read_config_crc(struct mxt_data *data, u32 *crc);
static int mxt_command_backup(struct mxt_data *data, u8 value);

char *knockon_event[2] = { "TOUCH_GESTURE_WAKEUP=WAKEUP", NULL };

static int ghost_detect_solution(struct mxt_data *data, int finger_id)
{
	static struct timeval ghost_time_interval_check = {0,};

	if (data->incoming_call == INCOMING_CALL_RINGING && data->ts_data.total_num >= 2 ) {
		TOUCH_INFO_MSG("incoming_call num = %d \n", data->ts_data.total_num);
		goto ghost_err;
	}

	if (data->ts_data.total_num) {
		do_gettimeofday(&t_ex_debug[TIME_EX_CURR_INT_TIME]);
		// Ghost Case #5
		if ( t_ex_debug[TIME_EX_CURR_INT_TIME].tv_sec - t_ex_debug[TIME_EX_INIT_TIME].tv_sec < ghost_value[GHOST_DETECT_LIMIT_TIME]) {
			// Ghost Case #6
			#if 1
			if (data->ts_data.curr_data[finger_id].x_position < ghost_value[GHOST_DETECT_X] ||
				data->ts_data.curr_data[finger_id].x_position > data->pdata->lcd_x - ghost_value[GHOST_DETECT_X])
			#endif
			{
				if ( data->ts_data.curr_data[finger_id].pressure < ghost_value[LIMIT_OF_Z] &&
					data->ts_data.curr_data[finger_id].width_major <= 2 &&
					jitter_sub(data->ts_data.curr_data[finger_id].x_position,data->ts_data.prev_data[finger_id].x_position) <= ghost_value[JITTER_VALUE] &&
					jitter_sub(data->ts_data.curr_data[finger_id].y_position,data->ts_data.prev_data[finger_id].y_position) <= ghost_value[JITTER_VALUE]) {
					ghost_detection_check_count++;
				}

				if (ghost_detection_check_count > ghost_value[LIMIT_OF_COUNT]) {
					ghost_detection_check_count = 0;
					// Ghost Case #7
					if (last_reported_case != 7) {
						last_reported_case = 7;
						TOUCH_INFO_MSG("   Case #7 \n");
					}
					goto ghost_err;
				}

				if (ghost_detection_check_count == ghost_value[CAL_OF_TIME]) {
					// Ghost Case #9
					if (last_reported_case != 9) {
						last_reported_case = 9;
						TOUCH_INFO_MSG("   Case #9 (%d)\n", ghost_detection_check_count);
					}
					do_gettimeofday(&t_ex_debug[TIME_EX_GHOST_INT_TIME]);
					ghost_time_interval_check = t_ex_debug[TIME_EX_GHOST_INT_TIME];
				} else if (ghost_detection_check_count > ghost_value[CAL_OF_TIME] + 1 ) {
					if (last_reported_case != 10 || ghost_detection_check_count % 50 == 0) {
						last_reported_case = 10;
						TOUCH_INFO_MSG("   Case #10 (%d)\n", ghost_detection_check_count);
					}
					do_gettimeofday(&t_ex_debug[TIME_EX_GHOST_INT_TIME]);

					if ( t_ex_debug[TIME_EX_GHOST_INT_TIME].tv_sec - ghost_time_interval_check.tv_sec >= 4)
						ghost_detection_check_count = 0;
				}
			}

		}
	}

	return 0;

ghost_err:
	ghost_detection_check_count = 0;

	mxt_reset_slots(data);
	memset(&data->ts_data, 0, sizeof(data->ts_data));
return NEED_TO_OUT;

}

static int touch_enable_irq_wake(unsigned int irq)
{
	int ret = 0;

	mutex_lock(&irq_lock);

	if (!touch_irq_wake_mask) {
		touch_irq_wake_mask = 1;
		ret = enable_irq_wake(irq);
		if (ret != 0)
			TOUCH_INFO_MSG("%s : %d \n", __func__, ret);
	}

	mutex_unlock(&irq_lock);
	return ret;
}

static int touch_disable_irq_wake(unsigned int irq)
{
	int ret = 0;

	mutex_lock(&irq_lock);

	if (touch_irq_wake_mask) {
		touch_irq_wake_mask = 0;
		ret = disable_irq_wake(irq);
		if (ret != 0)
			TOUCH_INFO_MSG("%s : %d \n", __func__, ret);
	}

	mutex_unlock(&irq_lock);

	return ret;
}

static void touch_enable_irq(unsigned int irq)
{
	mutex_lock(&irq_lock);

	if (!touch_irq_mask) {
		touch_irq_mask = 1;
		enable_irq(irq);
	}

	mutex_unlock(&irq_lock);
}

static void touch_disable_irq(unsigned int irq)
{
	mutex_lock(&irq_lock);

	if (touch_irq_mask) {
		touch_irq_mask = 0;
		disable_irq_nosync(irq);
	}

	mutex_unlock(&irq_lock);
}

static char mxt_power_block_get(void)
{
	return power_block_mask;
}

static void mxt_power_block(char value)
{
	power_block_mask |= value;
}

static void mxt_power_unblock(char value)
{
	power_block_mask &= ~(value);
}

static char mxt_patchevent_get(char value)
{
	return patchevent_mask & value;
}

static void mxt_patchevent_set(char value)
{
	patchevent_mask |= value;
}

static void mxt_patchevent_unset(char value)
{
	patchevent_mask &= ~(value);
}

static inline size_t mxt_obj_size(const struct mxt_object *obj)
{
	return obj->size_minus_one + 1;
}

static inline size_t mxt_obj_instances(const struct mxt_object *obj)
{
	return obj->instances_minus_one + 1;
}

static bool mxt_object_readable(unsigned int type)
{
	switch (type) {
		case MXT_GEN_COMMAND_T6:
		case MXT_GEN_POWER_T7:
		case MXT_GEN_ACQUIRE_T8:
		case MXT_TOUCH_KEYARRAY_T15:
		case MXT_SPT_COMMSCONFIG_T18:
		case MXT_TOUCH_PROXIMITY_T23:
		case MXT_PROCI_ONETOUCH_T24:
		case MXT_SPT_SELFTEST_T25:
		case MXT_SPT_USERDATA_T38:
		case MXT_PROCI_GRIPSUPPRESSION_T40:
		case MXT_PROCI_TOUCHSUPPRESSION_T42:
		case MXT_SPT_CTECONFIG_T46:
		case MXT_PROCI_ADAPTIVETHRESHOLD_T55:
		case MXT_PROCI_SHIELDLESS_T56:
		case MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71:
		return true;
	default:
		return false;
	}
}

static void mxt_dump_message(struct mxt_data *data, u8 *message)
{
	print_hex_dump(KERN_ERR, "[Touch] MXT MSG:", DUMP_PREFIX_NONE, 16, 1,
		       message, data->T5_msg_size, false);
}

static int mxt_wait_for_completion(struct mxt_data *data,
			struct completion *comp, unsigned int timeout_ms)
{
	unsigned long timeout = msecs_to_jiffies(timeout_ms);
	long ret = 0;

	ret = wait_for_completion_interruptible_timeout(comp, timeout);
	if (ret < 0) {
		TOUCH_INFO_MSG("Wait for completion interrupted.\n");
		return -EINTR;
	} else if (ret == 0) {
		TOUCH_INFO_MSG("Wait for completion timed out.\n");
		return -ETIMEDOUT;
	}
	return 0;
}

static int mxt_bootloader_read(struct mxt_data *data, u8 *val, unsigned int count)
{
	int ret = 0;
	struct i2c_msg msg = {0};

	msg.addr = data->bootloader_addr;
	msg.flags = data->client->flags & I2C_M_TEN;
	msg.flags |= I2C_M_RD;
	msg.len = count;
	msg.buf = val;

	ret = i2c_transfer(data->client->adapter, &msg, 1);
	if (ret == 1) {
		ret = 0;
	} else {
		ret = (ret < 0) ? ret : -EIO;
		TOUCH_INFO_MSG("i2c recv failed (%d)\n", ret);
	}

	return ret;
}

static int mxt_bootloader_write(struct mxt_data *data, const u8 * const val, unsigned int count)
{
	int ret = 0;
	struct i2c_msg msg = {0};

	msg.addr = data->bootloader_addr;
	msg.flags = data->client->flags & I2C_M_TEN;
	msg.len = count;
	msg.buf = (u8 *)val;

	ret = i2c_transfer(data->client->adapter, &msg, 1);
	if (ret == 1) {
		ret = 0;
	} else {
		ret = (ret < 0) ? ret : -EIO;
		TOUCH_INFO_MSG("i2c send failed (%d)\n", ret);
	}

	return ret;
}

static int mxt_lookup_bootloader_address(struct mxt_data *data, u8 retry)
{
	u8 appmode = data->client->addr;
	u8 bootloader = 0;
	u8 family_id = 0;

	if (data->info)
		family_id = data->info->family_id;

	TOUCH_INFO_MSG("%s appmode=0x%x\n", __func__, appmode);

	switch (appmode) {
	case 0x4a:
	case 0x4b:
		/* Chips after 1664S use different scheme */
		if ((retry % 2) || family_id >= 0xa2) {
			bootloader = appmode - 0x24;
			break;
		}
		/* Fall through for normal case */
	case 0x4c:
	case 0x4d:
	case 0x5a:
	case 0x5b:
		bootloader = appmode - 0x26;
		break;
	default:
		TOUCH_INFO_MSG(
			"Appmode i2c address 0x%02x not found\n",
			appmode);
		return -EINVAL;
	}

	data->bootloader_addr = bootloader;
	TOUCH_INFO_MSG("%s bootloader_addr=0x%x\n", __func__, bootloader);
	return 0;
}

static int mxt_probe_bootloader(struct mxt_data *data, u8 retry)
{
	int ret = 0;
	u8 val = 0;
	bool crc_failure = false;

	TOUCH_INFO_MSG("%s\n", __func__);

	ret = mxt_lookup_bootloader_address(data, retry);
	if (ret)
		return ret;

	ret = mxt_bootloader_read(data, &val, 1);
	if (ret)
		return ret;

	/* Check app crc fail mode */
	crc_failure = (val & ~MXT_BOOT_STATUS_MASK) == MXT_APP_CRC_FAIL;

	TOUCH_INFO_MSG("Detected bootloader, status:%02X%s\n", val, crc_failure ? ", APP_CRC_FAIL" : "");

	return 0;
}

static u8 mxt_get_bootloader_version(struct mxt_data *data, u8 val)
{
	u8 buf[3] = {0};

	if (val & MXT_BOOT_EXTENDED_ID) {
		if (mxt_bootloader_read(data, &buf[0], 3) != 0) {
			TOUCH_INFO_MSG("%s: i2c failure\n", __func__);
			return -EIO;
		}

		TOUCH_INFO_MSG("Bootloader ID:%d Version:%d\n", buf[1], buf[2]);

		return buf[0];
	} else {
		TOUCH_INFO_MSG("Bootloader ID:%d\n", val & MXT_BOOT_ID_MASK);

		return val;
	}
}

static int mxt_check_bootloader(struct mxt_data *data, unsigned int state)
{
	u8 val = 0;
	int ret = 0;

recheck:
	ret = mxt_bootloader_read(data, &val, 1);
	if (ret)
		return ret;

	if (state == MXT_WAITING_BOOTLOAD_CMD)
		val = mxt_get_bootloader_version(data, val);

	switch (state) {
	case MXT_WAITING_BOOTLOAD_CMD:
	case MXT_WAITING_FRAME_DATA:
	case MXT_APP_CRC_FAIL:
		val &= ~MXT_BOOT_STATUS_MASK;
		break;
	case MXT_FRAME_CRC_PASS:
		if (val == MXT_FRAME_CRC_CHECK) {
			goto recheck;
		} else if (val == MXT_FRAME_CRC_FAIL) {
			TOUCH_INFO_MSG("Bootloader CRC fail\n");
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (val != state) {
		TOUCH_INFO_MSG("Invalid bootloader state %02X != %02X\n",
			val, state);
		return -EINVAL;
	}

	return 0;
}

static int mxt_send_bootloader_cmd(struct mxt_data *data, bool unlock)
{
	int ret = 0;
	u8 buf[2] = {0};

	TOUCH_INFO_MSG("%s : %d\n", __func__, unlock);

	if (unlock) {
		buf[0] = MXT_UNLOCK_CMD_LSB;
		buf[1] = MXT_UNLOCK_CMD_MSB;
	} else {
		buf[0] = 0x01;
		buf[1] = 0x01;
	}

	ret = mxt_bootloader_write(data, buf, 2);
	if (ret)
		return ret;

	return 0;
}

static int __mxt_read_reg(struct i2c_client *client, u16 reg, u16 len, void *val)
{
	struct i2c_msg xfer[2];
	u8 buf[2] = {0};
	int i = 0;

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;

	/* Write register */
	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = 2;
	xfer[0].buf = buf;

	/* Read data */
	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = val;

	do {
		if (i2c_transfer(client->adapter, xfer, ARRAY_SIZE(xfer))==2)
			return 0;
		TOUCH_INFO_MSG("%s: i2c retry %d\n", __func__, i+1);
		msleep(MXT_WAKEUP_TIME);
	} while (++i < 10);

	TOUCH_INFO_MSG("%s: i2c transfer failed\n", __func__);
	return -EIO;
}

static int __mxt_write_reg(struct i2c_client *client, u16 reg, u16 len, const void *val)
{
	u8 data[288] = {0};
	u8 *buf = NULL;
	size_t count = 0;
	int i = 0;
	bool alloced = false;
	int ret = 0;

	count = len + 2;

	if (unlikely(count > 288)) {
		buf = kzalloc(count, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		alloced = true;
	} else {
		buf = data;
	}

	buf[0] = reg & 0xff;
	buf[1] = (reg >> 8) & 0xff;
	memcpy(&buf[2], val, len);
	do {
		if (i2c_master_send(client, buf, count)==count) {
			ret = 0;
			goto out;
		}
		TOUCH_INFO_MSG("%s: i2c retry %d\n", __func__, i+1);
		msleep(MXT_WAKEUP_TIME);
	} while (++i < 10);
	TOUCH_INFO_MSG("%s: i2c transfer failed\n", __func__);
	ret = -EIO;

out :
	if (unlikely(alloced)) {
		kfree(buf);
	}

	return ret;
}

static int mxt_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	return __mxt_write_reg(client, reg, 1, &val);
}

struct mxt_object *mxt_get_object(struct mxt_data *data, u8 type)
{
	struct mxt_object *object = NULL;
	int i = 0;

	for (i = 0; i < data->info->object_num; i++) {
		object = data->object_table + i;
		if (object->type == type)
			return object;
	}

	TOUCH_INFO_MSG("Invalid object type T%u\n", type);
	return NULL;
}

int mxt_read_object(struct mxt_data *data, u8 type, u8 offset, u8 *val)
{
	struct mxt_object *object = NULL;
	int error = 0;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;

	error = __mxt_read_reg(data->client, object->start_address + offset, 1, val);
	if (error)
		TOUCH_INFO_MSG("Error to read T[%d] offset[%d] val[%d]\n",
				type, offset, *val);

	return error;
}

int mxt_write_object(struct mxt_data *data, u8 type, u8 offset, u8 val)
{
	struct mxt_object *object = NULL;
	int error = 0;
	u16 reg = 0;

	object = mxt_get_object(data, type);
	if (!object)
		return -EINVAL;
/*
	if (offset >= mxt_obj_size(object) * mxt_obj_instances(object)) {
		TOUCH_INFO_MSG("Tried to write outside object T%d offset:%d, size:%d\n",
				type, offset, mxt_obj_size(object));

		return -EINVAL;
	}

	TOUCH_INFO_MSG("mxt_write_object T[%d:%X] offset[%d] val[%d]\n",
			type, object->start_address, offset, val);
*/

	reg = object->start_address;
	error = __mxt_write_reg(data->client, reg + offset, 1, &val);
	if (error)
		TOUCH_INFO_MSG("Error to write T[%d] offset[%d] val[%d]\n",
				type, offset, val);

	return error;
}

static int mxt_set_diagnostic_mode(struct mxt_data *data, u8 dbg_mode)
{
	u8 cur_mode = 0;
	int ret = 0;
	int retry_cnt = 0;

	ret = mxt_write_object(data, MXT_GEN_COMMAND_T6/*MXT_GEN_COMMANDPROCESSOR_T6*/,
			MXT_COMMAND_DIAGNOSTIC, dbg_mode);

	if (ret) {
		TOUCH_INFO_MSG("Failed change diagnositc mode to %d\n",
			 dbg_mode);
		goto out;
	}

	if (dbg_mode & MXT_DIAG_MODE_MASK) {
		do {
			ret = mxt_read_object(data, MXT_DEBUG_DIAGNOSTIC_T37,
				MXT_DIAGNOSTIC_MODE, &cur_mode);
			if (ret || retry_cnt++ >= 4) {
				TOUCH_INFO_MSG("Failed getting diagnositc mode(%d)\n", retry_cnt);
				goto out;
			}
			msleep(20);
		} while (cur_mode != dbg_mode);
		TOUCH_INFO_MSG("current dianostic chip mode is %d\n", cur_mode);
	}

out:
	return ret;
}

static bool mxt_check_xy_range(struct mxt_data *data, u16 node)
{
	u8 x_line = node / data->info->matrix_ysize;
	u8 y_line = node % data->info->matrix_ysize;

	return (y_line < data->rawdata->num_ynode) ?
		(x_line < data->rawdata->num_xnode) : false;
}

static int mxt_treat_dbg_data(struct mxt_data *data,
	struct mxt_object *dbg_object, u8 dbg_mode, u8 read_point, u16 num)
{
	struct mxt_raw_data *rawdata = data->rawdata;
	u8 data_buffer[DATA_PER_NODE] = { 0 };
	int ret = 0;

	if (dbg_mode == MXT_DIAG_DELTA_MODE) {
		/* read delta data */
		__mxt_read_reg(data->client, dbg_object->start_address + read_point,
			DATA_PER_NODE, data_buffer);

		rawdata->delta[num] =
			((u16)data_buffer[1]<<8) + (u16)data_buffer[0];
		ret = rawdata->delta[num];
	} else if (dbg_mode == MXT_DIAG_REFERENCE_MODE) {
		/* read reference data */
		__mxt_read_reg(data->client, dbg_object->start_address + read_point,
			DATA_PER_NODE, data_buffer);

		rawdata->reference[num] =
			((u16)data_buffer[1] << 8) + (u16)data_buffer[0]
			- REF_OFFSET_VALUE;

#if 0
		TOUCH_INFO_MSG("reference[%d] = %d\n",
				num, rawdata->reference[num]);
#endif
		ret = rawdata->reference[num];
	}
	return ret;
}

static void mxt_treat_dbg_data_temp(struct mxt_data *data,
	struct mxt_object *dbg_object, u8 dbg_mode, u8 read_point, u16 num)
{
	struct mxt_raw_data *rawdata = data->rawdata;
	u8 data_buffer[DATA_PER_NODE] = { 0 };

	if (dbg_mode == MXT_DIAG_REFERENCE_MODE) {
		/* read reference data */
		__mxt_read_reg(data->client, dbg_object->start_address + read_point,
			DATA_PER_NODE, data_buffer);

		rawdata->reference[num] =
			((u16)data_buffer[1] << 8) + (u16)data_buffer[0]
			- REF_OFFSET_VALUE;

#if 0
		TOUCH_INFO_MSG("reference[%d] = %d\n", num, rawdata->reference[num]);
#endif
	}
}

static void mxt_prepare_debug_data(struct mxt_data *data)
{
	struct mxt_raw_data *rawdata = NULL;
	int error = 0;

	if (data->rawdata) {
		if (data->rawdata->reference) {
			kfree(data->rawdata->reference);
			data->rawdata->reference = NULL;
		}
		if (data->rawdata->delta) {
			kfree(data->rawdata->delta);
			data->rawdata->delta = NULL;
		}
		kfree(data->rawdata);
		data->rawdata = NULL;
	}

	rawdata = kzalloc(sizeof(struct mxt_raw_data), GFP_KERNEL);
	if (rawdata == NULL) {
		TOUCH_INFO_MSG("Fail to allocate sysfs data.\n");
		error = -ENOMEM;
		return ;
	}

	rawdata->num_xnode = data->max_x;
	rawdata->num_ynode = data->max_y;
	rawdata->num_nodes = rawdata->num_xnode * rawdata->num_ynode;

	TOUCH_INFO_MSG("%s: x=%d, y=%d, total=%d\n",
		__func__, rawdata->num_xnode, rawdata->num_ynode, rawdata->num_nodes);

	rawdata->reference = kzalloc(rawdata->num_nodes * sizeof(u16), GFP_KERNEL);
	if (!rawdata->reference) {
		TOUCH_INFO_MSG("Fail to alloc reference of rawdata\n");
		error = -ENOMEM;
		goto err_alloc_reference;
	}

	rawdata->delta = kzalloc(rawdata->num_nodes * sizeof(s16), GFP_KERNEL);
	if (!rawdata->delta) {
		TOUCH_INFO_MSG("Fail to alloc delta of fdata\n");
		error = -ENOMEM;
		goto err_alloc_delta;
	}

	data->rawdata = rawdata;
	return ;

err_alloc_delta:
err_alloc_reference:
	TOUCH_INFO_MSG("kfree in %s \n", __func__);
	if (rawdata->delta)
		kfree(rawdata->delta);
	if (rawdata->reference)
		kfree(rawdata->reference);
	if (rawdata)
		kfree(rawdata);
	return ;

}

static void mxt_prepare_debug_data_temp(struct mxt_data *data)
{
	struct mxt_raw_data *rawdata = NULL;
	int error = 0;

	if (data->rawdata) {
		if (data->rawdata->reference) {
			kfree(data->rawdata->reference);
			data->rawdata->reference = NULL;
		}
		if (data->rawdata->delta) {
			kfree(data->rawdata->delta);
			data->rawdata->delta = NULL;
		}
		kfree(data->rawdata);
		data->rawdata = NULL;
	}

	rawdata = kzalloc(sizeof(struct mxt_raw_data), GFP_KERNEL);
	if (rawdata == NULL) {
		TOUCH_INFO_MSG("Fail to allocate sysfs data.\n");
		error = -ENOMEM;
		return ;
	}

	rawdata->num_xnode = 2;
	rawdata->num_ynode = 14;
	rawdata->num_nodes = rawdata->num_xnode * rawdata->num_ynode;

	TOUCH_INFO_MSG("%s: x=%d, y=%d, total=%d\n",
		__func__, rawdata->num_xnode, rawdata->num_ynode, rawdata->num_nodes);

	rawdata->reference = kzalloc(rawdata->num_nodes * sizeof(u16), GFP_KERNEL);
	if (!rawdata->reference) {
		TOUCH_INFO_MSG("Fail to alloc reference of rawdata\n");
		error = -ENOMEM;
		goto err_alloc_reference;
	}
	data->rawdata = rawdata;

	return ;

err_alloc_reference:
	TOUCH_INFO_MSG("kfree in %s \n", __func__);
	if (rawdata->reference)
		kfree(rawdata->reference);
	if (rawdata)
		kfree(rawdata);

	return ;
}

static int mxt_get_cap_diff(struct mxt_data *data)
{
	int t_diff = 0;
	int b_diff = 0;
	int x = 0;
	int y = 0;
	int ret = 0;
	int i = 0;
	u8 buf_t71[MXT_PATCH_T71_DATA_MAX] = {0};
	s8 *cap_diff = NULL;
	u8 ref_new_chk = 0;
	u16 diff_butt_ref[4] = {0};
	struct mxt_object *object = NULL;
	u8 scaling_value = MXT_DEFAULT_SCALING_FACTOR;

	object = mxt_get_object(data, MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71);
	data->ref_limit.diff_scaling = scaling_value;

	/* Get config CRC from device */

	touch_disable_irq(data->irq);

	TOUCH_INFO_MSG("Write T71 CRC [%X]\n", data->fw_info.cfg_crc);
	ret = mxt_write_mem(data, object->start_address + 108, 4, (u8*)&data->fw_info.cfg_crc);

	/* allocation of cap_diff */
	cap_diff = NULL;

	cap_diff = kzalloc(data->info->matrix_ysize *sizeof(s8), GFP_KERNEL);
	if(!cap_diff)
		TOUCH_INFO_MSG("full_cap data allocation error\n");

	if(cap_diff != NULL) {
		data->bad_sample = 0;
		for(y = data->channel_size.start_y; y < data->channel_size.start_y + data->channel_size.size_y -1; y++) {
			t_diff = 0;
			for(x = data->channel_size.start_x; x < data->channel_size.start_x + data->channel_size.size_x; x++) {
				t_diff += (data->full_cap[x][y + 1] - data->full_cap[x][y]) / data->ref_limit.diff_scaling ;
			}

			t_diff = t_diff / data->channel_size.size_x;

			if(jitter_abs(t_diff * data->ref_limit.diff_scaling) > 1000) {
				data->bad_sample = 1;
			}

			if( t_diff < -0x7F) /* limit diff max */
				cap_diff[y + 1] = -0x7F;
			else if ( t_diff > 0x7F)
				cap_diff[y + 1] = 0x7F;
			else
				cap_diff[y + 1] = (s8)t_diff;
			TOUCH_INFO_MSG("cap_diff[%d] %d\n",y + 1 ,cap_diff[y + 1]);
		}
	}

	if (object!=NULL) {
		for( y = data->channel_size.start_y ; y < data->channel_size.start_y + data->channel_size.size_y; y++) {
			ret = mxt_write_mem(data, object->start_address + 60 + y, 1, (u8*)&cap_diff[y]);
		}
	}
	/* Scaling Factor save to ic */
	ret = mxt_write_mem(data, object->start_address + 106, 1, &scaling_value);
	if (ret) {
		TOUCH_INFO_MSG("scaling_value write fail \n");
	} else {
		TOUCH_INFO_MSG("scaling_value write %d \n", scaling_value);
	}

	/* Key Button write to device */
	if (object!=NULL && data->pdata->butt_check_enable) {
		for (i = 0; i < data->pdata->t15_num_keys-1; i++) {
			b_diff = (data->full_cap[data->pdata->t15_key_array_x[i+1]][data->pdata->t15_key_array_y[i+1]] - \
						data->full_cap[data->pdata->t15_key_array_x[i]][data->pdata->t15_key_array_y[i]]) / 8;
			TOUCH_INFO_MSG("diff_butt_ref(%d) %d prev[%d] cur[%d]\n", i, b_diff,
				data->full_cap[data->pdata->t15_key_array_x[i+1]][data->pdata->t15_key_array_y[i+1]],
						data->full_cap[data->pdata->t15_key_array_x[i]][data->pdata->t15_key_array_y[i]]);
			ret = mxt_write_mem(data, object->start_address + 74 + i, 1, (u8*)&b_diff);
			if (ret) {
				TOUCH_INFO_MSG("button diff mem write fail\n");
			}
		}
	}

	ret = mxt_read_mem(data, object->start_address + 60, 14, buf_t71);
	for( y = 0; y < 14; y++) {
		TOUCH_INFO_MSG("verify cap:diff Y%d Line, %d\n", y, (s8)buf_t71[y]);
	}
	if (data->pdata->butt_check_enable) {
		ret = mxt_read_mem(data, object->start_address + 74, data->pdata->t15_num_keys - 1, diff_butt_ref);
		for( i = 0; i < data->pdata->t15_num_keys - 1; i++) {
			TOUCH_INFO_MSG("verify button cap:diff %d, %d\n", i, (s8)diff_butt_ref[i]);
		}
	}

	// Manual Reference Check Uploading Byte Writing
	ref_new_chk = 1;
	ret = mxt_write_mem(data, object->start_address + 107, 1, &ref_new_chk);

	ret = mxt_command_backup(data, MXT_BACKUP_VALUE);
	TOUCH_INFO_MSG("backup return:%d\n", ret);

	if (cap_diff) {
		kfree(cap_diff);
		cap_diff = NULL;
	}

	touch_enable_irq(data->irq);
	return ret;
}

static int mxt_read_all_diagnostic_data(struct mxt_data *data, u8 dbg_mode, char *buf, int* len)
{
	struct mxt_object *dbg_object = NULL;
	u8 read_page = 0, cur_page = 0, end_page = 0, read_point = 0;
	u16 node = 0, num = 0,  cnt = 0;
	int ret = 0;
	int i = 0;
	int value = 0;
	u8 x_line = 0;
	u8 y_line = 0;


	touch_disable_irq(data->irq);
	wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));
	mutex_lock(&data->input_dev->mutex);

	/* to make the Page Num to 0 */
	ret = mxt_set_diagnostic_mode(data, MXT_DIAG_CTE_MODE);
	if (ret)
		goto out;

	/* change the debug mode */
	ret = mxt_set_diagnostic_mode(data, dbg_mode);
	if (ret)
		goto out;

	/* get object info for diagnostic */
	dbg_object = mxt_get_object(data, MXT_DEBUG_DIAGNOSTIC_T37);
	if (!dbg_object) {
		TOUCH_INFO_MSG("fail to get object_info\n");
		ret = -EINVAL;
		goto out;
	}

	//LGE
	mxt_prepare_debug_data(data);

	*len += snprintf(buf + *len, PAGE_SIZE - *len, "\n===============================================");
	*len += snprintf(buf + *len, PAGE_SIZE - *len, "===============================================");

	end_page = (data->info->matrix_xsize * data->info->matrix_ysize) / NODE_PER_PAGE;
	*len += snprintf(buf + *len , PAGE_SIZE - *len, "\n       ");
	for(i=0; i<data->info->matrix_ysize; i++)
		*len += snprintf(buf + *len , PAGE_SIZE - *len, "[Y%02d] ", i);


	*len += snprintf(buf + *len, PAGE_SIZE - *len, "\n===============================================");
	*len += snprintf(buf + *len, PAGE_SIZE - *len, "===============================================");

	/* read the dbg data */
	for (read_page = 0 ; read_page < end_page; read_page++) {
		for (node = 0; node < NODE_PER_PAGE; node++) {
			x_line = cnt / data->info->matrix_ysize;
			y_line = cnt % data->info->matrix_ysize;

			if (cnt%data->info->matrix_ysize == 0)
				*len += snprintf(buf + *len , PAGE_SIZE - *len, "\n[X%02d] ", cnt/data->info->matrix_ysize);
			read_point = (node * DATA_PER_NODE) + 2;

			if (!mxt_check_xy_range(data, cnt++)) {
				break;
			}

			value = mxt_treat_dbg_data(data, dbg_object, dbg_mode, read_point, num);

			*len += snprintf(buf + *len , PAGE_SIZE - *len, "%6d", value);

			if(dbg_mode == MXT_DIAG_REFERENCE_MODE && data->full_cap != NULL )
				data->full_cap[num / data->info->matrix_ysize][num % data->info->matrix_ysize] = value;

			num++;
		}
		ret = mxt_set_diagnostic_mode(data, MXT_DIAG_PAGE_UP);
		if (ret)
			goto out;
		do {
			msleep(20);
			ret = __mxt_read_reg(data->client,
				dbg_object->start_address + MXT_DIAGNOSTIC_PAGE, 1, &cur_page);
			if (ret) {
				TOUCH_INFO_MSG("%s Read fail page\n", __func__);
				goto out;
			}
		} while (cur_page != read_page + 1);
	}
	*len += snprintf(buf + *len, PAGE_SIZE - *len, "\n===============================================");
	*len += snprintf(buf + *len, PAGE_SIZE - *len, "===============================================\n");


out:
	mutex_unlock(&data->input_dev->mutex);

	touch_enable_irq(data->irq);

	return ret;
}

static int mxt_read_one_page_diagnostic_data(struct mxt_data *data, u8 dbg_mode)
{
	struct mxt_object *dbg_object = NULL;
	u8 read_point = 0;
	u16 node = 0, num = 0,  cnt = 0;
	int ret = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	/* to make the Page Num to 0 */
	ret = mxt_set_diagnostic_mode(data, MXT_DIAG_CTE_MODE);
	if (ret)
		goto out;

	/* change the debug mode */
	ret = mxt_set_diagnostic_mode(data, dbg_mode);
	if (ret)
		goto out;

	/* get object info for diagnostic */
	dbg_object = mxt_get_object(data, MXT_DEBUG_DIAGNOSTIC_T37);
	if (!dbg_object) {
		TOUCH_INFO_MSG("fail to get object_info\n");
		ret = -EINVAL;
		goto out;
	}

	//LGE
	mxt_prepare_debug_data_temp(data);

	/* read the dbg data */
	for (node = 0; node < 28; node++) {
		read_point = (node * DATA_PER_NODE) + 2;

		if (mxt_check_xy_range(data, cnt)) {
			mxt_treat_dbg_data_temp(data, dbg_object, dbg_mode, read_point, num);
			num++;
		}
		cnt++;
	}
out:
	return ret;
}

static int run_reference_read(void *device_data, char *buf, int *len)
{
	struct mxt_data *data = (struct mxt_data *)device_data;
	int ret = 0;

	ret = mxt_read_all_diagnostic_data(data, MXT_DIAG_REFERENCE_MODE, buf, len);

	return ret;
}

static int run_delta_read(void *device_data, char *buf, int *len)
{
	struct mxt_data *data = (struct mxt_data *)device_data;
	int ret = 0;

	ret = mxt_read_all_diagnostic_data(data, MXT_DIAG_DELTA_MODE, buf, len);
	return ret;
}

static void mxt_proc_t6_messages(struct mxt_data *data, u8 *msg)
{
	u8 status = msg[1];
	u32 crc = msg[2] | (msg[3] << 8) | (msg[4] << 16);
	static u8 t6_reset_cnt = 0;
	struct input_dev *input_dev = data->input_dev;

	if (crc != data->config_crc) {
		data->config_crc = crc;
		TOUCH_INFO_MSG("T6 Config Checksum: 0x%06X\n", crc);
		complete(&data->crc_completion);
	}

	/* Detect transition out of reset */
	if ((data->t6_status & MXT_T6_STATUS_RESET) && !(status & MXT_T6_STATUS_RESET))
		complete(&data->reset_completion);

	/* Output debug if status has changed */
	if (status != data->t6_status)
		TOUCH_INFO_MSG("T6 Status 0x%02X%s%s%s%s%s%s%s\n",
			status,
			(status == 0) ? " OK" : "",
			(status & MXT_T6_STATUS_RESET) ? " RESET" : "",
			(status & MXT_T6_STATUS_OFL) ? " OFL" : "",
			(status & MXT_T6_STATUS_SIGERR) ? " SIGERR" : "",
			(status & MXT_T6_STATUS_CAL) ? " CAL" : "",
			(status & MXT_T6_STATUS_CFGERR) ? " CFGERR" : "",
			(status & MXT_T6_STATUS_COMSERR) ? " COMSERR" : "");

	/* Save current status */
	data->t6_status = status;

	if (status == 0) {
		t6_reset_cnt = 0;
	}

	if (status & MXT_T6_STATUS_RESET) {
		t6_reset_cnt++;
	}

	if (status & MXT_T6_STATUS_CAL) {
		data->ref_chk = 1;
	}

	if (status == 0 && data->ref_chk && data->ref_limit.ref_chk_using && data->pdata->panel_on == POWER_ON) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(1000));

		mutex_lock(&input_dev->mutex);

		data->cal_cnt++;
		mxt_get_reference_chk(data);

		mutex_unlock(&input_dev->mutex);
	}

	/* Save current status */
	data->t6_status = status;

	if (status & MXT_T6_STATUS_CAL || status & MXT_T6_STATUS_RESET) {
		mxt_regulator_enable(data);
		mxt_reset_slots(data);
	}

	if (t6_reset_cnt == 1) {
		mxt_hw_reset(data);
	}

	if (t6_reset_cnt > 1 && status & MXT_T6_STATUS_RESET && global_mxt_data) {
		TOUCH_INFO_MSG("RESET Detected. Start Recover(%d) \n", data->ts_data.total_num);
		data->reset.total_num = data->ts_data.total_num;
		data->reset.state = HW_RESET_STATE;
		data->patch.src_item[MXT_PATCH_ITEM_USER7] = HW_RESET_STATE;

		if (mxt_patchevent_get(PATCH_EVENT_TA)) {
			TOUCH_INFO_MSG("   Stage 1 : USB/TA \n");
			mxt_patch_event(global_mxt_data, CHARGER_PLUGGED);
		}

		if (mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
			TOUCH_INFO_MSG("   Stage 2 : Knock On \n");
			if (mxt_patchevent_get(PATCH_EVENT_TA)) {
				mxt_patch_event(global_mxt_data, CHARGER_KNOCKON_SLEEP);
			} else {
				mxt_patch_event(global_mxt_data, NOCHARGER_KNOCKON_SLEEP);
			}
		}
		TOUCH_INFO_MSG("Recover Complete\n");
	}
}

static void mxt_input_sync(struct input_dev *input_dev)
{
	input_mt_report_pointer_emulation(input_dev, false);
	input_sync(input_dev);
}

static char *get_touch_button_string(u16 key_code)
{
	static char str[16] = {0};

	switch(key_code) {
		case KEY_BACK : /*158 0x9E*/
			sprintf(str, "BACK");
			break;
		case KEY_HOMEPAGE : /*172 0xAC*/
			sprintf(str, "HOME");
			break;
		case KEY_MENU : /* 139 0x8B*/
			sprintf(str, "MENU");
			break;
		case KEY_SIMSWITCH : /*249 0xF9*/
			sprintf(str, "SIM_SWITCH");
			break;
		default :
			sprintf(str, "Unknown");
			break;
	}

	return str;
}

static void mxt_button_lock_func(struct work_struct *work_button_lock)
{
	struct mxt_data *data = container_of(to_delayed_work(work_button_lock), struct mxt_data, work_button_lock);

	mutex_lock(&data->input_dev->mutex);
	data->button_lock = false;
	mutex_unlock(&data->input_dev->mutex);
}

static void mxt_palm_unlock_func(struct work_struct *work_palm_unlock)
{
	struct mxt_data *data = container_of(to_delayed_work(work_palm_unlock), struct mxt_data, work_palm_unlock);

	mutex_lock(&data->input_dev->mutex);
	data->palm = false;
	mutex_unlock(&data->input_dev->mutex);
}

static void mxt_deepsleep_func(struct work_struct *work_deepsleep)
{
	struct mxt_data *data = container_of(to_delayed_work(work_deepsleep), struct mxt_data, work_deepsleep);

	mutex_lock(&data->input_dev->mutex);
	mxt_set_t7_power_cfg(data, MXT_POWER_CFG_DEEPSLEEP);
	mutex_unlock(&data->input_dev->mutex);
}

int mxt_get_reference_chk(struct mxt_data *data)
{
	u16 buf[336] = {0};
	u16 num = 0;
	u16 end_page = 0;
	u8 cur_page = 0;
	u8 read_page = 0;
	struct mxt_object *dbg_object = NULL;
	int diff_v = 0;
	int diff_b_v = 0;
	u16 pre_v = 0;
	int err_cnt = 0;
	int ret = 0;
	int i = 0;
	u8 err_tot_x[24] = {0}, err_tot_y[14] = {0};
	u16 curx = 0, cury=0, ref_min=16384, ref_max=0, diff_max=0;
	u8 ret_buf[NODE_PER_PAGE * DATA_PER_NODE] = {0};
	u8 line_err_chk =0;
	u8 err_line_num =0;
	u8 loop_chk = 0;
	static u8 err_chk = 0;
	static int last_call_statue = 0;
	static u8 time_reset_chk_cnt = 0;
	static struct timeval previous_cal_interval_check = {0,};

	data->patch.src_item[MXT_PATCH_ITEM_USER6] = 0;
	data->ref_chk = 0;

	/* to make the Page Num to 0 */
	ret = mxt_set_diagnostic_mode(data, MXT_DIAG_CTE_MODE);

	if (ret)
		return 1;

	/* change the debug mode */
	ret = mxt_set_diagnostic_mode(data, MXT_DIAG_REFERENCE_MODE);
	if (ret)
		return 1;

	dbg_object = mxt_get_object(data, MXT_DEBUG_DIAGNOSTIC_T37);
	if (!dbg_object) {
		TOUCH_INFO_MSG("Failed to get object_info\n");
		return 1;
	}

	//mxt_prepare_debug_data(data);
	if (data->incoming_call == INCOMING_CALL_RINGING) {
		half_err_cnt = data->pdata->error_check_count[0];
		data->pdata->panel_on = POWER_OFF;
	} else if (data->incoming_call == INCOMING_CALL_OFFHOOK) {
		half_err_cnt = data->pdata->error_check_count[1];
		data->pdata->panel_on = POWER_OFF;
	} else if (last_call_statue == INCOMING_CALL_OFFHOOK && data->incoming_call == INCOMING_CALL_IDLE) {
		do_gettimeofday(&t_ex_debug[TIME_EX_INIT_TIME]);
		data->pdata->panel_on = POWER_ON;
	}

	last_call_statue = data->incoming_call;

	if(set_rtc_failure) {
		do_gettimeofday(&t_ex_debug[TIME_EX_INIT_TIME]);
		TOUCH_INFO_MSG("rtc_failure recovery [TIME_EX_INIT_TIME] = %ld",t_ex_debug[TIME_EX_INIT_TIME].tv_sec);
		set_rtc_failure = 0;
	}

	if (data->pdata->panel_on) {
		do_gettimeofday(&t_ex_debug[TIME_EX_FIRST_INT_TIME]);

		if (time_reset_chk_cnt >= data->pdata->time_reset_threshold) {
			TOUCH_INFO_MSG("timer reset(%d).\n", data->pdata->time_reset_threshold);
			time_reset_chk_cnt = 0;
			do_gettimeofday(&t_ex_debug[TIME_EX_INIT_TIME]);
			if (data->t72_noise_state == true) {
				do_gettimeofday(&t_ex_debug[TIME_EX_T72_NOISE_INT_TIME]);
			}
		}

		if ( t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec - t_ex_debug[TIME_EX_INIT_TIME].tv_sec <= 3 ) {
			if (t_ex_debug[TIME_EX_T72_NOISE_INT_TIME].tv_sec - t_ex_debug[TIME_EX_INIT_TIME].tv_sec == 0) {
				if (get_time_interval(t_ex_debug[TIME_EX_T72_NOISE_INT_TIME].tv_usec, t_ex_debug[TIME_EX_INIT_TIME].tv_usec) <= 30*1000) {
					TOUCH_INFO_MSG("data->t72_noise_state is changed to true.\n");
					data->t72_noise_state = true;
				}
			}

			if (data->t72_noise_state == true) {
				half_err_cnt = 1;
			} else {
				half_err_cnt = data->pdata->error_check_count[2];
			}
		} else if ( t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec - t_ex_debug[TIME_EX_INIT_TIME].tv_sec <= 7 ) {
			if (data->pdata->error_check_count[3])
				half_err_cnt = data->pdata->error_check_count[3];
			else
				half_err_cnt = data->ref_limit.ref_err_cnt/2;
		} else {

			if (t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec - t_ex_debug[TIME_EX_INIT_TIME].tv_sec >= 50000) {
				TOUCH_INFO_MSG("considered for the case set RTC failure\n");
				set_rtc_failure = 1;
			} else {

				if (data->pdata->error_check_count[4])
					half_err_cnt = data->pdata->error_check_count[4];
				else
					half_err_cnt = data->ref_limit.ref_err_cnt;
				}
		}

	}

	if (!half_err_cnt)
		half_err_cnt = 7;
	if (data->boot_mode != LGE_BOOT_MODE_NORMAL) {
		half_err_cnt = 1;
	}
	if (data->pdata->diff_scaling == 8 && half_err_cnt == 1) {
		half_err_cnt = 3;
	}

	end_page = (data->info->matrix_xsize * data->info->matrix_ysize) / NODE_PER_PAGE;
	for (read_page = 0 ; read_page < end_page; read_page++) {
		__mxt_read_reg(data->client, dbg_object->start_address + 2, NODE_PER_PAGE * DATA_PER_NODE, ret_buf);
		for (i = 0; i < NODE_PER_PAGE; i++) {
			buf[num] =( ((u16)ret_buf[i*2+1] << 8) + (u16)ret_buf[i*2] ) - REF_OFFSET_VALUE;

			if ((curx >= data->channel_size.start_x + data->channel_size.size_x) || (curx < data->channel_size.start_x) ||
					(cury >= data->channel_size.start_y + data->channel_size.size_y) || (cury < data->channel_size.start_y)) {
				num++;
				cury++;
				if (cury >= data->info->matrix_ysize) {
					curx++;
					cury = 0;
				}
				continue;
			} else {
				if (ref_min > buf[num]) {
					ref_min = buf[num];
				}

				if (ref_max < buf[num]) {
					ref_max = buf[num];
				}

				if (cury <= data->channel_size.start_y) {
					pre_v = buf[num];
				} else {
					diff_v = buf[num] - pre_v;
					//if (diff_v < 0) diff_v = diff_v * -1;
					if ((diff_v > (data->ref_limit.y_line_dif[cury] * global_mxt_data->ref_limit.diff_scaling) + (data->ref_limit.err_weight) * 8) ||
						(diff_v < (data->ref_limit.y_line_dif[cury] * global_mxt_data->ref_limit.diff_scaling) - (data->ref_limit.err_weight) * 8)) {
						TOUCH_INFO_MSG("Err Node(%d, %d) buf<%d> pre_v<%d> diff_v<%d> (ref_max - ref_min) <%d>",
							curx, cury, buf[num], pre_v, diff_v, (ref_max - ref_min));
						TOUCH_INFO_MSG("Weight Node(%d, %d, %d)\n",
							(int)data->ref_limit.y_line_dif[cury],
							(int)(data->ref_limit.y_line_dif[cury] * global_mxt_data->ref_limit.diff_scaling) + (data->ref_limit.err_weight) * 8,
							(int)(data->ref_limit.y_line_dif[cury] * global_mxt_data->ref_limit.diff_scaling) - (data->ref_limit.err_weight) * 8);
						err_cnt++;
						++err_tot_x[curx];
						++err_tot_y[cury];
					}
					pre_v = buf[num];

					if (diff_max < diff_v) {
						diff_max = diff_v;
						if (diff_max > (data->ref_limit.ref_diff_max * data->pdata->ref_reg_weight_val)) {
							retry_chk++;
							TOUCH_INFO_MSG("Diff max exceed (set : %d) (diff_max : %d)  retry_chk(%d)",
							(data->ref_limit.ref_diff_max * data->pdata->ref_reg_weight_val), diff_max, retry_chk);
							if (retry_chk >= 10) {
								err_chk = 1;
							} else {
								mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
								data->ref_chk = 1;
								return 0;
							}
						}
					}
				}
				num++;
				cury++;
				if (cury >= data->info->matrix_ysize) {
					curx++;
					cury = 0;
				}
			}

			if(num >= 335) break;

			// reference max-min check
			if ((ref_max - ref_min) > (data->ref_limit.ref_rng_limit * data->pdata->ref_reg_weight_val)) {
				retry_chk++;
				TOUCH_INFO_MSG("Reference Max - Min exceed (set : %d) (Max - Min : %d) retry_chk(%d)"
					, data->ref_limit.ref_rng_limit * data->pdata->ref_reg_weight_val, (ref_max - ref_min), retry_chk);
				if (retry_chk >= 10) {
					err_chk = 1;
				} else {
					mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
					data->ref_chk = 1;
					return 0;
				}
			}
		}

		ret = mxt_set_diagnostic_mode(data, MXT_DIAG_PAGE_UP);
		if (ret) {
			TOUCH_INFO_MSG("Failed to set diagnostic mode!\n");
			retry_chk = 0;
			return 1;
		}

		loop_chk = 0;
		do {
			msleep(20);
			ret = __mxt_read_reg(data->client,
				dbg_object->start_address + MXT_DIAGNOSTIC_PAGE, 1, &cur_page);
			if (ret  || loop_chk++ >= 4) {
				TOUCH_INFO_MSG("%s Read fail page(%d)\n", __func__, loop_chk);
				retry_chk = 0;
				return 1;
			}
		} while (cur_page != read_page + 1);
	}


	data->ref_chk = 0;
	//TOUCH_INFO_MSG("Err Cnt is %d, limit Cnt is %d\n",err_cnt, data->ref_limit.ref_err_cnt);
	TOUCH_INFO_MSG("Err Cnt is %d, limit Cnt is %d\n", err_cnt, half_err_cnt);
	data->patch.src_item[MXT_PATCH_ITEM_USER6] = err_cnt;
	data->err_node_cnt = err_cnt;
	if (err_chk == 1) {
		err_chk = 0;
		goto no_cal;
	}

	//processing for bad sample
	if (data->ref_limit.ref_x_all_err_line == 1) {
		for (i=0; i<data->info->matrix_xsize; i++) {
			if (err_tot_x[i] != 0) {
				line_err_chk++;
				err_line_num = i;
			}

			if (err_tot_x[i] >= data->channel_size.size_y - 1) {
				TOUCH_INFO_MSG("X%d Line all Fail, calibration skip error count(%d)\n", i, err_tot_x[i]);
				goto no_cal;
			}
		}
	}

	if (line_err_chk ==1 ) {
		if (err_tot_x[err_line_num] > data->ref_limit.xline_max_err_cnt) {
			TOUCH_INFO_MSG("X%d Line only Fail, calibration skip error count(%d), limit(%d)\n", i, err_tot_x[i], data->ref_limit.xline_max_err_cnt);
			goto no_cal;
		}
	}

	line_err_chk =0;
	err_line_num =0;

	if (data->ref_limit.ref_y_all_err_line == 1) {
		for (i = 0; i < data->info->matrix_ysize; i++) {
			if (err_tot_y[i] != 0) {
				line_err_chk++;
				err_line_num = i;
			}
			if (err_tot_y[i] >= data->channel_size.size_x - 1) {
				TOUCH_INFO_MSG("Y%d Line all Fail, calibration skip error count(%d)\n", i, err_tot_y[i]);
				goto no_cal;
			}
		}
	}

	if (line_err_chk ==1 ) {
		if (err_tot_y[err_line_num] > data->ref_limit.yline_max_err_cnt) {
			TOUCH_INFO_MSG("Y%d Line only Fail, calibration skip error count(%d), limit(%d)\n", i, err_tot_y[i], data->ref_limit.yline_max_err_cnt);
			goto no_cal;
		}
	}

	if (err_cnt >= data->pdata->time_reset_error_node_chk) {
		do_gettimeofday(&previous_cal_interval_check);
		TOUCH_INFO_MSG("time_reset_chk_cnt(%d) threashold(%d), Time Diff(%ld, %ld)\n", time_reset_chk_cnt, data->pdata->time_reset_error_node_chk,
			previous_cal_interval_check.tv_sec - t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec,
			get_time_interval(previous_cal_interval_check.tv_usec, t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_usec));

		if (previous_cal_interval_check.tv_sec - t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec == 0) {
			if (get_time_interval(previous_cal_interval_check.tv_usec, t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_usec) <= 500*1000) {
				TOUCH_INFO_MSG("time_reset_chk_cnt(%d) threashold(%d), Time Diff(%ld, %ld)\n", ++time_reset_chk_cnt, data->pdata->time_reset_error_node_chk,
					 t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec - previous_cal_interval_check.tv_sec,
					get_time_interval(previous_cal_interval_check.tv_usec, t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_usec));
			}
		} else if (previous_cal_interval_check.tv_sec - t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec == 1) {
			if (get_time_interval(previous_cal_interval_check.tv_usec + 1000*1000, t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_usec) <= 500*1000) {
				TOUCH_INFO_MSG("time_reset_chk_cnt(%d) threashold(%d), Time Diff(%ld, %ld)\n", ++time_reset_chk_cnt, data->pdata->time_reset_error_node_chk,
					previous_cal_interval_check.tv_sec - t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec,
					get_time_interval(previous_cal_interval_check.tv_usec + 1000*1000, t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_usec));
			}
		} else {
			time_reset_chk_cnt = 0;
			TOUCH_INFO_MSG("time_reset_chk_cnt(%d) threashold(%d), Time Diff(%ld, %ld)\n", ++time_reset_chk_cnt, data->pdata->time_reset_error_node_chk,
				previous_cal_interval_check.tv_sec - t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_sec,
				get_time_interval(previous_cal_interval_check.tv_usec, t_ex_debug[TIME_EX_FIRST_INT_TIME].tv_usec));
		}
	}


	if (err_cnt && (err_cnt >= half_err_cnt)) {
		TOUCH_INFO_MSG("Reference Err Calibration: %d\n",err_cnt);
	        mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
	        data->ref_chk = 1;
	}

	/* butten Hole check */
	if (data->pdata->butt_check_enable) {
		if (err_cnt < half_err_cnt) {
			//TOUCH_INFO_MSG("Check the Button Calibration\n");
			time_reset_chk_cnt = 0;
			data->t72_noise_state = false;
			set_rtc_failure = 0;
			for (i=0; i < data->pdata->t15_num_keys - 1; i++) {
				diff_b_v = buf[data->pdata->t15_key_array_x[i+1]*data->info->matrix_ysize + data->pdata->t15_key_array_y[i+1]] - \
						buf[data->pdata->t15_key_array_x[i]*data->info->matrix_ysize + data->pdata->t15_key_array_y[i]];

				/* button channel check */
				if (diff_b_v > 4000 || diff_b_v < -4000) {
					TOUCH_INFO_MSG("damaged key channel no calibration\n");
					err_chk = 0;
					goto no_cal;
				}

				if (diff_b_v > (data->ref_limit.butt_dif[i] + data->ref_limit.err_weight) * 8 ||
					diff_b_v < (data->ref_limit.butt_dif[i] - data->ref_limit.err_weight) * 8) {
					TOUCH_INFO_MSG("Prev[%d] Curr[%d] Diff[%d] Range : %d ~ %d\n",
						buf[data->pdata->t15_key_array_x[i+1]*data->info->matrix_ysize + data->pdata->t15_key_array_y[i+1]],
						buf[data->pdata->t15_key_array_x[i]*data->info->matrix_ysize + data->pdata->t15_key_array_y[i]],
						diff_b_v,
						(data->ref_limit.butt_dif[i] + data->ref_limit.err_weight) * 8,
						(data->ref_limit.butt_dif[i] - data->ref_limit.err_weight) * 8);

					TOUCH_INFO_MSG("%s Button Hole Occur!", get_touch_button_string(data->pdata->t15_keymap[i]));
					mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
					return 1;
				}
			}
		}
	}

	err_chk = 0;
	retry_chk = 0;
	return 0;

no_cal:
	time_reset_chk_cnt = 0;
	retry_chk = 0;
	return 1;
}

static void mxt_proc_t9_message(struct mxt_data *data, u8 *message)
{
	struct input_dev *input_dev = data->input_dev;
	int id = 0;
	u8 status = 0;
	int x = 0;
	int y = 0;
	int area = 0;
	int amplitude = 0;
	u8 vector = 0;
	int ret = 0;
	int i = 0;

	/* do not report events if input device not yet registered */
	if (!data->enable_reporting)
		return;

	id = message[0] - data->T9_reportid_min;
	status = message[1];
	x = (message[2] << 4) | ((message[4] >> 4) & 0xf);
	y = (message[3] << 4) | ((message[4] & 0xf));

	/* Handle 10/12 bit switching */
	if (data->max_x < 1024)
		x >>= 2;
	if (data->max_y < 1024)
		y >>= 2;

	area = message[5];
	amplitude = message[6];
	vector = message[7];

	if (unlikely( id < 0 || id >= MXT_MAX_FINGER)) {
		TOUCH_INFO_MSG("%s wrong id:%d \n", __func__, id);
		return;
	}

	if (status & MXT_T9_SUPPRESS) {
		if (touched_finger_count) {
			TOUCH_INFO_MSG(" MXT_T9_SUPPRESS\n");
			mxt_reset_slots(data);
		}
		if (data->palm) {
			cancel_delayed_work_sync(&data->work_palm_unlock);
		}
		data->palm = true;
		return;
	} else if (data->palm) {
		queue_delayed_work(touch_wq, &data->work_palm_unlock, msecs_to_jiffies(ghost_value[PALM_CAL_TIME]));
		return;
	}

	if (status & MXT_T9_DETECT) {
		if ((status & MXT_T9_PRESS || (status & MXT_T9_MOVE)) && data->ts_data.prev_data[id].status < MXT_STATE_PRESS) {
			if (data->reported_keycode) {
				TOUCH_INFO_MSG("KEY[%s:%d] is canceled\n", get_touch_button_string(data->reported_keycode), data->reported_keycode);
				input_report_key(input_dev, data->reported_keycode, 0xFF);
				data->reported_keycode = 0;
				return ;
			}

			//TOUCH_INFO_MSG("%d finger pressed <%d> : x[%3d] y[%3d] z[%3d] area[%3d]\n", ++touched_finger_count, id, x, y, amplitude, area);
		}

		if (data->reset.state == IDLE_STATE) {
			data->reset.x_position = x;
			data->reset.y_position = y;
		}

		data->fingers[id].state = MXT_STATE_PRESS;
		data->ts_data.curr_data[id].status = MXT_STATE_PRESS;

		input_mt_slot(input_dev, id);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 1);
		input_report_abs(input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(input_dev, ABS_MT_PRESSURE, amplitude);
		input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, area);
		input_report_abs(input_dev, ABS_MT_ORIENTATION, vector);

		data->button_lock = true;
	}

	if (status & MXT_T9_RELEASE) {
		if (touched_finger_count && data->ts_data.prev_data[id].status < MXT_STATE_PRESS) {
			for (i = MXT_MAX_FINGER - 1; i >= 0; i--) {
				if (data->ts_data.prev_data[i].status>= MXT_STATE_PRESS) {
					TOUCH_INFO_MSG("finger id changed <%d> -> <%d> \n", id, i);
					id = i;
					data->ts_data.prev_data[id].status = MXT_STATE_PRESS;
					break;
				}
			}
		}

		input_mt_slot(input_dev, id);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
//		TOUCH_INFO_MSG("touch_release    <%d> : x[%3d] y[%3d]\n", id, x, y);
		data->fingers[id].state = MXT_STATE_RELEASE;
		data->ts_data.curr_data[id].status= MXT_STATE_RELEASE;
		data->ts_data.curr_data[id].pressure = 0;
		if (touched_finger_count)
			data->ts_data.total_num = --touched_finger_count;

		if (!touched_finger_count) {
			if (data->reset.state == HW_RESET_STATE && data->reset.total_num) {
				if (jitter_sub(data->reset.x_position, x) > 150 || jitter_sub(data->reset.y_position,y) > 150) {
					TOUCH_INFO_MSG("finger all released and HW reset detection(%d, %d) (%d, %d)\n",\
						data->reset.x_position, x, data->reset.y_position, y);
					memset(&data->ts_data, 0, sizeof(data->ts_data));
					mxt_reset_slots(data);
					data->patch.src_item[MXT_PATCH_ITEM_USER7] = IDLE_STATE;
					data->reset.state = IDLE_STATE;
					goto out;
				}
			}
			queue_delayed_work(touch_wq, &data->work_button_lock, msecs_to_jiffies(200));
		}
	}

	data->ts_data.curr_data[id].x_position = x;
	data->ts_data.curr_data[id].y_position = y;
	data->ts_data.curr_data[id].pressure = amplitude;
	data->ts_data.curr_data[id].width_major = area;
	data->ts_data.curr_data[id].width_orientation = vector;
	data->ts_data.total_num = touched_finger_count;

	/* Ghost detection solution */
	if (data->pdata->ghost_detection_enable) {
		ret = ghost_detect_solution(data, id);
		if (ret == NEED_TO_OUT)
			goto out;
	}

	data->ts_data.prev_data[id]= data->ts_data.curr_data[id];
	data->ts_data.prev_total_num = data->ts_data.total_num;
	data->update_input = true;

	return ;

out:
	mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
	mxt_reset_slots(data);
	return;
}

static void mxt_proc_t15_messages(struct mxt_data *data, u8 *msg)
{
	struct input_dev *input_dev = data->input_dev;
	unsigned long keystates = le32_to_cpu(msg[2]);
	u16 keycode = 0;
	int i = 0;

	/* do not report events if input device not yet registered */
	if (!data->enable_reporting)
		return;

	if ((touched_finger_count || data->button_lock || data->palm)) {
		return;
	}

	if (data->reported_keycode == 0 && keystates == 0)
		return;

	for (i = 0; i < data->pdata->t15_num_keys; i++) {
		if (keystates == data->pdata->t15_keystate[i]) {
			keycode = data->pdata->t15_keymap[i];
			break;
		}
	}

	if (data->reported_keycode && keystates == 0) {
		TOUCH_INFO_MSG("KEY[%s:%d] is released\n", get_touch_button_string(data->reported_keycode), data->reported_keycode);
		input_report_key(input_dev, data->reported_keycode, 0);
		data->reported_keycode = 0;
	} else if (data->reported_keycode == 0 && keystates) {
		TOUCH_INFO_MSG("KEY[%s:%d] is pressed\n", get_touch_button_string(keycode), keycode);
		input_report_key(input_dev, keycode, 1);
		data->reported_keycode = keycode;
	}

	input_sync(input_dev);

}

static void mxt_proc_t42_messages(struct mxt_data *data, u8 *msg)
{
	u8 status = msg[1];

	if (status & MXT_T42_MSG_TCHSUP) {
		TOUCH_INFO_MSG("Palm detected %d\n", touched_finger_count);
		data->button_lock = true;
	} else {
		TOUCH_INFO_MSG("Palm released \n");
		queue_delayed_work(touch_wq, &data->work_button_lock, msecs_to_jiffies(200));
	}
	mxt_reset_slots(data);
}

static int mxt_proc_t48_messages(struct mxt_data *data, u8 *msg)
{
	u8 status = 0, state = 0;

	status = msg[1];
	state  = msg[4];

	TOUCH_INFO_MSG("T48 state %d status %02X %s%s%s%s%s\n",
			state,
			status,
			(status & 0x01) ? "FREQCHG " : "",
			(status & 0x02) ? "APXCHG " : "",
			(status & 0x04) ? "ALGOERR " : "",
			(status & 0x10) ? "STATCHG " : "",
			(status & 0x20) ? "NLVLCHG " : "");

	return 0;
}

static void mxt_proc_t57_messages(struct mxt_data *data, u8 *message)
{
	u16 area = 0;
	u16 touch_area = 0;
	u16 anti_touch_area = 0;

	area =				(message[2] << 8 | message[1]);
	touch_area =			(message[4] << 8 | message[3]);
	anti_touch_area =		(message[6] << 8 | message[5]);
/*
	if (data->t57_debug_enabled || data->ref_chk) {
		TOUCH_INFO_MSG("T57 :%3d %3d %3d\n", area, touch_area, anti_touch_area);
	}
*/
	if (data->cal_cnt > 5 && anti_touch_area >= 1 && \
		1 <= data->patch.src_item[MXT_PATCH_ITEM_USER6] && data->patch.src_item[MXT_PATCH_ITEM_USER6] < half_err_cnt) {
		data->t72_noise_state = true;
		data->cal_cnt = 0;
		mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
	}
}

static int mxt_proc_t25_message(struct mxt_data *data, u8 *message)
{
	u8 status = message[1];

	if (!selftest_enable)
		return 0;

	TOUCH_INFO_MSG("T25 Self Test completed %u\n",status);

	memset(data->self_test_status, 0, sizeof(data->self_test_status));

	if (selftest_show)
		data->self_test_status[0] = status;

	if ( status == 0xFE ) {
		TOUCH_INFO_MSG("[SUCCESS] All tests passed\n");
		data->self_test_result = true;
	} else {
		if (status == 0xFD) {
			TOUCH_INFO_MSG("[FAIL] Invalid test code\n");
		} else if (status == 0xFC)  {
			TOUCH_INFO_MSG("[FAIL] Unrelated fault\n");
		} else if (status == 0x01) {
			TOUCH_INFO_MSG("[FAIL] AVdd or XVdd is not present\n");
		} else if (status == 0x12) {
			TOUCH_INFO_MSG("[FAIL] Pin fault (SEQ_NUM %u, X_PIN %u, Y_PIN %u)\n", message[2], message[3], message[4]);
			if (selftest_show) {
				data->self_test_status[1] = message[2];
				data->self_test_status[2] = message[3];
				data->self_test_status[3] = message[4];
			}
		} else if (status == 0x17) {
			TOUCH_INFO_MSG("[FAIL] Signal limit fault (TYPE_NUM %u, TYPE_INSTANCE %u)\n", message[2], message[3]);
			if (selftest_show) {
				data->self_test_status[1] = message[2];
				data->self_test_status[2] = message[3];
			}
		} else;
		data->self_test_result = false;
	}

	selftest_enable = false;
	complete(&data->t25_completion);
	return 0;
}

static void mxt_proc_t24_messages(struct mxt_data *data, u8 *message)
{
	u8 msg = 0;
	int x = 0;
	int y = 0;

        struct input_dev *input_dev = data->input_dev;

	if (data->in_bootloader)
		return;

	msg = message[1];

	x = (message[2] << 4) | ((message[4] >> 4) & 0xf);
	y = (message[3] << 4) | ((message[4] & 0xf));

	/* Handle 10/12 bit switching */
	if (data->max_x < 1024)
		x >>= 2;
	if (data->max_y < 1024)
		y >>= 2;

	if (msg == 0x04) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));
		TOUCH_INFO_MSG("Knock On detected x[%3d] y[%3d] \n", x, y);
		kobject_uevent_env(&lge_touch_sys_device.kobj, KOBJ_CHANGE, knockon_event);
		input_report_key(input_dev, KEY_DOUBLE_TAP, 1);
		input_report_key(input_dev, KEY_DOUBLE_TAP, 0);
		input_sync(input_dev);
	} else {
		TOUCH_INFO_MSG("%s msg = %d \n", __func__, msg);
	}
}

static int mxt_update_file_name(struct device *dev, char **file_name, const char *buf, size_t count)
{
	char *file_name_tmp = NULL;

	/* Simple sanity check */
	if (count > 128) {
		TOUCH_INFO_MSG("File name too long %d\n", count);
		return -EINVAL;
	}

	file_name_tmp = krealloc(*file_name, count + 1, GFP_KERNEL);
	if (!file_name_tmp) {
		TOUCH_INFO_MSG("no memory\n");
		return -ENOMEM;
	}

	*file_name = file_name_tmp;
	memcpy(*file_name, buf, count);

	/* Echo into the sysfs entry may append newline at the end of buf */
	if (buf[count - 1] == '\n')
		(*file_name)[count - 1] = '\0';
	else
		(*file_name)[count] = '\0';

	return 0;
}

static ssize_t mxt_update_patch_store(struct mxt_data *data, const char *buf, size_t count)
{
	u8 *patch_data = NULL;
	const struct firmware *fw = NULL;
	char *name = NULL;
	int ret = 0;

	ret = mxt_update_file_name(&data->client->dev, &name, buf, count);
	if (ret) {
		TOUCH_INFO_MSG("%s error patch name [%s] \n", __func__, name);
		goto out;
	}

	if (request_firmware(&fw, name, &data->client->dev) >= 0) {
		patch_data = kzalloc(fw->size, GFP_KERNEL);
		if (!patch_data) {
			TOUCH_INFO_MSG("Failed to alloc buffer for fw\n");
			ret = -ENOMEM;
			goto out;
		}
		memcpy(patch_data, fw->data, fw->size);
		if (data->patch.patch) {
			kfree(data->patch.patch);
			data->patch.patch = NULL;
		}
		data->patch.patch = patch_data;
	} else {
		TOUCH_INFO_MSG("Fail to request firmware(%s)\n", name);
		goto out;
	}
	TOUCH_INFO_MSG("%s patch file size:%d\n", __func__, fw->size);
	TOUCH_INFO_MSG("%s ppatch:%p %p\n", __func__, patch_data, data->patch.patch);

	ret = mxt_patch_init(data, data->patch.patch);
	if (ret == 0) {
		global_mxt_data = data;
	} else {
		global_mxt_data = NULL;
		TOUCH_INFO_MSG("%s global_mxt_data is NULL \n", __func__);
	}
	release_firmware(fw);

	return 1;

out:
	if (patch_data) {
		kfree(patch_data);
	}
	data->patch.patch = NULL;

	return 1;
}

void trigger_usb_state_from_otg(int usb_type)
{

	if (global_mxt_data && global_mxt_data->patch.event_cnt) {

		if (global_mxt_data->mfts_enable && global_mxt_data->pdata->use_mfts) {
			TOUCH_INFO_MSG("MFTS : Not support USB trigger \n");
			return;
		}

		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));

		if (mutex_is_locked(&i2c_suspend_lock)) {
			TOUCH_INFO_MSG("%s mutex_is_locked \n", __func__);
		}

		mutex_lock(&i2c_suspend_lock);
		if (usb_type == 0) {
			if (mxt_patchevent_get(PATCH_EVENT_TA)) {
				if (mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
					mxt_patchevent_unset(PATCH_EVENT_KNOCKON);
					mxt_patch_event(global_mxt_data, CHARGER_KNOCKON_WAKEUP);
				} else {
					if (global_mxt_data->suspended == true) {
						if (global_mxt_data->work_deepsleep_enabled) {
							cancel_delayed_work_sync(&global_mxt_data->work_deepsleep);
						}
						global_mxt_data->work_deepsleep_enabled = true;
						queue_delayed_work(touch_wq, &global_mxt_data->work_deepsleep, msecs_to_jiffies(2000));
					}
				}
				global_mxt_data->charging_mode = 0;
				mxt_patch_event(global_mxt_data, CHARGER_UNplugged);
				mxt_patchevent_unset(PATCH_EVENT_TA);
			}
		} else {
			if (mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
				mxt_patch_event(global_mxt_data, NOCHARGER_KNOCKON_WAKEUP);
				mxt_patchevent_unset(PATCH_EVENT_KNOCKON);
			}
			global_mxt_data->charging_mode = 1;
			mxt_patch_event(global_mxt_data, CHARGER_PLUGGED);
			mxt_patchevent_set(PATCH_EVENT_TA);
		}

		do_gettimeofday(&t_ex_debug[TIME_EX_INIT_TIME]);

		mutex_unlock(&i2c_suspend_lock);
	}
}

static void mxt_proc_message_log(struct mxt_data *data, u8 type)
{
	if (mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
		if (type != 6 && type != 46 && type != 56 && type != 61 && type != 72)
			TOUCH_INFO_MSG("mxt_interrupt T%d \n", type);
	}
}

static int mxt_proc_message(struct mxt_data *data, u8 *message)
{
	u8 report_id = message[0];
	u8 type = 0;
	bool dump = data->debug_enabled;

	if (report_id == MXT_RPTID_NOMSG)
		return 0;

	type = data->reportids[report_id].type;

	mxt_proc_message_log(data, type);

	if (type == MXT_GEN_COMMAND_T6) {
		mxt_proc_t6_messages(data, message);
	} else if (type == MXT_TOUCH_MULTI_T9) {
		mxt_proc_t9_message(data, message);
	} else if (type == MXT_TOUCH_KEYARRAY_T15) {
		mxt_proc_t15_messages(data, message);
	} else if (type == MXT_PROCI_ONETOUCH_T24 && data->mxt_knock_on_enable) {
		mxt_proc_t24_messages(data, message);
	} else if (type == MXT_SPT_SELFTEST_T25) {
		mxt_proc_t25_message(data, message);
	} else if (type == MXT_PROCI_TOUCHSUPPRESSION_T42) {
		mxt_proc_t42_messages(data, message);
	} else if (type == MXT_SPT_CTECONFIG_T46) {
		TOUCH_INFO_MSG("MXT_SPT_CTECONFIG_T46");
	} else if (type == MXT_PROCG_NOISESUPPRESSION_T48) {
		mxt_proc_t48_messages(data, message);
	} else if (type == MXT_PROCI_SHIELDLESS_T56) {
		TOUCH_INFO_MSG("MXT_PROCI_SHIELDLESS_T56");
	} else if (type == MXT_PROCI_EXTRATOUCHSCREENDATA_T57) {
		mxt_proc_t57_messages(data, message);
	} else if (type == MXT_PROCG_NOISESUPPRESSION_T72) {
		do_gettimeofday(&t_ex_debug[TIME_EX_T72_NOISE_INT_TIME]);
		TOUCH_INFO_MSG("MXT_PROCG_NOISESUPPRESSION_T72");

		if (t_ex_debug[TIME_EX_T72_NOISE_INT_TIME].tv_sec - t_ex_debug[TIME_EX_KNOCK_INT_TIME].tv_sec ==  0) {
			if (get_time_interval(t_ex_debug[TIME_EX_T72_NOISE_INT_TIME].tv_sec, t_ex_debug[TIME_EX_KNOCK_INT_TIME].tv_sec) <= 30*1000) {
				TOUCH_INFO_MSG("the time interval less than 30ms after Knock on/code 1\n");
				data->t72_noise_state = true;
			}
		} else if (t_ex_debug[TIME_EX_T72_NOISE_INT_TIME].tv_sec - t_ex_debug[TIME_EX_KNOCK_INT_TIME].tv_sec ==  1) {
			if (get_time_interval(t_ex_debug[TIME_EX_T72_NOISE_INT_TIME].tv_sec + 1000*1000, t_ex_debug[TIME_EX_KNOCK_INT_TIME].tv_sec) <= 30*1000) {
				TOUCH_INFO_MSG("the time interval less than 30ms after Knock on/code 2\n");
				data->t72_noise_state = true;
			}
		}

	} else {
		if (type != MXT_SPT_TIMER_T61) {
			TOUCH_INFO_MSG("%s : Unknown T%d \n", __func__, type);
			mxt_dump_message(data, message);
		}
	}

	if (dump)
		mxt_dump_message(data, message);

	if (mxt_power_block_get() == 0)
		mxt_patch_message(data, (struct mxt_message*)message);

	return 1;
}

static int mxt_read_and_process_messages(struct mxt_data *data, u8 count)
{
	int ret = 0;
	int i = 0;
	u8 num_valid = 0;

	/* Safety check for msg_buf */
	if (count > data->max_reportid)
		return -EINVAL;

	if (data->msg_buf == NULL) {
		TOUCH_INFO_MSG("%s data->msg_buf = NULL \n", __func__);
	}

	/* Process remaining messages if necessary */
	ret = __mxt_read_reg(data->client, data->T5_address, data->T5_msg_size * count, data->msg_buf);
	if (ret) {
		TOUCH_INFO_MSG("Failed to read %u messages (%d)\n", count, ret);
		return ret;
	}

	for (i = 0;  i < count; i++) {
		ret = mxt_proc_message(data, data->msg_buf + data->T5_msg_size * i);

		if (ret == 1)
			num_valid++;
	}

	/* return number of messages read */
	return num_valid;
}

static irqreturn_t mxt_process_messages(struct mxt_data *data)
{
	int total_handled = 0, num_handled = 0;
	u8 count = data->last_message_count;

	if (count < 1 || count > data->max_reportid)
		count = 1;

	/* include final invalid message */
	total_handled = mxt_read_and_process_messages(data, count);
	if (total_handled < 0)
		return IRQ_HANDLED;

	/* if there were invalid messages, then we are done */
	else if (total_handled <= count)
		goto update_count;

	/* read two at a time until an invalid message or else we reach
	 * reportid limit */
	do {
		num_handled = mxt_read_and_process_messages(data, 2);
		if (num_handled < 0)
			return IRQ_HANDLED;

		total_handled += num_handled;

		if (num_handled < 2)
			break;
	} while (total_handled < data->num_touchids);

update_count:
	data->last_message_count = total_handled;

	if (data->enable_reporting && data->update_input) {
		mxt_input_sync(data->input_dev);
		data->update_input = false;
	}

	return IRQ_HANDLED;
}

static irqreturn_t mxt_interrupt(int irq, void *dev_id)
{
	struct mxt_data *data = dev_id;
	irqreturn_t ret = IRQ_NONE;

	if (data->in_bootloader) {
		/* bootloader state transition completion */
		complete(&data->bl_completion);
		return IRQ_HANDLED;
	}

	if (!data->object_table)
		return IRQ_HANDLED;

	mutex_lock(&i2c_suspend_lock);

	ret = mxt_process_messages(data);

	mutex_unlock(&i2c_suspend_lock);

	return ret;
}

static int mxt_t6_command(struct mxt_data *data, u16 cmd_offset, u8 value, bool wait)
{
	u16 reg = 0;
	u8 command_register = 0;
	int timeout_counter = 0;
	int ret = 0;

	reg = data->T6_address + cmd_offset;

	ret = mxt_write_reg(data->client, reg, value);
	if (ret)
		return ret;

	if (!wait)
		return 0;

	do {
		msleep(20);
		ret = __mxt_read_reg(data->client, reg, 1, &command_register);
		if (ret)
			return ret;
	} while ((command_register != 0) && (timeout_counter++ <= 100));

	if (timeout_counter > 100) {
		TOUCH_INFO_MSG("%s Command failed!\n", __func__);
		return -EIO;
	}

	return 0;
}

static int mxt_t25_command(struct mxt_data *data, u8 value, bool wait)
{
	u16 reg = 0;
	u8 command_register = 0;
	int timeout_counter = 0;
	int ret = 0;

	if (!selftest_enable)
		return 0;

	reg = data->T25_address + 1 ;

	ret = mxt_write_reg(data->client, reg, value);
	if (ret) {
		TOUCH_INFO_MSG("Write Self Test Command fail!\n");
		return ret;
	}

	if (!wait)
		return 0;

	do {
		msleep(20);
		ret = __mxt_read_reg(data->client, reg, 1, &command_register);
		if (ret)
			return ret;
	} while ((command_register != 0) && (timeout_counter++ <= 100));

	if (timeout_counter > 100) {
		TOUCH_INFO_MSG("%s Command failed!\n", __func__);
		return -EIO;
	}

	return 0;
}

static int mxt_soft_reset(struct mxt_data *data)
{
	int ret = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	INIT_COMPLETION(data->reset_completion);

	ret = mxt_t6_command(data, MXT_COMMAND_RESET, MXT_RESET_VALUE, false);
	if (ret)
		return ret;

	msleep(MXT_RESET_TIME);

	return 0;
}

static int mxt_hw_reset(struct mxt_data *data)
{
	TOUCH_INFO_MSG("%s \n", __func__);

	gpio_set_value(data->pdata->gpio_reset, 0);
	mdelay(5);

	gpio_set_value(data->pdata->gpio_reset, 1);
	msleep(MXT_RESET_TIME);

	return 0;
}

static void mxt_update_crc(struct mxt_data *data, u8 cmd, u8 value)
{
	/* on failure, CRC is set to 0 and config will always be downloaded */
	data->config_crc = 0;
	INIT_COMPLETION(data->crc_completion);

	mxt_t6_command(data, cmd, value, true);
	/* Wait for crc message. On failure, CRC is set to 0 and config will
	 * always be downloaded */
#if 1
	msleep(MXT_CRC_TIMEOUT);
#else
	mxt_wait_for_completion(data, &data->crc_completion, MXT_CRC_TIMEOUT);
#endif
}

static void mxt_calc_crc24(u32 *crc, u8 firstbyte, u8 secondbyte)
{
	static const unsigned int crcpoly = 0x80001B;
	u32 result = 0;
	u32 data_word = 0;

	data_word = (secondbyte << 8) | firstbyte;
	result = ((*crc << 1) ^ data_word);

	if (result & 0x1000000)
		result ^= crcpoly;

	*crc = result;
}

static u32 mxt_calculate_crc(u8 *base, off_t start_off, off_t end_off)
{
	u32 crc = 0;
	u8 *ptr = base + start_off;
	u8 *last_val = base + end_off - 1;

	if (end_off < start_off)
		return -EINVAL;

	while (ptr < last_val) {
		mxt_calc_crc24(&crc, *ptr, *(ptr + 1));
		ptr += 2;
	}

	/* if len is odd, fill the last byte with 0 */
	if (ptr == last_val)
		mxt_calc_crc24(&crc, *ptr, 0);

	/* Mask to 24-bit */
	crc &= 0x00FFFFFF;

	return crc;
}

static int mxt_set_t7_power_cfg(struct mxt_data *data, u8 sleep)
{
	int error = 0;
	struct t7_config *new_config = NULL;
	struct t7_config deepsleep = { .active = 0, .idle = 0 };

	if (sleep == MXT_POWER_CFG_DEEPSLEEP)
		new_config = &deepsleep;
	else
		new_config = &data->t7_cfg;

	error = __mxt_write_reg(data->client, data->T7_address, sizeof(data->t7_cfg), new_config);
	if (error)
		return error;

	TOUCH_INFO_MSG("Set T7 ACTV:%d IDLE:%d\n", new_config->active, new_config->idle);

	return 0;
}

static int mxt_init_t7_power_cfg(struct mxt_data *data)
{
	int error = 0;
	bool retry = false;

recheck:
	error = __mxt_read_reg(data->client, data->T7_address, sizeof(data->t7_cfg), &data->t7_cfg);
	if (error)
		return error;

	if (data->t7_cfg.active == 0 || data->t7_cfg.idle == 0) {
		if (!retry) {
			TOUCH_INFO_MSG("T7 cfg zero, resetting\n");
			mxt_soft_reset(data);
			retry = true;
			goto recheck;
		} else {
		    TOUCH_INFO_MSG("T7 cfg zero after reset, overriding\n");
		    data->t7_cfg.active = 20;
		    data->t7_cfg.idle = 100;
		    return mxt_set_t7_power_cfg(data, MXT_POWER_CFG_RUN);
		}
	} else {
		TOUCH_INFO_MSG("Initialised power cfg: ACTV %d, IDLE %d\n", data->t7_cfg.active, data->t7_cfg.idle);
		return 0;
	}
}

static int mxt_check_reg_init(struct mxt_data *data, const char *name)
{
	struct device *dev = &data->client->dev;
	struct mxt_info cfg_info = {0};
	struct mxt_object *object = NULL;
	const struct firmware *cfg = NULL;
	int ret = 0;
	int offset = 0;
	int data_pos = 0;
	int byte_offset = 0;
	int i = 0;
	int cfg_start_ofs = 0;
	u32 info_crc = 0, config_crc = 0, calculated_crc = 0;
	u8 *config_mem = 0;
	size_t config_mem_size = 0;
	unsigned int type = 0, instance, size = 0;
	u8 val = 0;
	u16 reg = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	if (!name) {
		TOUCH_INFO_MSG("Skipping cfg download \n");
		return 0;
	}

	ret = request_firmware(&cfg, name, dev);
	if (ret < 0) {
		TOUCH_INFO_MSG("Failure to request config file [%s]\n", name);
		return -EINVAL;
	}

	TOUCH_INFO_MSG("Open [%s] configuration file \n", name);

	mxt_update_crc(data, MXT_COMMAND_REPORTALL, 1);

	if (strncmp(cfg->data, MXT_CFG_MAGIC, strlen(MXT_CFG_MAGIC))) {
		TOUCH_INFO_MSG("Unrecognised config file\n");
		ret = -EINVAL;
		goto release;
	}

	data_pos = strlen(MXT_CFG_MAGIC);

	/* Load information block and check */
	for (i = 0; i < sizeof(struct mxt_info); i++) {
		ret = sscanf(cfg->data + data_pos, "%hhx%n", (unsigned char *)&cfg_info + i, &offset);
		if (ret != 1) {
			TOUCH_INFO_MSG("Bad format\n");
			ret = -EINVAL;
			goto release;
		}

		data_pos += offset;
	}

	/* Read CRCs */
	ret = sscanf(cfg->data + data_pos, "%x%n", &info_crc, &offset);
	if (ret != 1) {
		TOUCH_INFO_MSG("Bad format: failed to parse Info CRC\n");
		ret = -EINVAL;
		goto release;
	}
	data_pos += offset;

	ret = sscanf(cfg->data + data_pos, "%x%n", &config_crc, &offset);
	if (ret != 1) {
		TOUCH_INFO_MSG("Bad format: failed to parse Config CRC\n");
		ret = -EINVAL;
		goto release;
	}
	data_pos += offset;

	TOUCH_INFO_MSG("RAW Config CRC is 0x%06X \n", config_crc);

	if (data->config_crc == config_crc) {
		TOUCH_INFO_MSG("Config already applied \n");
		ret = 0;
		goto release_mem;
	}

	if (memcmp((char *)data->info, (char *)&cfg_info, sizeof(struct mxt_info)) != 0) {
		TOUCH_INFO_MSG("Compatibility Error. Could not apply\n");
			TOUCH_INFO_MSG("Info Block [IC]   %02X %02X %02X %02X %02X %02X %02X \n",
			data->info->family_id, data->info->variant_id, data->info->version, data->info->build,
			data->info->matrix_xsize, data->info->matrix_ysize, data->info->object_num);

		TOUCH_INFO_MSG("Info Block [File] %02X %02X %02X %02X %02X %02X %02X \n",
			cfg_info.family_id, cfg_info.variant_id, cfg_info.version, cfg_info.build,
			cfg_info.matrix_xsize, cfg_info.matrix_ysize, cfg_info.object_num);

		ret = -EINVAL;
		goto release_mem;
	}

	/* Malloc memory to store configuration */
	cfg_start_ofs = MXT_OBJECT_START + data->info->object_num * sizeof(struct mxt_object) + MXT_INFO_CHECKSUM_SIZE;
	config_mem_size = data->mem_size - cfg_start_ofs;
	config_mem = kzalloc(config_mem_size, GFP_KERNEL);
	if (!config_mem) {
		TOUCH_INFO_MSG("Failed to allocate memory\n");
		ret = -ENOMEM;
		goto release;
	}

	while (data_pos < cfg->size) {
		/* Read type, instance, length */
		ret = sscanf(cfg->data + data_pos, "%x %x %x%n", &type, &instance, &size, &offset);
		if (ret == 0) {
			/* EOF */
			break;
		} else if (ret != 3) {
			TOUCH_INFO_MSG("Bad format: failed to parse object\n");
			ret = -EINVAL;
			goto release_mem;
		}
		data_pos += offset;

		object = mxt_get_object(data, type);
		if (!object) {
			/* Skip object */
			for (i = 0; i < size; i++) {
				ret = sscanf(cfg->data + data_pos, "%hhx%n", &val, &offset);
				data_pos += offset;
			}
			continue;
		}

		if (instance >= mxt_obj_instances(object)) {
			TOUCH_INFO_MSG("Object instances exceeded!\n");
			ret = -EINVAL;
			goto release_mem;
		}

		reg = object->start_address + mxt_obj_size(object) * instance;

		TOUCH_INFO_MSG("\t %04X %04X %04X \n", type, instance, size);

		if (size != mxt_obj_size(object)) {
			TOUCH_INFO_MSG("Size mismatched \n");
			ret = -EINVAL;
			goto release_mem;
		}

		for (i = 0; i < size; i++) {
			ret = sscanf(cfg->data + data_pos, "%hhx%n", &val, &offset);
			if (ret != 1) {
				TOUCH_INFO_MSG("Bad format in T%d\n", type);
				ret = -EINVAL;
				goto release_mem;
			}

			if (i > mxt_obj_size(object))
				continue;

			byte_offset = reg + i - cfg_start_ofs;

			if ((byte_offset >= 0) && (byte_offset <= config_mem_size)) {
				*(config_mem + byte_offset) = val;
			} else {
				TOUCH_INFO_MSG("Bad object: reg:%d, T%d, ofs=%d\n", reg, object->type, byte_offset);
				ret = -EINVAL;
				goto release_mem;
			}

			data_pos += offset;
		}
	}

	/* calculate crc of the received configs (not the raw config file) */
	if (data->T7_address < cfg_start_ofs) {
		TOUCH_INFO_MSG("Bad T7 address, T7addr = %x, config offset %x\n", data->T7_address, cfg_start_ofs);
		ret = -EINVAL;
		goto release_mem;
	}

	calculated_crc = mxt_calculate_crc(config_mem, data->T7_address - cfg_start_ofs, config_mem_size);

	/* Check the crc, calculated should match what is in file */
	if (config_crc > 0 && (config_crc != calculated_crc)) {
		TOUCH_INFO_MSG("CRC mismatch in config file, calculated=0x%06X, file=0x%06X\n", calculated_crc, config_crc);
		TOUCH_INFO_MSG("Config not apply \n");
		ret = -EINVAL;
		goto release_mem;
	}

	/* Write configuration as blocks */
	byte_offset = 0;
	while (byte_offset < config_mem_size) {
		size = config_mem_size - byte_offset;

		if (size > MXT_MAX_BLOCK_WRITE)
			size = MXT_MAX_BLOCK_WRITE;

		ret = __mxt_write_reg(data->client, cfg_start_ofs + byte_offset, size, config_mem + byte_offset);
		if (ret != 0) {
			TOUCH_INFO_MSG("Config write error, ret=%d\n", ret);
			ret = -EINVAL;
			goto release_mem;
		}

		byte_offset += size;
	}

	mxt_update_crc(data, MXT_COMMAND_BACKUPNV, MXT_BACKUP_VALUE);

	if ((config_crc > 0) && (config_crc != data->config_crc)) {
		TOUCH_INFO_MSG("Config CRC is mismatched 0x%06X \n", data->config_crc);
	}

	ret = mxt_soft_reset(data);
	if (ret)
		goto release_mem;

	TOUCH_INFO_MSG("Config written\n");

	/* T7 config may have changed */
	mxt_init_t7_power_cfg(data);

release_mem:
	TOUCH_INFO_MSG("kfree in %s \n", __func__);
	kfree(config_mem);
release:
	release_firmware(cfg);

	return ret;
}

static int mxt_acquire_irq(struct mxt_data *data)
{

	touch_enable_irq(data->irq);

#if 0
	if (data->use_retrigen_workaround) {
		error = mxt_process_messages_until_invalid(data);
		if (error)
			return error;
	}
#endif

	return 0;
}

static void mxt_free_input_device(struct mxt_data *data)
{
	if (data->input_dev && is_probing) {
		TOUCH_INFO_MSG("mxt_free_input_device\n");
		input_unregister_device(data->input_dev);
		data->input_dev = NULL;
	}
}

static void mxt_free_object_table(struct mxt_data *data)
{
	TOUCH_INFO_MSG("%s \n", __func__);

	if (data->raw_info_block)
		kfree(data->raw_info_block);

	data->info = NULL;
	data->raw_info_block = NULL;
#if 0
	if (data->msg_buf)
		kfree(data->msg_buf);
	data->msg_buf = NULL;
#endif

	mxt_free_input_device(data);
	data->enable_reporting = false;

	data->T5_address = 0;
	data->T5_msg_size = 0;
	data->T6_reportid = 0;
	data->T7_address = 0;
	data->T9_address = 0;
	data->T9_reportid_min = 0;
	data->T9_reportid_max = 0;
	data->T15_reportid_min = 0;
	data->T15_reportid_max = 0;
	data->T18_address = 0;
	data->T24_reportid = 0;
	data->T35_reportid = 0;
	data->T25_reportid = 0;
	data->T42_reportid_min = 0;
	data->T42_reportid_max = 0;
	data->T42_address = 0;
	data->T46_address = 0;
	data->T48_reportid = 0;
	data->T56_address = 0;
	data->T65_address = 0;
	data->T72_address = 0;
	data->T93_address = 0;
	data->T93_reportid = 0;
	data->max_reportid = 0;
}

static int mxt_parse_object_table(struct mxt_data *data)
{
	int i = 0;
	u8 reportid = 0;
	u16 end_address = 0;
	struct mxt_object *object = NULL;
	u8 min_id = 0, max_id = 0;

	/* Valid Report IDs start counting from 1 */
	reportid = 1;
	data->mem_size = 0;

	for (i = 0; i < data->info->object_num; i++) {
		object = data->object_table + i;

		le16_to_cpus(&object->start_address);

		if (object->num_report_ids) {
			min_id = reportid;
			reportid += object->num_report_ids * mxt_obj_instances(object);
			max_id = reportid - 1;
		} else {
			min_id = 0;
			max_id = 0;
		}

#if 0
		TOUCH_INFO_MSG("\t T%02u Start:%u Size:%03u Instances:%u Report IDs:%u-%u\n",
			object->type, object->start_address,
			mxt_obj_size(object), mxt_obj_instances(object),
			min_id, max_id);
#endif

		switch (object->type) {
		case MXT_GEN_MESSAGE_T5:
			if (data->info->family_id == 0x80) {
				/* On mXT224 read and discard unused CRC byte
				 * otherwise DMA reads are misaligned */
				data->T5_msg_size = mxt_obj_size(object);
			} else {
				/* CRC not enabled, so skip last byte */
				data->T5_msg_size = mxt_obj_size(object) - 1;
			}
			data->T5_address = object->start_address;
			break;
		case MXT_GEN_COMMAND_T6:
			data->T6_reportid = min_id;
			data->T6_address = object->start_address;
			break;
		case MXT_GEN_POWER_T7:
			data->T7_address = object->start_address;
			break;
		case MXT_TOUCH_MULTI_T9:
			/* Only handle messages from first T9 instance */
			data->T9_address = object->start_address;
			data->T9_reportid_min = min_id;
			data->T9_reportid_max = min_id + object->num_report_ids - 1;
			data->num_touchids = object->num_report_ids;
			//TOUCH_INFO_MSG("T9_reportid_min:%d T9_reportid_max:%d\n", data->T9_reportid_min, data->T9_reportid_max);
			break;
		case MXT_TOUCH_KEYARRAY_T15:
			data->T15_reportid_min = min_id;
			data->T15_reportid_max = max_id;
			break;
		case MXT_SPT_COMMSCONFIG_T18:
			data->T18_address = object->start_address;
			break;
		case MXT_PROCI_ONETOUCH_T24:
			data->T24_reportid = min_id;
			break;
		case MXT_SPT_PROTOTYPE_T35:
			data->T35_reportid = min_id;
			break;
		case MXT_SPT_SELFTEST_T25:
			data->T25_reportid = min_id;
			data->T25_address = object->start_address;
			break;
		case MXT_PROCI_TOUCHSUPPRESSION_T42:
			data->T42_address = object->start_address;
			data->T42_reportid_min = min_id;
			data->T42_reportid_max = max_id;
			break;
		case MXT_SPT_CTECONFIG_T46:
			data->T46_address = object->start_address;
			break;
		case MXT_SPT_NOISESUPPRESSION_T48:
			data->T48_reportid = min_id;
			break;
		case MXT_PROCI_SHIELDLESS_T56:
			data->T56_address = object->start_address;
			break;
		case MXT_PROCI_LENSBENDING_T65:
			data->T65_address = object->start_address;
			break;
		case MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71:
			data->T71_address = object->start_address;
			break;
		case MXT_PROCG_NOISESUPPRESSION_T72:
			data->T72_address = object->start_address;
			break;
		case MXT_PROCI_TOUCH_SEQUENCE_LOGGER_T93:
			data->T93_reportid = min_id;
			data->T93_address = object->start_address;
			break;
		}

		end_address = object->start_address + mxt_obj_size(object) * mxt_obj_instances(object) - 1;

		if (end_address >= data->mem_size)
			data->mem_size = end_address + 1;
	}

	/* Store maximum reportid */
	data->max_reportid = reportid;

	if (data->msg_buf) {
		kfree(data->msg_buf);
	}

	data->msg_buf = kzalloc((data->max_reportid * data->T5_msg_size), GFP_KERNEL);
	if (!data->msg_buf) {
		TOUCH_INFO_MSG("%s d Failed to allocate message buffer\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

static int mxt_read_info_block(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int error = 0;
	size_t size = 0;
	void *buf = NULL;
	struct mxt_info *info = NULL;
	#if 0
	u32 calculated_crc = 0;
	u8 *crc_ptr = NULL;
	#endif

	TOUCH_INFO_MSG("%s \n", __func__);

#if 0 // Test
	/* If info block already allocated, free it */
	if (data->raw_info_block != NULL)
		mxt_free_object_table(data);
#endif

	/* Read 7-byte ID information block starting at address 0 */
	size = sizeof(struct mxt_info);
	buf = kzalloc(size, GFP_KERNEL);
	if (!buf) {
		TOUCH_INFO_MSG("%s Failed to allocate memory 1\n", __func__);
		return -ENOMEM;
	}

	error = __mxt_read_reg(client, 0, size, buf);
	if (error) {
		TOUCH_INFO_MSG("%s __mxt_read_reg error \n", __func__);
		goto err_free_mem;
	}

	/* Resize buffer to give space for rest of info block */
	info = (struct mxt_info *)buf;
	size += (MXT_OBJECT_NUM_MAX * sizeof(struct mxt_object)) + MXT_INFO_CHECKSUM_SIZE;
	buf = krealloc(buf, size, GFP_KERNEL);
	if (!buf) {
		TOUCH_INFO_MSG("%s Failed to allocate memory 2\n", __func__);
		error = -ENOMEM;
		goto err_free_mem;
	}

	/* Read rest of info block */
	error = __mxt_read_reg(client, MXT_OBJECT_START,
			       size - MXT_OBJECT_START, buf + MXT_OBJECT_START);
	if (error) {
		TOUCH_INFO_MSG("%s __mxt_read_reg error \n", __func__);
		goto err_free_mem;
	}

	#if 0
	/* Extract & calculate checksum */
	crc_ptr = buf + size - MXT_INFO_CHECKSUM_SIZE;
	data->info_crc = crc_ptr[0] | (crc_ptr[1] << 8) | (crc_ptr[2] << 16);

	calculated_crc = mxt_calculate_crc(buf, 0, size - MXT_INFO_CHECKSUM_SIZE);

	/* CRC mismatch can be caused by data corruption due to I2C comms
	 * issue or else device is not using Object Based Protocol */
	if (data->info_crc != calculated_crc) {
		TOUCH_INFO_MSG("*************** Info Block CRC error calculated=0x%06X read=0x%06X\n",
			data->info_crc, calculated_crc);
		//temp return -EIO;
	}
	#endif

	/* Save pointers in device data structure */
	data->raw_info_block = buf;
	data->info = (struct mxt_info *)buf;

	if (data->object_table == NULL) {
		data->object_table = (struct mxt_object *)(buf + MXT_OBJECT_START);
	}

	TOUCH_INFO_MSG("Family:%02X Variant:%02X Binary:%u.%u.%02X TX:%d RX:%d Objects:%d\n",
		 data->info->family_id, data->info->variant_id, data->info->version >> 4, data->info->version & 0xF,
		 data->info->build, data->info->matrix_xsize, data->info->matrix_ysize, data->info->object_num);

	/* Parse object table information */
	error = mxt_parse_object_table(data);
	if (error) {
		TOUCH_INFO_MSG("%s Error %d reading object table\n", __func__, error);
		mxt_free_object_table(data);
		return error;
	}

	return 0;

err_free_mem:
	kfree(buf);
	data->raw_info_block = NULL;
	data->info = NULL;
	data->object_table = NULL;
	return error;
}

static int mxt_read_t9_resolution(struct mxt_data *data)
{
	struct i2c_client *client = data->client;
	int error = 0;
	struct t9_range range = {0};
	unsigned char orient = 0;
	struct mxt_object *object = NULL;
	memset(&range, 0, sizeof(range));

	object = mxt_get_object(data, MXT_TOUCH_MULTI_T9);
	if (!object)
		return -EINVAL;

	error = __mxt_read_reg(client, object->start_address + MXT_T9_RANGE, sizeof(range), &range);
	if (error)
		return error;

	le16_to_cpus(range.x);
	le16_to_cpus(range.y);

	error =  __mxt_read_reg(client, object->start_address + MXT_T9_ORIENT, 1, &orient);
	if (error)
		return error;

	/* Handle default values */
	if (range.x == 0)
		range.x = 2159;

	if (range.y == 0)
		range.y = 3839;

	if (orient & MXT_T9_ORIENT_SWITCH) {
		data->max_x = range.y;
		data->max_y = range.x;
	} else {
		data->max_x = range.x;
		data->max_y = range.y;
	}

	TOUCH_INFO_MSG("Touchscreen size X:%u Y:%u\n", data->max_x, data->max_y);

	return 0;
}

static void mxt_regulator_enable(struct mxt_data *data)
{
	int error = 0;

	if (data->regulator_status == 1)
		return;

	TOUCH_INFO_MSG("%s \n", __func__);

	if (data->vdd_ana) {
		error = regulator_enable(data->vdd_ana);
		if (error < 0) {
			TOUCH_INFO_MSG("vdd_ana regulator enable fail\n");
			return;
		}
		mdelay(1);
	}

	data->regulator_status = 1;

	INIT_COMPLETION(data->bl_completion);
}

static void mxt_regulator_disable(struct mxt_data *data)
{
	int error = 0;

	if (data->regulator_status == 0)
		return;

	TOUCH_INFO_MSG("%s \n", __func__);

	if (data->vdd_ana) {
		error = regulator_disable(data->vdd_ana);
		if (error < 0) {
			TOUCH_INFO_MSG("vdd_ana regulator disable fail\n");
			return;
		}
	}

	data->regulator_status = 0;
}

static void mxt_probe_regulators(struct mxt_data *data)
{
	struct device *dev = &data->client->dev;
	int error = 0;

	/* According to maXTouch power sequencing specification, RESET line
	 * must be kept low until some time after regulators come up to
	 * voltage */

	TOUCH_INFO_MSG("%s \n", __func__);

	data->regulator_status = 0;

	if (!data->pdata->gpio_reset) {
		TOUCH_INFO_MSG("Must have reset GPIO to use regulator support\n");
		goto fail;
	}

	data->vdd_ana = regulator_get(dev, "vdd_ana");
	if (IS_ERR(data->vdd_ana)) {
		error = PTR_ERR(data->vdd_ana);
		TOUCH_INFO_MSG("Error %d getting ana regulator\n", error);
		goto fail;
	}

	error = regulator_set_voltage(data->vdd_ana, 2950000, 2950000);
	if (error < 0) {
		TOUCH_INFO_MSG("Error %d cannot control ana regulator\n", error);
		goto fail;
	}

	data->use_regulator = true;
	return;

fail:
	TOUCH_INFO_MSG("%s fail\n", __func__);
	data->vdd_ana = NULL;
	data->use_regulator = false;
}

static int mxt_configure_objects(struct mxt_data *data)
{
	int error = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	error = mxt_init_t7_power_cfg(data);
	if (error) {
		TOUCH_INFO_MSG("Failed to initialize power cfg\n");
		return error;
	}

	/* Check register init values */
	error = mxt_check_reg_init(data, NULL);
	if (error) {
		TOUCH_INFO_MSG("Error %d initialising configuration\n", error);
		return error;
	}

	if (data->T9_reportid_min) {
		error = mxt_initialize_t9_input_device(data);
		if (error)
			return error;
	} else {
		TOUCH_INFO_MSG("No touch object detected\n");
	}
	return 0;
}

static int mxt_rest_init(struct mxt_data *data)
{
	int error = 0;

	error = mxt_acquire_irq(data);
	if (error)
		return error;

	error = mxt_configure_objects(data);
	if (error)
		return error;

	return 0;
}

static void mxt_read_fw_version(struct mxt_data *data)
{
	TOUCH_INFO_MSG("==================================\n");
	TOUCH_INFO_MSG("Firmware Version = %d.%02d \n", data->pdata->fw_ver[0], data->pdata->fw_ver[1]);
	TOUCH_INFO_MSG("FW Product       = %s \n", data->pdata->product);
	TOUCH_INFO_MSG("Binary Version   = %u.%u.%02X \n", data->info->version >> 4, data->info->version & 0xF, data->info->build);
	TOUCH_INFO_MSG("Config CRC       = 0x%X  \n", data->config_crc);
	TOUCH_INFO_MSG("Family Id        = 0x%02X \n", data->info->family_id);
	TOUCH_INFO_MSG("Variant          = 0x%02X \n", data->info->variant_id);
	TOUCH_INFO_MSG("==================================\n");
}

/* Firmware Version is returned as Major.Minor.Build */
static ssize_t mxt_fw_version_show(struct mxt_data *data, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u.%u.%02X\n",
			 data->info->version >> 4, data->info->version & 0xf, data->info->build);
}

/* Hardware Version is returned as FamilyID.VariantID */
static ssize_t mxt_hw_version_show(struct mxt_data *data, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%02X.%02X\n",
			data->info->family_id, data->info->variant_id);
}

static ssize_t mxt_testmode_ver_show(struct mxt_data *data, char *buf)
{
	int ret = 0;

	ret += sprintf(buf+ret, "%d.%02d", data->pdata->fw_ver[0], data->pdata->fw_ver[1]);
	ret += sprintf(buf+ret, "(%s)\n", data->pdata->product);

	return ret;
}

static ssize_t mxt_info_show(struct mxt_data *data, char *buf)
{
	int ret = 0;

	mxt_read_fw_version(data);

	ret += sprintf(buf+ret, "Firmware Version = %d.%02d \n", data->pdata->fw_ver[0], data->pdata->fw_ver[1]);
	ret += sprintf(buf+ret, "FW Product       = %s \n", data->pdata->product);
	ret += sprintf(buf+ret, "Binary Version   = %u.%u.%02X \n", data->info->version >> 4, data->info->version & 0xF, data->info->build);
	ret += sprintf(buf+ret, "Config CRC       = 0x%X \n", data->config_crc);
	ret += sprintf(buf+ret, "Family Id        = 0x%02X \n", data->info->family_id);
	ret += sprintf(buf+ret, "Variant          = 0x%02X \n", data->info->variant_id);

	return ret;
}

static ssize_t mxt_selftest(struct mxt_data *data, char *buf, int len)
{
	int ret = len;
	int test_all_cmd = 0xFE;

	selftest_enable = true;
	selftest_show = true;

	mxt_t25_command(data, test_all_cmd, false);
	msleep(MXT_SELFTEST_TIME);

	if (data->self_test_status[0] == 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,  "Need more time. Try Again.\n");
		return ret;
	}

	if (data->self_test_status[0] == 0xFD) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Invalid Test Code. Try Again.");
	} else if (data->self_test_status[0] == 0xFC) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "The test could not be completed due to an unrelated fault. Try again.");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "All Test Result: %s", (data->self_test_status[0] == 0xFE) ? "Pass\n" : "Fail\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "AVdd power Test Result: %s", (data->self_test_status[0] != 0x01) ? "Pass\n" : "Fail\n");

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Pin Fault Test Result: %s", (data->self_test_status[0] != 0x12) ? "Pass\n" : "Fail\n");
		if (data->self_test_status[0] == 0x12)
			ret += snprintf(buf+ret, PAGE_SIZE - ret, "# Fail # seq_num(%u) x_pin(%u) y_pin(%u)\n",
										data->self_test_status[1], data->self_test_status[2], data->self_test_status[3]);

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Signal Limit Test: %s", (data->self_test_status[0] != 0x17) ? "Pass\n" : "Fail\n");
		if (data->self_test_status[0] == 0x17)
			ret += snprintf(buf+ret, PAGE_SIZE - ret, "# Fail # type_num(%u) type_instance(%u)\n", data->self_test_status[1], data->self_test_status[2]);
	}

	selftest_show = false;
	return ret;
}

static ssize_t mxt_show_instance(char *buf, int count, struct mxt_object *object, int instance, const u8 *val)
{
	int i = 0;

	if (mxt_obj_instances(object) > 1)
		count += scnprintf(buf + count, PAGE_SIZE - count, "Instance %u\n", instance);

	for (i = 0; i < mxt_obj_size(object); i++)
		count += scnprintf(buf + count, PAGE_SIZE - count, "\t[%2u]: %02x (%d)\n", i, val[i], val[i]);

	count += scnprintf(buf + count, PAGE_SIZE - count, "\n");

	return count;
}

static ssize_t mxt_object_show(struct mxt_data *data, char *buf)
{
	struct mxt_object *object = NULL;
	int count = 0;
	int i = 0, j = 0;
	int error = 0;
	u8 *obuf = NULL;
	u16 size = 0;
	u16 addr = 0;

	/* Pre-allocate buffer large enough to hold max sized object. */
	obuf = kzalloc(256, GFP_KERNEL);
	if (!obuf)
		return -ENOMEM;

	error = 0;
	for (i = 0; i < data->info->object_num; i++) {
		object = data->object_table + i;

		if (!mxt_object_readable(object->type))
			continue;

		count += scnprintf(buf + count, PAGE_SIZE - count, "T%u:\n", object->type);

		for (j = 0; j < mxt_obj_instances(object); j++) {
			size = mxt_obj_size(object);
			addr = object->start_address + j * size;

			error = __mxt_read_reg(data->client, addr, size, obuf);
			if (error)
				goto done;

			count = mxt_show_instance(buf, count, object, j, obuf);
		}
	}

done:
	kfree(obuf);

	return error ?: count;
}

static ssize_t mxt_object_control(struct mxt_data *data, const char *buf, size_t count)
{
	struct mxt_object *object = NULL;
	unsigned char command[6] = {0};
	int type = 0;
	int addr_offset = 0;
	int value = 0;
	int error = 0;
	int i = 0, j = 0;
	u8 *obuf = NULL;
	u16 size = 0;
	u16 addr = 0;

	sscanf(buf, "%s %d %d %d", command, &type, &addr_offset, &value);

	obuf = kzalloc(256, GFP_KERNEL);
	if (!obuf)
		return -ENOMEM;

	if (type == 25)
		selftest_enable = true;

	object = mxt_get_object(data, type);
	if (!object) {
        TOUCH_INFO_MSG("error Cannot get object_type T%d\n", type);
        return -EINVAL;
    }

	if ((mxt_obj_size(object) == 0) || (object->start_address == 0)) {
		TOUCH_INFO_MSG("error object_type T%d\n", type);
		return -ENODEV;
	}

	if (!strncmp(command, "read", 4)) {	/*read*/
		TOUCH_INFO_MSG("Object Read T%d: start_addr=%d, size=%d * instance=%d\n",
		type, object->start_address, mxt_obj_size(object), mxt_obj_instances(object));

		for (j = 0; j < mxt_obj_instances(object); j++) {
			size = mxt_obj_size(object);
			addr = object->start_address + j * size;

			error = __mxt_read_reg(data->client, addr, size, obuf);
			if (error)
				TOUCH_INFO_MSG("Object Read Fail\n");
		}

		for (i = 0; i < mxt_obj_size(object)*mxt_obj_instances(object); i++)
			TOUCH_INFO_MSG("T%d [%d] %d[0x%x]\n", type, i, obuf[i], obuf[i]);

	}else if (!strncmp(command, "write", 4)) {	/*write*/
		TOUCH_INFO_MSG("Object Write T%d: start_addr=%d, size=%d * instance=%d\n",
			type, object->start_address, mxt_obj_size(object), mxt_obj_instances(object));

		error = mxt_write_reg(data->client, object->start_address+addr_offset, value);
		if (error)
			TOUCH_INFO_MSG("Object Write Fail\n");

		TOUCH_INFO_MSG("Object Write Success. Execute Read Object and Check Value.\n");
	}else{
		TOUCH_INFO_MSG("Command Fail. Usage: echo [read | write] object cmd_field value > object_ctrl\n");
	}
	return count;
}

static int mxt_check_firmware_format(struct device *dev, const struct firmware *fw)
{
	unsigned int pos = 0;
	char c = 0;

	while (pos < fw->size) {
		c = *(fw->data + pos);

		if (c < '0' || (c > '9' && c < 'A') || c > 'F')
			return 0;

		pos++;
	}

	/* To convert file try
	 * xxd -r -p mXTXXX__APP_VX-X-XX.enc > maxtouch.fw */
	TOUCH_INFO_MSG("Aborting: firmware file must be in binary format\n");

	return -1;
}

static int mxt_load_bin(struct device *dev, const char *name)
{
	struct mxt_data *data = dev_get_drvdata(dev);
	const struct firmware *fw = NULL;
	unsigned int frame_size = 0;
	unsigned int pos = 0;
	unsigned int retry = 0;
	unsigned int frame = 0;
	int ret = 0;

	if (!name) {
		TOUCH_INFO_MSG("Skipping bin download \n");
		return 0;
	}

	ret = request_firmware(&fw, name, dev);
	if (ret) {
		TOUCH_INFO_MSG("Unable to open bin [%s]  ret %d\n",name, ret);
		return 1;
	}
	else {
		TOUCH_INFO_MSG("Open bin [%s]\n", name);
	}

	/* Check for incorrect enc file */
	ret = mxt_check_firmware_format(dev, fw);
	if (ret)
		goto release_firmware;

	if (data->suspended) {
		if (data->use_regulator)
			mxt_regulator_enable(data);

		touch_enable_irq(data->irq);
		data->suspended = false;
	}

	if (!data->in_bootloader) {
		/* Change to the bootloader mode */
		data->in_bootloader = true;

		ret = mxt_t6_command(data, MXT_COMMAND_RESET, MXT_BOOT_VALUE, false);
		if (ret)
			goto release_firmware;

		msleep(MXT_RESET_TIME);

		/* At this stage, do not need to scan since we know
		 * family ID */
		ret = mxt_lookup_bootloader_address(data, 0);
		if (ret)
			goto release_firmware;
	}

	mxt_free_object_table(data);
	INIT_COMPLETION(data->bl_completion);

	ret = mxt_check_bootloader(data, MXT_WAITING_BOOTLOAD_CMD);
	if (ret) {
		/* Bootloader may still be unlocked from previous update
		 * attempt */
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret)
			goto disable_irq;
	} else {
		TOUCH_INFO_MSG("Unlocking bootloader\n");

		/* Unlock bootloader */
		ret = mxt_send_bootloader_cmd(data, true);
		if (ret)
			goto disable_irq;
	}

	while (pos < fw->size) {
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret)
			goto disable_irq;

		frame_size = ((*(fw->data + pos) << 8) | *(fw->data + pos + 1));

		/* Take account of CRC bytes */
		frame_size += 2;

		/* Write one frame to device */
		ret = mxt_bootloader_write(data, fw->data + pos, frame_size);
		if (ret)
			goto disable_irq;

		ret = mxt_check_bootloader(data, MXT_FRAME_CRC_PASS);
		if (ret) {
			retry++;

			/* Back off by 20ms per retry */
			msleep(retry * 20);

			if (retry > 20) {
				TOUCH_INFO_MSG("Retry count exceeded\n");
				goto disable_irq;
			}
		} else {
			retry = 0;
			pos += frame_size;
			frame++;
		}

		if (frame % 50 == 0)
			TOUCH_INFO_MSG("Sent %d frames, %d/%zd bytes\n",
				 frame, pos, fw->size);
	}

	/* Wait for flash */
	ret = mxt_wait_for_completion(data, &data->bl_completion,
				      MXT_FW_RESET_TIME);
	if (ret)
		goto disable_irq;
	TOUCH_INFO_MSG("Sent %d frames, %zd bytes\n", frame, pos);

#if 0	/*To avoid reset timeout*/
	/* Wait for device to reset */
	mxt_wait_for_completion(data, &data->bl_completion, MXT_RESET_TIMEOUT);
#endif
	data->in_bootloader = false;

disable_irq:
	touch_disable_irq(data->irq);
release_firmware:
	release_firmware(fw);
	return ret;
}

static ssize_t mxt_update_bin_store(struct mxt_data *data, const char *buf, size_t count)
{
	int error = 0;
	char *name = NULL;

	error = mxt_update_file_name(&data->client->dev, &name, buf, count);
	if (error)
		return error;

	error = mxt_load_bin(&data->client->dev, name);
	if (error) {
		TOUCH_INFO_MSG("The bin update failed(%d)\n", error);
		count = error;
	} else {
		TOUCH_INFO_MSG("The bin update succeeded\n");

		data->suspended = false;

		mxt_hw_reset(data);

		error = mxt_read_info_block(data);
		if (error)
			return error;

		error = mxt_rest_init(data);
		if (error)
			return error;

		TOUCH_INFO_MSG("Need to update proper Configuration(RAW) \n");
	}
	return count;
}

static ssize_t mxt_update_raw_store(struct mxt_data *data, const char *buf, size_t count)
{
	int ret = 0;
	int value = 0;
	char *name = NULL;

	sscanf(buf, "%d", &value);
	TOUCH_INFO_MSG("Update mxt Configuration.\n");

	if (data->in_bootloader) {
		TOUCH_INFO_MSG("Not in appmode\n");
		return -EINVAL;
	}

	ret = mxt_update_file_name(&data->client->dev, &name, buf, count);
	if (ret)
		return ret;

	data->enable_reporting = false;

	ret = mxt_check_reg_init(data, name);
	if (ret < 0) {
		TOUCH_INFO_MSG("Error mxt_check_reg_init ret=%d\n", ret);
		goto out;
	}

	TOUCH_INFO_MSG("Update mxt Configuration Success.\n");

out:
	data->enable_reporting = true;

	return count;
}

static ssize_t mxt_update_fw_store(struct mxt_data *data, const char *buf, size_t count)
{
	char *package_name = NULL;
	int error = 0;
	int wait_cnt = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));

	if (data->suspended) {
		TOUCH_INFO_MSG("LCD On \n");
		kobject_uevent_env(&lge_touch_sys_device.kobj, KOBJ_CHANGE, knockon_event);
		while(1) {
			if (data->suspended) {
				mdelay(100);
				wait_cnt++;
			}

			if (!data->suspended || wait_cnt > 50)
				break;
		}
	}

	TOUCH_INFO_MSG("wait_cnt = %d \n", wait_cnt);

	touch_disable_irq(data->irq);

	error = mxt_update_file_name(&data->client->dev, &package_name, buf, count);
	if (error) {
		TOUCH_INFO_MSG("%s error package_name [%s] \n", __func__, package_name);
		goto exit;
	}

	error = mxt_update_firmware(data, package_name);
	if (error) {
		TOUCH_INFO_MSG("%s error \n", __func__);
		goto exit;
	}

	if (global_mxt_data) {
		error = __mxt_read_reg(global_mxt_data->client, global_mxt_data->T71_address + 51, 26, &global_mxt_data->ref_limit);
		global_mxt_data->ref_limit.diff_scaling = data->pdata->diff_scaling;


		if (!error) {
			TOUCH_INFO_MSG("Succeed to read diff_scaling:%d reference limit u:%d x_all_err_chk:%d y_all_err_chk:%d only_x_err_cnt : %d only_y_err_cnt : %d max-min rng:%d diff_rng:%d err_cnt:%d err_weight:%d\n",
				global_mxt_data->ref_limit.diff_scaling,
				global_mxt_data->ref_limit.ref_chk_using, global_mxt_data->ref_limit.ref_x_all_err_line,
				global_mxt_data->ref_limit.ref_y_all_err_line, global_mxt_data->ref_limit.xline_max_err_cnt,
				global_mxt_data->ref_limit.yline_max_err_cnt, global_mxt_data->ref_limit.ref_rng_limit,
				global_mxt_data->ref_limit.ref_diff_max, global_mxt_data->ref_limit.ref_err_cnt,
				global_mxt_data->ref_limit.err_weight);
			TOUCH_INFO_MSG("Err Range y: %d %d %d %d %d %d %d %d %d %d %d %d %d %d, Button range:%d %d %d\n",
				global_mxt_data->ref_limit.y_line_dif[0], global_mxt_data->ref_limit.y_line_dif[1],
				global_mxt_data->ref_limit.y_line_dif[2], global_mxt_data->ref_limit.y_line_dif[3],
				global_mxt_data->ref_limit.y_line_dif[4], global_mxt_data->ref_limit.y_line_dif[5],
				global_mxt_data->ref_limit.y_line_dif[6], global_mxt_data->ref_limit.y_line_dif[7],
				global_mxt_data->ref_limit.y_line_dif[8], global_mxt_data->ref_limit.y_line_dif[9],
				global_mxt_data->ref_limit.y_line_dif[10], global_mxt_data->ref_limit.y_line_dif[11],
				global_mxt_data->ref_limit.y_line_dif[12], global_mxt_data->ref_limit.y_line_dif[13],
				global_mxt_data->ref_limit.butt_dif[0], global_mxt_data->ref_limit.butt_dif[1],
				global_mxt_data->ref_limit.butt_dif[2]);
		}
	}

exit:
	if (package_name) {
		kfree(package_name);
	}

	if (mxt_patchevent_get(PATCH_EVENT_TA)) {
		trigger_usb_state_from_otg(1);
	}

	touch_enable_irq(data->irq);

	mxt_read_fw_version(data);

	return count;
}

static ssize_t mxt_debug_enable_show(struct mxt_data *data, char *buf)
{
	int count = 0;
	char c = 0;

	c = data->debug_enabled ? '1' : '0';
	count = sprintf(buf, "%c\n", c);

	return count;
}

static ssize_t mxt_debug_enable_store(struct mxt_data *data, const char *buf, size_t count)
{
	int i = 0;

	if (sscanf(buf, "%u", &i) == 1 && i < 2) {
		data->debug_enabled = (i == 1);

		TOUCH_INFO_MSG("%s\n", i ? "debug enabled" : "debug disabled");
		return count;
	} else {
		TOUCH_INFO_MSG("debug_enabled write error\n");
		return -EINVAL;
	}
}

static ssize_t mxt_t57_debug_enable_store(struct mxt_data *data, const char *buf, size_t count)
{
	int i = 0;

	if (sscanf(buf, "%u", &i) == 1 && i < 2) {
		data->t57_debug_enabled = (i == 1);

		TOUCH_INFO_MSG("%s\n", i ? "t57 debug enabled" : "t57 debug disabled");
		return count;
	} else {
		TOUCH_INFO_MSG("t57_debug_enabled write error\n");
		return -EINVAL;
	}
}

static ssize_t mxt_patch_debug_enable_show(struct mxt_data *data, char *buf)
{
	int count = 0;
	char c = 0;

	if (data->patch.patch == NULL) {
		TOUCH_INFO_MSG("patch not support \n");
		return count;
	}

	c = data->patch.debug ? '1' : '0';
	count = sprintf(buf, "%c\n", c);

	return count;
}

static ssize_t mxt_patch_debug_enable_store(struct mxt_data *data, const char *buf, size_t count)
{
	int i = 0;

	if (data->patch.patch == NULL) {
		TOUCH_INFO_MSG("patch not support \n");
		return count;
	}

	if (sscanf(buf, "%u", &i) == 1 && i < 2) {
		data->patch.debug = (i == 1);

		TOUCH_INFO_MSG("%s\n", i ? "patch debug enabled" : "patch debug disabled");
		return count;
	} else {
		TOUCH_INFO_MSG("patch_debug_enabled write error\n");
		return -EINVAL;
	}
}

static ssize_t mxt_power_control_show(struct mxt_data *data, char *buf)
{
	int ret = 0;

	ret += sprintf(buf+ret, "usage: echo [0|1|2|3] > power_control \n");
	ret += sprintf(buf+ret, "  0 : power off \n");
	ret += sprintf(buf+ret, "  1 : power on \n");
	ret += sprintf(buf+ret, "  2 : reset by I2C \n");
	ret += sprintf(buf+ret, "  3 : reset by reset_gpio \n");

	return ret;
}

static ssize_t mxt_power_control_store(struct mxt_data *data, const char *buf, size_t count)
{
	int cmd = 0;

	if (sscanf(buf, "%d", &cmd) != 1)
		return -EINVAL;
	switch (cmd) {
		case 0:
			mxt_set_t7_power_cfg(data, MXT_POWER_CFG_DEEPSLEEP);
			mxt_regulator_disable(data);
			break;
		case 1:
			mxt_regulator_enable(data);
			mxt_set_t7_power_cfg(data, MXT_POWER_CFG_RUN);
			mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
			break;
		case 2:
			mxt_soft_reset(data);
			break;
		case 3:
			mxt_hw_reset(data);
			break;
		default:
			TOUCH_INFO_MSG("usage: echo [0|1|2|3] > power_control \n");
			TOUCH_INFO_MSG("  0 : power off \n");
			TOUCH_INFO_MSG("  1 : power on \n");
			TOUCH_INFO_MSG("  2 : reset by I2C \n");
			TOUCH_INFO_MSG("  3 : reset by reset_gpio \n");
			break;
	}
	return count;

}

static int mxt_check_mem_access_params(struct mxt_data *data, loff_t off, size_t *count)
{
	data->mem_size = 32768;

	if (off >= data->mem_size)
		return -EIO;

	if (off + *count > data->mem_size)
		*count = data->mem_size - off;

	if (*count > MXT_MAX_BLOCK_WRITE)
		*count = MXT_MAX_BLOCK_WRITE;

	return 0;
}

static ssize_t mxt_mem_access_read(struct file *filp, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret = 0;

	ret = mxt_check_mem_access_params(data, off, &count);
	if (ret < 0)
		return ret;

	if (count > 0)
		ret = __mxt_read_reg(data->client, off, count, buf);

	return ret == 0 ? count : ret;
}

static ssize_t mxt_mem_access_write(struct file *filp, struct kobject *kobj,
	struct bin_attribute *bin_attr, char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct mxt_data *data = dev_get_drvdata(dev);
	int ret = 0;

	ret = mxt_check_mem_access_params(data, off, &count);
	if (ret < 0)
		return ret;

	if (count > 0)
		ret = __mxt_write_reg(data->client, off, count, buf);

	return ret == 0 ? count : 0;
}

static ssize_t mxt_get_knockon_type(struct mxt_data *data, char *buf)
{
	int ret = 0;

	ret += sprintf(buf+ret, "%d", data->pdata->knock_on_type);

	return ret;
}

static ssize_t mxt_knock_on_store(struct mxt_data *data, const char *buf, size_t size)
{
	int value = 0;

	if (mutex_is_locked(&i2c_suspend_lock)) {
		TOUCH_INFO_MSG("%s mutex_is_locked \n", __func__);
	}

	sscanf(buf, "%d", &value);

	if (data->suspended) {
		mxt_reset_slots(data);
		if (value) {
			mxt_regulator_enable(data);
			mxt_set_t7_power_cfg(data, MXT_POWER_CFG_RUN);

			if (mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
				TOUCH_INFO_MSG("Suspend : Knock On already enabled \n");
			} else {
				mxt_patchevent_set(PATCH_EVENT_KNOCKON);
				if (mxt_patchevent_get(PATCH_EVENT_TA)) {
					TOUCH_INFO_MSG("Suspend : Knock On Enabled(TA) \n");
					mxt_patch_event(global_mxt_data, CHARGER_KNOCKON_SLEEP);
				} else {
					TOUCH_INFO_MSG("Suspend : Knock On Enabled \n");
					mxt_patch_event(global_mxt_data, NOCHARGER_KNOCKON_SLEEP);
				}
			}
			touch_enable_irq_wake(data->irq);
			touch_enable_irq(data->irq);
			data->enable_reporting = true;
			data->mxt_knock_on_enable = true;
		} else {
			if (mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
				TOUCH_INFO_MSG("Suspend : Knock On Disabled \n");
				mxt_set_t7_power_cfg(data, MXT_POWER_CFG_DEEPSLEEP);
				mxt_regulator_disable(data);
				mxt_patchevent_unset(PATCH_EVENT_KNOCKON);
			} else {
				TOUCH_INFO_MSG("Suspend : Knock On already disabled \n");
			}
			touch_disable_irq_wake(data->irq);
			touch_disable_irq(data->irq);
			data->enable_reporting = false;
		}
	} else {
		if (value)
			data->mxt_knock_on_enable = true;
		else
			data->mxt_knock_on_enable = false;
		TOUCH_INFO_MSG("Knock On : %s\n", data->mxt_knock_on_enable ? "Enabled" : "Disabled");
	}

	return size;
}

static void write_file(char *filename, char *data, int time)
{
	int fd = 0;
	char time_string[64] = {0};
	struct timespec my_time;
	struct tm my_date;
	mm_segment_t old_fs = get_fs();

	my_time = __current_kernel_time();
	time_to_tm(my_time.tv_sec, sys_tz.tz_minuteswest * 60 * (-1), &my_date);
	snprintf(time_string, 64, "%02d-%02d %02d:%02d:%02d.%03lu \n",
		my_date.tm_mon + 1,my_date.tm_mday,
		my_date.tm_hour, my_date.tm_min, my_date.tm_sec,
		(unsigned long) my_time.tv_nsec / 1000000);

	set_fs(KERNEL_DS);
	fd = sys_open(filename, O_WRONLY|O_CREAT|O_APPEND, 0666);
	TOUCH_INFO_MSG("write open %s, fd : %d\n", (fd >= 0)? "success":"fail",fd);
	if (fd >= 0) {
		if (time > 0) {
			sys_write(fd, time_string, strlen(time_string));
			TOUCH_INFO_MSG("Time write success.\n");
		}
		sys_write(fd, data, strlen(data));
		sys_close(fd);
	}
	set_fs(old_fs);

}

static ssize_t mxt_run_delta_show(struct mxt_data *data, char *buf)
{
	int len = 0;

	mxt_power_block(POWERLOCK_SYSFS);

	if (data->pdata->panel_on == POWER_OFF) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : OFF ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
	} else if (data->pdata->panel_on == POWER_ON) {
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : O N ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
	}

	run_delta_read(data, buf, &len);
	msleep(30);

	mxt_power_unblock(POWERLOCK_SYSFS);

	return len;
}

static ssize_t mxt_run_chstatus_show(struct mxt_data *data, char *buf)
{
	int len=0;

	mxt_power_block(POWERLOCK_SYSFS);

	if (data->pdata->panel_on == POWER_OFF) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : OFF ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
	} else if (data->pdata->panel_on == POWER_ON) {
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : O N ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
	}

	len += snprintf(buf + len, PAGE_SIZE - len, "====== MXT Self Test Info ======\n");
	len = mxt_selftest(data, buf, len);

	run_reference_read(data, buf, &len);
	msleep(30);

	mxt_power_unblock(POWERLOCK_SYSFS);

	return len;
}

static ssize_t mxt_run_rawdata_show(struct mxt_data *data, char *buf)
{
	int len=0;

	mxt_power_block(POWERLOCK_SYSFS);

	if (data->pdata->panel_on == POWER_OFF) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : OFF ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
	} else if (data->pdata->panel_on == POWER_ON) {
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : O N ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
	}

	run_reference_read(data, buf, &len);
	msleep(30);

	mxt_power_unblock(POWERLOCK_SYSFS);

	return len;
}

static ssize_t mxt_run_self_diagnostic_show(struct mxt_data *data, char *buf)
{
	int i = 0;
	int len = 0;
	int info_len = 0;
	int ref_len = 0;
	int error = 0;
	char *ref_buf = NULL;
	char *temp_buf = NULL;
	bool chstatus_result = 1;
	bool rawdata_result = 1;
	struct input_dev *input_dev = data->input_dev;

	mxt_power_block(POWERLOCK_SYSFS);

	data->self_test_result_status = SELF_DIAGNOSTIC_STATUS_RUNNING;

	if (data->pdata->panel_on == POWER_OFF) {
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "*** LCD STATUS : OFF ***\n");
		len += snprintf(buf + len, PAGE_SIZE - len, "************************\n");
		mxt_power_unblock(POWERLOCK_SYSFS);
		data->self_test_result_status = SELF_DIAGNOSTIC_STATUS_COMPLETE;
		return len;
	}

	mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);
	msleep(30);

	ref_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!ref_buf) {
		TOUCH_INFO_MSG("%s Failed to allocate memory\n", __func__);
		mxt_power_unblock(POWERLOCK_SYSFS);
		data->self_test_result_status = SELF_DIAGNOSTIC_STATUS_COMPLETE;
		return 0;
	}
	temp_buf = kzalloc(100, GFP_KERNEL);
	if (!temp_buf) {
		TOUCH_INFO_MSG("%s Failed to allocate memory\n", __func__);
		mxt_power_unblock(POWERLOCK_SYSFS);
		data->self_test_result_status = SELF_DIAGNOSTIC_STATUS_COMPLETE;
		return 0;
	}

	/* allocation of full_cap */
	data->full_cap = NULL;

	data->full_cap = (int **)kzalloc(data->info->matrix_xsize * sizeof(int *), GFP_KERNEL);
	if(!data->full_cap)
	{
		TOUCH_INFO_MSG("full_cap data allocation error\n");
		return -1;
	}
	memset(data->full_cap, 0, data->info->matrix_xsize * sizeof(int *));

	for(i = 0; i < data->info->matrix_xsize; i++) {
		data->full_cap[i] = kzalloc(data->info->matrix_ysize * sizeof(int), GFP_KERNEL);
	}

	write_file(SELF_DIAGNOSTIC_FILE_PATH, buf, 1);
	msleep(30);
	len += mxt_info_show(data, buf);
	len += snprintf(buf + len, PAGE_SIZE - len, "=======RESULT========\n");
	info_len = len;
	len = mxt_selftest(data, buf, len);
	write_file(SELF_DIAGNOSTIC_FILE_PATH, buf, 0);
	msleep(30);
	run_reference_read(data, ref_buf, &ref_len);
	msleep(30);

	mxt_get_cap_diff(data);

	snprintf(temp_buf, PAGE_SIZE, "Cap Diff : %s\n", global_mxt_data->bad_sample == 0 ? "Pass" : "Fail");
	write_file(SELF_DIAGNOSTIC_FILE_PATH, temp_buf, 0);

	error = mxt_scaling_factor_chk(data);
	if (error) {
		TOUCH_INFO_MSG("%s Failed to mxt_scailing_factor_chk\n", __func__);
	}

	if (global_mxt_data) {
		error = __mxt_read_reg(global_mxt_data->client, global_mxt_data->T71_address + 51, 26, &global_mxt_data->ref_limit);
		global_mxt_data->ref_limit.diff_scaling = data->pdata->diff_scaling;

		if (!error) {
			TOUCH_INFO_MSG("Succeed to read diff_scaling:%d reference limit u:%d x_all_err_chk:%d y_all_err_chk:%d only_x_err_cnt : %d only_y_err_cnt : %d max-min rng:%d diff_rng:%d err_cnt:%d err_weight:%d\n",
				global_mxt_data->ref_limit.diff_scaling,
				global_mxt_data->ref_limit.ref_chk_using, global_mxt_data->ref_limit.ref_x_all_err_line,
				global_mxt_data->ref_limit.ref_y_all_err_line, global_mxt_data->ref_limit.xline_max_err_cnt,
				global_mxt_data->ref_limit.yline_max_err_cnt, global_mxt_data->ref_limit.ref_rng_limit,
				global_mxt_data->ref_limit.ref_diff_max, global_mxt_data->ref_limit.ref_err_cnt,
				global_mxt_data->ref_limit.err_weight);
			TOUCH_INFO_MSG("Err Range y: %d %d %d %d %d %d %d %d %d %d %d %d %d %d, Button range:%d %d %d\n",
				global_mxt_data->ref_limit.y_line_dif[0], global_mxt_data->ref_limit.y_line_dif[1],
				global_mxt_data->ref_limit.y_line_dif[2], global_mxt_data->ref_limit.y_line_dif[3],
				global_mxt_data->ref_limit.y_line_dif[4], global_mxt_data->ref_limit.y_line_dif[5],
				global_mxt_data->ref_limit.y_line_dif[6], global_mxt_data->ref_limit.y_line_dif[7],
				global_mxt_data->ref_limit.y_line_dif[8], global_mxt_data->ref_limit.y_line_dif[9],
				global_mxt_data->ref_limit.y_line_dif[10], global_mxt_data->ref_limit.y_line_dif[11],
				global_mxt_data->ref_limit.y_line_dif[12], global_mxt_data->ref_limit.y_line_dif[13],
				global_mxt_data->ref_limit.butt_dif[0], global_mxt_data->ref_limit.butt_dif[1],
				global_mxt_data->ref_limit.butt_dif[2]);
		}
	}

	memset(temp_buf, 0, sizeof(temp_buf));
	if (data->ref_limit.ref_chk_using) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(1000));

		mutex_lock(&input_dev->mutex);

		mxt_get_reference_chk(data);

		mutex_unlock(&input_dev->mutex);
	}
	TOUCH_INFO_MSG("self-diagnostic error node cnt:%d\n", data->err_node_cnt);
	snprintf(temp_buf, PAGE_SIZE, "Error node Check: %s(err count:%d)\n", (data->err_node_cnt == 0 ? "Pass" : "Fail"), data->err_node_cnt);

	write_file(SELF_DIAGNOSTIC_FILE_PATH, temp_buf, 0);
	write_file(SELF_DIAGNOSTIC_FILE_PATH, ref_buf, 0);
	kfree(ref_buf);
	kfree(temp_buf);

	if (data->full_cap != NULL) {
		for(i = 0; i < data->info->matrix_xsize; i++) {
			if(data->full_cap[i] != NULL)
				kfree(data->full_cap[i]);
		}
		kfree((int*)data->full_cap);
		data->full_cap = NULL;
	}

	if ((data->self_test_status[0] == 0x01) || (data->self_test_status[0] == 0x12))
		chstatus_result = 0;

	if (data->self_test_status[0] == 0x17 || data->bad_sample || data->err_node_cnt)
		rawdata_result = 0;
	if (data->self_test_status[0] == 0) {
		info_len += snprintf(buf + info_len, PAGE_SIZE - info_len, "Need more time. Try Again.\n");
	} else if (data->self_test_status[0] == 0xFD) {
		info_len += snprintf(buf + info_len, PAGE_SIZE - info_len, "Invalid Test Code. Try Again.\n");
	} else if (data->self_test_status[0] == 0xFC) {
		info_len += snprintf(buf + info_len, PAGE_SIZE - info_len, "The test could not be completed. Try Again.\n");
	} else {
		info_len += snprintf(buf + info_len, PAGE_SIZE - info_len, "Channel Status : %s\n", chstatus_result == 1 ? "PASS" : "FAIL");
		info_len += snprintf(buf + info_len, PAGE_SIZE - info_len, "Raw Data : %s\n", rawdata_result == 1 ? "PASS" : "FAIL");
	}
	mxt_power_unblock(POWERLOCK_SYSFS);

	data->self_test_result_status = SELF_DIAGNOSTIC_STATUS_COMPLETE;

	return info_len;
}

static ssize_t mxt_run_self_diagnostic_status_show(struct mxt_data *data, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d \n", data->self_test_result_status);

	return len;
}

static ssize_t mxt_ghost_detection_enable_store(struct mxt_data *data, const char *buf, size_t count)
{
	int value = 0;

	sscanf(buf, "%d", &value);

	TOUCH_INFO_MSG("%s = %d \n", __func__, value);
	data->pdata->ghost_detection_enable= value;

	return count;
}


static ssize_t mxt_global_access_pixel_show(struct mxt_data *data, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d\n", data->pdata->global_access_pixel);

	return len;
}

static ssize_t mxt_global_access_pixel_store(struct mxt_data *data, const char *buf, size_t count)
{
	int value = 0;

	sscanf(buf, "%d", &value);

	TOUCH_INFO_MSG("%s = %d \n", __func__, value);

	data->pdata->global_access_pixel = value;

	return count;
}

static ssize_t mxt_force_rebase_show(struct mxt_data *data, char *buf)
{
	int len = 0;

	if (data->pdata->panel_on == POWER_OFF) {
		wake_lock_timeout(&touch_wake_lock, msecs_to_jiffies(2000));
	}

	TOUCH_INFO_MSG("MXT_COMMAND_CALIBRATE \n");
	mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);

	return len;
}

static ssize_t mxt_mfts_enable_show(struct mxt_data *data, char *buf)
{
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "%d\n", data->mfts_enable);

	return len;
}

static ssize_t mxt_mfts_enable_store(struct mxt_data *data, const char *buf, size_t count)
{
	int value = 0;

	sscanf(buf, "%d", &value);

	TOUCH_INFO_MSG("%s = %d \n", __func__, value);

	data->mfts_enable = value;

	return count;
}

static ssize_t store_incoming_call(struct mxt_data *data, const char *buf, size_t count)
{
	static char incoming_call_str[3][8] = {"IDLE", "RINGING", "OFFHOOK"};

	sscanf(buf, "%d", &data->incoming_call);

	if (data->incoming_call <= INCOMING_CALL_OFFHOOK)
		TOUCH_INFO_MSG("%s : %s(%d) \n", __func__, incoming_call_str[data->incoming_call], data->incoming_call);
	else
		TOUCH_INFO_MSG("%s : %d \n", __func__, data->incoming_call);

	return count;
}

static ssize_t show_lockscreen(struct mxt_data *data, char *buf)
{
	int ret = 0;

	ret += sprintf(buf+ret,"%s : %d \n", __func__, data->lockscreen);

	return ret;
}

static ssize_t store_lockscreen(struct mxt_data *data, const char *buf, size_t count)
{
	int value = 0;

	data->lockscreen = value;

	sscanf(buf, "%d", &data->lockscreen);

	TOUCH_INFO_MSG("%s : %d \n", __func__, data->lockscreen);

	return count;
}

static ssize_t store_ime_status(struct mxt_data *data, const char *buf, size_t count)
{
	int value = 0;
	int error = 0;
	static u8 cfg_tchdi = 0;
	static u8 cfg_jumplimit = 0;
	sscanf(buf, "%d", &value);

	if(ime_status_value == value)
		return count;

	TOUCH_INFO_MSG("%s : %d \n", __func__, value);

	if (cfg_tchdi == 0) {
		error = mxt_read_object(data, MXT_TOUCH_MULTI_T9, MXT_T9_TCHDI, &cfg_tchdi);
		if (error)
			TOUCH_INFO_MSG("Failed to get t9 8th config\n");
		else
			TOUCH_INFO_MSG("default t9 8th config is %d\n", cfg_tchdi);
	}

	if (cfg_jumplimit == 0) {
		error = mxt_read_object(data, MXT_TOUCH_MULTI_T9, MXT_T9_JUMPLIMIT, &cfg_jumplimit);
		if (error)
			TOUCH_INFO_MSG("Failed to get t9 30th config\n");
		else
			TOUCH_INFO_MSG("default t9 30th config is %d\n", cfg_jumplimit);
	}

	if (value == 1){
		error = mxt_write_object(data, MXT_TOUCH_MULTI_T9, MXT_T9_TCHDI, 3);
	} else if (value == 0) {
		error = mxt_write_object(data, MXT_TOUCH_MULTI_T9, MXT_T9_TCHDI, cfg_tchdi);
	}

	if (value == 1){
		error = mxt_write_object(data, MXT_TOUCH_MULTI_T9, MXT_T9_JUMPLIMIT, 4);
	} else if (value == 0) {
		error = mxt_write_object(data, MXT_TOUCH_MULTI_T9, MXT_T9_JUMPLIMIT, cfg_jumplimit);
	}

	if (error)
		TOUCH_INFO_MSG("Failed to write register for keyguard\n");
	else
		ime_status_value = value;

	return count;
}


static LGE_TOUCH_ATTR(fw_version, S_IRUGO, mxt_fw_version_show, NULL);
static LGE_TOUCH_ATTR(hw_version, S_IRUGO, mxt_hw_version_show, NULL);
static LGE_TOUCH_ATTR(testmode_ver, S_IRUGO | S_IWUSR, mxt_testmode_ver_show, NULL);
static LGE_TOUCH_ATTR(version, S_IRUGO, mxt_info_show, NULL);
static LGE_TOUCH_ATTR(mxt_info, S_IRUGO, mxt_info_show, NULL);
static LGE_TOUCH_ATTR(object, S_IRUGO, mxt_object_show, NULL);
static LGE_TOUCH_ATTR(object_ctrl, S_IWUSR, NULL, mxt_object_control);
static LGE_TOUCH_ATTR(update_bin, S_IWUSR, NULL, mxt_update_bin_store);
static LGE_TOUCH_ATTR(update_raw, S_IWUSR, NULL, mxt_update_raw_store);
static LGE_TOUCH_ATTR(debug_enable, S_IWUSR | S_IRUSR, mxt_debug_enable_show, mxt_debug_enable_store);
static LGE_TOUCH_ATTR(t57_debug_enable, S_IWUSR | S_IRUSR, NULL, mxt_t57_debug_enable_store);
static LGE_TOUCH_ATTR(patch_debug_enable, S_IWUSR | S_IRUSR, mxt_patch_debug_enable_show, mxt_patch_debug_enable_store);
static LGE_TOUCH_ATTR(knock_on_type, S_IRUGO, mxt_get_knockon_type, NULL);
static LGE_TOUCH_ATTR(touch_gesture,S_IRUGO | S_IWUSR, NULL, mxt_knock_on_store);
static LGE_TOUCH_ATTR(delta, S_IRUGO, mxt_run_delta_show, NULL);
static LGE_TOUCH_ATTR(chstatus, S_IRUGO, mxt_run_chstatus_show, NULL);
static LGE_TOUCH_ATTR(rawdata, S_IRUGO, mxt_run_rawdata_show, NULL);
static LGE_TOUCH_ATTR(sd, S_IRUGO, mxt_run_self_diagnostic_show, NULL);
static LGE_TOUCH_ATTR(sd_status, S_IRUGO, mxt_run_self_diagnostic_status_show, NULL);
static LGE_TOUCH_ATTR(update_patch, S_IWUSR, NULL, mxt_update_patch_store);
static LGE_TOUCH_ATTR(update_fw, S_IWUSR, NULL, mxt_update_fw_store);
static LGE_TOUCH_ATTR(power_control, S_IRUGO | S_IWUSR, mxt_power_control_show, mxt_power_control_store);
static LGE_TOUCH_ATTR(ghost_detection_enable, S_IWUSR, NULL, mxt_ghost_detection_enable_store);
static LGE_TOUCH_ATTR(global_access_pixel, S_IWUSR | S_IRUSR, mxt_global_access_pixel_show, mxt_global_access_pixel_store);
static LGE_TOUCH_ATTR(rebase, S_IRUGO, mxt_force_rebase_show, NULL);
static LGE_TOUCH_ATTR(mfts, S_IWUSR | S_IRUSR, mxt_mfts_enable_show, mxt_mfts_enable_store);
static LGE_TOUCH_ATTR(incoming_call, S_IRUGO | S_IWUSR, NULL, store_incoming_call);
static LGE_TOUCH_ATTR(lockscreen, S_IRUGO | S_IWUSR, show_lockscreen, store_lockscreen);
static LGE_TOUCH_ATTR(ime_status, S_IRUGO | S_IWUSR, NULL, store_ime_status);


static struct attribute *lge_touch_attribute_list[] = {
	&lge_touch_attr_fw_version.attr,
	&lge_touch_attr_hw_version.attr,
	&lge_touch_attr_testmode_ver.attr,
	&lge_touch_attr_version.attr,
	&lge_touch_attr_mxt_info.attr,
	&lge_touch_attr_object.attr,
	&lge_touch_attr_object_ctrl.attr,
	&lge_touch_attr_update_bin.attr,
	&lge_touch_attr_update_raw.attr,
	&lge_touch_attr_debug_enable.attr,
	&lge_touch_attr_t57_debug_enable.attr,
	&lge_touch_attr_patch_debug_enable.attr,
	&lge_touch_attr_knock_on_type.attr,
	&lge_touch_attr_touch_gesture.attr,
	&lge_touch_attr_delta.attr,
	&lge_touch_attr_chstatus.attr,
	&lge_touch_attr_rawdata.attr,
	&lge_touch_attr_sd.attr,
	&lge_touch_attr_sd_status.attr,
	&lge_touch_attr_update_patch.attr,
	&lge_touch_attr_update_fw.attr,
	&lge_touch_attr_power_control.attr,
	&lge_touch_attr_ghost_detection_enable.attr,
	&lge_touch_attr_global_access_pixel.attr,
	&lge_touch_attr_rebase.attr,
	&lge_touch_attr_mfts.attr,
	&lge_touch_attr_incoming_call.attr,
	&lge_touch_attr_lockscreen.attr,
	&lge_touch_attr_ime_status.attr,
	NULL
};

static ssize_t lge_touch_attr_show(struct kobject *lge_touch_kobj, struct attribute *attr, char *buf)
{
	struct mxt_data *ts = container_of(lge_touch_kobj, struct mxt_data, lge_touch_kobj);
	struct lge_touch_attribute *lge_touch_priv = container_of(attr, struct lge_touch_attribute, attr);
	ssize_t ret = 0;

	if (lge_touch_priv->show)
		ret = lge_touch_priv->show(ts, buf);

	return ret;
}

static ssize_t lge_touch_attr_store(struct kobject *lge_touch_kobj, struct attribute *attr, const char *buf, size_t count)
{
	struct mxt_data *ts = container_of(lge_touch_kobj, struct mxt_data, lge_touch_kobj);
	struct lge_touch_attribute *lge_touch_priv = container_of(attr, struct lge_touch_attribute, attr);
	ssize_t ret = 0;

	if (lge_touch_priv->store)
		ret = lge_touch_priv->store(ts, buf, count);

	return ret;
}

static const struct sysfs_ops lge_touch_sysfs_ops = {
	.show	= lge_touch_attr_show,
	.store	= lge_touch_attr_store,
};

static struct kobj_type lge_touch_kobj_type = {
	.sysfs_ops		= &lge_touch_sysfs_ops,
	.default_attrs 	= lge_touch_attribute_list,
};

static struct sysdev_class lge_touch_sys_class = {
	.name = LGE_TOUCH_NAME,
};

static struct sys_device lge_touch_sys_device = {
	.id		= 0,
	.cls	= &lge_touch_sys_class,
};

static void mxt_reset_slots(struct mxt_data *data)
{
	struct input_dev *input_dev = data->input_dev;
	int id = 0;

	for (id = 0; id < data->pdata->numtouch; id++) {
		input_mt_slot(input_dev, id);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
	}

	mxt_input_sync(input_dev);
	TOUCH_INFO_MSG("Release all event \n");

	memset(data->fingers, 0x0, sizeof(struct mxt_finger) * MXT_MAX_FINGER);
	memset(data->ts_data.prev_data, 0x0, sizeof(data->ts_data.prev_data));
	touched_finger_count = 0;
	data->button_lock = false;
	data->palm = false;
}

static void mxt_gesture_mode_start(struct mxt_data *data)
{
	if (global_mxt_data) {
		mxt_patchevent_set(PATCH_EVENT_KNOCKON);
		if (mxt_patchevent_get(PATCH_EVENT_TA)) {
			mxt_patch_event(global_mxt_data, CHARGER_KNOCKON_SLEEP);
		} else {
			mxt_patch_event(global_mxt_data, NOCHARGER_KNOCKON_SLEEP);
		}
	}
}

static void mxt_active_mode_start(struct mxt_data *data)
{
	if (global_mxt_data) {
		if (mxt_patchevent_get(PATCH_EVENT_TA)) {
			if (data->mxt_knock_on_enable || mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
				mxt_patch_event(global_mxt_data, CHARGER_KNOCKON_WAKEUP);
			}
		} else {
			if (data->mxt_knock_on_enable || mxt_patchevent_get(PATCH_EVENT_KNOCKON)) {
				mxt_patch_event(global_mxt_data, NOCHARGER_KNOCKON_WAKEUP);
			}
		}

		mxt_patchevent_unset(PATCH_EVENT_KNOCKON);
	}
}

static void mxt_start(struct mxt_data *data)
{
	if (!data->suspended || data->in_bootloader)
		return;

	TOUCH_INFO_MSG("%s \n", __func__);

	touch_disable_irq(data->irq);

	mxt_regulator_enable(data);

	mxt_set_t7_power_cfg(data, MXT_POWER_CFG_RUN);
	if (!mxt_patchevent_get(PATCH_EVENT_KNOCKON))
		mxt_patch_event(global_mxt_data, DEEP_SLEEP_WAKEUP);

	mxt_active_mode_start(data);

	/* Recalibrate since chip has been in deep sleep */
	mxt_t6_command(data, MXT_COMMAND_CALIBRATE, 1, false);

	mxt_reset_slots(data);
	data->suspended = false;
	data->button_lock = false;
	data->enable_reporting = true;
	touch_enable_irq(data->irq);
}

static void mxt_stop(struct mxt_data *data)
{
	if (data->suspended || data->in_bootloader)
		return;

	TOUCH_INFO_MSG("%s \n", __func__);

	touch_disable_irq(data->irq);

	if (data->mxt_knock_on_enable) {
		mxt_gesture_mode_start(data);
	} else {
		TOUCH_INFO_MSG("%s MXT_POWER_CFG_DEEPSLEEP\n", __func__);
		mxt_set_t7_power_cfg(data, MXT_POWER_CFG_DEEPSLEEP);
		mxt_regulator_disable(data);
	}

	mxt_reset_slots(data);
	data->suspended = true;
	data->button_lock = false;
	data->enable_reporting = false;

	if (data->mxt_knock_on_enable) {
		touch_enable_irq(data->irq);
	}
}

static int mxt_input_open(struct input_dev *dev)
{
	struct mxt_data *data = input_get_drvdata(dev);

	mxt_start(data);

	return 0;
}

static void mxt_input_close(struct input_dev *dev)
{
	struct mxt_data *data = input_get_drvdata(dev);

	mxt_stop(data);
}

static int device_is_d415 = 0;
/*
static int __init device_model_name(char *s)
{
       if (s == NULL) {
               device_is_d415 = 0;
               return 1;
       }

       if (!strcmp(s,"LG-D415") || !strcmp(s,"LG-D405")) {
               device_is_d415 = 1;
       } else {
               device_is_d415 = 0;
       }

       return 1;
}
__setup("model.name=", device_model_name);
*/
static int mxt_parse_dt(struct device *dev, struct mxt_platform_data *pdata)
{
	struct device_node *node = dev->of_node;
	struct property *prop = NULL;
	u32 temp_array[16] = {0};
	int rc = 0;
	int i = 0;
	u32 temp_val = 0;

	TOUCH_INFO_MSG("%s\n", __func__);

	/* reset, irq gpio info */
	if (node == NULL)
		return -ENODEV;

	pdata->gpio_reset= of_get_named_gpio_flags(node, "atmel,reset-gpio", 0, NULL);
	if (pdata->gpio_reset)
		TOUCH_INFO_MSG("DT : gpio_reset = %lu\n", pdata->gpio_reset);
	else
		TOUCH_INFO_MSG("DT get gpio_reset error \n");

	pdata->gpio_int = of_get_named_gpio_flags(node, "atmel,irq-gpio", 0, NULL);
	if (pdata->gpio_int)
		TOUCH_INFO_MSG("DT : gpio_int = %lu\n", pdata->gpio_int);
	else
		TOUCH_INFO_MSG("DT get gpio_int error \n");

	rc = of_property_read_u32(node, "atmel,numtouch", &temp_val);
	if (rc) {
		TOUCH_INFO_MSG("DT : Unable to read numtouch\n");
	} else {
		pdata->numtouch = temp_val;
		TOUCH_INFO_MSG("DT : numtouch = %d\n", pdata->numtouch);
	}

	rc = of_property_read_u32(node, "atmel,auto_fw_update", &temp_val);
	if (rc) {
		TOUCH_INFO_MSG("DT : Unable to read auto_fw_update\n");
	} else {
		pdata->auto_fw_update = temp_val;
		TOUCH_INFO_MSG("DT : auto_fw_update = %d\n", pdata->auto_fw_update);
	}

	rc = of_property_read_string(node, "atmel,fw_name",  &pdata->fw_name[0]);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG("DT : atmel,fw_name error \n");
		pdata->fw_name[0] = NULL;
	} else {
		TOUCH_INFO_MSG("DT : fw_name : %s \n", pdata->fw_name[0]);
	}

	rc = of_property_read_string(node, "atmel,fw2_name",  &pdata->fw_name[1]);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG("DT : atmel,fw2_name error \n");
		pdata->fw_name[1] = NULL;
	} else {
		TOUCH_INFO_MSG("DT : fw2_name : %s \n", pdata->fw_name[1]);
	}

	rc = of_property_read_u32(node, "atmel,panel_check", &temp_val);
	if (rc) {
		pdata->panel_check = 0;
	} else {
		pdata->panel_check = temp_val;
		TOUCH_INFO_MSG("DT : panel_check = %d\n", pdata->panel_check);
	}

	rc = of_property_read_u32(node, "atmel,maker_id", &temp_val);
	if (rc) {
		pdata->maker_id = 0;
	} else {
		pdata->maker_id = temp_val;
		TOUCH_INFO_MSG("DT : maker_id = %d\n", pdata->maker_id);
	}

	rc = of_property_read_u32(node, "atmel,maker_id_gpio", &temp_val);
	if (rc) {
		pdata->maker_id_gpio = 0;
	} else {
		pdata->maker_id_gpio = temp_val;
		TOUCH_INFO_MSG("DT : maker_id_gpio = %d\n", pdata->maker_id_gpio);
	}

	rc = of_property_read_u32(node, "atmel,maker_id2_gpio", &temp_val);
	if (rc) {
		pdata->maker_id2_gpio = 0;
	} else {
		pdata->maker_id2_gpio = temp_val;
		TOUCH_INFO_MSG("DT : maker_id2_gpio = %d\n", pdata->maker_id2_gpio);
	}

	rc = of_property_read_u32(node, "atmel,ref_reg_weight_val", &temp_val);
	if (rc) {
		pdata->ref_reg_weight_val = 16;
	} else {
		pdata->ref_reg_weight_val = temp_val;
		TOUCH_INFO_MSG("DT : ref_reg_weight_val = %d\n", pdata->ref_reg_weight_val);
	}

	if (pdata->panel_check) {
		rc = of_property_read_string(node, "atmel,extra_fw_name",  &pdata->extra_fw_name);
		if (rc && (rc != -EINVAL)) {
			TOUCH_INFO_MSG("DT : atmel,extra_fw_name error \n");
			pdata->extra_fw_name = NULL;
		}
		else
			TOUCH_INFO_MSG("DT : extra_fw_name : %s \n", pdata->extra_fw_name);
	} else {
		pdata->extra_fw_name = NULL;
	}

	rc = of_property_read_u32(node, "atmel,knock_on_type",  &temp_val);
	if (rc) {
		TOUCH_INFO_MSG("DT : Unable to read knock_on_type - set as 0\n" );
		pdata->knock_on_type = 0;
	} else {
		pdata->knock_on_type = temp_val;
	}
	TOUCH_INFO_MSG("DT : knock_on_type = %d \n",pdata->knock_on_type);

	rc = of_property_read_u32(node, "atmel,global_access_pixel",  &temp_val);
	if (rc) {
		TOUCH_INFO_MSG("DT : Unable to read global_access_pixel - set as 0\n" );
		pdata->global_access_pixel = 0;
	} else {
		pdata->global_access_pixel = temp_val;
	}
	TOUCH_INFO_MSG("DT : global_access_pixel = %d \n",pdata->global_access_pixel);

	rc = of_property_read_u32(node, "atmel,use_mfts",  &temp_val);
	if (rc) {
		TOUCH_INFO_MSG("DT : Unable to read use_mfts - set as false \n" );
		pdata->use_mfts = 0;
	} else {
		pdata->use_mfts = temp_val;
	}
	TOUCH_INFO_MSG("DT : use_mfts = %d \n",pdata->use_mfts);

	rc = of_property_read_u32(node, "atmel,lcd_x", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read lcd_x\n");
		pdata->lcd_x = 540;
	} else {
		pdata->lcd_x = temp_val;
	}
	TOUCH_INFO_MSG("DT : lcd_x: %d",pdata->lcd_x);

	rc = of_property_read_u32(node, "atmel,lcd_y", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read lcd_y\n");
		pdata->lcd_y = 960;
	} else {
		pdata->lcd_y = temp_val;
	}
	TOUCH_INFO_MSG("DT : lcd_y: %d",pdata->lcd_y);

	rc = of_property_read_u32(node, "atmel,diff_scaling", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read diff_scaling\n");
		pdata->diff_scaling = MXT_DEFAULT_SCALING_FACTOR;
	} else {
		pdata->diff_scaling = temp_val;
	}
	TOUCH_INFO_MSG("DT : diff_scaling: %d",pdata->diff_scaling);

	rc = of_property_read_u32(node, "atmel,time_reset_threshold", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read time_reset_threshold\n");
		pdata->time_reset_threshold = 5;
	} else {
		pdata->time_reset_threshold = temp_val;
	}
	TOUCH_INFO_MSG("DT : time_reset_threshold: %d",pdata->time_reset_threshold);

	rc = of_property_read_u32(node, "atmel,time_reset_error_node_chk", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read time_reset_error_node_chk\n");
		pdata->time_reset_error_node_chk = 5;
	} else {
		pdata->time_reset_error_node_chk = temp_val;
	}
	TOUCH_INFO_MSG("DT : time_reset_error_node_chk: %d",pdata->time_reset_error_node_chk);

	rc = of_property_read_u32(node, "atmel,ghost_detection_enable", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read ghost_detection_enable\n");
		pdata->ghost_detection_enable = 0;
	} else {
		pdata->ghost_detection_enable = temp_val;
	}
	TOUCH_INFO_MSG("DT : ghost_detection_enable: %d",pdata->ghost_detection_enable);

	if (pdata->ghost_detection_enable) {
		prop = of_find_property(node, "atmel,ghost_detection_value", NULL);
		if (prop) {
			temp_val = prop->length / sizeof(temp_val);
			if (temp_val <= GHOST_DETECTION_VALUE_MAX) {
				rc = of_property_read_u32_array(node, "atmel,ghost_detection_value", temp_array, temp_val);
				if (rc) {
					TOUCH_INFO_MSG("DT : Unable to read ghost_detection_enable\n");
					pdata->ghost_detection_enable = 0;
					TOUCH_INFO_MSG("DT : ghost_detection_enable: %d",pdata->ghost_detection_enable);
				}
				for (i = 0; i < temp_val; i++) {
					pdata->ghost_detection_value[i] = temp_array[i];
					TOUCH_INFO_MSG("DT : Ghost[%d] = %d \n", i, temp_array[i]);
				}
			}
		}
	}
	if (device_is_d415) {
		prop = of_find_property(node, "atmel,t15_key_array_d415", NULL);
	} else {
		prop = of_find_property(node, "atmel,t15_key_array", NULL);
	}
	if (prop) {
		temp_val = prop->length / sizeof(temp_val);
		if (temp_val <= MXT_MAX_KEY*2) {
			if (device_is_d415) {
				rc = of_property_read_u32_array(node, "atmel,t15_key_array_d415", temp_array, temp_val);
			} else {
				rc = of_property_read_u32_array(node, "atmel,t15_key_array", temp_array, temp_val);
			}
			if (rc) {
				TOUCH_INFO_MSG("DT : Unable to read t15_key_array\n");
			}
				for (i = 0; i < temp_val; i+=2) {
				pdata->t15_key_array_x[(i / 2)] = temp_array[i];
				pdata->t15_key_array_y[(i / 2)] = temp_array[i + 1];
				TOUCH_INFO_MSG("DT : t15_keystate_array[%d] = [%d,%d] \n", (i / 2), pdata->t15_key_array_x[(i / 2)], pdata->t15_key_array_y[(i / 2)]);
			}
		}
	}

	if (!device_is_d415) {
		prop = of_find_property(node, "atmel,t15_key_state", NULL);
		if (prop) {
			temp_val = prop->length / sizeof(temp_val);
			pdata->t15_num_keys = temp_val;
			TOUCH_INFO_MSG("DT : num_buttons = %d\n", pdata->t15_num_keys);

			if (temp_val <= MXT_MAX_KEY) {
				rc = of_property_read_u32_array(node, "atmel,t15_key_state", temp_array, temp_val);
				if (rc) {
					TOUCH_INFO_MSG("DT : Unable to read t15_key_state\n");
				}

				for(i=0; i<temp_val; i++) {
					pdata->t15_keystate[i] = temp_array[i];
					TOUCH_INFO_MSG("DT : t15_keystate[%d] = [%3d] \n", i, pdata->t15_keystate[i]);
				}
			}
		}

		if (pdata->panel_check) {
			prop = of_find_property(node, "atmel,t15_extra_key_state", NULL);
			if (prop) {
				temp_val = prop->length / sizeof(temp_val);
				if (temp_val <= MXT_MAX_KEY) {
					rc = of_property_read_u32_array(node, "atmel,t15_extra_key_state", temp_array, temp_val);
					if (rc) {
						TOUCH_INFO_MSG("DT : Unable to read t15_extra_key_state\n");
						pdata->t15_extra_keystate[0] = 0;
					} else {
						for(i=0; i<temp_val; i++) {
							pdata->t15_extra_keystate[i] = temp_array[i];
							TOUCH_INFO_MSG("DT : t15_extra_keystate[%d] = [%3d] \n", i, pdata->t15_extra_keystate[i]);
						}
					}
				}
			} else {
				pdata->t15_extra_keystate[0] = 0;
			}
		}

		prop = of_find_property(node, "atmel,t15_extra_key_array", NULL);
		if (prop) {
			temp_val = prop->length / sizeof(temp_val);
			if (temp_val <= MXT_MAX_KEY*2) {
				rc = of_property_read_u32_array(node, "atmel,t15_extra_key_array", temp_array, temp_val);
				if (rc) {
					TOUCH_INFO_MSG("DT : Unable to read t15_extra_key_array\n");
				}

				for (i = 0; i < temp_val; i+=2) {
					pdata->t15_extra_key_array_x[(i / 2)] = temp_array[i];
					pdata->t15_extra_key_array_y[(i / 2)] = temp_array[i + 1];
					TOUCH_INFO_MSG("DT : t15_extra_key_array[%d] = [%d,%d] \n", (i / 2), pdata->t15_extra_key_array_x[(i / 2)], pdata->t15_extra_key_array_y[(i / 2)]);
				}
			}
		}

		prop = of_find_property(node, "atmel,t15_key_map", NULL);
		if (prop) {
			temp_val = prop->length / sizeof(temp_val);
			if (pdata->t15_num_keys != temp_val) {
				TOUCH_INFO_MSG("DT : error t15_key_map == %d\n", temp_val);
			}

			if (temp_val <= MXT_MAX_KEY) {
				rc = of_property_read_u32_array(node, "atmel,t15_key_map", temp_array, temp_val);
				if (rc) {
					TOUCH_INFO_MSG("DT : Unable to read key codes\n");
					return rc;
				}

				for(i=0; i<temp_val; i++) {
					pdata->t15_keymap[i] = temp_array[i];
					TOUCH_INFO_MSG("DT : button[%d] = [%3d:%s] \n", i, pdata->t15_keystate[i], get_touch_button_string(pdata->t15_keymap[i]));
				}
			}
		}
	}

	rc = of_property_read_u32(node, "atmel,butt_check_enable", &temp_val);
	if (rc && (rc != -EINVAL)) {
		TOUCH_INFO_MSG( "DT : Unable to read butt_check_enable\n");
		pdata->butt_check_enable = 0;
	} else {
		pdata->butt_check_enable = temp_val;
	}
	TOUCH_INFO_MSG("DT : butt_check_enable: %d",pdata->butt_check_enable);

	prop = of_find_property(node, "atmel,error_check_count", NULL);
	if (prop) {
		temp_val = prop->length / sizeof(temp_val);
		if (temp_val <= GHOST_DETECTION_VALUE_MAX) {
			rc = of_property_read_u32_array(node, "atmel,error_check_count", temp_array, temp_val);
			if (rc) {
				TOUCH_INFO_MSG("DT : Unable to read error_check_count\n");
			}
			for (i = 0; i < temp_val; i++) {
				pdata->error_check_count[i] = (u8)temp_array[i];
				TOUCH_INFO_MSG("DT : err_cnt[%d] = %d \n", i, pdata->error_check_count[i]);
			}
		}
	}

	return 0;

}

int mxt_initialize_t9_input_device(struct mxt_data *data)
{
	struct input_dev *input_dev = NULL;
	int error = 0;
	unsigned int num_mt_slots = 0;
	int i = 0;

	if (data->input_dev && !is_probing) {
		TOUCH_INFO_MSG("ignore %s \n", __func__);
		return 0;
	}

	error = mxt_read_t9_resolution(data);
	if (error)
		TOUCH_INFO_MSG("Failed to initialize T9 resolution\n");

	input_dev = input_allocate_device();
	if (!input_dev) {
		TOUCH_INFO_MSG("Failed to allocate memory\n");
		return -ENOMEM;
	}

	input_dev->name = MXT_DEVICE_NAME;
	input_dev->phys = data->phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->open = mxt_input_open;
	input_dev->close = mxt_input_close;

	__set_bit(EV_ABS, input_dev->evbit);
	input_set_capability(input_dev, EV_KEY, BTN_TOUCH);

	num_mt_slots = data->num_touchids;
	error = input_mt_init_slots(input_dev, num_mt_slots);
	if (error) {
		TOUCH_INFO_MSG("Error %d initialising slots\n", error);
		goto err_free_mem;
	}

	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, MXT_MAX_AREA, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, data->max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, data->max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_ORIENTATION, 0, 255, 0, 0);

	/* For T15 key array */
	if (data->T15_reportid_min) {
		data->t15_keystatus = 0;

		input_dev->evbit[0] = BIT_MASK(EV_ABS) | BIT_MASK(EV_KEY);

		for (i = 0; i < data->pdata->t15_num_keys; i++)
			input_set_capability(input_dev, EV_KEY, data->pdata->t15_keymap[i]);
			input_dev->keybit[BIT_WORD(data->pdata->t15_keymap[i])] |= BIT_MASK(data->pdata->t15_keymap[i]);
	}

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(KEY_DOUBLE_TAP, input_dev->keybit);

	input_set_drvdata(input_dev, data);

	error = input_register_device(input_dev);
	if (error) {
		TOUCH_INFO_MSG("Error %d registering input device\n", error);
		goto err_free_mem;
	} else {
		TOUCH_INFO_MSG("input_register_device done\n");
	}

	data->input_dev = input_dev;

	return 0;

err_free_mem:
	input_free_device(input_dev);
	return error;
}

static int mxt_command_reset(struct mxt_data *data, u8 value)
{
	int error = 0;

	error = mxt_write_object(data, MXT_GEN_COMMAND_T6, MXT_COMMAND_RESET, value);
	msleep(MXT_RESET_TIME);

	if (error)
		TOUCH_INFO_MSG("Not respond after reset command[%d]\n", value);

	return error;
}

static int mxt_command_backup(struct mxt_data *data, u8 value)
{
	mxt_write_object(data, MXT_GEN_COMMAND_T6,MXT_COMMAND_BACKUPNV, value);
	msleep(MXT_BACKUP_TIME);

	return 0;
}

int mxt_read_mem(struct mxt_data *data, u16 reg, u8 len, void *buf)
{
	int ret = 0;

	ret = __mxt_read_reg(data->client, reg, len, buf);

	return ret;
}

int mxt_write_mem(struct mxt_data *data, u16 reg, u8 len, const u8 *buf)
{
	int ret = 0;

	ret = __mxt_write_reg(data->client, reg, len, buf);

	return ret;
}

int mxt_verify_fw(struct mxt_fw_info *fw_info, const struct firmware *fw)
{
	struct mxt_data *data = fw_info->data;
	struct mxt_fw_image *fw_img = NULL;
	char *extra_info = NULL;
	struct patch_header* ppheader = NULL;
	u8* patch = NULL;
	u32 ppos = 0;

	if (!fw) {
		TOUCH_INFO_MSG("could not find firmware file\n");
		return -ENOENT;
	}

	fw_img = (struct mxt_fw_image *)fw->data;

	if (le32_to_cpu(fw_img->magic_code) != MXT_FW_MAGIC) {
		/* In case, firmware file only consist of firmware */
		TOUCH_INFO_MSG("Firmware file only consist of raw firmware\n");
		fw_info->fw_len = fw->size;
		fw_info->fw_raw_data = fw->data;
	} else {
		/*
		 * In case, firmware file consist of header,
		 * configuration, firmware.
		 */
		TOUCH_INFO_MSG("Firmware file consist of header, configuration, firmware\n");
		fw_info->bin_ver = fw_img->bin_ver;
		fw_info->build_ver = fw_img->build_ver;
		fw_info->hdr_len = le32_to_cpu(fw_img->hdr_len);
		fw_info->cfg_len = le32_to_cpu(fw_img->cfg_len);
		fw_info->fw_len = le32_to_cpu(fw_img->fw_len);
		fw_info->cfg_crc = le32_to_cpu(fw_img->cfg_crc);

		extra_info = fw_img->extra_info;
		fw_info->data->pdata->fw_ver[0] = extra_info[0];
		fw_info->data->pdata->fw_ver[1] = extra_info[1];
		memcpy(fw_info->data->pdata->product, &extra_info[4], 10);

		/* Check the firmware file with header */
		if (fw_info->hdr_len != sizeof(struct mxt_fw_image)
			|| fw_info->hdr_len + fw_info->cfg_len + fw_info->fw_len != fw->size) {

			ppos = fw_info->hdr_len + fw_info->cfg_len + fw_info->fw_len;
			ppheader = (struct patch_header*)(fw->data + ppos);
			if (ppheader->magic == MXT_PATCH_MAGIC) {
				TOUCH_INFO_MSG("Firmware file has patch size: %d\n", ppheader->size);
				if (ppheader->size) {
					patch = NULL;
					if (data->patch.patch) {
						kfree(data->patch.patch);
						data->patch.patch = NULL;
					}
					patch = kzalloc(ppheader->size, GFP_KERNEL);
					if (patch) {
						memcpy(patch, (u8*)ppheader, ppheader->size);
						data->patch.patch = patch;
						TOUCH_INFO_MSG("%s Patch Updated \n", __func__);
					} else {
						data->patch.patch = NULL;
						TOUCH_INFO_MSG("%s Patch Update Failed \n", __func__);
					}
				}
			} else {
				TOUCH_INFO_MSG("Firmware file is invaild !!hdr size[%d] cfg,fw size[%d,%d] filesize[%d]\n",
					fw_info->hdr_len, fw_info->cfg_len, fw_info->fw_len, fw->size);
				return -EINVAL;
			}
		}

		if (!fw_info->cfg_len) {
			TOUCH_INFO_MSG("Firmware file dose not include configuration data\n");
			return -EINVAL;
		}

		if (!fw_info->fw_len) {
			TOUCH_INFO_MSG("Firmware file dose not include raw firmware data\n");
			return -EINVAL;
		}

		/* Get the address of configuration data */
		fw_info->cfg_raw_data = fw_img->data;

		/* Get the address of firmware data */
		fw_info->fw_raw_data = fw_img->data + fw_info->cfg_len;
	}

	return 0;
}

static int mxt_read_id_info(struct mxt_data *data)
{
	int ret = 0;
	u8 id[MXT_INFOMATION_BLOCK_SIZE] = {0};

	/* Read IC information */
	ret = mxt_read_mem(data, 0, MXT_INFOMATION_BLOCK_SIZE, id);
	if (ret) {
		TOUCH_INFO_MSG("Read fail. IC information\n");
		goto out;
	} else {
		TOUCH_INFO_MSG("Family:%02X Variant:%02X Binary:%u.%u.%02X TX:%d RX:%d Objects:%d\n",
			id[0], id[1], id[2] >> 4, id[2] & 0xF, id[3], id[4], id[5], id[6]);
		if (data->info) {
			kfree(data->info);
		}
		data->info = kzalloc(sizeof(struct mxt_info), GFP_KERNEL);
		data->info->family_id = id[0];
		data->info->variant_id = id[1];
		data->info->version = id[2];
		data->info->build = id[3];
		data->info->matrix_xsize = id[4];
		data->info->matrix_ysize = id[5];
		data->info->object_num = id[6];
	}

out:
	return ret;
}

static int mxt_get_object_table(struct mxt_data *data)
{
	int error = 0;
	int i = 0;
	u16 reg = 0;
	u8 reportid = 0;
	u8 buf[MXT_OBJECT_TABLE_ELEMENT_SIZE] = {0};
	struct mxt_object *object = NULL;

	//TOUCH_INFO_MSG("%s data=[0x%X] \n", __func__, (int)data);
	//TOUCH_INFO_MSG("%s data->object_table=[0x%X] \n", __func__, (int)data->object_table);

	for (i = 0; i < data->info->object_num; i++) {
		object = data->object_table + i;

		reg = MXT_OBJECT_TABLE_START_ADDRESS + (MXT_OBJECT_TABLE_ELEMENT_SIZE * i);
		error = mxt_read_mem(data, reg, MXT_OBJECT_TABLE_ELEMENT_SIZE, buf);
		if (error) {
			TOUCH_INFO_MSG("%s mxt_read_mem error \n", __func__);
			return error;
		}

		object->type = buf[0];
		object->start_address = (buf[2] << 8) | buf[1];
		/* the real size of object is buf[3]+1 */
		object->size_minus_one = buf[3];
		/* the real instances of object is buf[4]+1 */
		object->instances_minus_one= buf[4];
		object->num_report_ids = buf[5];

#if 0
		TOUCH_INFO_MSG(
			"\t Object:T%02d Address:%03d Size:%03d Instance:%d Report Id's:%d\n",
			object->type, object->start_address,
			object->size_minus_one, object->instances_minus_one,
			object->num_report_ids);
#endif

		if (object->num_report_ids) {
			reportid += object->num_report_ids * (object->instances_minus_one+1);
			data->max_reportid = reportid;
		}
	}

	/* Store maximum reportid */
	data->max_reportid = reportid;

	return 0;
}

static int mxt_enter_bootloader(struct mxt_data *data)
{
	int error = 0;

	if (data->object_table) {
		memset(data->object_table, 0x0, (MXT_OBJECT_NUM_MAX * sizeof(struct mxt_object)));
	}

	/* Get object table information*/
	error = mxt_get_object_table(data);
	if (error)
		goto err_free_mem;

	/* Change to the bootloader mode */
	error = mxt_command_reset(data, MXT_BOOT_VALUE);
	if (error)
		goto err_free_mem;

err_free_mem:
	return error;
}

static int mxt_flash_fw(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	const u8 *fw_data = fw_info->fw_raw_data;
	size_t fw_size = fw_info->fw_len;
	unsigned int frame_size = 0;
	unsigned int frame = 0;
	unsigned int pos = 0;
	int ret = 0;

	if (!fw_data) {
		TOUCH_INFO_MSG("%s firmware data is Null\n", __func__);
		return -ENOMEM;
	}

	ret = mxt_lookup_bootloader_address(data, 0);
	if (ret) {
		TOUCH_INFO_MSG("Failed to lookup bootloader address\n");
		return ret;
	}

	INIT_COMPLETION(data->bl_completion);

	ret = mxt_check_bootloader(data, MXT_WAITING_BOOTLOAD_CMD);
	if (ret) {
		/*may still be unlocked from previous update attempt */
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret) {
			TOUCH_INFO_MSG("Failed to check bootloader\n");
			goto out;
		}
	} else {
		TOUCH_INFO_MSG("Unlocking bootloader\n");
		/* Unlock bootloader */
		//mxt_unlock_bootloader(client);
		mxt_send_bootloader_cmd(data, true);
	}
	while (pos < fw_size) {
		ret = mxt_check_bootloader(data, MXT_WAITING_FRAME_DATA);
		if (ret) {
			TOUCH_INFO_MSG("Fail updating firmware. wating_frame_data err\n");
			goto out;
		}

		frame_size = ((*(fw_data + pos) << 8) | *(fw_data + pos + 1));

		/*
		* We should add 2 at frame size as the the firmware data is not
		* included the CRC bytes.
		*/

		frame_size += 2;

		/* Write one frame to device */
		//mxt_fw_write(client, fw_data + pos, frame_size);
		mxt_bootloader_write(data, fw_data + pos, frame_size);

		ret = mxt_check_bootloader(data, MXT_FRAME_CRC_PASS);
		if (ret) {
			TOUCH_INFO_MSG("Fail updating firmware. frame_crc err\n");
			goto out;
		}

		pos += frame_size;
		frame++;

		if (frame % 50 == 0)
			TOUCH_INFO_MSG("\t Updated %5d / %5d bytes\n", pos, fw_size);

		msleep(20);
	}

	msleep(MXT_FW_RESET_TIME);

out:
	return ret;
}

static int mxt_flash_fw_on_probe(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	int error = 0;
	//struct mxt_object *object = NULL;
	//int ret = 0;

	error = mxt_read_id_info(data);

	if (error) {
		/* need to check IC is in boot mode */
		error = mxt_probe_bootloader(data, 0);
		if (error) {
			TOUCH_INFO_MSG("Failed to verify bootloader's status\n");
			goto out;
		}

		TOUCH_INFO_MSG("Updating firmware from boot-mode\n");
		goto load_fw;
	}

	/* compare the version to verify necessity of firmware updating */
	TOUCH_INFO_MSG("Binary Version [IC:%u.%u.%02X] [FW:%u.%u.%02X]\n",
		data->info->version >> 4, data->info->version & 0xF, data->info->build, fw_info->bin_ver >> 4,
		fw_info->bin_ver & 0xF, fw_info->build_ver);

	if (data->info->version == fw_info->bin_ver && data->info->build == fw_info->build_ver) {
		TOUCH_INFO_MSG("Binary Version is same \n");
		goto out;
	}

// get T71 diff value before firmware upload
/*
	object = mxt_get_object(data, MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71);

	if(object != NULL)
	{
		ret = mxt_read_mem(data, object->start_address + 107, 1, &data->t71_diff_using);
		TOUCH_INFO_MSG("data->t71_diff_using %d \n", data->t71_diff_using);
		if (ret) {
			TOUCH_INFO_MSG("t71 read fail before firmware change\n");
			goto out;
		}

		ret = mxt_read_mem(data, object->start_address + 60, 14, data->t71_diff_val);
		TOUCH_INFO_MSG("data->t71_diff_val %d %d %d %d %d %d \n", data->t71_diff_val[0],data->t71_diff_val[1],data->t71_diff_val[2],data->t71_diff_val[3],data->t71_diff_val[4],data->t71_diff_val[5]);
		if (ret) {
			TOUCH_INFO_MSG("Reference Diff value Read fail before firmware change\n");
			goto out;
		}
	}
*/


	error = mxt_enter_bootloader(data);
	if (error) {
		TOUCH_INFO_MSG("Failed enter bootloader mode\n");
		goto out;
	}

load_fw:
	error = mxt_flash_fw(fw_info);
	if (error)
		TOUCH_INFO_MSG("Failed updating firmware\n");
	else
		TOUCH_INFO_MSG("succeeded updating firmware\n");
out:
	return error;
}

static void mxt_make_reportid_table(struct mxt_data *data)
{
	struct mxt_object *objects = data->object_table;
	struct mxt_reportid *reportids = data->reportids;
	int i = 0, j = 0;
	int id = 0;

	for (i = 0; i < data->info->object_num; i++) {
		for (j = 0; j < objects[i].num_report_ids * (objects[i].instances_minus_one+1); j++) {
			id++;

			reportids[id].type = objects[i].type;
			reportids[id].index = j;

#if 0
			TOUCH_INFO_MSG("\t Report_id[%d]:\tT%d\tIndex[%d]\n",
				id, reportids[id].type, reportids[id].index);
#endif
		}
	}
}

static int mxt_read_info_crc(struct mxt_data *data, u32 *crc_pointer)
{
	u16 crc_address = 0;
	u8 msg[3] = {0};
	int ret = 0;

	/* Read Info block CRC address */
	crc_address = MXT_OBJECT_TABLE_START_ADDRESS +
			data->info->object_num * MXT_OBJECT_TABLE_ELEMENT_SIZE;

	ret = mxt_read_mem(data, crc_address, 3, msg);
	if (ret)
		return ret;

	*crc_pointer = msg[0] | (msg[1] << 8) | (msg[2] << 16);

	return 0;
}

static int mxt_table_initialize(struct mxt_data *data)
{
	u32 read_info_crc = 0;
	int ret = 0;

	ret = mxt_read_id_info(data);
	if (ret)
		return ret;

	if (data->object_table)
		memset(data->object_table, 0x0, (MXT_OBJECT_NUM_MAX * sizeof(struct mxt_object)));

	/* Get object table infomation */
	ret = mxt_get_object_table(data);
	if (ret)
		goto out;

	if (data->reportids) {
		kfree(data->reportids);
	}

	data->reportids = kzalloc(((data->max_reportid + 1) * sizeof(struct mxt_reportid)), GFP_KERNEL);
	if (!data->reportids) {
		TOUCH_INFO_MSG("%s Failed to allocate memory 2\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	/* Make report id table */
	mxt_make_reportid_table(data);

	/* Verify the info CRC */
	ret = mxt_read_info_crc(data, &read_info_crc);
	if (ret)
		goto out;

	#if 0
	ret = mxt_calculate_infoblock_crc(data, &calc_info_crc);
	if (ret)
		goto out;

	if (read_info_crc != calc_info_crc) {
		TOUCH_INFO_MSG("Infomation CRC error :[CRC 0x%06X!=0x%06X]\n",
				read_info_crc, calc_info_crc);
		ret = -EFAULT;
		goto out;
	}
	#endif
	return 0;

out:
	return ret;
}

static int mxt_read_message(struct mxt_data *data, struct mxt_message *message)
{
	struct mxt_object *object = NULL;

	object = mxt_get_object(data, MXT_GEN_MESSAGE_T5);
	if (!object) {
		TOUCH_INFO_MSG("mxt_read_message-mxt_get_object error\n");
		return -EINVAL;
	}

	return mxt_read_mem(data, object->start_address, sizeof(struct mxt_message), message);
}

static int mxt_read_message_reportid(struct mxt_data *data, struct mxt_message *message, u8 reportid)
{
	int try = 0;
	int error = 0;
	int fail_count = 0;

	fail_count = data->max_reportid * 2;

	while (++try < fail_count) {
		error = mxt_read_message(data, message);
		if (error) {
			TOUCH_INFO_MSG("mxt_read_message error\n");
			print_hex_dump(KERN_DEBUG, "[Touch] CRC : ", DUMP_PREFIX_NONE, 16, 1,
				   message, sizeof(struct mxt_message), false);
			return error;
		}

		if (message->reportid == 0xff)
			continue;

		if (message->reportid == reportid)
			return 0;
	}

	return -EINVAL;
}

static int mxt_read_config_crc(struct mxt_data *data, u32 *crc)
{
	struct mxt_message message = {0};
	struct mxt_object *object = NULL;
	int error = 0;

	object = mxt_get_object(data, MXT_GEN_COMMAND_T6);
	if (!object)
		return -EIO;

	/* Try to read the config checksum of the existing cfg */
	mxt_write_object(data, MXT_GEN_COMMAND_T6, MXT_COMMAND_REPORTALL, 1);

	/* Read message from command processor, which only has one report ID */
	error = mxt_read_message_reportid(data, &message, 1);//data->max_reportid);
	if (error) {
		TOUCH_INFO_MSG("Failed to retrieve CRC\n");
		return error;
	}

	/* Bytes 1-3 are the checksum. */
	*crc = message.message[1] | (message.message[2] << 8) | (message.message[3] << 16);

	return 0;
}

static int mxt_write_config(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	struct mxt_object *object = NULL;
	struct mxt_cfg_data *cfg_data = NULL;
	u32 current_crc = 0;
	u32 t71_cfg_crc = 0;
	u8 buf_crc_t71[5] = {0};
	u8 i = 0, val = 0;
	u16 reg = 0, index = 0;
	int ret = 0;
	u8 prev_scaling_chk = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	if (!fw_info->cfg_raw_data) {
		TOUCH_INFO_MSG("No cfg data in file\n");
		return ret;
	}

	/* Get config CRC from device */
	ret = mxt_read_config_crc(data, &current_crc);
	if (ret) {
		TOUCH_INFO_MSG("fail to read config crc\n");
		return ret;
	}

	/* Check Version information */
	if (fw_info->bin_ver != data->info->version) {
		TOUCH_INFO_MSG("Warning: version mismatch! %s\n", __func__);
		return 0;
	}
	if (fw_info->build_ver != data->info->build) {
		TOUCH_INFO_MSG("Warning: build num mismatch! %s\n", __func__);
		return 0;
	}

	object = mxt_get_object(data, MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71);

	ret = mxt_read_mem(data, object->start_address + 107, 5, buf_crc_t71);
	if (ret) {
		TOUCH_INFO_MSG("T71 CRC read fail \n");
	}

	/* Read scaling factor */
	ret = mxt_read_mem(data, object->start_address + 106, 1, &prev_scaling_chk);
	if (ret) {
		TOUCH_INFO_MSG("T71 CRC read fail \n");
	} else {
		TOUCH_INFO_MSG("T71 Scaling Factor [%d] \n", prev_scaling_chk);
	}

	t71_cfg_crc = buf_crc_t71[3] << 16 | buf_crc_t71[2] << 8 | buf_crc_t71[1];
	TOUCH_INFO_MSG("T71 CRC[%06X] FW CRC[%06X]\n", t71_cfg_crc, fw_info->cfg_crc);

	/* Check config CRC */
	if (current_crc == fw_info->cfg_crc || t71_cfg_crc == fw_info->cfg_crc) {
		TOUCH_INFO_MSG("Same Config[%06X] Skip Writing\n", current_crc);
		return 0;
	}

	/* Restore memory and stop event handing */
	ret = mxt_command_backup(data, MXT_DISALEEVT_VALUE);
	if (ret) {
		TOUCH_INFO_MSG("Failed Restore NV and stop event\n");
		return -EINVAL;
	}

	TOUCH_INFO_MSG("Writing Config:[IC:%06X] [FW:%06X]\n", fw_info->cfg_crc, current_crc);

	/* Saved diff value */
	if (buf_crc_t71[0] == 1) {
		TOUCH_INFO_MSG("buf_crc_t71[0] == 1\n");
		ret = mxt_read_mem(data, object->start_address + 60, 14 + data->pdata->t15_num_keys, &data->t71_diff_val);
		if (ret) {
			TOUCH_INFO_MSG("Reference Diff value Read fail\n");
			return ret;
		}
	}

	/* Write config info */
	for (index = 0; index < fw_info->cfg_len;) {
		if (index + sizeof(struct mxt_cfg_data) >= fw_info->cfg_len) {
			TOUCH_INFO_MSG("index(%d) of cfg_data exceeded total size(%d)!!\n",
				index + sizeof(struct mxt_cfg_data), fw_info->cfg_len);
			return -EINVAL;
		}

		/* Get the info about each object */
		cfg_data = (struct mxt_cfg_data *)(&fw_info->cfg_raw_data[index]);

		index += sizeof(struct mxt_cfg_data) + cfg_data->size;
		if (index > fw_info->cfg_len) {
			TOUCH_INFO_MSG("index(%d) of cfg_data exceeded total size(%d) in T%d object!!\n",
				index, fw_info->cfg_len, cfg_data->type);
			return -EINVAL;
		}

		object = mxt_get_object(data, cfg_data->type);
		if (!object) {
			TOUCH_INFO_MSG("T%d is Invalid object type\n", cfg_data->type);
			return -EINVAL;
		}

		/* Check and compare the size, instance of each object */
		if (cfg_data->size > (object->size_minus_one+1)) {
			TOUCH_INFO_MSG("T%d Object length exceeded!\n", cfg_data->type);
			return -EINVAL;
		}
		if (cfg_data->instance >= (object->instances_minus_one+1)) {
			TOUCH_INFO_MSG("T%d Object instances exceeded!\n", cfg_data->type);
			return -EINVAL;
		}

		TOUCH_INFO_MSG("\t Writing config for T%02d len %3d instance %d (%3d/%3d)\n",
			cfg_data->type, object->size_minus_one, cfg_data->instance, index, fw_info->cfg_len);

		reg = object->start_address + (object->size_minus_one+1) * cfg_data->instance;

		/* Write register values of each object */
		ret = mxt_write_mem(data, reg, cfg_data->size, cfg_data->register_val);
		if (ret) {
			TOUCH_INFO_MSG("Write T%d Object failed\n", object->type);
			return ret;
		}

		/*
		 * If firmware is upgraded, new bytes may be added to end of
		 * objects. It is generally forward compatible to zero these
		 * bytes - previous behaviour will be retained. However
		 * this does invalidate the CRC and will force a config
		 * download every time until the configuration is updated.
		 */
		if (cfg_data->size < (object->size_minus_one+1)) {
			TOUCH_INFO_MSG("Warning: zeroing %d byte(s) in T%d\n",
				 (object->size_minus_one+1) - cfg_data->size, cfg_data->type);

			for (i = cfg_data->size + 1; i < (object->size_minus_one+1); i++) {
				ret = mxt_write_mem(data, reg + i, 1, &val);
				if (ret)
					return ret;
			}
		}

		//TOUCH_INFO_MSG("Type[%2d] Size[%3d] Instance[%d] Len[%3d] \n", cfg_data->type, object->size_minus_one, cfg_data->instance, fw_info->cfg_len);
	}

	TOUCH_INFO_MSG("Configuration Updated \n");

	/* Restore diff value */
	if (buf_crc_t71[0] == 1) {
		TOUCH_INFO_MSG("Restore diff value \n");
		object = mxt_get_object(data, MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71);

		ret = mxt_write_mem(data, object->start_address + 60, 14 + data->pdata->t15_num_keys, (u8*)&data->t71_diff_val);
		if (ret) {
			TOUCH_INFO_MSG("Reference Diff Restore fail\n");
			return ret;
		}

		buf_crc_t71[0] = 1;
		buf_crc_t71[1] = (u8)(fw_info->cfg_crc & 0x000000FF);
		buf_crc_t71[2] = (u8)((fw_info->cfg_crc & 0x0000FF00) >> 8);
		buf_crc_t71[3] = (u8)((fw_info->cfg_crc & 0x00FF0000) >> 16);
		buf_crc_t71[4] = (u8)((fw_info->cfg_crc & 0xFF000000) >> 24);

		ret = mxt_write_mem(data, object->start_address + 106, 1, &prev_scaling_chk);
		if (ret) {
			TOUCH_INFO_MSG("Scaling Factor Restore fail\n");
			return ret;
		} else {
			TOUCH_INFO_MSG("Scaling Factor Restore [%d] \n", prev_scaling_chk);
		}

		ret = mxt_write_mem(data, object->start_address + 107, 5, buf_crc_t71);
		if (ret) {
			TOUCH_INFO_MSG("Reference Diff Restore fail\n");
			return ret;
		}

	}

	/* Backup to memory */
	ret = mxt_command_backup(data, MXT_BACKUP_VALUE);
	if (ret) {
		TOUCH_INFO_MSG("Failed backup NV data\n");
		return -EINVAL;
	}

	/* Soft reset */
	ret = mxt_command_reset(data, MXT_RESET_VALUE);
	if (ret) {
		TOUCH_INFO_MSG("Failed Reset IC\n");
		return -EINVAL;
	}

	return ret;
}

static int  mxt_config_initialize(struct mxt_fw_info *fw_info)
{
	struct mxt_data *data = fw_info->data;
	int ret = 0;

	TOUCH_INFO_MSG("%s \n", __func__);

	ret = mxt_write_config(fw_info);
	if (ret) {
		TOUCH_INFO_MSG("Failed to write config from file\n");
		goto out;
	}

	if (data->patch.patch) {
		ret = mxt_patch_init(data, data->patch.patch);
	}

	if (ret == 0) {
		global_mxt_data = data;
	} else {
		global_mxt_data = NULL;
		TOUCH_INFO_MSG("Failed to get global_mxt_data(NULL) \n");
	}

out:
	return ret;
}

void mxt_request_firmware_work(const struct firmware *fw, void *context)
{
	struct mxt_data *data = context;
	int error = 0;

	mxt_power_block(POWERLOCK_FW_UP);

	data->fw_info.data = data;

	error = mxt_verify_fw(&data->fw_info, fw);
	if (error)
		goto ts_rest_init;

	/* Skip update on boot up if firmware file does not have a header */
	if (!data->fw_info.hdr_len)
		goto ts_rest_init;

	error = mxt_flash_fw_on_probe(&data->fw_info);
	if (error)
		goto out;

ts_rest_init:
	error = mxt_table_initialize(data);
	if (error) {
		TOUCH_INFO_MSG("Failed to initialize\n");
		goto out;
	}

	//mxt_acquire_irq(data);
	error = mxt_config_initialize(&data->fw_info);
	if (error) {
		TOUCH_INFO_MSG("Failed to rest initialize\n");
		goto out;
	}

out:
	release_firmware(fw);
	mxt_power_unblock(POWERLOCK_FW_UP);
	return;
}

int mxt_update_firmware(struct mxt_data *data, const char *fwname)
{
	int error = 0;
	const struct firmware *fw = NULL;

	TOUCH_INFO_MSG("%s [%s]\n", __func__, fwname);

	if (fwname) {
		error = request_firmware(&fw, fwname, &data->client->dev);
		if (error) {
			TOUCH_INFO_MSG("%s error request_firmware \n", __func__);
			return 1;
		}
	}

	mxt_request_firmware_work(fw, data);

	error = mxt_read_info_block(data);
	if (error) {
		TOUCH_INFO_MSG("%s error mxt_read_info_block \n", __func__);
		return 1;
	}

	error = mxt_init_t7_power_cfg(data);
	if (error) {
		TOUCH_INFO_MSG("%s error mxt_init_t7_power_cfg \n", __func__);
		return 1;
	}

	return 0;

}

int mxt_scaling_factor_chk(struct mxt_data *data)
{
	u8 prev_scaling_chk = 0;
	struct mxt_object *object = NULL;
	u8 t71_crc_buf[4];
	u32 t71_cfg_crc = 0;
	int ret = 0;

	TOUCH_INFO_MSG("=====================================\n");

	ret = mxt_table_initialize(data);
	if (ret) {
		TOUCH_INFO_MSG("%s Failed to initialize\n", __func__);
		return 1;
	}

	object = mxt_get_object(data, MXT_SPT_DYNAMICCONFIGURATIONCONTAINER_T71);
	if (!object) {
		TOUCH_INFO_MSG("%s error mxt_get_object \n", __func__);
		return 1;
	}

	ret = mxt_read_mem(data, object->start_address + 108, 4, &t71_crc_buf);
	t71_cfg_crc = t71_crc_buf[2] << 16 | t71_crc_buf[1] << 8 | t71_crc_buf[0];
	TOUCH_INFO_MSG("T71 CRC [%06X] \n", t71_cfg_crc);
	if (ret) {
		TOUCH_INFO_MSG("T71 CRC read fail \n");
		return 1;
	}

	/*read scaling factor from IC*/
	ret = mxt_read_mem(data, object->start_address + 106, 1, &prev_scaling_chk);
	TOUCH_INFO_MSG("Read T71 Scaling Factor [%d] \n", prev_scaling_chk);
	if (ret) {
		TOUCH_INFO_MSG("T71 Scaling Factor read fail \n");
		return 1;
	}

	if (prev_scaling_chk != 0) {
		TOUCH_INFO_MSG("Update T71 Diff Scaling Factor [%d]\n", prev_scaling_chk);
		data->pdata->diff_scaling = prev_scaling_chk;
		if (data->pdata->diff_scaling == 8)
			data->pdata->error_check_count[0] = data->pdata->error_check_count[1];
	}

	if (t71_cfg_crc == MXT_FIRMWARE_V204_CRC) {
		TOUCH_INFO_MSG("T71 FW204 found \n");
		if (prev_scaling_chk == 0 && prev_scaling_chk != MXT_DEFAULT_SCALING_FACTOR) {
			prev_scaling_chk = MXT_FW204_SCALING_FACTOR;
			ret = mxt_write_mem(data, object->start_address + 106, 1, &prev_scaling_chk);
			if (ret) {
				TOUCH_INFO_MSG("T71 Scaling Factor write fail \n");
				return 1;
			}
			TOUCH_INFO_MSG("T71 Scaling Factor write [%d]\n", prev_scaling_chk);
			data->pdata->diff_scaling = prev_scaling_chk;
		}
	}
	TOUCH_INFO_MSG("=====================================\n");

	return 0;

}

static int mxt_get_panel_id(struct mxt_data *data){
	int panel_id = 0xFF;
	int ret = 0;
	int value = 0;
	static bool atmel_init = false;
	if (atmel_init && data->pdata->panel_id) {
		return data->pdata->panel_id;
	}
	if (!atmel_init) {
		if (data->pdata->maker_id_gpio && gpio_is_valid(data->pdata->maker_id_gpio)) {
			ret = gpio_request(data->pdata->maker_id_gpio, "touch_id");
			if (ret < 0) {
				TOUCH_ERR_MSG("FAIL: touch_id gpio_request = %d\n", ret);
				goto exit;
			} else {
				gpio_direction_input(data->pdata->maker_id_gpio);
			}
		} else {
			TOUCH_INFO_MSG("maker_id_gpio is invalid\n");
		}

		if (data->pdata->maker_id2_gpio && gpio_is_valid(data->pdata->maker_id2_gpio)) {
			ret = gpio_request(data->pdata->maker_id2_gpio, "touch_id2");
			if (ret < 0) {
				TOUCH_ERR_MSG("FAIL: touch_id2 gpio_request = %d\n", ret);
				goto exit;
			} else {
				gpio_direction_input(data->pdata->maker_id2_gpio);
			}
		} else {
			TOUCH_INFO_MSG("maker_id2_gpio is invalid\n");
		}
		atmel_init = true;
	}

	if (data->pdata->maker_id_gpio && gpio_is_valid(data->pdata->maker_id_gpio)) {
		value = gpio_get_value(data->pdata->maker_id_gpio);
		TOUCH_INFO_MSG("MAKER_ID : %s\n", value ? "High" : "Low");
		panel_id = ((~value) & 0x1);
	}
	if (data->pdata->maker_id2_gpio && gpio_is_valid(data->pdata->maker_id2_gpio)) {
		value = gpio_get_value(data->pdata->maker_id2_gpio);
		TOUCH_INFO_MSG("MAKER_ID_2 : %s\n", value ? "High" : "Low");
		panel_id += (((~value) & 0x1) << 1);
	}
	atmel_init = true;
	data->pdata->panel_id = panel_id;
	TOUCH_INFO_MSG("Touch panel id : %d", data->pdata->panel_id);
	return panel_id;
exit :
	TOUCH_ERR_MSG("%s FAIL \n", __func__);
	return 0xFF;
}

static int mxt_fb_resume(struct mxt_data *data);

static int __devinit mxt_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct mxt_data *data = NULL;
	int error = 0;
	int i = 0;
	int panel_id = 0;

	is_probing = true;
	TOUCH_INFO_MSG("%s \n", __func__);

	wake_lock_init(&touch_wake_lock, WAKE_LOCK_SUSPEND, "touch_wakelock");
	mutex_init(&i2c_suspend_lock);
	mutex_init(&irq_lock);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		TOUCH_INFO_MSG("i2c functionality check error\n");
		return -ENOMEM;
	}

	data = kzalloc(sizeof(struct mxt_data), GFP_KERNEL);
	if (!data) {
		TOUCH_INFO_MSG("Failed to allocate memory\n");
		return -ENOMEM;
	}

	data->ref_chk = 1;
	data->boot_mode = lge_get_boot_mode();
	if (data->boot_mode == LGE_BOOT_MODE_NORMAL) {
		TOUCH_INFO_MSG("Boot mode is Normal\n");
	} else {
		TOUCH_INFO_MSG("Boot mode is not Normal\n");
	}

	snprintf(data->phys, sizeof(data->phys), "i2c-%u-%04x/input0", client->adapter->nr, client->addr);
	TOUCH_INFO_MSG("i2c-%u-%04x/input0\n", client->adapter->nr, client->addr);
	data->client = client;
	data->irq = client->irq;
	i2c_set_clientdata(client, data);

	/*read dtsi data*/
	if (client->dev.of_node) {
		data->pdata = devm_kzalloc(&client->dev, sizeof(struct mxt_platform_data), GFP_KERNEL);
		if (!data->pdata) {
			TOUCH_INFO_MSG("Failed to allocate pdata memory\n");
			error = -ENOMEM;
			goto err_free_mem;
		}

		error = mxt_parse_dt(&client->dev, data->pdata);
		if (error)
			goto err_free_mem;
	}

	init_completion(&data->bl_completion);
	init_completion(&data->reset_completion);
	init_completion(&data->crc_completion);
	init_completion(&data->t25_completion); /* Self Test */

	/* request reset pin */
	if (data->pdata->gpio_reset> 0) {
		error = gpio_request(data->pdata->gpio_reset, "touch_reset");
		if (error < 0) {
			TOUCH_INFO_MSG("FAIL: touch_reset gpio_request\n");
			goto err_interrupt_failed;
		}
	}

	/* request interrupt pin */
	if (data->pdata->gpio_int > 0) {
		error = gpio_request(data->pdata->gpio_int, "touch_int");
		if (error < 0) {
			TOUCH_INFO_MSG("FAIL: touch_int gpio_request\n");
			goto err_interrupt_failed;
		}
		gpio_direction_input(data->pdata->gpio_int);
	}

	TOUCH_INFO_MSG("request_threaded_irq %s \n", __func__);
	error = request_threaded_irq(data->irq, NULL, mxt_interrupt,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_NO_SUSPEND, client->name, data);

	if (error) {
		TOUCH_INFO_MSG("Failed to register interrupt\n");
		goto err_free_pdata;
	}


	mxt_probe_regulators(data);
	mxt_regulator_enable(data);
	mxt_hw_reset(data);

	touch_disable_irq(data->irq);

	data->object_table = kzalloc((MXT_OBJECT_NUM_MAX * sizeof(struct mxt_object)), GFP_KERNEL);
	if (!data->object_table) {
		TOUCH_INFO_MSG("%s Failed to allocate memory\n", __func__);
		goto err_free_pdata;
	}

	error = mxt_scaling_factor_chk(data);
	if (error) {
		TOUCH_INFO_MSG("%s Failed to mxt_scailing_factor_chk\n", __func__);
	}

	if (data->pdata->maker_id) {
		panel_id = mxt_get_panel_id(data);
		if (panel_id == 0xFF) {
			TOUCH_INFO_MSG("fail get panel id\n");
			panel_id = 0;
		} else {
			TOUCH_INFO_MSG("success get panel id\n");
		}
	}

	if (data->pdata->fw_name[panel_id]) {
		error = mxt_update_firmware(data, data->pdata->fw_name[panel_id]);
		if (error) {
			TOUCH_INFO_MSG("Failed to request firmware\n");
			goto err_free_irq;
		}
	}

	if (data->pdata->panel_check && data->pdata->extra_fw_name) {
		TOUCH_INFO_MSG("%s panel check start \n", __func__);
		mxt_read_one_page_diagnostic_data(data,	MXT_DIAG_REFERENCE_MODE);

		if (data->rawdata->reference[0] < 1000 || data->rawdata->reference[3] > 50000) {
			TOUCH_INFO_MSG("Extra Pattern panel detected. Config need to change \n");

			error = mxt_update_firmware(data, data->pdata->extra_fw_name);
			if (error) {
				TOUCH_INFO_MSG("Failed to request firmware\n");
				goto err_free_irq;
			}

			if (data->pdata->t15_extra_keystate[0] > 0) {
				for(i = 0; i < data->pdata->t15_num_keys; i++) {
					data->pdata->t15_keystate[i] = data->pdata->t15_extra_keystate[i];
					TOUCH_INFO_MSG("Update t15_keystate[%d] = [%3d] \n", i, data->pdata->t15_keystate[i]);
				}
				for(i = 0; i < data->pdata->t15_num_keys; i++) {
					data->pdata->t15_key_array_x[i] = data->pdata->t15_extra_key_array_x[i];
					data->pdata->t15_key_array_y[i] = data->pdata->t15_extra_key_array_y[i];
					TOUCH_INFO_MSG("Update t15_keystate_array[%d] = [%d,%d] \n", i, data->pdata->t15_key_array_x[i], data->pdata->t15_key_array_y[i]);
				}
			}
		} else {
			TOUCH_INFO_MSG("No need to change Config \n");
		}
		TOUCH_INFO_MSG("%s panel check done \n", __func__);
	}

	mxt_write_object(data, MXT_GEN_COMMAND_T6, MXT_COMMAND_REPORTALL, 1);

	mxt_read_fw_version(data);

	if (data->T9_reportid_min) {
		error = mxt_initialize_t9_input_device(data);
		if (error) {
			TOUCH_INFO_MSG("Failed to init t9\n");
			goto err_free_irq;
		}
	} else {
		TOUCH_INFO_MSG("Failed to read touch object\n");
		goto err_free_irq;
	}

	/* Timer set */
#if 0
	if (data->pdata->ghost_detection_enable) {
		   hrtimer_init(&hr_delta_chk_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		   hr_delta_chk_timer.function = delta_chk_timer_handler;
	}
#endif

	INIT_DELAYED_WORK(&data->work_button_lock, mxt_button_lock_func);
	INIT_DELAYED_WORK(&data->work_palm_unlock, mxt_palm_unlock_func);
	INIT_DELAYED_WORK(&data->work_deepsleep, mxt_deepsleep_func);

	data->fb_notif.notifier_call = fb_notifier_callback;

	error = fb_register_client(&data->fb_notif);
	if (error)
		TOUCH_INFO_MSG("Unable to register fb_notifier: %d\n", error);

	/*channal size init for reference check*/
	error = __mxt_read_reg(data->client, data->T9_address + 1, 4, &data->channel_size);
	if (!error)
		TOUCH_INFO_MSG("Succeed to read channel_size %d %d %d %d \n", data->channel_size.start_x, data->channel_size.start_y, data->channel_size.size_x, data->channel_size.size_y);

	if (global_mxt_data) {
		error = __mxt_read_reg(global_mxt_data->client, global_mxt_data->T71_address + 51, 26, &global_mxt_data->ref_limit);
		global_mxt_data->ref_limit.diff_scaling = data->pdata->diff_scaling;


		if (!error) {
			TOUCH_INFO_MSG("Succeed to read diff_scaling:%d reference limit u:%d x_all_err_chk:%d y_all_err_chk:%d only_x_err_cnt : %d only_y_err_cnt : %d max-min rng:%d diff_rng:%d err_cnt:%d err_weight:%d\n",
				global_mxt_data->ref_limit.diff_scaling,
				global_mxt_data->ref_limit.ref_chk_using, global_mxt_data->ref_limit.ref_x_all_err_line,
				global_mxt_data->ref_limit.ref_y_all_err_line, global_mxt_data->ref_limit.xline_max_err_cnt,
				global_mxt_data->ref_limit.yline_max_err_cnt, global_mxt_data->ref_limit.ref_rng_limit,
				global_mxt_data->ref_limit.ref_diff_max, global_mxt_data->ref_limit.ref_err_cnt,
				global_mxt_data->ref_limit.err_weight);
			TOUCH_INFO_MSG("Err Range y: %d %d %d %d %d %d %d %d %d %d %d %d %d %d, Button range:%d %d %d\n",
				global_mxt_data->ref_limit.y_line_dif[0], global_mxt_data->ref_limit.y_line_dif[1],
				global_mxt_data->ref_limit.y_line_dif[2], global_mxt_data->ref_limit.y_line_dif[3],
				global_mxt_data->ref_limit.y_line_dif[4], global_mxt_data->ref_limit.y_line_dif[5],
				global_mxt_data->ref_limit.y_line_dif[6], global_mxt_data->ref_limit.y_line_dif[7],
				global_mxt_data->ref_limit.y_line_dif[8], global_mxt_data->ref_limit.y_line_dif[9],
				global_mxt_data->ref_limit.y_line_dif[10], global_mxt_data->ref_limit.y_line_dif[11],
				global_mxt_data->ref_limit.y_line_dif[12], global_mxt_data->ref_limit.y_line_dif[13],
				global_mxt_data->ref_limit.butt_dif[0], global_mxt_data->ref_limit.butt_dif[1],
				global_mxt_data->ref_limit.butt_dif[2]);
		}
	}

	data->ref_chk = 0;

	/* disabled report touch event to prevent unnecessary event.
	* it will be enabled in open function
	*/
	mxt_stop(data);

	/* Register sysfs for making fixed communication path to framework layer */
	error = sysdev_class_register(&lge_touch_sys_class);
	if (error < 0) {
		TOUCH_INFO_MSG("sysdev_class_register is failed\n");
		goto err_lge_touch_sys_class_register;
	}

	error = sysdev_register(&lge_touch_sys_device);
	if (error < 0) {
		TOUCH_INFO_MSG("sysdev_register is failed\n");
		goto err_lge_touch_sys_dev_register;
	}

	error = kobject_init_and_add(&data->lge_touch_kobj, &lge_touch_kobj_type,
			data->input_dev->dev.kobj.parent, "%s", LGE_TOUCH_NAME);
	if (error < 0) {
		TOUCH_INFO_MSG("kobject_init_and_add is failed\n");
		goto err_lge_touch_sysfs_init_and_add;
	}

	sysfs_bin_attr_init(&data->mem_access_attr);
	data->mem_access_attr.attr.name = "mem_access";
	data->mem_access_attr.attr.mode = S_IWUSR | S_IRUSR;
	data->mem_access_attr.read = mxt_mem_access_read;
	data->mem_access_attr.write = mxt_mem_access_write;
	data->mem_access_attr.size = data->mem_size;

	if (sysfs_create_bin_file(&client->dev.kobj, &data->mem_access_attr) < 0) {
		TOUCH_INFO_MSG("Failed to create %s\n", data->mem_access_attr.attr.name);
		goto err_lge_touch_sysfs_init_and_add;
	}

	TOUCH_INFO_MSG("%s success \n", __func__);
	data->reset.state = IDLE_STATE;
	data->patch.src_item[MXT_PATCH_ITEM_USER7] = IDLE_STATE;

	if (global_mxt_data) {
		TOUCH_INFO_MSG("%s global_mxt_data exist \n", __func__);
	} else {
		TOUCH_INFO_MSG("%s global_mxt_data is NULL \n", __func__);
	}
	is_probing = false;

       /*Fix touch init, eg. in TWRP */
       mxt_fb_resume(data);

	return 0;

err_lge_touch_sysfs_init_and_add:
	kobject_del(&data->lge_touch_kobj);
err_lge_touch_sys_dev_register:
	sysdev_unregister(&lge_touch_sys_device);
err_lge_touch_sys_class_register:
	sysdev_class_unregister(&lge_touch_sys_class);
	mxt_free_object_table(data);
err_free_irq:
	free_irq(data->irq, data);
err_interrupt_failed:
err_free_pdata:
err_free_mem:
	if (data)
		kfree(data);
	return error;
}

static int __devexit mxt_remove(struct i2c_client *client)
{
	struct mxt_data *data = i2c_get_clientdata(client);

	TOUCH_INFO_MSG("%s \n", __func__);

	if (data) {
		if (fb_unregister_client(&data->fb_notif))
			TOUCH_INFO_MSG("Error occurred while unregistering fb_notifier.\n");

		kobject_del(&data->lge_touch_kobj);
		sysdev_unregister(&lge_touch_sys_device);
		sysdev_class_unregister(&lge_touch_sys_class);

		if (data->pdata->gpio_int > 0)
			gpio_free(data->pdata->gpio_int);
		if (data->irq)
			free_irq(data->irq, data);
		if (data->vdd_ana)
			regulator_put(data->vdd_ana);
		if (data->vcc_i2c)
			regulator_put(data->vcc_i2c);
		if (data->vcc_dig)
			regulator_put(data->vcc_dig);
		mxt_free_object_table(data);

		if (data)
			kfree(data);

		mutex_destroy(&i2c_suspend_lock);
		mutex_destroy(&irq_lock);

		wake_lock_destroy(&touch_wake_lock);
	}

	return 0;
}

static int mxt_suspend(struct device *device)
{
	TOUCH_INFO_MSG("%s \n", __func__);
	mutex_lock(&i2c_suspend_lock);
	return 0;
}

static int mxt_resume(struct device *device)
{
	TOUCH_INFO_MSG("%s \n", __func__);
	mutex_unlock(&i2c_suspend_lock);
	return 0;
}

static int mxt_fb_suspend(struct mxt_data *data)
{
	struct input_dev *input_dev = data->input_dev;

	TOUCH_INFO_MSG("%s \n", __func__);

	if (mxt_power_block_get()) {
		TOUCH_INFO_MSG("Still in use. Do nothing \n");
		return 0;
	}

	mutex_lock(&input_dev->mutex);
	touch_disable_irq(data->irq);

	data->pdata->panel_on = POWER_OFF;
	data->reset.state = IDLE_STATE;
	data->patch.src_item[MXT_PATCH_ITEM_USER7] = IDLE_STATE;
	data->t72_noise_state = false;
	data->cal_cnt = 0;

	if (input_dev->users)
		mxt_stop(data);

	if (data->mxt_knock_on_enable) {
		touch_enable_irq_wake(data->irq);
		touch_enable_irq(data->irq);
	}

	mutex_unlock(&input_dev->mutex);
	return 0;
}

static int mxt_fb_resume(struct mxt_data *data)
{
	struct input_dev *input_dev = data->input_dev;
	char *package_name = NULL;
	int error = 0;
	int panel_id = 0;
	panel_id = data->pdata->panel_id;
	TOUCH_INFO_MSG("%s \n", __func__);

	if (mxt_power_block_get()) {
		TOUCH_INFO_MSG("Still in use. Do nothing \n");
		return 0;
	}

	mutex_lock(&input_dev->mutex);

	if (data->work_deepsleep_enabled) {
		data->work_deepsleep_enabled = false;
		cancel_delayed_work_sync(&data->work_deepsleep);
	}

	data->pdata->panel_on = POWER_ON;
	data->palm = false;

	do_gettimeofday(&t_ex_debug[TIME_EX_INIT_TIME]);
	ghost_detection_check_count = 0;

	if (data->mxt_knock_on_enable) {
		touch_disable_irq_wake(data->irq);
	}

	if (data->pdata->use_mfts && data->mfts_enable) {
		TOUCH_INFO_MSG("MFTS : IC Init start \n");

		touch_disable_irq(data->irq);

		mxt_hw_reset(data);
		error = mxt_update_file_name(&data->client->dev, &package_name, data->pdata->fw_name[panel_id], strlen(data->pdata->fw_name[panel_id]));
		if (error) {
			TOUCH_INFO_MSG("%s error package_name [%s] \n", __func__, package_name);
			goto exit;
		}

		error = mxt_update_firmware(data, package_name);
		if (error) {
			TOUCH_INFO_MSG("%s error \n", __func__);
			goto exit;
		}

	exit:
		if (package_name) {
			kfree(package_name);
		}

		mxt_read_fw_version(data);
	}

	if (input_dev->users)
		mxt_start(data);

	if (data->pdata->use_mfts && data->mfts_enable) {
		mxt_soft_reset(data);
		TOUCH_INFO_MSG("MFTS : IC Init complete \n");
	}

	mutex_unlock(&input_dev->mutex);
	return 0;
}

int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank = NULL;
	struct mxt_data *ts = container_of(self, struct mxt_data, fb_notif);

	if (evdata && evdata->data && event == FB_EVENT_BLANK && ts && ts->client) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK)
			mxt_fb_resume(ts);
		else if (*blank == FB_BLANK_POWERDOWN)
			mxt_fb_suspend(ts);
	}

	return 0;
}

static void mxt_shutdown(struct i2c_client *client)
{
	struct mxt_data *data = i2c_get_clientdata(client);

	TOUCH_INFO_MSG("%s \n", __func__);

	if (data && data->irq) {
		touch_disable_irq(data->irq);
	}
}

static struct of_device_id mxt_match_table[] = {
	{ .compatible = "atmel,s336",},
	{ },
};

static const struct i2c_device_id mxt_id[] = {
	{ MXT_DEVICE_NAME, 0},
	{ }
};

MODULE_DEVICE_TABLE(i2c, mxt_id);

static struct dev_pm_ops touch_pm_ops = {
	.suspend 	= mxt_suspend,
	.resume 	= mxt_resume,
};

static struct i2c_driver mxt_driver = {
	.driver = {
		.name	= "touch_atmel",
		.of_match_table = mxt_match_table,
		.owner	= THIS_MODULE,
		.pm	= &touch_pm_ops,
	},
	.probe		= mxt_probe,
	.remove		= __devexit_p(mxt_remove),
	.shutdown	= mxt_shutdown,
	.id_table	= mxt_id,
};

static void async_mxt_init(void *data, async_cookie_t cookie)
{
	i2c_add_driver(&mxt_driver);
	return;
}

#ifdef CONFIG_LGE_PM_CHARGING_CHARGERLOGO
extern int lge_boot_mode_for_touch;
#endif

static int __init mxt_init(void)
{
#ifdef CONFIG_LGE_PM_CHARGING_CHARGERLOGO
	if (lge_boot_mode_for_touch == 2) { // Chargerlogo mode
		return -EMLINK;
	}
#endif

	touch_wq = create_singlethread_workqueue("touch_wq");
	if (!touch_wq) {
		TOUCH_INFO_MSG("touch_wq error\n");
		return -ENOMEM;
	}

	async_schedule(async_mxt_init, NULL);

	return 0;
}

static void __exit mxt_exit(void)
{
#ifdef CONFIG_LGE_PM_CHARGING_CHARGERLOGO
	if (lge_boot_mode_for_touch == 2) { // Chargerlogo mode
		return;
	}
#endif

	i2c_del_driver(&mxt_driver);

	if (touch_wq)
		destroy_workqueue(touch_wq);
}

module_init(mxt_init);
module_exit(mxt_exit);

/* Module information */
MODULE_AUTHOR("<WX-BSP-TS@lge.com>");
MODULE_DESCRIPTION("Atmel Touchscreen driver");
MODULE_LICENSE("GPL");
