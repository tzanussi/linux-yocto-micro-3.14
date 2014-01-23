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
#include <linux/mtd/partitions.h>
#include <linux/mtd/physmap.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/spi/pxa2xx_spi.h>
#include <linux/spi/spi.h>
#include <linux/spi/flash.h>
#include <linux/platform_data/at24.h>

#define DRIVER_NAME 		"cln-plat-galileo"
#define GPIO_RESTRICT_NAME 	"cln-gpio-restrict-sc"
#define LPC_SCH_SPINAME		"spi-lpc-sch"

#define CLN_SPI_MAX_CLK_DEFAULT		5000000

/******************************************************************************
 *             Analog Devices AD7298 SPI Device Platform Data
 ******************************************************************************/
#include "linux/platform_data/ad7298.h"

/* Maximum input voltage allowed for each ADC input, in milliVolts */
#define AD7298_MAX_EXT_VIN 5000

static const struct ad7298_platform_data ad7298_platform_data = {
	.ext_ref = false,
	.ext_vin_max = { AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN,
		AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN,
		AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN,
		AD7298_MAX_EXT_VIN, AD7298_MAX_EXT_VIN }
};

static struct at24_platform_data at24_platform_data = {
	.byte_len = (11 * 1024),
	.page_size = 1,
	.flags = AT24_FLAG_ADDR16,
};

/******************************************************************************
 *                        Intel Izmir i2c clients
 ******************************************************************************/
static struct i2c_board_info __initdata galileo_i2c_board_info[] = {
	{
		/* Note following address may change at driver load time */
		I2C_BOARD_INFO("cy8c9540a", 0x20),
	},
	{
		I2C_BOARD_INFO("at24", 0x50),
		.platform_data = &at24_platform_data,
	},
};

/******************************************************************************
 *                 Intel Clanton SPI Controller Data
 ******************************************************************************/
static struct pxa2xx_spi_chip cln_ffrd_spi_0_cs_0 = {
	.gpio_cs = 8,
};

static struct pxa2xx_spi_chip cln_ffrd_spi_1_cs_0 = {
	.gpio_cs = 10,
};

#define LPC_SCH_SPI_BUS_ID 0x03

static struct platform_device lpc_sch_spi = {
	.name = "spi-lpc-sch-drv",
	.id = LPC_SCH_SPI_BUS_ID,
};

/* TODO: extract this data from layout.conf encoded in flash */
struct mtd_partition ilb_partitions [] = {
	{
		.name		= "grub",
		.size		= 4096,
		.offset		= 0,
	},
	{
		.name		= "grub.conf",
		.size		= 0xA00,
		.offset		= 0x50500,
	},
	{
		.name		= "layout.conf",
		.size		= 4096,
		.offset		= 0x708000,
	},
	{
		.name		= "sketch",
		.size		= 0x40000,
		.offset		= 0x750000,
	},
	{
		.name		= "raw",
		.size		= 8192000,
		.offset		= 0,

	},
};

static struct flash_platform_data ilb_flash = {
	.type = "s25fl064k",
	.parts = ilb_partitions,
	.nr_parts = ARRAY_SIZE(ilb_partitions),
};

static struct spi_board_info spi_onboard_devs[] = {
	{
		.modalias = "m25p80",
		.platform_data = &ilb_flash,
		.bus_num = LPC_SCH_SPI_BUS_ID,
		.chip_select = 0,
	},
	{
		.modalias = "ad7298",
		.max_speed_hz = CLN_SPI_MAX_CLK_DEFAULT,
		.platform_data = &ad7298_platform_data,
		.mode = SPI_MODE_2,
		.bus_num = 0,
		.chip_select = 0,
		.controller_data = &cln_ffrd_spi_0_cs_0,
	},
	{
		.modalias = "spidev",
		.chip_select = 0,
		.controller_data = &cln_ffrd_spi_1_cs_0,
		.max_speed_hz = 50000000,
		.bus_num = 1,
	},
};

/**
 * intel_cln_spi_add_onboard_devs
 *
 * @return 0 on success or standard errnos on failure
 *
 * Registers onboard SPI device(s) present on the Izmir platform
 */
static int intel_cln_spi_add_onboard_devs(void)
{

	return spi_register_board_info(spi_onboard_devs,
			ARRAY_SIZE(spi_onboard_devs));
}


/**
 * intel_cln_gpio_restrict_probe
 *
 * Make GPIOs pertaining to Firmware inaccessible by requesting them.  The
 * GPIOs are never released nor accessed by this driver.
 */
static int intel_cln_gpio_restrict_probe(struct platform_device *pdev)
{
	int ret = 0;

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

/* LPC SPI */
static int intel_cln_plat_galileo_lpcspi_probe(struct platform_device *pdev)
{
	lpc_sch_spi.resource = pdev->resource;
	return platform_device_register(&lpc_sch_spi);
}

static struct platform_driver intel_cln_plat_galileo_lpcspi_pdriver = {
	.driver		= {
		.name	= LPC_SCH_SPINAME,
		.owner	= THIS_MODULE,
	},
	.probe		= intel_cln_plat_galileo_lpcspi_probe,
};

static int intel_cln_plat_galileo_probe(struct platform_device *pdev)
{
	int ret = 0;

	/* i2c */
	ret = i2c_register_board_info(0, galileo_i2c_board_info,
		ARRAY_SIZE(galileo_i2c_board_info));
	if (ret) {
		goto end;
	}

	/* gpio */
	ret = platform_driver_register(&gpio_restrict_pdriver);
	if (ret)
		goto end;

#if 0
	/* legacy SPI - TBD */
	ret = platform_driver_register(&intel_cln_plat_galileo_lpcspi_pdriver);
	if (ret)
		goto end;
#endif	
end:
	return ret;
}

static int intel_cln_plat_galileo_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver cln_galileo_driver = {
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= intel_cln_plat_galileo_probe,
	.remove		= intel_cln_plat_galileo_remove,
};

module_platform_driver(cln_galileo_driver);

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("Galileo BSP Data");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:"DRIVER_NAME);

