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
 * Intel Clanton IMR driver
 *
 * IMR stand for Insolate Memory Region, supported by Clanton SoC.
 *
 * The IMR id 3 is pre-defined as the use for kernel data protection
 *
 * The early imr protects entire memory (from the beginning of kernel text
 * section to the top of memory) during linux boot time. In the linux run
 * time, the protection need to resize down to memory region that only
 * contains: kernel text, read only data, and initialized data section.
 *
 */
#include <linux/errno.h>
#include <linux/intel_cln_sb.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_data/clanton.h>
#include <linux/printk.h>
#include "intel_cln_imr.h"

/* pre-defined imr id for uncompressed kernel */
#define IMR_KERNEL_ID	3

/**
 * addr_hw_ready
 *
 * shift input address value to match HW required 1k aligned format
 */
static inline uint32_t addr_hw_ready(uint32_t addr)
{
	/* memory alignment */
	addr &= (~((1 << 10) - 1));

	/* prepare input addr in HW required format */
	addr = (addr >> 8) & IMR_REG_MASK;
	return addr;
}

/**
 * void intel_cln_imr_runt_kerndata_setup
 *
 * Setup imr for kernel text, read only data section
 *
 * The read only data (rodata) section placed between text and initialized data
 * section by kernel.
 */
static void intel_cln_imr_runt_kerndata_setup(void)
{
	uint32_t hi;
	uint32_t lo;

	hi = (uint32_t)virt_to_phys(&__init_begin);
	lo = (uint32_t)virt_to_phys(&_text);

	/* Set a locked IMR around the kernel .text section */
	if (intel_cln_imr_alloc((hi - IMR_MEM_ALIGN), lo,
				IMR_DEFAULT_MASK, IMR_DEFAULT_MASK,
				"KERNEL RUNTIME DATA", 1)) {
		pr_err("IMR: Set up runtime kernel data imr faild!\n");
		return;
	}
}

/**
 * intel_cln_imr_teardown_unlocked
 *
 * Remove any unlocked IMR
 *
 */
static void intel_cln_imr_teardown_unlocked(void)
{
	int i = 0;
	for (i = 0; i < IMR_MAXID; i++)
		intel_cln_remove_imr_entry(i);
}

/**
 * intel_cln_imr_runt_setparams
 *
 * set imr range for text, read only, initialised data in linux run time
 */
int intel_cln_imr_runt_setparams(void)
{
	/* Setup an IMR around the kernel .text area */
	intel_cln_imr_runt_kerndata_setup();

	/* Remove any other unlocked IMR */
	intel_cln_imr_teardown_unlocked();

	return 0;
}
EXPORT_SYMBOL(intel_cln_imr_runt_setparams);

/**
 * intel_cln_imr_runt_init
 *
 * module entry point
 */
static int  __init intel_cln_imr_runt_init(void)
{
	return 0;
}

/**
 * intel_cln_imr_runt_exit
 *
 * Module exit
 */
static void __exit intel_cln_imr_runt_exit(void)
{
	/* do nothing */
}

MODULE_DESCRIPTION("Intel Clanton SOC IMR API ");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("Dual BSD/GPL");

subsys_initcall(intel_cln_imr_runt_init);
module_exit(intel_cln_imr_runt_exit);
