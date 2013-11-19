/*
 *  lpc_sch.c - LPC interface for Intel Poulsbo SCH
 *
 *  LPC bridge function of the Intel SCH contains many other
 *  functional units, such as Interrupt controllers, Timers,
 *  Power Management, System Management, GPIO, RTC, and LPC
 *  Configuration Registers.
 *
 *  Copyright (c) 2010 CompuLab Ltd
 *  Author: Denis Turischev <denis@compulab.co.il>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include <linux/mfd/core.h>

#define SMBASE		0x40
#define SMBUS_IO_SIZE	64

#define GPIOBASE	0x44
#define GPIO_IO_SIZE	64
#define GPIO_IO_SIZE_CENTERTON	128

#define WDTBASE		0x84
#define WDT_IO_SIZE	64

/* BIOS control reg */
#define LPC_BIOS_CNTL	0xD8
#define LPC_BIOS_CNTL_WE 0x01

/* Root complex base address derived registers */
#define RCBA_BASE	0xF0

static struct resource smbus_sch_resource = {
		.flags = IORESOURCE_IO,
};

static struct resource gpio_sch_resource = {
		.flags = IORESOURCE_IO,
};

static struct resource spi_res = {
	.flags		= IORESOURCE_MEM,
	.start		= 0,
	.end		= 0,
};

static struct platform_device lpc_sch_spi = {
	.name 		= "spi-lpc-sch",
	.id 		= -1,
	.resource	= &spi_res,
};

static struct resource wdt_sch_resource = {
		.flags = IORESOURCE_IO,
};

static struct mfd_cell lpc_sch_cells[3];

static struct mfd_cell isch_smbus_cell = {
	.name = "isch_smbus",
	.num_resources = 1,
	.resources = &smbus_sch_resource,
	.ignore_resource_conflicts = true,
};

static struct mfd_cell sch_gpio_cell = {
	.name = "sch_gpio",
	.num_resources = 1,
	.resources = &gpio_sch_resource,
	.ignore_resource_conflicts = true,
};

static struct mfd_cell wdt_sch_cell = {
	.name = "ie6xx_wdt",
	.num_resources = 1,
	.resources = &wdt_sch_resource,
	.ignore_resource_conflicts = true,
};

static const struct pci_device_id lpc_sch_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_SCH_LPC) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ITC_LPC) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_CENTERTON_ILB) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_CLANTON_ILB) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, lpc_sch_ids);

static int lpc_sch_probe(struct pci_dev *dev,
				const struct pci_device_id *id)
{
	unsigned int base_addr_cfg;
	unsigned short base_addr;
	u32 rcba_base, bios_cntl;
	int i, cells = 0;
	int ret;

	/* Clanton does not support iLB SMBUS */
	if (id->device != PCI_DEVICE_ID_INTEL_CLANTON_ILB) {
		pci_read_config_dword(dev, SMBASE, &base_addr_cfg);
		base_addr = 0;
		if (!(base_addr_cfg & (1 << 31)))
			dev_warn(&dev->dev, "Decode of the SMBus I/O range disabled\n");
		else
			base_addr = (unsigned short)base_addr_cfg;

		if (base_addr == 0) {
			dev_warn(&dev->dev, "I/O space for SMBus uninitialized\n");
		} else {
			lpc_sch_cells[cells++] = isch_smbus_cell;
			smbus_sch_resource.start = base_addr;
			smbus_sch_resource.end = base_addr + SMBUS_IO_SIZE - 1;
		}
	}

	pci_read_config_dword(dev, GPIOBASE, &base_addr_cfg);
	base_addr = 0;
	if (!(base_addr_cfg & (1 << 31)))
		dev_warn(&dev->dev, "Decode of the GPIO I/O range disabled\n");
	else
		base_addr = (unsigned short)base_addr_cfg;

	if (base_addr == 0) {
		dev_warn(&dev->dev, "I/O space for GPIO uninitialized\n");
	} else {
		lpc_sch_cells[cells++] = sch_gpio_cell;
		gpio_sch_resource.start = base_addr;
		if (id->device == PCI_DEVICE_ID_INTEL_CENTERTON_ILB)
			gpio_sch_resource.end = base_addr + GPIO_IO_SIZE_CENTERTON - 1;
		else
			gpio_sch_resource.end = base_addr + GPIO_IO_SIZE - 1;
	}

	/* Add RCBA SPI device */
	if (id->device == PCI_DEVICE_ID_INTEL_CLANTON_ILB) {
		pci_read_config_dword(dev, LPC_BIOS_CNTL, &bios_cntl);
		pr_info("%s BIOS_CNTL 0x%08x\n", __func__, bios_cntl);

		/* Enable flash write */
		bios_cntl |= LPC_BIOS_CNTL_WE;
		pci_write_config_dword(dev, LPC_BIOS_CNTL, bios_cntl);

		/* Verify */
		pci_read_config_dword(dev, LPC_BIOS_CNTL, &bios_cntl);
		pr_info("%s new BIOS_CNTL 0x%08x\n", __func__, bios_cntl);
	}

	pci_read_config_dword(dev, RCBA_BASE, &rcba_base);
	rcba_base &= 0xFFFFC000;
	spi_res.start = rcba_base + 0x3020;
	spi_res.end = rcba_base + 0x3088;
	pr_info("%s RCBA @ 0x%08x\n", __func__, rcba_base);
	ret = platform_device_register(&lpc_sch_spi);
	if (ret < 0){
		pr_err("unable to register %s plat dev\n", lpc_sch_spi.name);
		return ret;
	}

	if (id->device == PCI_DEVICE_ID_INTEL_ITC_LPC
	    || id->device == PCI_DEVICE_ID_INTEL_CENTERTON_ILB) {
		pci_read_config_dword(dev, WDTBASE, &base_addr_cfg);
		base_addr = 0;
		if (!(base_addr_cfg & (1 << 31)))
			dev_warn(&dev->dev, "Decode of the WDT I/O range disabled\n");
		else
			base_addr = (unsigned short)base_addr_cfg;
		if (base_addr == 0)
			dev_warn(&dev->dev, "I/O space for WDT uninitialized\n");
		else {
			lpc_sch_cells[cells++] = wdt_sch_cell;
			wdt_sch_resource.start = base_addr;
			wdt_sch_resource.end = base_addr + WDT_IO_SIZE - 1;
		}
	}

	if (WARN_ON(cells > ARRAY_SIZE(lpc_sch_cells))) {
		dev_err(&dev->dev, "Cell count exceeds array size");
		return -ENODEV;
	}

	if (cells == 0) {
		dev_err(&dev->dev, "All decode registers disabled.\n");
		return -ENODEV;
	}

	for (i = 0; i < cells; i++)
		lpc_sch_cells[i].id = id->device;

	ret = mfd_add_devices(&dev->dev, 0, lpc_sch_cells, cells, NULL, 0, NULL);
	if (ret)
		mfd_remove_devices(&dev->dev);

	return ret;
}

static void lpc_sch_remove(struct pci_dev *dev)
{
	mfd_remove_devices(&dev->dev);
}

static struct pci_driver lpc_sch_driver = {
	.name		= "lpc_sch",
	.id_table	= lpc_sch_ids,
	.probe		= lpc_sch_probe,
	.remove		= lpc_sch_remove,
};

module_pci_driver(lpc_sch_driver);

MODULE_AUTHOR("Denis Turischev <denis@compulab.co.il>");
MODULE_DESCRIPTION("LPC interface for Intel Poulsbo SCH");
MODULE_LICENSE("GPL");
