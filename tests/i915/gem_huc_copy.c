/*
 * Copyright © 2019 Intel Corporation
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
 */
#include "igt.h"
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "drm.h"
#include "i915/gem.h"
#include "i915/gem_create.h"

IGT_TEST_DESCRIPTION("A very simple workload for the HuC.");

#define HUC_COPY_DATA_BUF_SIZE	4096

static void
compare_huc_copy_result(int drm_fd, uint32_t src_handle, uint32_t dst_handle)
{
	char src_output[HUC_COPY_DATA_BUF_SIZE];
	char dst_output[HUC_COPY_DATA_BUF_SIZE];

	gem_read(drm_fd, src_handle, 0, src_output, HUC_COPY_DATA_BUF_SIZE);
	gem_read(drm_fd, dst_handle, 0, dst_output, HUC_COPY_DATA_BUF_SIZE);

	for (int i = 0; i < HUC_COPY_DATA_BUF_SIZE; i++)
		igt_assert_f(src_output[i] == dst_output[i],
			     "Exepected %c, found %c at %4d.\n",
			     src_output[i], dst_output[i], i);
}

static int get_huc_status(int fd, int *status)
{
	drm_i915_getparam_t gp = {
		.param = I915_PARAM_HUC_STATUS,
		.value = status,
	};

	if (igt_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp))
		return -errno;

	errno = 0;
	return errno;
}

static void test_huc_load(int fd)
{
	int err;
	int status = 0;

	err = get_huc_status(fd, &status);
	igt_skip_on_f(err == -ENODEV,
		      "HuC is not present on this platform!\n");
	igt_skip_on_f(err == -EOPNOTSUPP,
		      "HuC firmware is disabled!\n");
	igt_fail_on_f(err < 0, "HuC firmware loading error: %i, %s\n",
		      -err, strerror(-err));
	igt_fail_on_f(status == 0, "HuC firmware is not running!\n");
}

igt_main
{
	int drm_fd = -1;
	uint32_t devid;
	igt_huc_copyfunc_t huc_copy;

	igt_fixture {
		drm_fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(drm_fd);
		devid = intel_get_drm_devid(drm_fd);
		huc_copy = igt_get_huc_copyfunc(devid);

		igt_require_f(huc_copy, "no huc_copy function\n");
	}

	igt_describe("Make sure that Huc firmware works"
		     "by copying a char array using Huc"
		     "and verifying the copied result");

	igt_subtest("huc-copy") {
		char inputs[HUC_COPY_DATA_BUF_SIZE];
		struct drm_i915_gem_exec_object2 obj[3];

		test_huc_load(drm_fd);
		/* Initialize src buffer randomly */
		srand(time(NULL));
		for (int i = 0; i < HUC_COPY_DATA_BUF_SIZE; i++)
			inputs[i] = (char) (rand() % 256);

		memset(obj, 0, sizeof(obj));
		/* source buffer object for storing input */
		obj[0].handle = gem_create(drm_fd, HUC_COPY_DATA_BUF_SIZE);
		/* destination buffer object to receive input */
		obj[1].handle = gem_create(drm_fd, HUC_COPY_DATA_BUF_SIZE);
		/* execution buffer object */
		obj[2].handle = gem_create(drm_fd, 4096);

		gem_write(drm_fd, obj[0].handle, 0, inputs, HUC_COPY_DATA_BUF_SIZE);

		huc_copy(drm_fd, obj);
		compare_huc_copy_result(drm_fd, obj[0].handle, obj[1].handle);

		gem_close(drm_fd, obj[0].handle);
		gem_close(drm_fd, obj[1].handle);
		gem_close(drm_fd, obj[2].handle);
	}

	igt_fixture
		close(drm_fd);
}
