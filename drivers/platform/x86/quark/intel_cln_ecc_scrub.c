/*
 * Copyright(c) 2013 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Contact Information:
 * Intel Corporation
 */
/*
 * Intel Clanton DRAM ECC Scrub driver
 *
 * !!!!!!! Description
 *
 */
#include <asm-generic/uaccess.h>
#include <linux/intel_cln_sb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>

#define DRIVER_NAME			"intel-cln-ecc"
#define INTEL_CLN_ECC_SCRUB_PROCDIR	"driver/ecc_scrub"
#define STATUS				"status"
#define CONTROL				"control"
#define INTERVAL			"interval"
#define ECC_BLOCK_SIZE			"block_size"

#define CONTROL_USAGE		"ECC Scrub Control: invalid setting. "\
				"Valid values are 1 or 0\n"
#define CONTROL_SCRUB_ON_STR	"1\n"
#define CONTROL_SCRUB_OFF_STR	"0\n"
#define CONTROL_ON_STR		"on\n"
#define CONTROL_OFF_STR		"off\n"

#define INTERVAL_USAGE		"ECC Scrub Interval: invalid setting. "\
				"Valid range is 1 - 255\n"
#define SIZE_USAGE		"ECC Scrub Block Size: invalid setting. "\
				"Valid range is 64 - 512\n"

#define OVERRIDE_CONFIG_PARM_DESC	"Clanton ECC Scrub - "\
					"Override BIOS settings "\
					"for Scrub Config"

#define OVERRIDE_START_PARM_DESC	"Clanton ECC Scrub - "\
					"Override BIOS settings "\
					"for Scrub Start address"

#define OVERRIDE_END_PARM_DESC		"Clanton ECC Scrub - "\
					"Override BIOS settings "\
					"for Scrub End address"

#define OVERRIDE_NEXT_PARM_DESC		"Clanton ECC Scrub - "\
					"Override BIOS settings "\
					"for Scrub Next address"

#define MAX_SCRUB_BLOCK_SIZE 512
#define MIN_SCRUB_BLOCK_SIZE 64
#define MAX_SCRUB_REFRESH 255
#define MIN_SCRUB_REFRESH 0

#define NOT_OVERRIDDEN 0xfffffffful

/* Shorten fn names to fit 80 char limit */
#ifndef sb_read
#define sb_read				intel_cln_sb_read_reg
#endif
#ifndef sb_write
#define sb_write			intel_cln_sb_write_reg
#endif

/* Register ID */
#define ECC_SCRUB_CONFIG_REG		(0x50)
#define ECC_SCRUB_START_MEM_REG		(0x76)
#define ECC_SCRUB_END_MEM_REG		(0x77)
#define ECC_SCRUB_NEXT_READ_REG		(0x7C)


/* Reg commands */
#define THERMAL_CTRL_READ		(0x10)
#define THERMAL_CTRL_WRITE		(0x11)
#define THERMAL_RESUME_SCRUB		(0xC2)
#define THERMAL_PAUSE_SCRUB		(0xC3)

/**
 * struct intel_cln_ecc_scrub_dev
 *
 * Structure to represent module state/data/etc
 */
struct intel_cln_ecc_scrub_dev {

	/* Linux kernel structures */
	struct platform_device *pldev;		/* Platform device */

	/* Register copies */
	u32 start_address;
	u32 end_address;
	u32 next_address;
	u32 config;

};

static struct intel_cln_ecc_scrub_dev ecc_scrub_dev;

static u32 ecc_scrub_config_override = NOT_OVERRIDDEN;
static u32 ecc_scrub_start_override = NOT_OVERRIDDEN;
static u32 ecc_scrub_end_override = NOT_OVERRIDDEN;
static u32 ecc_scrub_next_override = NOT_OVERRIDDEN;

/**
 * intel_cln_ecc_scrub_stat_show
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: output buffer
 * @return number of bytes successfully read
 *
 * Populates ecc_scrub state via /sys/device/platform/intel-cln-ecc/status
 */
