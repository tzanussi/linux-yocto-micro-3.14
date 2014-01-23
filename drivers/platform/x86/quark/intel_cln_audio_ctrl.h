/*
 * Intel Clanton platform audio control driver
 *
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
 *
 * See intel_cln_audio_ctrl.c for a detailed description
 *
 */

#ifndef __INTEL_CLN_AUDIO_CTRL_H__
#define __INTEL_CLN_AUDIO_CTRL_H__

#include <linux/module.h>

#define INTEL_CLN_AUDIO_MODE_GSM_ONLY       0x0
#define INTEL_CLN_AUDIO_MODE_SPKR_ONLY      0x1
#define INTEL_CLN_AUDIO_MODE_SPKR_MIC       0x3
#define INTEL_CLN_AUDIO_MODE_GSM_SPKR_MIC   0x5

#define INTEL_CLN_AUDIO_MODE_IOC_GSM_ONLY \
	_IO('x', INTEL_CLN_AUDIO_MODE_GSM_ONLY)
#define INTEL_CLN_AUDIO_MODE_IOC_SPKR_ONLY \
	_IO('x', INTEL_CLN_AUDIO_MODE_SPKR_ONLY)
#define INTEL_CLN_AUDIO_MODE_IOC_SPKR_MIC \
	_IO('x', INTEL_CLN_AUDIO_MODE_SPKR_MIC)
#define INTEL_CLN_AUDIO_MODE_IOC_GSM_SPKR_MIC \
	_IO('x', INTEL_CLN_AUDIO_MODE_GSM_SPKR_MIC)

#endif /* __INTEL_CLN_AUDIO_CTRL_H__ */
