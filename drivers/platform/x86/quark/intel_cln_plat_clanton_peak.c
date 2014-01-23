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
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/spi/spi.h>

#define DRIVER_NAME "cln-plat-clanton-peak"

static struct spi_board_info spi_onboard_devs[] = {
	{
		.modalias = "spidev",
		.chip_select = 0,
		.max_speed_hz = 50000000,
		.bus_num = 0,
	},
	{
		.modalias = "spidev",
		.chip_select = 0,
		.max_speed_hz = 50000000,
		.bus_num = 1,
	},
};


/**
 * intel_cln_spi_add_onboard_devs
 *
 * @return 0 on success or standard errnos on failure
 *
 * Registers onboard SPI device(s) present on the Clanton Peak platform
 */
static int intel_cln_spi_add_onboard_devs(void)
{
	return spi_register_board_info(spi_onboard_devs,
			ARRAY_SIZE(spi_onboard_devs));
}

static int intel_cln_plat_clanton_peak_probe(struct platform_device *pdev)
{
	return intel_cln_spi_add_onboard_devs();
}

static int intel_cln_plat_clanton_peak_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver clanton_peak_driver = {
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= intel_cln_plat_clanton_peak_probe,
	.remove		= intel_cln_plat_clanton_peak_remove,
};

module_platform_driver(clanton_peak_driver);

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("Clanton Peak BSP Data");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:"DRIVER_NAME);
