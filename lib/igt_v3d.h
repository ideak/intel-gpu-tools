/*
 * Copyright © 2016 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef IGT_V3D_H
#define IGT_V3D_H

#include "v3d_drm.h"

#define PAGE_SIZE 4096

#define V3D_CSD_CFG012_WG_COUNT_SHIFT 16
/* Batches per supergroup minus 1.  8 bits. */
#define V3D_CSD_CFG3_BATCHES_PER_SG_M1_SHIFT 12
/* Workgroups per supergroup, 0 means 16 */
#define V3D_CSD_CFG3_WGS_PER_SG_SHIFT 8
#define V3D_CSD_CFG3_WG_SIZE_SHIFT 0

#define V3D_CSD_CFG5_PROPAGATE_NANS (1 << 2)
#define V3D_CSD_CFG5_SINGLE_SEG (1 << 1)
#define V3D_CSD_CFG5_THREADING (1 << 0)

struct v3d_cl;

struct v3d_bo {
	int handle;
	uint32_t offset;
	uint32_t size;
	void *map;
};

struct v3d_cl_job {
	struct drm_v3d_submit_cl *submit;
	struct v3d_cl *bcl;
	struct v3d_cl *rcl;
	struct v3d_cl *icl;
	struct v3d_bo *tile_alloc;
	struct v3d_bo *tile_state;
};

struct v3d_csd_job {
	struct drm_v3d_submit_csd *submit;
	struct v3d_bo *shader_assembly;
	struct v3d_bo *cl;
};

struct v3d_bo *igt_v3d_create_bo(int fd, size_t size);
void igt_v3d_free_bo(int fd, struct v3d_bo *bo);

/* IOCTL wrappers */
uint32_t igt_v3d_get_bo_offset(int fd, uint32_t handle);
uint32_t igt_v3d_get_param(int fd, enum drm_v3d_param param);
void *igt_v3d_mmap_bo(int fd, uint32_t handle, uint32_t size, unsigned prot);

void igt_v3d_bo_mmap(int fd, struct v3d_bo *bo);

void igt_v3d_wait_bo(int fd, struct v3d_bo *bo, uint64_t timeout_ns);

uint32_t igt_v3d_perfmon_create(int fd, uint32_t ncounters, uint8_t *counters);
void igt_v3d_perfmon_get_values(int fd, uint32_t id);
void igt_v3d_perfmon_destroy(int fd, uint32_t id);

void igt_v3d_set_multisync(struct drm_v3d_multi_sync *ms, enum v3d_queue wait_stage);

struct v3d_cl_job *igt_v3d_noop_job(int fd);
void igt_v3d_free_cl_job(int fd, struct v3d_cl_job *job);

struct v3d_csd_job *igt_v3d_empty_shader(int fd);
void igt_v3d_free_csd_job(int fd, struct v3d_csd_job *job);

#endif /* IGT_V3D_H */