static ssize_t
intel_cln_ecc_scrub_stat_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int len = 0, size = 0;
	unsigned int count = PAGE_SIZE;
	u32 reg_data = 0;
	char *scrub_status = CONTROL_OFF_STR;

	/* Display start of memory address */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_START_MEM_REG,
		&reg_data, 1);
	len += snprintf(buf + len, count - len,
			"ecc scrub mem start\t\t\t: 0x%08x\n", reg_data);


	/* Display end of memory address */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_END_MEM_REG,
		&reg_data, 1);
	len += snprintf(buf + len, count - len,
			"ecc scrub mem end\t\t\t: 0x%08x\n", reg_data);

	/* Display next address to be read */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_NEXT_READ_REG,
		&reg_data, 1);
	len += snprintf(buf + len, count - len,
			"ecc scrub next read\t\t\t: 0x%08x\n", reg_data);

	/* Display config settings */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_CONFIG_REG,
		&reg_data, 1);

	/* Interval is the lsbyte of the config reg, so mask out just
	 * that byte in the data printed. */
	len += snprintf(buf + len, count - len,
			"ecc scrub interval\t\t\t: %d\n",
			(reg_data & 0x000000ff));

	/* Size is indicated in bits 12:8 of register in
	* terms of 32 byte blocks. */
	size = ((reg_data & 0x00001f00) >> 8)*32;
	len += snprintf(buf + len, count - len,
			"ecc scrub block_size\t\t\t: %d\n", size);

	/* Status is indicated in bit 13 of register. */
	if ((reg_data & 0x00002000) > 0)
		scrub_status = CONTROL_ON_STR;

	len += snprintf(buf + len, count - len,
			"ecc scrub status\t\t\t: %s\n", scrub_status);
	return len;
}

/**
 * intel_cln_ecc_scrub_ctrl_show
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: output buffer
 * @return number of bytes successfully read
 *
 * Populates ecc_scrub state via /sys/device/platform/intel-cln-ecc/control
 */
static ssize_t
intel_cln_ecc_scrub_ctrl_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	unsigned int count = PAGE_SIZE;
	u32 reg_data = 0;
	char *on_or_off = CONTROL_SCRUB_OFF_STR;

	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_CONFIG_REG,
	&reg_data, 1);

	/* Status is indicated in bit 13 of register. */
	if ((reg_data & 0x00002000) > 0)
		/* interval > 0 assume scrubbing on */
		on_or_off = CONTROL_SCRUB_ON_STR;

	return snprintf(buf, count,"%s", on_or_off);
}

/**
 * intel_cln_ecc_scrub_ctrl_store
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: input buffer
 * @param size: size of input data
 * @return number of bytes successfully written
 *
 * Function allows user-space to switch on/off scrubbing with a simple
 * echo 1/0 > /sys/device/platform/intel-cln-ecc/control command
 */
static ssize_t
intel_cln_ecc_scrub_ctrl_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	ssize_t ret = 0;

	if (count <= 1)
		return -EINVAL;

	ret = -EINVAL;

	/* Check for command starting with "scrub"
	*  and ending with "on" or "off" */

	if (!strcmp(buf, CONTROL_SCRUB_ON_STR)) {
			sb_write(SB_ID_THERMAL, THERMAL_RESUME_SCRUB,
			0, 0, 1);
			ret = 0;
	} else if (!strcmp(buf, CONTROL_SCRUB_OFF_STR)) {
		sb_write(SB_ID_THERMAL, THERMAL_PAUSE_SCRUB, 0,
			 0, 1);
		ret = 0;
	}


	if (ret == 0)
		ret = (ssize_t)count;

	else if (ret == -EINVAL)
		printk(CONTROL_USAGE);

	return ret;
}

/**
 * intel_cln_ecc_scrub_intrvl_show
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: output buffer
 * @return number of bytes successfully read
 *
 * Populates ecc_scrub state via /sys/device/platform/intel-cln-ecc/interval
 */
