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
 * Intel Clanton Legacy Platform Data accessor layer
 *
 * Simple Legacy SPI flash access layer
 *
 * Author : Bryan O'Donoghue <bryan.odonoghue@linux.intel.com> 2013
 */

#include <asm/io.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>

#define DRIVER_NAME				"board_data"
#define PFX					"MFH: "
#define SPIFLASH_BASEADDR			0xFFF00000
#define MFH_OFFSET				0x00008000
#define PLATFORM_DATA_OFFSET			0x00010000
#define MTD_PART_OFFSET				0x00050000
#define MTD_PART_LEN				0x00040000
#define MFH_PADDING				0x1E8
#define MFH_MAGIC				0x5F4D4648
#define FLASH_SIZE				0x00400000

/* MFH types supported @ version #1 */
#define MFH_ITEM_FW_STAGE1			0x00000000
#define MFH_ITEM_FW_STAGE1_SIGNED		0x00000001
#define MFH_ITEM_FW_STAGE2			0x00000003
#define MFH_ITEM_FW_STAGE2_SIGNED		0x00000004
#define MFH_ITEM_FW_STAGE2_CONFIG		0x00000005
#define MFH_ITEM_FW_STAGE2_CONFIG_SIGNED	0x00000006
#define MFH_ITEM_FW_PARAMS			0x00000007
#define MFH_ITEM_FW_RECOVERY			0x00000008
#define MFH_ITEM_FW_RECOVERY_SIGNED		0x00000009
#define MFH_ITEM_BOOTLOADER			0x0000000B
#define MFH_ITEM_BOOTLOADER_SIGNED		0x0000000C
#define MFH_ITEM_BOOTLOADER_CONFIG		0x0000000D
#define MFH_ITEM_BOOTLOADER_CONFIG_SIGNED	0x0000000E
#define MFH_ITEM_KERNEL				0x00000010
#define MFH_ITEM_KERNEL_SIGNED			0x00000011
#define MFH_ITEM_RAMDISK			0x00000012
#define MFH_ITEM_RAMDISK_SIGNED			0x00000013
#define MFH_ITEM_LOADABLE_PROGRAM		0x00000015
#define MFH_ITEM_LOADABLE_PROGRAM_SIGNED	0x00000016
#define MFH_ITEM_BUILD_INFO			0x00000018
#define MFH_ITEM_VERSION			0x00000019

struct intel_cln_mfh {
	u32	id;
	u32	ver;
	u32	flags;
	u32	next_block;
	u32	item_count;
	u32	boot_priority_list;
	u8	padding[MFH_PADDING];
};

struct intel_cln_mfh_item {
	u32	type;
	u32	addr;
	u32	len;
	u32	res0;
};

static struct resource conf_res __initdata = {
	.flags		= IORESOURCE_MEM,
	.start		= 0,
	.end		= 0,
};

static struct resource plat_res __initdata = {
	.flags		= IORESOURCE_MEM,
	.start		= 0,
	.end		= 0,
};

static struct platform_device conf_pdev = {
	.name		= "cln-layout-conf",
	.id		= -1,
	.resource	= &conf_res,
};

struct kobject * board_data_kobj;
EXPORT_SYMBOL_GPL(board_data_kobj);

static bool mfh_plat_found = false;

static long unsigned int flash_version_data;
static ssize_t flash_version_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
        return snprintf(buf, 12, "%#010lx\n", flash_version_data);
}

static struct kobj_attribute flash_version_attr =
        __ATTR(flash_version, 0644, flash_version_show, NULL);

extern int intel_cln_plat_probe(struct resource * pres);

/**
 * intel_cln_board_data_init
 *
 * Module entry point
 */
static int __init intel_cln_board_data_init(void)
{
	extern struct kobject * firmware_kobj;
	struct intel_cln_mfh __iomem * mfh;
	struct intel_cln_mfh_item __iomem * item;
	struct platform_device * pdev;
	u32 count;
	void __iomem * spi_data;
	int ret = 0;

	spi_data = ioremap(SPIFLASH_BASEADDR, FLASH_SIZE);
	if (!spi_data)
		return -ENODEV;

	/* get mfh and first item pointer */	
	mfh = spi_data + MFH_OFFSET;
	if (mfh->id != MFH_MAGIC){
		pr_err(PFX"Bad MFH magic want 0x%08x found 0x%08x @ 0x%p\n",
		MFH_MAGIC, mfh->id, &mfh->id);
		return -ENODEV;
	}

	pr_info(PFX"mfh @ 0x%p: id 0x%08lx ver 0x%08lx entries 0x%08lx\n",
		mfh, (unsigned long)mfh->id, (unsigned long)mfh->ver,
		(unsigned long)mfh->item_count);
	item = (struct intel_cln_mfh_item __iomem *)
		&mfh->padding [sizeof(u32) * mfh->boot_priority_list];
	
	/* board_data_kobj subordinate of firmware @ /sys/firmware/board_data */
	board_data_kobj = kobject_create_and_add("board_data", firmware_kobj);
	if (!board_data_kobj) {
		pr_err(PFX"kset create error\n");
		return -ENODEV;
	}

	/* Register flash regions as seperate platform devices */
	for (count = 0; count < mfh->item_count; count++, item++){
		pdev = NULL;

		switch (item->type){
		case MFH_ITEM_BUILD_INFO:
			conf_res.start = item->addr;
			conf_res.end = item->addr + item->len;
			pdev = &conf_pdev;
			break;
		case MFH_ITEM_VERSION:
			flash_version_data = item->res0;
			if(sysfs_create_file(board_data_kobj, 
					&flash_version_attr.attr)) {
                		pr_err("failed to create sysfs entry for flash version\n");
				flash_version_data = 0;
		        }
			break;
		default:
			break;
		}

		if (pdev != NULL)
			platform_device_register(pdev);
	}

	/* This ought to be encoded in the MFH ! */	
	if (mfh_plat_found == false){
		pr_err(PFX"Warning platform data MFH missing - using hardcoded "
			"offsets\n");

		/* Platform data */
		plat_res.start = SPIFLASH_BASEADDR + PLATFORM_DATA_OFFSET;
		count = *(uint32_t*)(spi_data + PLATFORM_DATA_OFFSET + sizeof(uint32_t));
		plat_res.end = count;
		ret = intel_cln_plat_probe(&plat_res);
	}

	iounmap(spi_data);
	return ret;
}

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("Intel Clanton SPI Data API");
MODULE_LICENSE("Dual BSD/GPL");
subsys_initcall(intel_cln_board_data_init);

