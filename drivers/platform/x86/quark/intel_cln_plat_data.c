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
#include <linux/crc32.h>
#include <linux/crc32c.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_data/clanton.h>
#include <linux/printk.h>
#include <linux/slab.h>

#define PREFIX		"CLN-PLT: "
#define PLAT_MAGIC	0x54414450	/* PDAT */
#define DESC_LEN	0x0A
#define MAC_STRLEN	20
#define MAC_LEN		6

struct cln_plat_dat_hdr {
	uint32_t magic;
	uint32_t length;
	uint32_t crc32;
};

struct cln_plat_data {
	uint16_t plat_id;
	uint16_t length;
	uint8_t desc[DESC_LEN];
	uint16_t version;
};

struct cln_bsp_reg {
	struct platform_device pdev;
	cln_plat_id_t id;
};

static struct cln_bsp_reg bsp_data [] = {
	{
		.pdev.name	= "cln-plat-clanton-peak",
		.pdev.id	= -1,
		.id		= CLANTON_PEAK,
	},
	{
		.pdev.name	= "cln-plat-kips-bay",
		.pdev.id	= -1,
		.id		= KIPS_BAY,
	},
	{
		.pdev.name	= "cln-plat-cross-hill",
		.pdev.id	= -1,
		.id		= CROSS_HILL,
	},
	{
		.pdev.name	= "cln-plat-clanton-hill",
		.pdev.id	= -1,
		.id		= CLANTON_HILL,
	},
	{
		.pdev.name	= "cln-plat-galileo",
		.pdev.id	= -1,
		.id		= IZMIR,
	},

};

/**
 * struct cln_plat_data_list
 *
 * Structure to hold a linked list of platform data refs
 */
struct cln_plat_data_list {
	char name[DESC_LEN+1];
	struct cln_plat_data * plat_data;
	struct kobj_attribute plat_attr;
	struct list_head list;
};

static char __iomem * plat_data;
static char * plat_bin_name = 	"pdat_bin";
static unsigned int plat_bin_size;
static struct cln_plat_dat_hdr * plat_hdr;
static struct list_head entry_list;

/**
 * intel_cln_plat_sysfs_show_bin
 *
 * Generic show routine for any of the sysfs entries of this module
 */
static ssize_t intel_cln_plat_sysfs_show_bin(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	ssize_t plen = plat_bin_size;
	if( plen > PAGE_SIZE )
		plen = PAGE_SIZE;

	memcpy(buf, plat_data, plen);
	return plen;
}

/**
 * intel_cln_plat_sysfs_show
 *
 * Generic show routine for any of the sysfs entries of this module
 */
static ssize_t intel_cln_plat_sysfs_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	unsigned char * data;
	char fmt[0x20];
	struct cln_plat_data_list * plat_item_list;
	ssize_t plen = 0;

	list_for_each_entry(plat_item_list, &entry_list, list){
		if ( attr == &plat_item_list->plat_attr ){

			/* Derive length */
			plen = plat_item_list->plat_data->length;
			if (unlikely(plen > PAGE_SIZE))
				plen = PAGE_SIZE;

			/* Hook data */
			data =(char*)(plat_item_list->plat_data);
			data += +sizeof(struct cln_plat_data);

			/* Enumrate return */
			switch (plat_item_list->plat_data->plat_id){
			case PLAT_DATA_ID:
			case PLAT_DATA_SN:
				snprintf(fmt, sizeof(fmt), "0x%%0%dx\n",
					 plen*2);
				return sprintf(buf, fmt, *(int16_t*)data);
			case PLAT_DATA_MAC0:
			case PLAT_DATA_MAC1:
				if (unlikely(plen != MAC_LEN)){
					return sprintf(buf, "invalid mac\n");
				}
				return snprintf(buf, MAC_STRLEN,
					"%02x:%02x:%02x:%02x:%02x:%02x\n",
					data[0], data[1], data[2], data[3],
					data[4], data[5]);
			default:
				/* Treat as string data */
				return snprintf(buf, plen, "%s", data);
			}
		}
	}
	return 0;
}

/**
 * intel_cln_plat_cleanup
 *
 * Generic cleanup code for the platform data interface
 *
 */