static ssize_t
intel_cln_ecc_scrub_intrvl_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	u32 reg_data = 0;

	/* Interval is the lsbyte of the config reg,
	* so mask out just that byte in the data printed. */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_CONFIG_REG,
		&reg_data, 1);

	return snprintf(buf, PAGE_SIZE, "%d\n", (reg_data & 0x000000ff));
}

/**
 * intel_cln_ecc_scrub_intrvl_store
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: input buffer
 * @param size: size of input data
 * @return number of bytes successfully written
 *
 * Function allows user-space to set scrub interval with a value of 1-255
 * echo 1-255 > /sys/device/platform/intel-cln-ecc/interval type command
 */
static ssize_t
intel_cln_ecc_scrub_intrvl_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	ssize_t ret = 0;
	unsigned long val = 0;
	u32 reg_data = 0;
	int ret_temp = 0;

	if (count <= 1)
		return -EINVAL;

	ret = -EINVAL;

	ret_temp = kstrtoul(buf, 10, &val);

	if (ret_temp)
		return ret_temp;

	if (val > MIN_SCRUB_REFRESH && val <= MAX_SCRUB_REFRESH) {
		/* Need to read-modify-write config register. */
		sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ,
			ECC_SCRUB_CONFIG_REG,
			&reg_data, 1);

		reg_data &= 0xffffff00;	/* clear lsb. */
		reg_data |= val;		/* now set interval. */

		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_CONFIG_REG,
			 reg_data, 1);
		ret = 0;
	} else {
		printk(INTERVAL_USAGE);
	}

	if (ret == 0)
		ret = (ssize_t)count;
	return ret;
}

/**
 * intel_cln_ecc_scrub_size_show
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: output buffer
 * @return number of bytes successfully read
 *
 * Populates ecc_scrub state via /sys/device/platform/intel-cln-ecc/block_size
 */
static ssize_t
intel_cln_ecc_scrub_size_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int size = 0;
	u32 reg_data = 0;

	/* Size is indicated in bits 12:8 of config register
	 * multiply x32 to get num bytes). */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_CONFIG_REG,
		&reg_data, 1);
	size = ((reg_data & 0x00001f00) >> 8)*32;

	return snprintf(buf, PAGE_SIZE, "%d\n", size);
}

/**
 * intel_cln_ecc_scrub_size_store
 *
 * @param dev: pointer to device
 * @param attr: attribute pointer
 * @param buf: input buffer
 * @param size: size of input data
 * @return number of bytes successfully written
 *
 * Function allows user-space to set scrub block size of 64-512 with a simple
 * echo 64-512 > /sys/device/platform/intel-cln-ecc/block_size command
 */
static ssize_t
intel_cln_ecc_scrub_size_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	ssize_t ret = 0;
	unsigned long val = 0;
	u32 reg_data = 0;
	int ret_temp = 0;

	if (count <= 1)
		return -EINVAL;

	ret = -EINVAL;
	ret_temp = kstrtoul(buf, 10, &val);

	if (ret_temp)
		return ret_temp;

	if (val >= MIN_SCRUB_BLOCK_SIZE && val <= MAX_SCRUB_BLOCK_SIZE){

		/* Need to read-modify-write config register. */
		sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ,
			ECC_SCRUB_CONFIG_REG,
			&reg_data, 1);

		reg_data &= 0xfffffe0ff;	/* clear bits 12:8 */
		reg_data |= (val/32)<<8;	/* now set size */

		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_CONFIG_REG, reg_data, 1);
		ret = 0;
	} else {
		printk(SIZE_USAGE);
	}

	if (ret == 0)
		ret = (ssize_t)count;

	return ret;
}

static struct device_attribute dev_attr_status = {
	.attr = {
		.name = "status",
		.mode = 0444,
		},
	.show = intel_cln_ecc_scrub_stat_show,
};

static struct device_attribute dev_attr_control = {
	.attr = {
		.name = "control",
		.mode = 0644,
		},
	.show = intel_cln_ecc_scrub_ctrl_show,
	.store = intel_cln_ecc_scrub_ctrl_store,
};

