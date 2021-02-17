/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2021 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include "igt.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>

static amdgpu_device_handle dev;

static void query_firmware_version_test(void)
{
	struct amdgpu_gpu_info gpu_info = {};
	uint32_t version, feature;

	igt_assert_f(amdgpu_query_gpu_info(dev, &gpu_info) == 0,
		     "Failed to query the gpu information\n");

	igt_assert_f(amdgpu_query_firmware_version(dev, AMDGPU_INFO_FW_VCE, 0,
						   0, &version, &feature) == 0,
		     "Failed to query the firmware version\n");
}

IGT_TEST_DESCRIPTION("Test the consistency of the data provided through the "
		     "DRM_AMDGPU_INFO IOCTL");
igt_main
{
	int fd = -1;

	igt_fixture {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &dev);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
	}

	igt_describe("Make sure we can retrieve the firmware version");
	igt_subtest("query-firmware-version")
		query_firmware_version_test();

	igt_fixture {
		amdgpu_device_deinitialize(dev);
		close(fd);
	}
}
