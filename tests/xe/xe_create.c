// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Check bo create ioctl
 * Category: Software building block
 * Sub-category: uapi
 * Functionality: device
 * Test category: functionality test
 * Run type: BAT
 */

#include <string.h>

#include "igt.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define PAGE_SIZE 0x1000

static int __create_bo(int fd, uint32_t vm, uint64_t size, uint32_t flags,
		       uint32_t *handlep)
{
	struct drm_xe_gem_create create = {
		.vm_id = vm,
		.size = size,
		.flags = flags,
	};
	int ret = 0;

	igt_assert(handlep);

	if (igt_ioctl(fd, DRM_IOCTL_XE_GEM_CREATE, &create)) {
		ret = -errno;
		errno = 0;
	}
	*handlep = create.handle;

	return ret;
}

/**
 * SUBTEST: create-invalid-size
 * Description: Verifies xe bo create returns expected error code on invalid
 *              buffer sizes.
 */
static void create_invalid_size(int fd)
{
	struct drm_xe_query_mem_region *memregion;
	uint64_t memreg = all_memory_regions(fd), region;
	uint32_t vm;
	uint32_t handle;
	int ret;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);

	xe_for_each_mem_region(fd, memreg, region) {
		memregion = xe_mem_region(fd, region);

		/* first try, use half of possible min page size */
		ret = __create_bo(fd, vm, memregion->min_page_size >> 1,
				  region, &handle);
		if (!ret) {
			gem_close(fd, handle);
			xe_vm_destroy(fd, vm);
		}
		igt_assert_eq(ret, -EINVAL);

		/*
		 * second try, add page size to min page size if it is
		 * bigger than page size.
		 */
		if (memregion->min_page_size > PAGE_SIZE) {
			ret = __create_bo(fd, vm,
					  memregion->min_page_size + PAGE_SIZE,
					  region, &handle);
			if (!ret) {
				gem_close(fd, handle);
				xe_vm_destroy(fd, vm);
			}
			igt_assert_eq(ret, -EINVAL);
		}
	}

	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: create-massive-size
 * Description: Verifies xe bo create returns expected error code on massive
 *              buffer sizes.
 */
static void create_massive_size(int fd)
{
	uint64_t memreg = all_memory_regions(fd), region;
	uint32_t vm;
	uint32_t handle;
	int ret;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);

	xe_for_each_mem_region(fd, memreg, region) {
		ret = __create_bo(fd, vm, -1ULL >> 32, region, &handle);
		igt_assert_eq(ret, -ENOSPC);
	}
}

igt_main
{
	int xe;

	igt_fixture {
		xe = drm_open_driver(DRIVER_XE);
		xe_device_get(xe);
	}

	igt_subtest("create-invalid-size") {
		create_invalid_size(xe);
	}

	igt_subtest("create-massive-size") {
		create_massive_size(xe);
	}


	igt_fixture {
		xe_device_put(xe);
		close(xe);
	}
}
