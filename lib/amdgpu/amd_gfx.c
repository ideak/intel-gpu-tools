/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *  *
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

#include "amd_memory.h"
#include "amd_gfx.h"

/**
 *
 * @param device
 */
void amdgpu_command_submission_gfx_separate_ibs(amdgpu_device_handle device)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle, ib_result_ce_handle;
	void *ib_result_cpu, *ib_result_ce_cpu;
	uint64_t ib_result_mc_address, ib_result_ce_mc_address;
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info[2];
	struct amdgpu_cs_fence fence_status = {0};
	uint32_t *ptr;
	uint32_t expired;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle, va_handle_ce;
	int r;

	r = amdgpu_cs_ctx_create(device, &context_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_handle, &ib_result_cpu,
				    &ib_result_mc_address, &va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_ce_handle, &ib_result_ce_cpu,
				    &ib_result_ce_mc_address, &va_handle_ce);
	igt_assert_eq(r, 0);

	r = amdgpu_get_bo_list(device, ib_result_handle,
			       ib_result_ce_handle, &bo_list);
	igt_assert_eq(r, 0);

	memset(ib_info, 0, 2 * sizeof(struct amdgpu_cs_ib_info));

	/* IT_SET_CE_DE_COUNTERS */
	ptr = ib_result_ce_cpu;
	ptr[0] = 0xc0008900;
	ptr[1] = 0;
	ptr[2] = 0xc0008400;
	ptr[3] = 1;
	ib_info[0].ib_mc_address = ib_result_ce_mc_address;
	ib_info[0].size = 4;
	ib_info[0].flags = AMDGPU_IB_FLAG_CE;

	/* IT_WAIT_ON_CE_COUNTER */
	ptr = ib_result_cpu;
	ptr[0] = 0xc0008600;
	ptr[1] = 0x00000001;
	ib_info[1].ib_mc_address = ib_result_mc_address;
	ib_info[1].size = 2;

	ibs_request.ip_type = AMDGPU_HW_IP_GFX;
	ibs_request.number_of_ibs = 2;
	ibs_request.ibs = ib_info;
	ibs_request.resources = bo_list;
	ibs_request.fence_info.handle = NULL;

	r = amdgpu_cs_submit(context_handle, 0,&ibs_request, 1);

	igt_assert_eq(r, 0);

	fence_status.context = context_handle;
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request.seq_no;

	r = amdgpu_cs_query_fence_status(&fence_status,
					 AMDGPU_TIMEOUT_INFINITE,
					 0, &expired);
	igt_assert_eq(r, 0);

	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
				 ib_result_mc_address, 4096);
	amdgpu_bo_unmap_and_free(ib_result_ce_handle, va_handle_ce,
				 ib_result_ce_mc_address, 4096);

	r = amdgpu_bo_list_destroy(bo_list);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_free(context_handle);
	igt_assert_eq(r, 0);
}

/**
 *
 * @param device
 */
void amdgpu_command_submission_gfx_shared_ib(amdgpu_device_handle device)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info[2];
	struct amdgpu_cs_fence fence_status = {0};
	uint32_t *ptr;
	uint32_t expired;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	int r;

	r = amdgpu_cs_ctx_create(device, &context_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_handle, &ib_result_cpu,
				    &ib_result_mc_address, &va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_get_bo_list(device, ib_result_handle, NULL,
			       &bo_list);
	igt_assert_eq(r, 0);
	r = sizeof(ib_info);
	memset(ib_info, 0, 2 * sizeof(struct amdgpu_cs_ib_info));

	/* IT_SET_CE_DE_COUNTERS */
	ptr = ib_result_cpu;
	ptr[0] = 0xc0008900;
	ptr[1] = 0;
	ptr[2] = 0xc0008400;
	ptr[3] = 1;
	ib_info[0].ib_mc_address = ib_result_mc_address;
	ib_info[0].size = 4;
	ib_info[0].flags = AMDGPU_IB_FLAG_CE;

	ptr = (uint32_t *)ib_result_cpu + 4;
	ptr[0] = 0xc0008600;
	ptr[1] = 0x00000001;
	ib_info[1].ib_mc_address = ib_result_mc_address + 16;
	ib_info[1].size = 2;

	ibs_request.ip_type = AMDGPU_HW_IP_GFX;
	ibs_request.number_of_ibs = 2;
	ibs_request.ibs = ib_info;
	ibs_request.resources = bo_list;
	ibs_request.fence_info.handle = NULL;

	r = amdgpu_cs_submit(context_handle, 0, &ibs_request, 1);

	igt_assert_eq(r, 0);

	fence_status.context = context_handle;
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request.seq_no;

	r = amdgpu_cs_query_fence_status(&fence_status,
					 AMDGPU_TIMEOUT_INFINITE,
					 0, &expired);
	igt_assert_eq(r, 0);

	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
				 ib_result_mc_address, 4096);

	r = amdgpu_bo_list_destroy(bo_list);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_free(context_handle);
	igt_assert_eq(r, 0);
}
