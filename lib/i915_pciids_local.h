/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */
#ifndef _I915_PCIIDS_LOCAL_H_
#define _I915_PCIIDS_LOCAL_H_

#include "i915_pciids.h"

/* DG2 */
#define INTEL_DG2_IDS(info) \
	INTEL_VGA_DEVICE(0x56A0, info), \
	INTEL_VGA_DEVICE(0x56A1, info), \
	INTEL_VGA_DEVICE(0x56A2, info), \
	INTEL_VGA_DEVICE(0x56A3, info), \
	INTEL_VGA_DEVICE(0x56A4, info), \
	INTEL_VGA_DEVICE(0x56A5, info), \
	INTEL_VGA_DEVICE(0x56A6, info)

/* ATS-M */
#define INTEL_ATS_M150_IDS(info) \
	INTEL_VGA_DEVICE(0x56C0, info)

#define INTEL_ATS_M75_IDS(info) \
	INTEL_VGA_DEVICE(0x56C1, info)

#define INTEL_ATS_M_IDS(info) \
	INTEL_ATS_M150_IDS(info), \
	INTEL_ATS_M75_IDS(info)

#endif /* _I915_PCIIDS_LOCAL_H */
