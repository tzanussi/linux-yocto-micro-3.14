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
 * Intel Clanton platform data definition
 */

#ifndef _PDATA_CLANTON_H
#define _PDATA_CLANTON_H

typedef enum  {
	CLANTON_PLAT_UNDEFINED = 0,
	CLANTON_EMULATION = 1,
	CLANTON_PEAK = 2,
	KIPS_BAY = 3,
	CROSS_HILL = 4,
	CLANTON_HILL = 5,
	IZMIR = 6,
}cln_plat_id_t;

typedef enum {
	PLAT_DATA_ID = 1,
	PLAT_DATA_SN = 2,
	PLAT_DATA_MAC0 = 3,
	PLAT_DATA_MAC1 = 4,
}plat_dataid_t;


#ifdef CONFIG_INTEL_QUARK_X1000_SOC
extern cln_plat_id_t intel_cln_plat_get_id(void);
extern int intel_cln_plat_get_mac(plat_dataid_t id, char * mac);
#else
static inline cln_plat_id_t intel_cln_plat_get_id(void)
{
	return CLANTON_PLAT_UNDEFINED;
}
static int intel_cln_plat_get_mac(plat_dataid_t id, char * mac)
{
	return -ENODEV;
}
#endif
#endif /* _PDATA_CLANTON_H */
