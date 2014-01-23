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

#include <asm/io.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>

#define DRIVER_NAME "cln-layout-conf"
static char __iomem * layout_conf_data;
static int len;

static ssize_t layout_conf_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	ssize_t plen = len+1;
	if( plen > PAGE_SIZE )
		plen = PAGE_SIZE;
	memcpy(buf, layout_conf_data, plen);
	return plen;
}

static struct kobj_attribute layout_conf_attr = 
	__ATTR(layout_conf, 0644, layout_conf_show, NULL);

static int intel_cln_layout_data_probe(struct platform_device *pdev)
{
	extern struct kobject * board_data_kobj;
	int ret = 0;

	layout_conf_data = ioremap(pdev->resource->start,
		resource_size(pdev->resource));
	if (!layout_conf_data)
		return -ENODEV;

	len = resource_size(pdev->resource);
	ret = sysfs_create_file(board_data_kobj, &layout_conf_attr.attr);
	if (ret != 0){
		pr_err("failed to create sysfs entry for layout config\n");
		iounmap(layout_conf_data);
		layout_conf_data = NULL;
	}

	return ret;
}

static int intel_cln_layout_data_remove(struct platform_device *pdev)
{
	extern struct kobject * board_data_kobj;

	if (layout_conf_data){
		sysfs_remove_file(board_data_kobj, &layout_conf_attr.attr);
		iounmap(layout_conf_data);
		
	}
	return 0;
}

static struct platform_driver cln_layout_data_driver = {
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
	.probe		= intel_cln_layout_data_probe,
	.remove		= intel_cln_layout_data_remove,
};

module_platform_driver(cln_layout_data_driver);

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("Intel Clanton SPI Data API");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:"DRIVER_NAME);