static void intel_cln_plat_cleanup (void)
{
	extern struct kobject * board_data_kobj;
	struct cln_plat_data_list * plat_item_list;

	if (plat_data != NULL){
		iounmap(plat_data);
		plat_data = NULL;
	}

	list_for_each_entry(plat_item_list, &entry_list, list){
		sysfs_remove_file(board_data_kobj,
				  &plat_item_list->plat_attr.attr);
		kfree(plat_item_list);
	}
}

/**
 * intel_cln_plat_get_desc_len
 *
 * @param desc: Pointer to desc string
 * @return len on success < 0 failure
 *
 * Function called to get a bounds checked desc field from platfrom data
 *
 */
static int intel_cln_plat_get_desc_len (char * desc)
{
	int len = 0;
	if (desc == NULL){
		return -EINVAL;
	}

	for(; *desc != '\0' && len < DESC_LEN; desc++, len++);
	return len;
}

/**
 * intel_cln_get_id
 *
 * @return platform id on success or < CLANTON_PLAT_UNDEFINED on error
 *
 * Function called to get platform id
 *
 */
cln_plat_id_t intel_cln_plat_get_id(void)
{
	unsigned char * data;
	struct cln_plat_data_list * plat_item_list;

	if (plat_data == NULL)
		return CLANTON_PLAT_UNDEFINED;

	list_for_each_entry(plat_item_list, &entry_list, list){

		/* Enumrate return */
		if(plat_item_list->plat_data->plat_id == PLAT_DATA_ID){

			/* Hook data */
			data =(char*)(plat_item_list->plat_data);
			data += +sizeof(struct cln_plat_data);

			/* Return payload */
			return *(int16_t*)data;
		}
	}
	return CLANTON_PLAT_UNDEFINED;
}
EXPORT_SYMBOL(intel_cln_plat_get_id);

/**
 * intel_cln_plat_get_mac
 *
 * @param id: Index of MAC address to find
 * @param mac: Output parameter for mac address
 *
 * @return 0 success < 0 failure
 *
 * Function called to remove the platfrom device from kernel space
 *
 */
int intel_cln_plat_get_mac(plat_dataid_t id, char * mac)
{
	unsigned char * data;
	unsigned int plen = 0;
	struct cln_plat_data_list * plat_item_list;

	if ((id != PLAT_DATA_MAC0 && id != PLAT_DATA_MAC1) || mac == NULL){
		pr_err("invalid input id %d mac %p\n", id, mac);
		return -EINVAL;
	}

	list_for_each_entry(plat_item_list, &entry_list, list){
		if(plat_item_list->plat_data->plat_id == id){

			/* Derive length */
			plen = plat_item_list->plat_data->length;
			if (unlikely(plen != MAC_LEN)){
				pr_err("%s mac len invalid!\n", __func__);
				return -ENODEV;
			}

			/* Hook data */
			data =(char*)(plat_item_list->plat_data);
			data += +sizeof(struct cln_plat_data);

			/* Good to go */
			memcpy(mac, data, MAC_LEN);
			return 0;
		}
	}
	return -ENODEV;
}
EXPORT_SYMBOL(intel_cln_plat_get_mac);


/**
 * intel_cln_plat_probe
 *
 * @param pdev: Pointer to platform device
 * @return 0 success < 0 failure
 *
 * Function called to probe platform device "cln-plat"
 *
 */