static struct device_attribute dev_attr_intrvl = {
	.attr = {
		.name = "interval",
		.mode = 0644,
		},
	.show = intel_cln_ecc_scrub_intrvl_show,
	.store = intel_cln_ecc_scrub_intrvl_store,
};

static struct device_attribute dev_attr_block_size = {
	.attr = {
		.name = "block_size",
		.mode = 0644,
		},
	.show = intel_cln_ecc_scrub_size_show,
	.store = intel_cln_ecc_scrub_size_store,
};

static struct attribute *platform_attributes[] = {
	&dev_attr_status.attr,
	&dev_attr_control.attr,
	&dev_attr_intrvl.attr,
	&dev_attr_block_size.attr,
	NULL,
};

static struct attribute_group ecc_attrib_group = {
	.attrs = platform_attributes
};

/*****************************************************************************
 *                        Module/PowerManagement hooks
 *****************************************************************************/
/**
 * intel_cln_ecc_probe
 *
 * @param pdev: Platform device
 * @return 0 success < 0 failure
 *
 * Callback from platform sub-system to probe
 *
 */
static int intel_cln_ecc_scrub_probe(struct platform_device *pdev)
{
	int value_overridden = 0;

#ifdef CONFIG_INTEL_CLN_ECC_SCRUB_OVERRIDE_CONFIG
	u32 scrubber_refresh = 0;
	u32 scrubber_block_size = 0;
	u32 config_settings = 0;
#endif

	memset(&ecc_scrub_dev, 0x00, sizeof(ecc_scrub_dev));

	/* Update config settings, if directed so to do */
	if (ecc_scrub_start_override != NOT_OVERRIDDEN) {
		/* start of memory address */
		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_START_MEM_REG, ecc_scrub_start_override, 1);

			value_overridden = 1;
	}
	if (ecc_scrub_end_override != NOT_OVERRIDDEN) {
		/* end of memory address */
		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_END_MEM_REG, ecc_scrub_end_override, 1);

			value_overridden = 1;
	}
	if (ecc_scrub_next_override != NOT_OVERRIDDEN) {
		/* next address to be read */
		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_NEXT_READ_REG, ecc_scrub_next_override, 1);

			value_overridden = 1;
	}
	if (ecc_scrub_config_override != NOT_OVERRIDDEN) {
		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_CONFIG_REG, ecc_scrub_config_override, 1);

			value_overridden = 1;
	}

	/* Config Reg can be updated by either command line or kconfig setting
	 * in the case where we have both the command line takes precedence.*/

	else {
#ifdef CONFIG_INTEL_CLN_ECC_SCRUB_OVERRIDE_CONFIG
		scrubber_refresh = CONFIG_INTEL_CLN_HW_ECC_REFRESH_RATE;
		scrubber_block_size = CONFIG_INTEL_CLN_HW_ECC_REFRESH_SIZE;

		if (scrubber_block_size > MAX_SCRUB_BLOCK_SIZE)
			scrubber_block_size = MAX_SCRUB_BLOCK_SIZE;

		else if (scrubber_block_size < MIN_SCRUB_BLOCK_SIZE)
			scrubber_block_size = MIN_SCRUB_BLOCK_SIZE;

		if (scrubber_refresh > MAX_SCRUB_REFRESH)
			scrubber_refresh = MAX_SCRUB_REFRESH;


		/* adjust block size to multiples of 32 -
		 * as that is what the register setting actually expects. */
		config_settings = scrubber_block_size/32;
		config_settings <<= 8;
		config_settings += scrubber_refresh;

		/* config settings */
		sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE,
			 ECC_SCRUB_CONFIG_REG, config_settings, 1);

		value_overridden = 1;
#endif
	}

	if (value_overridden)
		sb_write(SB_ID_THERMAL, THERMAL_RESUME_SCRUB, 0, 0, 1);

	return sysfs_create_group(&pdev->dev.kobj, &ecc_attrib_group);
}

