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
#ifndef AMD_IP_BLOCKS_H
#define AMD_IP_BLOCKS_H

#include "amd_registers.h"

enum amd_ip_block_type {
	AMD_IP_GFX,
	AMD_IP_COMPUTE,
	AMD_IP_DMA,
	AMD_IP_UVD,
	AMD_IP_VCE,
	AMD_IP_MAX,
};

/* aux struct to hold misc parameters for convenience to maintain */
struct amdgpu_ring_context {

	int ring_id; /* ring_id from amdgpu_query_hw_ip_info */
	int res_cnt; /* num of bo in amdgpu_bo_handle resources[2] */

	uint32_t write_length;  /* length of data */
	uint32_t *pm4; 		/* data of the packet */
	uint32_t pm4_size; 	/* max allocated packet size */
	bool secure; 		/* secure or not */

	uint64_t bo_mc;		/* result from amdgpu_bo_alloc_and_map */
	uint64_t bo_mc2;	/* result from amdgpu_bo_alloc_and_map */

	uint32_t pm4_dw;	/* actual size of pm4 */

	volatile uint32_t *bo_cpu;
	volatile uint32_t *bo2_cpu;

	uint32_t bo_cpu_origin;

	amdgpu_bo_handle bo;
	amdgpu_bo_handle bo2;
	amdgpu_bo_handle boa_vram[2];
	amdgpu_bo_handle boa_gtt[2];

	amdgpu_context_handle context_handle;
	struct drm_amdgpu_info_hw_ip hw_ip_info;  /* result of amdgpu_query_hw_ip_info */

	amdgpu_bo_handle resources[4]; /* amdgpu_bo_alloc_and_map */
	amdgpu_va_handle va_handle;    /* amdgpu_bo_alloc_and_map */
	amdgpu_va_handle va_handle2;   /* amdgpu_bo_alloc_and_map */

	struct amdgpu_cs_ib_info ib_info;     /* amdgpu_bo_list_create */
	struct amdgpu_cs_request ibs_request; /* amdgpu_cs_query_fence_status */
};


struct amdgpu_ip_funcs {
	uint32_t	family_id;
	uint32_t	align_mask;
	uint32_t	nop;
	uint32_t	deadbeaf;
	uint32_t	pattern;
	/* functions */
	int (*write_linear)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*const_fill)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*copy_linear)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, uint32_t *pm4_dw);
	int (*compare)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, int div);
	int (*compare_pattern)(const struct amdgpu_ip_funcs *func, const struct amdgpu_ring_context *context, int div);
	int (*get_reg_offset)(enum general_reg reg);

};

extern const struct amdgpu_ip_block_version gfx_v6_0_ip_block;

struct amdgpu_ip_block_version {
	const enum amd_ip_block_type type;
	const int major;
	const int minor;
	const int rev;
	const struct amdgpu_ip_funcs *funcs;
};

/* global holder for the array of in use ip blocks */

struct amdgpu_ip_blocks_device {
	const struct amdgpu_ip_block_version *ip_blocks[AMD_IP_MAX];
	int			num_ip_blocks;
};

extern  struct amdgpu_ip_blocks_device amdgpu_ips;

int
setup_amdgpu_ip_blocks(uint32_t major, uint32_t minor, struct amdgpu_gpu_info *amdinfo,
		       amdgpu_device_handle device);

const struct amdgpu_ip_block_version *
get_ip_block(amdgpu_device_handle device, enum amd_ip_block_type type);

struct amdgpu_cmd_base {
	uint32_t cdw;  /* Number of used dwords. */
	uint32_t max_dw; /* Maximum number of dwords. */
	uint32_t *buf; /* The base pointer of the chunk. */
	bool is_assigned_buf;

	/* functions */
	int (*allocate_buf)(struct amdgpu_cmd_base  *base, uint32_t size);
	int (*attach_buf)(struct amdgpu_cmd_base  *base, void *ptr, uint32_t size_bytes);
	void (*emit)(struct amdgpu_cmd_base  *base, uint32_t value);
	void (*emit_aligned)(struct amdgpu_cmd_base  *base,uint32_t mask, uint32_t value);
	void (*emit_repeat)(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t number_of_times);
	void (*emit_at_offset)(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t offset_dwords);
	void (*emit_buf)(struct amdgpu_cmd_base  *base, const void *ptr, uint32_t offset_bytes, uint32_t size_bytes);
};

struct amdgpu_cmd_base* get_cmd_base(void);

void free_cmd_base(struct amdgpu_cmd_base *base);

#endif
