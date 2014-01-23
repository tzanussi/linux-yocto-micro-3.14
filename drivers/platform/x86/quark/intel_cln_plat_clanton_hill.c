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
 * Intel Clanton Legacy Platform Data Layout.conf accessor
 *
 * Simple Legacy SPI flash access layer
 *
 * Author : Bryan O'Donoghue <bryan.odonoghue@linux.intel.com> 2013
 */

#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/spi/pxa2xx_spi.h>
#include <linux/spi/spi.h>

#define DRIVER_NAME "cln-plat-clanton-hill"
#define GPIO_RESTRICT_NAME "cln-gpio-restrict-nc"

/******************************************************************************
 *             Analog Devices AD7298 SPI Device Platform Data
 ******************************************************************************/
#include "linux/platform_data/ad7298.h"

/* Maximum input voltage allowed for each ADC input, in milliVolts */
#define AD7298_MAX_EXT_VIN 5000
#define AD7298_MAX_EXT_VIN_EXT_BATT 30000
#define AD7298_MAX_EXT_VIN_INT_BATT 9200

static const struct ad7298_platform_data ad7298_platform_data = {
	.ext_ref = false,
	.ext_vin_max = { AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN,
		AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN,
		AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN,
		AD7298_MAX_EXT_VIN_EXT_BATT, AD7298_MAX_EXT_VIN_INT_BATT }
};

/******************************************************************************
 *                 Intel Clanton SPI Controller Data
 ******************************************************************************/
static struct pxa2xx_spi_chip cln_ffrd_spi_0_cs_0 = {
	.gpio_cs = 8,
};

static struct spi_board_info spi_onboard_devs[] = {
	{
		.modalias = "ad7298",
		.max_speed_hz = 5000000,
		.platform_data = &ad7298_platform_data,
		.mode = SPI_MODE_2,
		.bus_num = 0,
		.chip_select = 0,
		.controller_data = &cln_ffrd_spi_0_cs_0,
	},
};

/******************************************************************************
 *             ST Microelectronics LIS331DLH I2C Device Platform Data
 ******************************************************************************/
#include <linux/platform_data/lis331dlh_intel_cln.h>

/* GPIO interrupt pins connected to the LIS331DLH */
#define ST_ACCEL_INT1_GPIO 15
#define ST_ACCEL_INT2_GPIO 4

static struct lis331dlh_intel_cln_platform_data lis331dlh_i2c_platform_data = {
	.irq1_pin = ST_ACCEL_INT1_GPIO,
};

static struct gpio reserved_gpios[] = {
	{
		ST_ACCEL_INT1_GPIO,
		GPIOF_IN,
		"st_accel_i2c-int1"
	},
	{
		ST_ACCEL_INT2_GPIO,
		GPIOF_IN,
		"st_accel_i2c-int2"
	},
};

static struct i2c_board_info i2c_onboard_devs[] = {
	{
		I2C_BOARD_INFO("intel-cln-max9867", 0x18),
	},
	{
		I2C_BOARD_INFO("lis331dlh_cln", 0x19),
		.platform_data  = &lis331dlh_i2c_platform_data,
	},
};

/**
 * intel_cln_spi_add_onboard_devs
 *
 * @return 0 on success or standard errnos on failure
 *
 * Registers onboard SPI device(s) present on the Clanton Hill platform
 */
static int intel_cln_spi_add_onboard_devs(void)
{
	return spi_register_board_info(spi_onboard_devs,
				       ARRAY_SIZE(spi_onboard_devs));
}

/**
 * intel_cln_i2c_add_onboard_devs
 *
 * @return 0 on success or standard errnos on failure
 *
 * Registers onboard I2C device(s) present on the Clanton Hill platform
 */
static int intel_cln_i2c_add_onboard_devs(void)
{
	return i2c_register_board_info(0, i2c_onboard_devs,
			ARRAY_SIZE(i2c_onboard_devs));
}


/**
 * intel_cln_gpio_restrict_probe
 *
 * Make GPIOs pertaining to Firmware inaccessible by requesting them.  The
 * GPIOs are never released nor accessed by this driver.
 */
static int intel_cln_gpio_restrict_probe(struct platform_device *pdev)
{
	int ret = gpio_request_array(reserved_gpios,
				     ARRAY_SIZE(reserved_gpios));
	if (ret == 0)
		ret = intel_cln_spi_add_onboard_devs();

	return ret;
}

static struct platform_driver gpio_restrict_pdriver = {
	.driver		= {
		.name	= GPIO_RESTRICT_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= intel_cln_gpio_restrict_probe,
};

static int intel_cln_plat_clanton_hill_probe(struct platform_device *pdev)
{
	int ret = 0;

	ret = intel_cln_i2c_add_onboard_devs();
	if (ret == 0)
		ret = platform_driver_register(&gpio_restrict_pdriver);

	return ret;
}

static int intel_cln_plat_clanton_hill_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver cln_clanton_hill_driver = {
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= intel_cln_plat_clanton_hill_probe,
	.remove		= intel_cln_plat_clanton_hill_remove,
};

module_platform_driver(cln_clanton_hill_driver);

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("Clanton Hill BSP Data");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:"DRIVER_NAME);

