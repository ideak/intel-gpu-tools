/*
 * Copyright © 2017 Broadcom
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
#include "igt_v3d.h"

IGT_TEST_DESCRIPTION("Tests for the V3D's mmap IOCTL");

igt_main
{
	int fd;

	igt_fixture
		fd = drm_open_driver(DRIVER_V3D);

	igt_describe("Make sure that flags is equal to zero.");
	igt_subtest("mmap-bad-flags") {
		struct drm_v3d_mmap_bo get = {
			.flags = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_MMAP_BO, &get, EINVAL);
	}

	igt_describe("Make sure an invalid BO cannot be mapped.");
	igt_subtest("mmap-bad-handle") {
		struct drm_v3d_mmap_bo get = {
			.handle = 0xd0d0d0d0,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_MMAP_BO, &get, ENOENT);
	}

	igt_describe("Test basics of newly mapped bo like default content, write and read "
		     "coherency, mapping existence after gem_close and unmapping.");
	igt_subtest("mmap-bo") {
		struct v3d_bo *bo = igt_v3d_create_bo(fd, PAGE_SIZE);
		uint8_t expected[PAGE_SIZE];

		igt_v3d_bo_mmap(fd, bo);

		/* Testing contents of newly created objects. */
		memset(expected, 0, sizeof(expected));
		igt_assert_eq(memcmp(bo->map, expected, sizeof(expected)), 0);

		memset(bo->map, 0xd0, PAGE_SIZE);
		memset(expected, 0xd0, PAGE_SIZE);
		igt_assert_eq(memcmp(expected, bo->map, sizeof(expected)), 0);

		/* Testing that mapping stays after close */
		gem_close(fd, bo->handle);
		igt_assert_eq(memcmp(expected, bo->map, sizeof(expected)), 0);

		/* Testing unmapping */
		munmap(bo->map, PAGE_SIZE);
		free(bo);
	}

	igt_fixture
		close(fd);
}
