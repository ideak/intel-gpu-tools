/*
 * Copyright © 2020 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <sys/ioctl.h>

#include "drm.h"
#include "drmtest.h"
#include "i915_drm.h"
#include "intel_chipset.h"

IGT_TEST_DESCRIPTION("Check that igt/i915 know about this PCI-ID");

static bool has_known_intel_chipset(int fd)
{
	int devid = 0;
	struct drm_i915_getparam gp = {
		.param = I915_PARAM_CHIPSET_ID,
		.value = &devid,
	};
	const struct intel_device_info *info;

	if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp)))
		return false;

	info = intel_get_device_info(devid);
	if (!info) {
		igt_warn("Unrecognised PCI-ID: %04x, lookup failed\n", devid);
		return false;
	}

	if (!info->gen) {
		igt_warn("Unknown PCI-ID: %04x\n", devid);
		return false;
	}

	igt_info("PCI-ID: %#04x, gen %d, %s\n",
		 devid, ffs(info->gen), info->codename);
	return true;
}

igt_simple_main
{
	int intel = drm_open_driver(DRIVER_INTEL);

	igt_assert(has_known_intel_chipset(intel));
}
