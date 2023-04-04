// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

/**
 * TEST: Check compute-related functionality
 * Category: Hardware building block
 * Sub-category: compute
 * Test category: functionality test
 * Run type: BAT
 */

#include <string.h>

#include "igt.h"
#include "xe/xe_query.h"
#include "xe/xe_compute.h"

/**
 * SUBTEST: compute-square
 * GPU requirement: only works on TGL
 * Description:
 *	Run an openCL Kernel that returns output[i] = input[i] * input[i],
 *	for an input dataset..
 * TODO: extend test to cover other platforms
 */
static void
test_compute_square(int fd)
{
	igt_require_f(run_xe_compute_kernel(fd), "GPU not supported\n");
}

igt_main
{
	int xe;

	igt_fixture {
		xe = drm_open_driver(DRIVER_XE);
		xe_device_get(xe);
	}

	igt_subtest("compute-square")
		test_compute_square(xe);

	igt_fixture {
		xe_device_put(xe);
		close(xe);
	}
}
