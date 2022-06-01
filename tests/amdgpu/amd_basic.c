/*
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
 * Based on libdrm/tests/amdgpu/basic_tests.c
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_compute.h"
#include "lib/amdgpu/amd_gfx.h"


#define BUFFER_SIZE (8 * 1024)

#define GFX_COMPUTE_NOP  0xffff1000


/**
 * MEM ALLOC TEST
 * @param device
 */
static void amdgpu_memory_alloc(amdgpu_device_handle device)
{
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t bo_mc;

	/* Test visible VRAM */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test invisible VRAM */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART Cacheable */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   0, &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART USWC */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_CPU_GTT_USWC,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);
}


/**
 * AMDGPU_HW_IP_GFX
 * @param device
 */
static void amdgpu_command_submission_gfx(amdgpu_device_handle device)
{
	/* write data using the CP */
	amdgpu_command_submission_write_linear_helper(device, get_ip_block(device, AMDGPU_HW_IP_GFX), false);
	/* const fill using the CP */
	amdgpu_command_submission_const_fill_helper(device, get_ip_block(device, AMDGPU_HW_IP_GFX));
	/* copy data using the CP */
	amdgpu_command_submission_copy_linear_helper(device, get_ip_block(device, AMDGPU_HW_IP_GFX));
	/* separate IB buffers for multi-IB submission */
	amdgpu_command_submission_gfx_separate_ibs(device);
	/* shared IB buffer for multi-IB submission */
	amdgpu_command_submission_gfx_shared_ib(device);
}

/**
 * AMDGPU_HW_IP_COMPUTE
 * @param device
 */
static void amdgpu_command_submission_compute(amdgpu_device_handle device)
{
	/* write data using the CP */
	amdgpu_command_submission_write_linear_helper(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE), false);
	/* const fill using the CP */
	amdgpu_command_submission_const_fill_helper(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE));
	/* copy data using the CP */
	amdgpu_command_submission_copy_linear_helper(device, get_ip_block(device, AMDGPU_HW_IP_COMPUTE));
	/* nop test */
	amdgpu_command_submission_compute_nop(device);
}

/**
 * AMDGPU_HW_IP_DMA
 * @param device
 */
static void amdgpu_command_submission_sdma(amdgpu_device_handle device)
{
	amdgpu_command_submission_write_linear_helper(device,  get_ip_block(device, AMDGPU_HW_IP_DMA), false);
	amdgpu_command_submission_const_fill_helper(device,  get_ip_block(device, AMDGPU_HW_IP_DMA));
	amdgpu_command_submission_copy_linear_helper(device,  get_ip_block(device, AMDGPU_HW_IP_DMA));
}

/**
 * SEMAPHORE
 * @param device
 */
