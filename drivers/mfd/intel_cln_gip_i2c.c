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
 * Intel Clanton GIP (GPIO/I2C) - I2C-specific PCI driver
 *
 *  PCI glue logic for Clanton GIP/I2C.
 *  The GIP I2C device is the DesignWare I2C. This file defines the PCI glue
 *  for this driver and is heavily based on
 *  on drivers/i2c/busses/i2c-designware-pcidrv.c.  Also, it relies on
 *  drivers/i2c/busses/i2c-designware-core.c for the core logic.
 *  Please note only a single instance of the I2C device is supported.
 */

#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include "intel_cln_gip.h"

enum dw_pci_ctl_id_t {
	clanton_0,
};

static unsigned int i2c_std_mode;
module_param(i2c_std_mode, uint, S_IRUSR);
MODULE_PARM_DESC(i2c_std_mode, "Force I2C standard mode");

#define INTEL_CLN_STD_CFG  (DW_IC_CON_MASTER |			\
				DW_IC_CON_SLAVE_DISABLE |	\
				DW_IC_CON_RESTART_EN)

static struct dw_pci_controller cln_gip_i2c_controller = {
	.bus_num	= 0,
	.bus_cfg	= INTEL_CLN_STD_CFG | DW_IC_CON_SPEED_FAST,
	.tx_fifo_depth	= 16,
	.rx_fifo_depth	= 16,
	.clk_khz	=
#ifdef CONFIG_INTEL_QUARK_X1000_SOC_FPGAEMU
			14000,
#else
			33000,
#endif
	.explicit_stop	= 1,
};

static struct i2c_algorithm i2c_dw_algo = {
	.master_xfer	= i2c_dw_xfer,
	.functionality	= i2c_dw_func,
};

/**
 * i2c_dw_get_clk_rate_khz
 * @param dev: Pointer to I2C device private data
 * @return clock rate in kHz
 *
 * Ancillary function returning the frequency of the clock supplied to the
 * interface.
 */
static u32 i2c_dw_get_clk_rate_khz(struct dw_i2c_dev *dev)
{
	return dev->controller->clk_khz;
}

/**
 * intel_cln_i2c_probe
 * @param pdev: Pointer to GIP PCI device
 * @param drvdata: private driver data
 * @return 0 success < 0 failure
 *
 * Perform I2C-specific probing on behalf of the top-level GIP driver.
 * Also call into I2C core driver routines for initiating the device.
 */
int intel_cln_i2c_probe(struct pci_dev *pdev,
			struct dw_i2c_dev **drvdata)
{
	int retval = 0;
	resource_size_t start = 0, len = 0;
	struct dw_i2c_dev *dev = NULL;
	struct i2c_adapter *adap = NULL;
	void __iomem *reg_base = NULL;
	struct dw_pci_controller *controller = NULL;

	controller = &cln_gip_i2c_controller;

	/* Clanton default configuration is fast mode, unless otherwise asked */
	if (i2c_std_mode)
		controller->bus_cfg = INTEL_CLN_STD_CFG | DW_IC_CON_SPEED_STD;

	/* Determine the address of the I2C area */
	start = pci_resource_start(pdev, GIP_I2C_BAR);
	len = pci_resource_len(pdev, GIP_I2C_BAR);
	if (!start || len == 0) {
		dev_err(&pdev->dev, "bar%d not set\n", GIP_I2C_BAR);
		retval = -ENODEV;
		goto err;
	}

	reg_base = ioremap_nocache(start, len);
	if (!reg_base) {
		dev_err(&pdev->dev, "I/O memory remapping failed\n");
		retval = -ENOMEM;
		goto err;
	}

	dev = kzalloc(sizeof(struct dw_i2c_dev), GFP_KERNEL);
	if (!dev) {
		retval = -ENOMEM;
		goto err_iounmap;
	}

	init_completion(&dev->cmd_complete);
	mutex_init(&dev->lock);
	dev->clk = NULL;
	dev->controller = controller;
	dev->get_clk_rate_khz = i2c_dw_get_clk_rate_khz;
	dev->base = reg_base;
	dev->dev = get_device(&pdev->dev);
	dev->functionality =
		I2C_FUNC_I2C |
		I2C_FUNC_10BIT_ADDR |
		I2C_FUNC_SMBUS_BYTE |
		I2C_FUNC_SMBUS_BYTE_DATA |
		I2C_FUNC_SMBUS_WORD_DATA |
		I2C_FUNC_SMBUS_I2C_BLOCK;
	dev->master_cfg =  controller->bus_cfg;

	*drvdata = dev;

	dev->tx_fifo_depth = controller->tx_fifo_depth;
	dev->rx_fifo_depth = controller->rx_fifo_depth;
	dev->explicit_stop = controller->explicit_stop;
	retval = i2c_dw_init(dev);
	if (retval)
		goto err_release_drvdata;

	adap = &dev->adapter;
	i2c_set_adapdata(adap, dev);
	adap->owner = THIS_MODULE;
	adap->class = 0;
	adap->algo = &i2c_dw_algo;
	adap->dev.parent = &pdev->dev;
	adap->nr = controller->bus_num;
	snprintf(adap->name, sizeof(adap->name), "intel_cln_gip_i2c");

	i2c_dw_disable_int(dev);
	i2c_dw_clear_int(dev);
	retval = i2c_add_numbered_adapter(adap);
	if (retval) {
		dev_err(&pdev->dev, "failure adding I2C adapter\n");
		goto err_release_drvdata;
	}

	return 0;

err_release_drvdata:
	put_device(&pdev->dev);
	kfree(dev);
err_iounmap:
	iounmap(reg_base);
err:
	return retval;
}

/**
 * intel_cln_i2c_remove
 * @param pdev: Pointer to GIP PCI device
 * @param dev: Pointer to I2C private data 
 *
 * Perform I2C-specific resource release on behalf of the top-level GIP driver.
 */
void intel_cln_i2c_remove(struct pci_dev *pdev,
	struct dw_i2c_dev *dev)
{

	if (NULL == dev) {
		dev_err(&pdev->dev, "%s: failure getting driver data\n",
			__func__);
		return;
	}

	i2c_dw_disable(dev);
	i2c_del_adapter(&dev->adapter);
	iounmap(dev->base);

	kfree(dev);
}
