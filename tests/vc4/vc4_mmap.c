// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Igalia S.L.
 */

#include "igt.h"
#include "igt_vc4.h"

IGT_TEST_DESCRIPTION("Tests for the VC4's mmap IOCTL");

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_VC4);
		igt_require(igt_vc4_is_v3d(fd));
	}

	igt_describe("Make sure an invalid BO cannot be mapped.");
	igt_subtest("mmap-bad-handle") {
		struct drm_vc4_mmap_bo get = {
			.handle = 0xd0d0d0d0,
		};
		do_ioctl_err(fd, DRM_IOCTL_VC4_MMAP_BO, &get, EINVAL);
	}

	igt_describe("Test basics of newly mapped bo like default content, write and read "
		     "coherency, mapping existence after gem_close and unmapping.");
	igt_subtest("mmap-bo") {
		int handle = igt_vc4_create_bo(fd, PAGE_SIZE);
		uint32_t *map = igt_vc4_mmap_bo(fd, handle, PAGE_SIZE, PROT_READ | PROT_WRITE);
		uint8_t expected[PAGE_SIZE];

		/* Testing contents of newly created objects. */
		memset(expected, 0, sizeof(expected));
		igt_assert_eq(memcmp(map, expected, sizeof(expected)), 0);

		memset(map, 0xd0, PAGE_SIZE);
		memset(expected, 0xd0, PAGE_SIZE);
		igt_assert_eq(memcmp(expected, map, sizeof(expected)), 0);

		/* Testing that mapping stays after close */
		gem_close(fd, handle);
		igt_assert_eq(memcmp(expected, map, sizeof(expected)), 0);

		/* Testing unmapping */
		munmap(map, PAGE_SIZE);
	}

	igt_fixture
		close(fd);
}
