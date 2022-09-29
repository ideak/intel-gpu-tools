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
#ifndef AMD_MEMORY_H
#define AMD_MEMORY_H

#include "drmtest.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>


amdgpu_bo_handle
gpu_mem_alloc(amdgpu_device_handle device_handle,
				      uint64_t size,
				      uint64_t alignment,
				      uint32_t type,
				      uint64_t flags,
				      uint64_t *vmc_addr,
				      amdgpu_va_handle *va_handle);
int
amdgpu_bo_alloc_wrap(amdgpu_device_handle dev, unsigned size,
		     unsigned alignment, unsigned heap, uint64_t flags,
		     amdgpu_bo_handle *bo);

void
gpu_mem_free(amdgpu_bo_handle bo,
			 amdgpu_va_handle va_handle,
			 uint64_t vmc_addr,
			 uint64_t size);

int
amdgpu_bo_alloc_and_map(amdgpu_device_handle dev, unsigned size,
			unsigned alignment, unsigned heap, uint64_t flags,
			amdgpu_bo_handle *bo, void **cpu, uint64_t *mc_address,
			amdgpu_va_handle *va_handle);

int
amdgpu_bo_alloc_and_map_raw(amdgpu_device_handle dev, unsigned size,
			unsigned alignment, unsigned heap, uint64_t alloc_flags,
			uint64_t mapping_flags, amdgpu_bo_handle *bo, void **cpu,
			uint64_t *mc_address, amdgpu_va_handle *va_handle);

void
amdgpu_bo_unmap_and_free(amdgpu_bo_handle bo, amdgpu_va_handle va_handle,
			 uint64_t mc_addr, uint64_t size);

int
amdgpu_get_bo_list(amdgpu_device_handle dev, amdgpu_bo_handle bo1,
		   amdgpu_bo_handle bo2, amdgpu_bo_list_handle *list);

void amdgpu_command_submission_multi_fence_wait_all(amdgpu_device_handle device,
						    bool wait_all);

#endif
