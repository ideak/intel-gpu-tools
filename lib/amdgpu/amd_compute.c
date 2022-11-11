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
 *
 */
#include "amd_PM4.h"
#include "amd_memory.h"
#include "amd_compute.h"

/**
 *
 * @param device
 */
void amdgpu_command_submission_compute_nop(amdgpu_device_handle device)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_fence fence_status;
	struct drm_amdgpu_info_hw_ip info;
	uint32_t *ptr;
	uint32_t expired;
	int r, instance;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;

	r = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_COMPUTE, 0, &info);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_create(device, &context_handle);
	igt_assert_eq(r, 0);

	for (instance = 0; info.available_rings & (1 << instance); instance++) {
		r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
					    AMDGPU_GEM_DOMAIN_GTT, 0,
					    &ib_result_handle, &ib_result_cpu,
					    &ib_result_mc_address, &va_handle);
		igt_assert_eq(r, 0);

		r = amdgpu_get_bo_list(device, ib_result_handle, NULL,
				       &bo_list);
		igt_assert_eq(r, 0);

		ptr = ib_result_cpu;
		memset(ptr, 0, 16);
		ptr[0] = PACKET3(PACKET3_NOP, 14);

		memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
		ib_info.ib_mc_address = ib_result_mc_address;
		ib_info.size = 16;

		memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
		ibs_request.ip_type = AMDGPU_HW_IP_COMPUTE;
		ibs_request.ring = instance;
		ibs_request.number_of_ibs = 1;
		ibs_request.ibs = &ib_info;
		ibs_request.resources = bo_list;
		ibs_request.fence_info.handle = NULL;

		memset(&fence_status, 0, sizeof(struct amdgpu_cs_fence));
		r = amdgpu_cs_submit(context_handle, 0,&ibs_request, 1);
		igt_assert_eq(r, 0);

		fence_status.context = context_handle;
		fence_status.ip_type = AMDGPU_HW_IP_COMPUTE;
		fence_status.ip_instance = 0;
		fence_status.ring = instance;
		fence_status.fence = ibs_request.seq_no;

		r = amdgpu_cs_query_fence_status(&fence_status,
						 AMDGPU_TIMEOUT_INFINITE,
						 0, &expired);
		igt_assert_eq(r, 0);

		r = amdgpu_bo_list_destroy(bo_list);
		igt_assert_eq(r, 0);

		amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
					 ib_result_mc_address, 4096);
	}

	r = amdgpu_cs_ctx_free(context_handle);
	igt_assert_eq(r, 0);
}

