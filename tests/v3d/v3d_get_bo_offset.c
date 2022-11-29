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

IGT_TEST_DESCRIPTION("Tests for the V3D's get BO offset IOCTL");

igt_main
{
	int fd;

	igt_fixture
		fd = drm_open_driver(DRIVER_V3D);

	igt_describe("Make sure the offset returned by the creation of the BO is "
		     "the same as the offset returned by the IOCTL");
	igt_subtest("create-get-offsets") {
		struct v3d_bo *bos[2] = {
			igt_v3d_create_bo(fd, PAGE_SIZE),
			igt_v3d_create_bo(fd, PAGE_SIZE),
		};
		uint32_t offsets[2] = {
			igt_v3d_get_bo_offset(fd, bos[0]->handle),
			igt_v3d_get_bo_offset(fd, bos[1]->handle),
		};

		igt_assert_neq(bos[0]->handle, bos[1]->handle);
		igt_assert_neq(bos[0]->offset, bos[1]->offset);
		igt_assert_eq(bos[0]->offset, offsets[0]);
		igt_assert_eq(bos[1]->offset, offsets[1]);

		/* 0 is an invalid offset for BOs to be placed at. */
		igt_assert_neq(bos[0]->offset, 0);
		igt_assert_neq(bos[1]->offset, 0);

		igt_v3d_free_bo(fd, bos[0]);
		igt_v3d_free_bo(fd, bos[1]);
	}

	igt_describe("Make sure an offset cannot be returned for an invalid BO handle.");
	igt_subtest("get-bad-handle") {
		struct drm_v3d_get_bo_offset get = {
			.handle = 0xd0d0d0d0,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_GET_BO_OFFSET, &get, ENOENT);
	}

	igt_fixture
		close(fd);
}
