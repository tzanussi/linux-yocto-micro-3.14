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
#ifndef _ASM_X86_CLN_H
#define _ASM_X86_CLN_H

#include <linux/pci.h>
#include <linux/msi.h>

/**
 * cln_pci_pvm_mask
 *
 * Mask PVM bit on a per function basis. Clanton SC components have but one
 * vector each - so we mask for what we need
 */
static inline void cln_pci_pvm_mask(struct pci_dev * dev)
{
	struct msi_desc *entry;
	int mask_bits = 1;

	if(unlikely(dev->msi_enabled == 0))
		return;

	entry = list_first_entry(&dev->msi_list, struct msi_desc, list);

	if(unlikely(entry == NULL))
		return;

	pci_write_config_dword(dev, entry->mask_pos, mask_bits);
}

/**
 * cln_pci_pvm_mask
 *
 * UnMask PVM bit on a per function basis. Clanton SC components have but one
 * vector each - so we unmask for what we need
 */
static inline void cln_pci_pvm_unmask(struct pci_dev * dev)
{
	struct msi_desc *entry;
	int mask_bits = 0;

	if(unlikely(dev->msi_enabled == 0))
		return;

	entry = list_first_entry(&dev->msi_list, struct msi_desc, list);

	if(unlikely(entry == NULL))
		return;

	pci_write_config_dword(dev, entry->mask_pos, mask_bits);
}

/* Convienence macros */
#if defined(CONFIG_INTEL_QUARK_X1000_SOC)
       #define mask_pvm(x) cln_pci_pvm_mask(x)
       #define unmask_pvm(x) cln_pci_pvm_unmask(x) 
#else
       #define mask_pvm(x)
       #define unmask_pvm(x)
#endif

/* Serial */
#if defined(CONFIG_INTEL_QUARK_X1000_SOC)
	#define SERIAL_PORT_DFNS
	#define BASE_BAUD 2764800
#endif

#endif /* _ASM_X86_CLN_H */
