// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Test if the driver is capable of doing mmap on different memory regions
 * Category: Software building block
 * Sub-category: mmap
 * Test category: functionality test
 * Run type: BAT
 */

#include "igt.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#include <string.h>


/**
 * SUBTEST: system
 * Description: Test mmap on system memory
 */

/**
 * SUBTEST: %s
 * Description: Test mmap on %arg[1] memory
 * GPU requirements: GPU needs to have dedicated VRAM
 *
 * arg[1]:
 *
 * @vram:		vram
 * @vram-system:	system vram
 */
static void
test_mmap(int fd, uint32_t flags)
{
	uint32_t bo;
	uint64_t mmo;
	void *map;

	igt_require_f(flags, "Device doesn't support such memory region\n");

	bo = xe_bo_create_flags(fd, 0, 4096, flags);
	mmo = xe_bo_mmap_offset(fd, bo);

	map = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);

	strcpy(map, "Write some data to the BO!");

	munmap(map, 4096);

	gem_close(fd, bo);
}

/**
 * SUBTEST: bad-flags
 * Description: Test mmap offset with bad flags.
 *
 */
static void test_bad_flags(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, 0, size),
		.flags = -1u,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, EINVAL);
	gem_close(fd, mmo.handle);
}

/**
 * SUBTEST: bad-extensions
 * Description: Test mmap offset with bad extensions.
 *
 */
static void test_bad_extensions(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct xe_user_extension ext;
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, 0, size),
	};

	mmo.extensions = to_user_pointer(&ext);
	ext.name = -1;

	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, EINVAL);
	gem_close(fd, mmo.handle);
}

/**
 * SUBTEST: bad-object
 * Description: Test mmap offset with bad object.
 *
 */
static void test_bad_object(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, 0, size),
	};

	mmo.handle = 0xdeadbeef;
	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, ENOENT);
}

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	igt_subtest("system")
		test_mmap(fd, system_memory(fd));

	igt_subtest("vram")
		test_mmap(fd, vram_memory(fd, 0));

	igt_subtest("vram-system")
		test_mmap(fd, vram_memory(fd, 0) | system_memory(fd));

	igt_subtest("bad-flags")
		test_bad_flags(fd);

	igt_subtest("bad-extensions")
		test_bad_extensions(fd);

	igt_subtest("bad-object")
		test_bad_object(fd);

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
