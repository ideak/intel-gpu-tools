/*
 * Copyright 2021 Advanced Micro Devices, Inc.
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
 *
 */

#include "igt.h"
#include "igt_kmod.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <sys/ioctl.h>

/**
 * sanity_check:
 *
 * Ensures the driver is able to respond to DRM_IOCTL_AMDGPU_INFO ioctl
 * with a timeout.
 *
 */
static void sanity_check(void)
{
	int err = 0;
	int fd;
	int arg_ret;
	struct drm_amdgpu_info args = {0};

	args.return_pointer = (uintptr_t) &arg_ret;
	args.return_size = sizeof(int);
	args.query = AMDGPU_INFO_HW_IP_INFO;
	args.query_hw_ip.type = AMDGPU_HW_IP_COMPUTE;

	fd = drm_open_driver(DRIVER_AMDGPU);
	igt_set_timeout(1, "Module reload timeout!");

	if (ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &args) < 0)
		err = -errno;

	igt_set_timeout(0, NULL);
	close(fd);

	igt_assert_eq(err, 0);
}

igt_main
{
	igt_describe("Make sure reloading amdgpu drivers works");
	igt_subtest("reload") {
		int err;
		igt_amdgpu_driver_unload();

		err = igt_amdgpu_driver_load(NULL);

		igt_assert_eq(err, 0);

		sanity_check();

		igt_amdgpu_driver_unload();
	}

	igt_fixture
	{
		/* load the module back in */
		igt_amdgpu_driver_load(NULL);
	}
}