/**
 * intel_cln_ecc_scrub_suspend
 *
 * @param pdev: Platform device structure (unused)
 * @return 0 success < 0 failure
 *
 */
static int intel_cln_ecc_scrub_suspend(struct device *pdev)
{
#ifdef CONFIG_INTEL_CLN_ECC_SCRUB_S3_CONFIG
	u32 reg_data = 0;

	/* Store off the 4 registers associated with scrubbing. */
	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_START_MEM_REG,
		&reg_data, 1);
	ecc_scrub_dev.start_address = reg_data;

	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_END_MEM_REG,
		&reg_data, 1);
	ecc_scrub_dev.end_address = reg_data;

	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_NEXT_READ_REG,
		&reg_data, 1);
	ecc_scrub_dev.next_address = reg_data;

	sb_read(SB_ID_THERMAL, THERMAL_CTRL_READ, ECC_SCRUB_CONFIG_REG,
		&reg_data, 1);
	ecc_scrub_dev.config = reg_data;
#endif
	return 0;
}

/**
 * intel_cln_ecc_scrub_resume
 *
 * @param pdev: Platform device structure (unused)
 * @return 0 success < 0 failure
 */
static int intel_cln_ecc_scrub_resume(struct device *pdev)
{
#ifdef CONFIG_INTEL_CLN_ECC_SCRUB_S3_CONFIG

	sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE, ECC_SCRUB_START_MEM_REG,
		 ecc_scrub_dev.start_address, 1);

	sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE, ECC_SCRUB_END_MEM_REG,
		 ecc_scrub_dev.end_address, 1);

	sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE, ECC_SCRUB_NEXT_READ_REG,
		 ecc_scrub_dev.next_address, 1);

	sb_write(SB_ID_THERMAL, THERMAL_CTRL_WRITE, ECC_SCRUB_CONFIG_REG,
		 ecc_scrub_dev.config, 1);

	sb_write(SB_ID_THERMAL, THERMAL_RESUME_SCRUB, 0, 0, 1);

#endif
	return 0;
}

/**
 * intel_cln_ecc_scrub_remove
 *
 * @return 0 success < 0 failure
 *
 * Removes a platform device
 */
static int intel_cln_ecc_scrub_remove(struct platform_device *pdev)
{
	return sysfs_create_group(&pdev->dev.kobj, &ecc_attrib_group);
}

/*
 * Power management operations
 */
static const struct dev_pm_ops intel_cln_ecc_scrub_pm_ops = {
	.suspend = intel_cln_ecc_scrub_suspend,
	.resume = intel_cln_ecc_scrub_resume,
};


/*
 * Platform structures useful for interface to PM subsystem
 */
static struct platform_driver intel_cln_ecc_scrub_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &intel_cln_ecc_scrub_pm_ops,
	},
	.probe = intel_cln_ecc_scrub_probe,
	.remove = intel_cln_ecc_scrub_remove,
};


MODULE_AUTHOR("Derek Harnett <derek.harnett@intel.com>");
MODULE_DESCRIPTION("Intel Clanton DRAM ECC-scrub driver");
MODULE_LICENSE("Dual BSD/GPL");

module_param(ecc_scrub_config_override, uint, 0644);
MODULE_PARM_DESC(ecc_scrub_config_override, OVERRIDE_CONFIG_PARM_DESC);

module_param(ecc_scrub_start_override, uint, 0644);
MODULE_PARM_DESC(ecc_scrub_start_override, OVERRIDE_START_PARM_DESC);

module_param(ecc_scrub_end_override, uint, 0644);
MODULE_PARM_DESC(ecc_scrub_end_override, OVERRIDE_END_PARM_DESC);

module_param(ecc_scrub_next_override, uint, 0644);
MODULE_PARM_DESC(ecc_scrub_next_override, OVERRIDE_NEXT_PARM_DESC);

module_platform_driver(intel_cln_ecc_scrub_driver);

