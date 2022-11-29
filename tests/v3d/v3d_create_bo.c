// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Igalia S.L.
 */

#include "igt.h"
#include "igt_v3d.h"

IGT_TEST_DESCRIPTION("Tests for the V3D's Create BO IOCTL");

igt_main
{
	int fd;

	igt_fixture
		fd = drm_open_driver(DRIVER_V3D);

	igt_describe("Make sure a BO cannot be created with flags different than zero.");
	igt_subtest("create-bo-invalid-flags") {
		struct drm_v3d_create_bo create = {
			.flags = 0x0a,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_CREATE_BO, &create, EINVAL);
	}

	igt_describe("Make sure a BO cannot be created with size zero.");
	igt_subtest("create-bo-0") {
		struct drm_v3d_create_bo create = {
			.size = 0,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_CREATE_BO, &create, EINVAL);
	}

	igt_describe("Sanity check for creating a BO with size 4096.");
	igt_subtest("create-bo-4096") {
		struct v3d_bo *bo = igt_v3d_create_bo(fd, PAGE_SIZE);
		igt_v3d_free_bo(fd, bo);
	}

	igt_describe("Make sure that BOs can be allocated in different fd without "
		     "carrying old contents from one another.");
	igt_subtest("create-bo-zeroed") {
		int fd2 = drm_open_driver(DRIVER_V3D);
		struct v3d_bo *bo;
		/* A size different from any used in our other tests, to try
		 * to convince it to land as the only one of its size in the
		 * kernel BO cache
		 */
		size_t size = 3 * PAGE_SIZE, i;

		/* Make a BO and free it on our main fd. */
		bo = igt_v3d_create_bo(fd, size);
		bo->map = igt_v3d_mmap_bo(fd, bo->handle, size, PROT_READ | PROT_WRITE);
		memset(bo->map, 0xd0, size);
		igt_v3d_free_bo(fd, bo);

		/* Now, allocate a BO on the other fd and make sure it doesn't
		 * have the old contents.
		 */
		bo = igt_v3d_create_bo(fd2, size);
		bo->map = igt_v3d_mmap_bo(fd2, bo->handle, size, PROT_READ | PROT_WRITE);
		for (i = 0; i < size / 4; i++)
			igt_assert_eq_u32(((uint32_t *)bo->map)[i], 0x0);
		igt_v3d_free_bo(fd2, bo);

		close(fd2);
	}

	igt_fixture
		close(fd);
}
