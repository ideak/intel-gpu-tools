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
 * SUBTEST: %s
 * Description: Test mmap on %s memory
 *
 * arg[1]:
 *
 * @system:		system
 * @vram:		vram
 * @vram-system:	system vram
 */

static void
test_mmap(int fd, uint32_t flags)
{
	uint32_t bo;
	uint64_t mmo;
	void *map;

	if (flags & vram_memory(fd, 0))
		igt_require(xe_has_vram(fd));

	bo = xe_bo_create_flags(fd, 0, 4096, flags);
	mmo = xe_bo_mmap_offset(fd, bo);

	map = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);

	strcpy(map, "Write some data to the BO!");

	munmap(map, 4096);

	gem_close(fd, bo);
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

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
