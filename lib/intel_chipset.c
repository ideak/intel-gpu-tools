/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "i915_drm.h"

#include "drmtest.h"
#include "intel_chipset.h"
#include "igt_core.h"

/**
 * SECTION:intel_chipset
 * @short_description: Feature macros and chipset helpers
 * @title: Chipset
 * @include: igt.h
 *
 * This library mostly provides feature macros which use raw pci device ids. It
 * also provides a few more helper functions to handle pci devices, chipset
 * detection and related issues.
 */

/**
 * intel_pch:
 *
 * Global variable to keep track of the pch type. Can either be set manually or
 * detected at runtime with intel_check_pch().
 */
enum pch_type intel_pch;

/**
 * intel_get_drm_devid:
 * @fd: open i915 drm file descriptor
 *
 * Queries the kernel for the pci device id corresponding to the drm file
 * descriptor.
 *
 * Returns:
 * The devid, exits the program on any failures.
 */
uint32_t
intel_get_drm_devid(int fd)
{
	struct drm_i915_getparam gp;
	const char *override;
	int devid = 0;

	igt_assert(is_i915_device(fd));

	override = getenv("INTEL_DEVID_OVERRIDE");
	if (override)
		return strtol(override, NULL, 0);

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_CHIPSET_ID;
	gp.value = &devid;
	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp));

	return devid;
}

/**
 * intel_check_pch:
 *
 * Detects the PCH chipset type of the running systems and fills in the results
 * into the global #intel_pch variable.
 */
void
intel_check_pch(void)
{
	struct pci_device *pch_dev;

	pch_dev = pci_device_find_by_slot(0, 0, 31, 0);
	if (pch_dev == NULL)
		return;

	if (pch_dev->vendor_id != 0x8086)
		return;

	switch (pch_dev->device_id & 0xff00) {
	case 0x3b00:
		intel_pch = PCH_IBX;
		break;
	case 0x1c00:
	case 0x1e00:
		intel_pch = PCH_CPT;
		break;
	case 0x8c00:
	case 0x9c00:
		intel_pch = PCH_LPT;
		break;
	default:
		intel_pch = PCH_NONE;
		return;
	}
}
