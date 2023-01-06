/* SPDX-License-Identifier: MIT
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 */

#ifndef __AMD_CP_DMA_H__
#define __AMD_CP_DMA_H__

int
amdgpu_cp_dma_generic(amdgpu_device_handle device_handle,
				amdgpu_device_handle exporting_device_handle,
				unsigned int ip_type,
				uint32_t src_heap, uint32_t dst_heap);

bool
amdgpu_cp_dma_misc_is_supported(const struct amdgpu_gpu_info *gpu_info);

bool
amdgpu_cp_dma_misc_p2p_is_supported(const struct amdgpu_gpu_info *gpu_info);

bool
asic_is_gfx_pipe_removed(const struct amdgpu_gpu_info *gpu_info);

#endif