int intel_cln_plat_probe(struct resource * pres)
{
	char __iomem * end_addr = NULL;
	char __iomem * data = NULL;
	cln_plat_id_t id = CLANTON_PLAT_UNDEFINED;
	extern struct kobject * board_data_kobj;
	struct cln_plat_data * plat_item = NULL;
	struct cln_plat_data_list * plat_item_list = NULL;
	u32 crc = 0;
	int ret = 0, i = 0;

	INIT_LIST_HEAD(&entry_list);
	plat_hdr = ioremap(pres->start, resource_size(pres));
	end_addr = (char*)plat_hdr + resource_size(pres);
	plat_data = (char*)plat_hdr;
	if (!plat_hdr)
		return -ENODEV;

	/* Verify header magic */
	if (plat_hdr->magic != 	PLAT_MAGIC){
		pr_err(PREFIX"Expected magic 0x%08x read 0x%08lx\n",
			PLAT_MAGIC, (unsigned long)plat_hdr->magic);
	}

	/* Validate length is sane */
	if ((char*)plat_hdr + plat_hdr->length > end_addr ||
		plat_hdr->length < sizeof(struct cln_plat_data)){
		pr_err(PREFIX"Invalid length 0x%08lx\n",
			(unsigned long)plat_hdr->length);
		return -ENODEV;
	}

	/* Point to real end addr */
	end_addr = (char*)plat_hdr +
			sizeof(struct cln_plat_dat_hdr) + plat_hdr->length;
	plat_bin_size = end_addr - plat_data;

	/* Get pointer to start of data */
	plat_item = (struct cln_plat_data*)(plat_hdr+1);
	data = ((char*)(plat_item)+sizeof(struct cln_plat_data));

	/* Validate CRC32 */
	crc = ~crc32(0xFFFFFFFF, plat_item, plat_hdr->length);
	if (crc != plat_hdr->crc32){
		pr_err(PREFIX"CRC 0x%08x header indicates 0x%08x - fatal!\n",
			crc, plat_hdr->crc32);
		return -EFAULT;
	}

	/* /sys/firmware/board_data/plat_bin - dump entire platform binary */
	plat_item_list = kzalloc(sizeof(struct cln_plat_data_list),
					 GFP_KERNEL);
	if (unlikely(plat_item_list == NULL)) {
		pr_err("kzalloc fail !\n");
		intel_cln_plat_cleanup();
		return -ENOMEM;
	}
	sysfs_attr_init(&plat_item_list->plat_attr.attr);
	plat_item_list->plat_attr.attr.name = plat_bin_name;
	plat_item_list->plat_attr.attr.mode = 0644;
	plat_item_list->plat_attr.show = intel_cln_plat_sysfs_show_bin;

	ret = sysfs_create_file(board_data_kobj,
				&plat_item_list->plat_attr.attr);
	if (unlikely(ret != 0)){
		intel_cln_plat_cleanup();
		pr_err("failed to create sysfs entry\n");
		return ret;
	}

	/* Add to list */
	list_add(&plat_item_list->list, &entry_list);

	/* Iterate through each entry - add sysfs entry as appropriate */
	while ( (char*)plat_item < end_addr){
	
		/* Bounds check */
		if (data + plat_item->length > end_addr){
			pr_err(PREFIX"Data 0x%p over-runs max-addr 0x%p\n",
				data, end_addr);
			break;
		}

		/* Extract data */
		switch(plat_item->plat_id){
		case PLAT_DATA_ID:
			id = *((uint16_t*)data);
			pr_info(PREFIX"Clanton Platform ID = %d\n", id);
			break;
		case PLAT_DATA_SN:
		case PLAT_DATA_MAC0:
		case PLAT_DATA_MAC1:
			break;
		default:
			/* Unknown identifier */
			break;
		}

		plat_item_list = kzalloc(sizeof(struct cln_plat_data_list),
					 GFP_KERNEL);
		if (unlikely(plat_item_list == NULL)) {
			pr_err("kzalloc fail !\n");
			intel_cln_plat_cleanup();
			return -ENOMEM;
		}

		/* Get name of entity */
		i = intel_cln_plat_get_desc_len(plat_item->desc);
		if (i <= 0){
			pr_err("desc len is %d!\n", i);
			intel_cln_plat_cleanup();
			return i;
		}

		memcpy(plat_item_list->name, plat_item->desc, i);
		plat_item_list->plat_data = plat_item;

		sysfs_attr_init(&plat_item_list->plat_attr.attr);
		plat_item_list->plat_attr.attr.name = plat_item_list->name;
		plat_item_list->plat_attr.attr.mode = 0644;
		plat_item_list->plat_attr.show = intel_cln_plat_sysfs_show;

		ret = sysfs_create_file(board_data_kobj,
					&plat_item_list->plat_attr.attr);
		if (unlikely(ret != 0)){
			intel_cln_plat_cleanup();
			pr_err("failed to create sysfs entry\n");
			return ret;
		}

		/* Add to list */
		list_add(&plat_item_list->list, &entry_list);

		/* Next */
		plat_item = (struct cln_plat_data*)
			(((char*)plat_item) + plat_item->length + sizeof(struct cln_plat_data));
		data = ((char*)(plat_item) + sizeof(struct cln_plat_data));
	}

	/* Register BSP enabling platform code */
	for (i = 0; i < sizeof(bsp_data)/sizeof(struct cln_bsp_reg); i++){
		if (bsp_data[i].id == id){
			platform_device_register(&bsp_data[i].pdev);
		}
	}

	return ret;
}
EXPORT_SYMBOL(intel_cln_plat_probe);