static void amdgpu_semaphore_test(amdgpu_device_handle device)
{
	amdgpu_context_handle context_handle[2];
	amdgpu_semaphore_handle sem;
	amdgpu_bo_handle ib_result_handle[2];
	void *ib_result_cpu[2];
	uint64_t ib_result_mc_address[2];
	struct amdgpu_cs_request ibs_request[2] = {};
	struct amdgpu_cs_ib_info ib_info[2] = {};
	struct amdgpu_cs_fence fence_status = {};
	uint32_t *ptr;
	uint32_t expired;
	amdgpu_bo_list_handle bo_list[2];
	amdgpu_va_handle va_handle[2];
	int r, i;

	r = amdgpu_cs_create_semaphore(&sem);
	igt_assert_eq(r, 0);
	for (i = 0; i < 2; i++) {
		r = amdgpu_cs_ctx_create(device, &context_handle[i]);
		igt_assert_eq(r, 0);

		r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
					    AMDGPU_GEM_DOMAIN_GTT, 0,
					    &ib_result_handle[i], &ib_result_cpu[i],
					    &ib_result_mc_address[i], &va_handle[i]);
		igt_assert_eq(r, 0);

		r = amdgpu_get_bo_list(device, ib_result_handle[i],
				       NULL, &bo_list[i]);
		igt_assert_eq(r, 0);
	}

	/* 1. same context different engine */
	ptr = ib_result_cpu[0];
	ptr[0] = SDMA_NOP;
	ib_info[0].ib_mc_address = ib_result_mc_address[0];
	ib_info[0].size = 1;

	ibs_request[0].ip_type = AMDGPU_HW_IP_DMA;
	ibs_request[0].number_of_ibs = 1;
	ibs_request[0].ibs = &ib_info[0];
	ibs_request[0].resources = bo_list[0];
	ibs_request[0].fence_info.handle = NULL;
	r = amdgpu_cs_submit(context_handle[0], 0,&ibs_request[0], 1);
	igt_assert_eq(r, 0);
	r = amdgpu_cs_signal_semaphore(context_handle[0], AMDGPU_HW_IP_DMA, 0, 0, sem);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_wait_semaphore(context_handle[0], AMDGPU_HW_IP_GFX, 0, 0, sem);
	igt_assert_eq(r, 0);
	ptr = ib_result_cpu[1];
	ptr[0] = GFX_COMPUTE_NOP;
	ib_info[1].ib_mc_address = ib_result_mc_address[1];
	ib_info[1].size = 1;

	ibs_request[1].ip_type = AMDGPU_HW_IP_GFX;
	ibs_request[1].number_of_ibs = 1;
	ibs_request[1].ibs = &ib_info[1];
	ibs_request[1].resources = bo_list[1];
	ibs_request[1].fence_info.handle = NULL;

	r = amdgpu_cs_submit(context_handle[0], 0,&ibs_request[1], 1);
	igt_assert_eq(r, 0);

	fence_status.context = context_handle[0];
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request[1].seq_no;
	r = amdgpu_cs_query_fence_status(&fence_status,
					 500000000, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_eq(expired, true);

	/* 2. same engine different context */
	ptr = ib_result_cpu[0];
	ptr[0] = GFX_COMPUTE_NOP;
	ib_info[0].ib_mc_address = ib_result_mc_address[0];
	ib_info[0].size = 1;

	ibs_request[0].ip_type = AMDGPU_HW_IP_GFX;
	ibs_request[0].number_of_ibs = 1;
	ibs_request[0].ibs = &ib_info[0];
	ibs_request[0].resources = bo_list[0];
	ibs_request[0].fence_info.handle = NULL;
	r = amdgpu_cs_submit(context_handle[0], 0,&ibs_request[0], 1);
	igt_assert_eq(r, 0);
	r = amdgpu_cs_signal_semaphore(context_handle[0], AMDGPU_HW_IP_GFX, 0, 0, sem);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_wait_semaphore(context_handle[1], AMDGPU_HW_IP_GFX, 0, 0, sem);
	igt_assert_eq(r, 0);
	ptr = ib_result_cpu[1];
	ptr[0] = GFX_COMPUTE_NOP;
	ib_info[1].ib_mc_address = ib_result_mc_address[1];
	ib_info[1].size = 1;

	ibs_request[1].ip_type = AMDGPU_HW_IP_GFX;
	ibs_request[1].number_of_ibs = 1;
	ibs_request[1].ibs = &ib_info[1];
	ibs_request[1].resources = bo_list[1];
	ibs_request[1].fence_info.handle = NULL;
	r = amdgpu_cs_submit(context_handle[1], 0,&ibs_request[1], 1);

	igt_assert_eq(r, 0);

	fence_status.context = context_handle[1];
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request[1].seq_no;
	r = amdgpu_cs_query_fence_status(&fence_status,
					 500000000, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_eq(expired, true);
	for (i = 0; i < 2; i++) {
		amdgpu_bo_unmap_and_free(ib_result_handle[i], va_handle[i],
					 ib_result_mc_address[i], 4096);

		r = amdgpu_bo_list_destroy(bo_list[i]);
		igt_assert_eq(r, 0);

		r = amdgpu_cs_ctx_free(context_handle[i]);
		igt_assert_eq(r, 0);
	}

	r = amdgpu_cs_destroy_semaphore(sem);
	igt_assert_eq(r, 0);
}


/**
 * MULTI FENCE
 * @param device
 */
static void amdgpu_command_submission_multi_fence(amdgpu_device_handle device)
{
	amdgpu_command_submission_multi_fence_wait_all(device, true);
	amdgpu_command_submission_multi_fence_wait_all(device, false);
}

static void amdgpu_userptr_test(amdgpu_device_handle device)
{
	const int pm4_dw = 256;
	const int sdma_write_length = 4;

	struct amdgpu_ring_context *ring_context;
	int r;

	const struct amdgpu_ip_block_version * ip_block = get_ip_block(device, AMDGPU_HW_IP_DMA);
	igt_assert(ip_block);
	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	/* setup parameters */

	ring_context->write_length =  sdma_write_length;
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->secure = false;
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	igt_assert(ring_context->pm4);

	r = amdgpu_cs_ctx_create(device, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	posix_memalign((void**)&ring_context->bo_cpu, sysconf(_SC_PAGE_SIZE), BUFFER_SIZE);
	igt_assert(ring_context->bo_cpu);
	memset((void*)ring_context->bo_cpu, 0, BUFFER_SIZE);

	r = amdgpu_create_bo_from_user_mem(device,
					   (void*)ring_context->bo_cpu,
					   BUFFER_SIZE, &ring_context->bo);
	igt_assert_eq(r, 0);

	ring_context->resources[0] = ring_context->bo;


	r = amdgpu_va_range_alloc(device,
				  amdgpu_gpu_va_range_general,
				  BUFFER_SIZE, 1, 0, &ring_context->bo_mc,
				  &ring_context->va_handle, 0);

	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(ring_context->bo, 0, BUFFER_SIZE, ring_context->bo_mc, 0, AMDGPU_VA_OP_MAP);

	igt_assert_eq(r, 0);

	ip_block->funcs->write_linear(ip_block->funcs, ring_context, &ring_context->pm4_dw);

	 amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context);

	r = ip_block->funcs->compare(ip_block->funcs, ring_context, 1);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(ring_context->bo, 0, BUFFER_SIZE, ring_context->bo_mc, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);
	r = amdgpu_va_range_free(ring_context->va_handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_free(ring_context->bo);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_free(ring_context->context_handle);
	igt_assert_eq(r, 0);

	free(ring_context->pm4);
	free(ring_context);
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

	igt_subtest("memory-alloc")
		amdgpu_memory_alloc(device);

	igt_subtest("userptr")
		amdgpu_userptr_test(device);

	igt_subtest("cs-gfx")
		amdgpu_command_submission_gfx(device);

	igt_subtest("cs-compute")
		amdgpu_command_submission_compute(device);

	igt_subtest("cs-multi-fence")
		amdgpu_command_submission_multi_fence(device);

	igt_subtest("cs-sdma")
		amdgpu_command_submission_sdma(device);

	igt_subtest("semaphore")
		amdgpu_semaphore_test(device);

	igt_fixture {
		amdgpu_device_deinitialize(device);
		close(fd);
	}
}
