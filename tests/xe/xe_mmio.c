// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Test if mmio feature
 * Category: Software building block
 * Sub-category: mmio
 * Functionality: mmap
 * Test category: functionality test
 * Run type: BAT
 */

#include "igt.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#include <string.h>

#define RCS_TIMESTAMP 0x2358

/**
 * SUBTEST: mmio-timestamp
 * Description:
 *	Try to run mmio ioctl with 32 and 64 bits and check it a timestamp
 *	matches
 */

static void test_xe_mmio_timestamp(int fd)
{
	int ret;
	struct drm_xe_mmio mmio = {
		.addr = RCS_TIMESTAMP,
		.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_64BIT,
	};
	ret = igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio);
	if (!ret)
		igt_debug("RCS_TIMESTAMP 64b = 0x%llx\n", mmio.value);
	igt_assert(!ret);
	mmio.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_32BIT;
	mmio.value = 0;
	ret = igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio);
	if (!ret)
		igt_debug("RCS_TIMESTAMP 32b = 0x%llx\n", mmio.value);
	igt_assert(!ret);
}


/**
 * SUBTEST: mmio-invalid
 * Description: Try to run mmio ioctl with 8, 16 and 32 and 64 bits mmio
 */

static void test_xe_mmio_invalid(int fd)
{
	int ret;
	struct drm_xe_mmio mmio = {
		.addr = RCS_TIMESTAMP,
		.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_8BIT,
	};
	ret = igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio);
	igt_assert(ret);
	mmio.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_16BIT;
	mmio.value = 0;
	ret = igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio);
	igt_assert(ret);
	mmio.addr = RCS_TIMESTAMP;
	mmio.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_64BIT;
	mmio.value = 0x1;
	ret = igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio);
	igt_assert(ret);
}

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	igt_subtest("mmio-timestamp")
		test_xe_mmio_timestamp(fd);
	igt_subtest("mmio-invalid")
		test_xe_mmio_invalid(fd);

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
