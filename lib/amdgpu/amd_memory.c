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

#include "amd_memory.h"

/**
 *
 * @param device_handle
 * @param size
 * @param alignment
 * @param type
 * @param flags
 * @param vmc_addr
 * @param va_handle
 * @return
 */
 amdgpu_bo_handle
 gpu_mem_alloc(amdgpu_device_handle device_handle,
				      uint64_t size,
				      uint64_t alignment,
				      uint32_t type,
				      uint64_t flags,
				      uint64_t *vmc_addr,
				      amdgpu_va_handle *va_handle)
{
	struct amdgpu_bo_alloc_request req = {
		.alloc_size = size,
		.phys_alignment = alignment,
		.preferred_heap = type,
		.flags = flags,
	};
	amdgpu_bo_handle buf_handle;
	int r;

	r = amdgpu_bo_alloc(device_handle, &req, &buf_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(device_handle,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, vmc_addr,
				  va_handle, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, size, *vmc_addr, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	return buf_handle;
}

 /**
  *
  * @param dev
  * @param size
  * @param alignment
  * @param heap
  * @param flags
  * @param bo
  * @return
  */
int
amdgpu_bo_alloc_wrap(amdgpu_device_handle dev, unsigned size,
		     unsigned alignment, unsigned heap, uint64_t flags,
		     amdgpu_bo_handle *bo)
{
	amdgpu_bo_handle buf_handle;
	int r;
	struct amdgpu_bo_alloc_request req = {
		.alloc_size = size,
		.phys_alignment = alignment,
		.preferred_heap = heap,
		.flags = flags,
	};

	r = amdgpu_bo_alloc(dev, &req, &buf_handle);
	if (r)
		return r;

	*bo = buf_handle;

	return 0;
}

 /**
  *
  * @param bo
  * @param va_handle
  * @param vmc_addr
  * @param size
  */
 void
 gpu_mem_free(amdgpu_bo_handle bo,
			 amdgpu_va_handle va_handle,
			 uint64_t vmc_addr,
			 uint64_t size)
{
	int r;

	r = amdgpu_bo_va_op(bo, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(bo);
	igt_assert_eq(r, 0);
}

/**
 *
 * @param dev
 * @param size
 * @param alignment
 * @param heap
 * @param flags
 * @param bo
 * @param cpu
 * @param mc_address
 * @param va_handle
 * @return
 */
int
amdgpu_bo_alloc_and_map(amdgpu_device_handle dev, unsigned size,
			unsigned alignment, unsigned heap, uint64_t flags,
			amdgpu_bo_handle *bo, void **cpu, uint64_t *mc_address,
			amdgpu_va_handle *va_handle)
{
	struct amdgpu_bo_alloc_request request = {
		.alloc_size = size,
		.phys_alignment = alignment,
		.preferred_heap = heap,
		.flags = flags,
	};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle handle;
	uint64_t vmc_addr;
	int r;

	r = amdgpu_bo_alloc(dev, &request, &buf_handle);
	if (r)
		return r;

	r = amdgpu_va_range_alloc(dev,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, &vmc_addr,
				  &handle, 0);
	if (r)
		goto error_va_alloc;

	r = amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_MAP);
	if (r)
		goto error_va_map;

	r = amdgpu_bo_cpu_map(buf_handle, cpu);
	if (r)
		goto error_cpu_map;

	*bo = buf_handle;
	*mc_address = vmc_addr;
	*va_handle = handle;

	return 0;

error_cpu_map:
	amdgpu_bo_cpu_unmap(buf_handle);

error_va_map:
	amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);

error_va_alloc:
	amdgpu_bo_free(buf_handle);
	return r;
}

int
amdgpu_bo_alloc_and_map_raw(amdgpu_device_handle dev, unsigned size,
			unsigned alignment, unsigned heap, uint64_t alloc_flags,
			uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			uint64_t *mc_address, amdgpu_va_handle *va_handle)
{
	struct amdgpu_bo_alloc_request request = {};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle handle;
	uint64_t vmc_addr;
	int r;

	request.alloc_size = size;
	request.phys_alignment = alignment;
	request.preferred_heap = heap;
	request.flags = alloc_flags;

	r = amdgpu_bo_alloc(dev, &request, &buf_handle);
	if (r)
		return r;

	r = amdgpu_va_range_alloc(dev,
				  amdgpu_gpu_va_range_general,
				  size, alignment, 0, &vmc_addr,
				  &handle, 0);
	if (r)
		goto error_va_alloc;

	r = amdgpu_bo_va_op_raw(dev, buf_handle, 0,  ALIGN(size, getpagesize()), vmc_addr,
				   AMDGPU_VM_PAGE_READABLE |
				   AMDGPU_VM_PAGE_WRITEABLE |
				   AMDGPU_VM_PAGE_EXECUTABLE |
				   mapping_flags,
				   AMDGPU_VA_OP_MAP);
	if (r)
		goto error_va_map;

	r = amdgpu_bo_cpu_map(buf_handle, cpu);
	if (r)
		goto error_cpu_map;

	*bo = buf_handle;
	*mc_address = vmc_addr;
	*va_handle = handle;

	return 0;

 error_cpu_map:
	amdgpu_bo_cpu_unmap(buf_handle);

 error_va_map:
	amdgpu_bo_va_op(buf_handle, 0, size, vmc_addr, 0, AMDGPU_VA_OP_UNMAP);

 error_va_alloc:
	amdgpu_bo_free(buf_handle);
	return r;
}

/**
 *
 * @param bo
 * @param va_handle
 * @param mc_addr
 * @param size
 */
void
amdgpu_bo_unmap_and_free(amdgpu_bo_handle bo, amdgpu_va_handle va_handle,
			 uint64_t mc_addr, uint64_t size)
{
	amdgpu_bo_cpu_unmap(bo);
	amdgpu_bo_va_op(bo, 0, size, mc_addr, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(va_handle);
	amdgpu_bo_free(bo);
}

/**
 *
 * @param dev
 * @param bo1
 * @param bo2
 * @param list
 * @return
 */
int
amdgpu_get_bo_list(amdgpu_device_handle dev, amdgpu_bo_handle bo1,
		   amdgpu_bo_handle bo2, amdgpu_bo_list_handle *list)
{
	amdgpu_bo_handle resources[] = {bo1, bo2};

	return amdgpu_bo_list_create(dev, bo2 ? 2 : 1, resources, NULL, list);
}

/**
 * MULTI FENCE
 * @param device
 * @param wait_all
 */
void amdgpu_command_submission_multi_fence_wait_all(amdgpu_device_handle device,
						    bool wait_all)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle, ib_result_ce_handle;
	void *ib_result_cpu, *ib_result_ce_cpu;
	uint64_t ib_result_mc_address, ib_result_ce_mc_address;
	struct amdgpu_cs_request ibs_request[2] = {};
	struct amdgpu_cs_ib_info ib_info[2];
	struct amdgpu_cs_fence fence_status[2] = {};
	uint32_t *ptr;
	uint32_t expired;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle, va_handle_ce;
	int r;
	int i, ib_cs_num = 2;

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

	for (i = 0; i < ib_cs_num; i++) {
		ibs_request[i].ip_type = AMDGPU_HW_IP_GFX;
		ibs_request[i].number_of_ibs = 2;
		ibs_request[i].ibs = ib_info;
		ibs_request[i].resources = bo_list;
		ibs_request[i].fence_info.handle = NULL;
	}

	r = amdgpu_cs_submit(context_handle, 0,ibs_request, ib_cs_num);

	igt_assert_eq(r, 0);

	for (i = 0; i < ib_cs_num; i++) {
		fence_status[i].context = context_handle;
		fence_status[i].ip_type = AMDGPU_HW_IP_GFX;
		fence_status[i].fence = ibs_request[i].seq_no;
	}

	r = amdgpu_cs_wait_fences(fence_status, ib_cs_num, wait_all,
				  AMDGPU_TIMEOUT_INFINITE,
				  &expired, NULL);
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


