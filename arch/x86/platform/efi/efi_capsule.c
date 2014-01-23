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

#include <asm/cln.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/efi.h>

#define DRIVER_NAME	"efi_capsule_update"
#define PFX		"efi-capsupdate: "
#define MAX_PATH	256
#define MAX_CHUNK	PAGE_SIZE
#define CSH_HDR_SIZE	0x400

typedef struct {
	u64 length;
	union {
		u64 data_block;
		u64 continuation_pointer;
	};
} efi_blk_desc_t;

static struct kobject * efi_capsule_kobj;
static struct list_head sg_list;
static char fpath[MAX_PATH];
static int csh_jump = CSH_HDR_SIZE;		/* Clanton EDK wants CSH jump */

/**
 * efi_capsule_trigger_update
 *
 * Trigger the EFI capsule update
 */
static int efi_capsule_trigger_update(void)
{
	struct file *fp = NULL;
	mm_segment_t old_fs = get_fs();
	int ret = 0;
	u32 nblocks = 0, i = 0, total_size = 0, data_len = 0, offset = 0;
	efi_capsule_header_t *chdr = NULL;
	efi_blk_desc_t * desc_block = NULL;
	u8 ** data = NULL;

	set_fs (KERNEL_DS);
	fp = filp_open(fpath, O_RDONLY, 0);

	/* Sanity check input */
	if (IS_ERR(fp)|| fp->f_op == NULL ||fp->f_op->read == NULL ||
		fp->f_dentry->d_inode->i_size == 0){
		pr_err(PFX"file open [%s] error!\n", fpath);
		ret = -EINVAL;
		goto done;
	}

	/* Determine necessary sizes */
	nblocks = (fp->f_dentry->d_inode->i_size/MAX_CHUNK) + 2;
	total_size = fp->f_dentry->d_inode->i_size;

	pr_info(PFX "nblocks %d total_size %d\n", nblocks, total_size);

	/* Allocate array of descriptor blocks + 1 for terminator */
	desc_block = (efi_blk_desc_t*)kzalloc(nblocks * sizeof(efi_blk_desc_t), GFP_KERNEL);
	if (desc_block == NULL){
		pr_info(PFX"%s failed to allocate %d blocks\n", __func__, nblocks);
		ret = -ENOMEM;
		goto done_close;
	}

	pr_info(PFX"File %s size %u descriptor blocks %u\n",
		fpath, total_size, nblocks);

	data = kmalloc(nblocks, GFP_KERNEL);
	if (data == NULL){
		ret = -ENOMEM;
		pr_info("Failed to allocate %d bytes\n", nblocks);
		goto done;
	}

	for (i = 0; i < nblocks; i++){
		data[i] = kmalloc(MAX_CHUNK, GFP_KERNEL);
		if (data[i] == NULL){
			ret = -ENOMEM;
			pr_info("Alloc fail %d bytes entry %d\n",
				nblocks, i);
			goto done;
		}
			
	}

	/* Read in data */
	for (i = 0; i < nblocks && offset < total_size; i++){
		/* Determine read len */
		data_len = offset < total_size - MAX_CHUNK ?
				MAX_CHUNK : total_size - offset;
		ret = fp->f_op->read(fp, data[i], data_len, &fp->f_pos);
		if (ret < 0){
			pr_err(PFX"Error reading @ data %u\n", offset);
			ret = -EIO;
			goto done;
		} 
		offset += data_len;

		/* Sanity check */
		if (i >= nblocks){
			pr_err(PFX"%s Driver bug line %d\n", __func__, __LINE__);
			ret = -EINVAL;
			goto done;
		}

		/* Validate header as appropriate */
		if (chdr == NULL){
			chdr = (efi_capsule_header_t*)&data[i][csh_jump];
			desc_block[i].data_block = __pa(&data[i][csh_jump]);
			desc_block[i].length = data_len - csh_jump;
			pr_info(PFX"hdr offset in file %d bytes\n", csh_jump);
			pr_info(PFX"hdr size %u flags 0x%08x imagesize 0x%08x\n",
				chdr->headersize, chdr->flags, chdr->imagesize);

		}else{
			desc_block[i].data_block = __pa(&data[i][0]);
			desc_block[i].length = data_len;
		}
		
		pr_info(PFX "block %d length %u data @ phys 0x%08x\n",
			i, (int)desc_block[i].length,
			(unsigned int)desc_block[i].data_block);
	}

	if (i > nblocks-1){
		pr_err(PFX"%s Used block %d expected %d !\n", __func__, i, nblocks-1);
		ret = -EINVAL;
		goto done;
	}

	pr_info(PFX"submitting capsule to EDKII firmware\n");
 
	ret = efi.update_capsule(&chdr, 1, __pa(desc_block));
	if(ret != EFI_SUCCESS) {
		pr_err(PFX"submission fail err=0x%08x\n", ret);
	}else{
		pr_info(PFX"submission success\n");
		ret = 0;
	}

	if (chdr != NULL && chdr->flags & 0x10000){
		pr_info(PFX"capsule persist across S3 skipping capsule free\n");
		goto done_close;
	}
done:
	for (i = 0; i < nblocks; i++){
		if (data && data[i])
			kfree(data[i]);
	}
	if (data)
		kfree(data);

	if (desc_block != NULL)
		kfree(desc_block);
done_close:
	if (!IS_ERR(fp))
		filp_close(fp, NULL);

	set_fs (old_fs);
	return ret;
}

