/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Based on libdrm/tests/amdgpu/deadlock_tests.c
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_dispatch.h"
#include "lib/amdgpu/amd_deadlock_helpers.h"

static void
amdgpu_dispatch_hang_slow_gfx(amdgpu_device_handle device_handle)
{
	amdgpu_dispatch_hang_slow_helper(device_handle, AMDGPU_HW_IP_GFX);
}

static void
amdgpu_dispatch_hang_slow_compute(amdgpu_device_handle device_handle)
{
	amdgpu_dispatch_hang_slow_helper(device_handle, AMDGPU_HW_IP_COMPUTE);
}

static void
amdgpu_deadlock_gfx(amdgpu_device_handle device_handle)
{
	amdgpu_wait_memory_helper(device_handle, AMDGPU_HW_IP_GFX);
}

static void
amdgpu_deadlock_compute(amdgpu_device_handle device_handle)
{
	amdgpu_wait_memory_helper(device_handle, AMDGPU_HW_IP_COMPUTE);
}

static void
amdgpu_deadlock_sdma(amdgpu_device_handle device_handle)
{
	amdgpu_wait_memory_helper(device_handle, AMDGPU_HW_IP_DMA);
}

static void
amdgpu_gfx_illegal_reg_access(amdgpu_device_handle device_handle)
{
	bad_access_helper(device_handle, 1, AMDGPU_HW_IP_GFX);
}

static void
amdgpu_gfx_illegal_mem_access(amdgpu_device_handle device_handle)
{
	bad_access_helper(device_handle, 0, AMDGPU_HW_IP_GFX);
}

igt_main
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	int fd = -1;
	int r;

	igt_fixture {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks( major, minor,  &gpu_info, device);
		igt_assert_eq(r, 0);

	}
	igt_subtest("amdgpu_deadlock_sdma")
	amdgpu_deadlock_sdma(device);

	igt_subtest("amdgpu_gfx_illegal_reg_access")
	amdgpu_gfx_illegal_reg_access(device);

	igt_subtest("amdgpu_gfx_illegal_mem_access")
	amdgpu_gfx_illegal_mem_access(device);

	igt_subtest("amdgpu_deadlock_gfx")
	amdgpu_deadlock_gfx(device);

	igt_subtest("amdgpu_deadlock_compute")
	amdgpu_deadlock_compute(device);

	igt_subtest("dispatch_hang_slow_compute")
	amdgpu_dispatch_hang_slow_compute(device);

	igt_subtest("dispatch_hang_slow_gfx")
	amdgpu_dispatch_hang_slow_gfx(device);

	igt_fixture {
		amdgpu_device_deinitialize(device);
		close(fd);
	}
}
