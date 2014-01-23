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

#ifndef __LINUX_PLATFORM_DATA_ST_ACCEL_H__
#define __LINUX_PLATFORM_DATA_ST_ACCEL_H__

/**
 * struct st_accel_i2c_platform_data - Platform data for the ST Micro accelerometer driver
 * @irq2: IRQ number for the second INT pin.
 **/
struct st_accel_i2c_platform_data {
	int irq2;
};

#endif /* LINUX_PLATFORM_DATA_ST_ACCEL_H_ */
