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
 * Intel Clanton GIP (GPIO/I2C) driver
 */

#ifndef __INTEL_CLNGIP_H__
#define __INTEL_CLNGIP_H__

#include <linux/i2c.h>
#include <linux/pci.h>
#include "../i2c/busses/i2c-designware-core.h"

/* PCI BAR for register base address */
#define GIP_I2C_BAR		0
#define GIP_GPIO_BAR		1

/**
 * intel_cln_gpio_probe
 *
 * @param pdev: Pointer to GIP PCI device
 * @return 0 success < 0 failure
 *
 * Perform GPIO-specific probing on behalf of the top-level GIP driver.
 */
int intel_cln_gpio_probe(struct pci_dev *pdev);

/**
 * intel_cln_gpio_remove
 *
 * @param pdev: Pointer to GIP PCI device
 *
 * Perform GPIO-specific resource release on behalf of the top-level GIP driver.
 */
void intel_cln_gpio_remove(struct pci_dev *pdev);

/**
 * intel_cln_gpio_isr
 *
 * @param irq: IRQ number to be served
 * @param dev_id: used to distinguish the device (for shared interrupts)
 *
 * Perform GPIO-specific ISR of the top-level GIP driver.
 */
irqreturn_t intel_cln_gpio_isr(int irq, void *dev_id);

/**
 * intel_cln_gpio_save_state
 *
 * Save GPIO register state for system-wide suspend events and mask out
 * interrupts.
 */
void intel_cln_gpio_save_state(void);

/**
 * intel_cln_gpio_restore_state
 *
 * Restore GPIO register state for system-wide resume events and clear out
 * spurious interrupts.
 */
void intel_cln_gpio_restore_state(void);

/**
 * intel_cln_i2c_probe
 * @param pdev: Pointer to GIP PCI device
 * @param drvdata: private driver data
 * @return 0 success < 0 failure
 *
 * Perform I2C-specific probing on behalf of the top-level GIP driver.
 */
int intel_cln_i2c_probe(struct pci_dev *pdev, 
			struct dw_i2c_dev **drvdata);

/**
 * intel_cln_i2c_remove
 * @param pdev: Pointer to GIP PCI device
 * @param dev: Pointer to I2C private data 
 *
 * Perform I2C-specific resource release on behalf of the top-level GIP driver.
 */
void intel_cln_i2c_remove(struct pci_dev *pdev,
	struct dw_i2c_dev *dev);

#endif /* __INTEL_CLNGIP_H__ */