/**
 * efi_capsule_csh_jump
 *
 * sysfs callback used to show current path
 */
static ssize_t efi_capsule_csh_jump_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
        return snprintf(buf, sizeof(fpath), "%d\n", csh_jump > 0);
}

/**
 * efi_capsule_path_store
 *
 * sysfs callback used to set a new capsule path
 */
static ssize_t efi_capsule_csh_jump_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	if (buf != NULL && buf[0] == '0')
		csh_jump = 0;
	else
		csh_jump = CSH_HDR_SIZE;
	return count;
}

static struct kobj_attribute efi_capsule_csh_jump_attr =
        __ATTR(csh_jump, 0644, efi_capsule_csh_jump_show, efi_capsule_csh_jump_store);

/**
 * efi_capsule_path_show
 *
 * sysfs callback used to show current path
 */
static ssize_t efi_capsule_path_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
        return snprintf(buf, sizeof(fpath), fpath);
}

/**
 * efi_capsule_path_store
 *
 * sysfs callback used to set a new capsule path
 */
static ssize_t efi_capsule_path_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	if (count > MAX_PATH-1)
		return -EINVAL;

	memset(fpath, 0x00, sizeof(fpath));
	memcpy(fpath, buf, count);

	return count;
}

static struct kobj_attribute efi_capsule_path_attr =
        __ATTR(capsule_path, 0644, efi_capsule_path_show, efi_capsule_path_store);

/**
 * efi_capsule_update_store
 *
 * sysfs callback used to initiate update
 */
static ssize_t efi_capsule_update_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{	int ret = 0;

	ret = efi_capsule_trigger_update();
	return ret == 0 ? count : ret;
}

static struct kobj_attribute efi_capsule_update_attr =
        __ATTR(capsule_update, 0644, NULL, efi_capsule_update_store);

#define SYSFS_ERRTXT "Error adding sysfs entry!\n"
/**
 * intel_cln_capsule_update_init
 *
 * @return 0 success < 0 failure
 *
 * Module entry point
 */
static int __init efi_capsule_update_init(void)
{
	int retval = 0;
	extern struct kobject * firmware_kobj;

	INIT_LIST_HEAD(&sg_list);

	/* efi_capsule_kobj subordinate of firmware @ /sys/firmware/efi */
	efi_capsule_kobj = kobject_create_and_add("efi", firmware_kobj);
	if (!efi_capsule_kobj) {
		pr_err(PFX"kset create error\n");
		retval = -ENODEV;
		goto err;
	}

	if(sysfs_create_file(efi_capsule_kobj, &efi_capsule_path_attr.attr)) {
		pr_err(PFX SYSFS_ERRTXT);
		retval = -ENODEV;
		goto err;
	}
	if(sysfs_create_file(efi_capsule_kobj, &efi_capsule_update_attr.attr)) {
		pr_err(PFX SYSFS_ERRTXT);
		retval = -ENODEV;
		goto err;

	}
	if(sysfs_create_file(efi_capsule_kobj, &efi_capsule_csh_jump_attr.attr)) {
		pr_err(PFX SYSFS_ERRTXT);
		retval = -ENODEV;
		goto err;

	}

err:
	return retval;
}

/**
 * intel_cln_esram_exit
 *
 * Module exit
 */
static void __exit efi_capsule_update_exit(void)
{
}

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("EFI Capsule Update driver");
MODULE_LICENSE("Dual BSD/GPL");

module_init(efi_capsule_update_init);
module_exit(efi_capsule_update_exit);
