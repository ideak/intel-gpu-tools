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

#include <assert.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_v3d.h"
#include "ioctl_wrappers.h"

#include "v3d/v3d_cl.h"
#include "v3d/v3d_packet.h"

/**
 * SECTION:igt_v3d
 * @short_description: V3D support library
 * @title: V3D
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing V3D
 * tests.
 */

struct v3d_bo *
igt_v3d_create_bo(int fd, size_t size)
{
	struct v3d_bo *bo = calloc(1, sizeof(*bo));

	struct drm_v3d_create_bo create = {
		.size = size,
	};

	do_ioctl(fd, DRM_IOCTL_V3D_CREATE_BO, &create);

	bo->handle = create.handle;
	bo->offset = create.offset;
	bo->size = size;

	return bo;
}

void
igt_v3d_free_bo(int fd, struct v3d_bo *bo)
{
	if (bo->map)
		munmap(bo->map, bo->size);
	gem_close(fd, bo->handle);
	free(bo);
}

uint32_t
igt_v3d_get_bo_offset(int fd, uint32_t handle)
{
	struct drm_v3d_get_bo_offset get = {
		.handle = handle,
	};

	do_ioctl(fd, DRM_IOCTL_V3D_GET_BO_OFFSET, &get);

	return get.offset;
}

/**
 * igt_v3d_get_param:
 * @fd: device file descriptor
 * @param: v3d parameter
 *
 * This wraps the GET_PARAM ioctl.
 *
 * Returns the current value of the parameter. If the parameter is
 * invalid, returns 0.
 */
uint32_t
igt_v3d_get_param(int fd, enum drm_v3d_param param)
{
	struct drm_v3d_get_param get = {
		.param = param,
	};
	int ret;

	ret = igt_ioctl(fd, DRM_IOCTL_V3D_GET_PARAM, &get);
	if (ret)
		return 0;

	return get.value;
}

void *
igt_v3d_mmap_bo(int fd, uint32_t handle, uint32_t size, unsigned prot)
{
	struct drm_v3d_mmap_bo mmap_bo = {
		.handle = handle,
	};
	void *ptr;

	do_ioctl(fd, DRM_IOCTL_V3D_MMAP_BO, &mmap_bo);

	igt_assert_eq(mmap_bo.offset % sysconf(_SC_PAGE_SIZE), 0);

	ptr = mmap(0, size, prot, MAP_SHARED, fd, mmap_bo.offset);
	if (ptr == MAP_FAILED)
		return NULL;
	else
		return ptr;
}

void igt_v3d_bo_mmap(int fd, struct v3d_bo *bo)
{
	bo->map = igt_v3d_mmap_bo(fd, bo->handle, bo->size,
				  PROT_READ | PROT_WRITE);
	igt_assert(bo->map);
}

void igt_v3d_wait_bo(int fd, struct v3d_bo *bo, uint64_t timeout_ns)
{
	struct drm_v3d_wait_bo arg = {
		.handle = bo->handle,
		.timeout_ns = timeout_ns
	};
	do_ioctl(fd, DRM_IOCTL_V3D_WAIT_BO, &arg);
}

uint32_t igt_v3d_perfmon_create(int fd, uint32_t ncounters, uint8_t *counters)
{
	struct drm_v3d_perfmon_create create = {
		.ncounters = ncounters,
	};

	memcpy(create.counters, counters, ncounters * sizeof(*counters));

	do_ioctl(fd, DRM_IOCTL_V3D_PERFMON_CREATE, &create);
	igt_assert_neq(create.id, 0);

	return create.id;
}

void igt_v3d_perfmon_get_values(int fd, uint32_t id)
{
	uint64_t *values = calloc(DRM_V3D_MAX_PERF_COUNTERS, sizeof(*values));
	struct drm_v3d_perfmon_get_values get = {
		.id = id,
		.values_ptr = to_user_pointer(values)
	};

	do_ioctl(fd, DRM_IOCTL_V3D_PERFMON_GET_VALUES, &get);

	free(values);
}

void igt_v3d_perfmon_destroy(int fd, uint32_t id)
{
	struct drm_v3d_perfmon_destroy destroy = {
		.id = id,
	};

	do_ioctl(fd, DRM_IOCTL_V3D_PERFMON_DESTROY, &destroy);
}

void igt_v3d_set_multisync(struct drm_v3d_multi_sync *ms, enum v3d_queue wait_stage)
{
	ms->base.next = to_user_pointer(NULL);
	ms->base.id = DRM_V3D_EXT_ID_MULTI_SYNC;
	ms->base.flags = 0;
	ms->wait_stage = wait_stage;
}

static void v3d_cl_init(int fd, struct v3d_cl **cl)
{
	struct v3d_bo *bo = igt_v3d_create_bo(fd, PAGE_SIZE);

	*cl = calloc(1, sizeof(**cl));

	igt_v3d_bo_mmap(fd, bo);

	(*cl)->bo = bo;
	(*cl)->base = bo->map;
	(*cl)->size = bo->size;
	(*cl)->next = (*cl)->base;
}

static void v3d_cl_destroy(int fd, struct v3d_cl *cl)
{
	igt_v3d_free_bo(fd, cl->bo);
	free(cl);
}

struct v3d_cl_job *igt_v3d_noop_job(int fd)
{
	struct v3d_cl_job *job;
	struct v3d_cl_reloc tile_list_start;
	uint32_t *bos;

	job = calloc(1, sizeof(*job));

	job->tile_alloc = igt_v3d_create_bo(fd, 131 * PAGE_SIZE);
	job->tile_state = igt_v3d_create_bo(fd, PAGE_SIZE);

	v3d_cl_init(fd, &job->bcl);
	v3d_cl_init(fd, &job->rcl);
	v3d_cl_init(fd, &job->icl);

	cl_emit(job->bcl, NUMBER_OF_LAYERS, config) {
		config.number_of_layers = 1;
	}

	cl_emit(job->bcl, TILE_BINNING_MODE_CFG, config) {
		config.width_in_pixels = 1;
		config.height_in_pixels = 1;
		config.number_of_render_targets = 1;
		config.multisample_mode_4x = false;
		config.double_buffer_in_non_ms_mode = false;
		config.maximum_bpp_of_all_render_targets = V3D_INTERNAL_BPP_32;
	}

	/* There's definitely nothing in the VCD cache we want. */
	cl_emit(job->bcl, FLUSH_VCD_CACHE, bin);

	/*
	 * "Binning mode lists must have a Start Tile Binning item (6) after
	 * any prefix state data before the binning list proper starts."
	 */
	cl_emit(job->bcl, START_TILE_BINNING, bin);

	cl_emit(job->bcl, FLUSH, flush);

	cl_emit(job->rcl, TILE_RENDERING_MODE_CFG_COMMON, config) {
		config.early_z_disable = true;
		config.image_width_pixels = 1;
		config.image_height_pixels = 1;
		config.number_of_render_targets = 1;
		config.multisample_mode_4x = false;
		config.maximum_bpp_of_all_render_targets = V3D_INTERNAL_BPP_32;
	}

	cl_emit(job->rcl, TILE_RENDERING_MODE_CFG_COLOR, rt) {
		rt.render_target_0_internal_bpp = V3D_INTERNAL_BPP_32;
		rt.render_target_0_internal_type = V3D_INTERNAL_TYPE_8;
		rt.render_target_0_clamp = V3D_RENDER_TARGET_CLAMP_NONE;
	}

	cl_emit(job->rcl, TILE_RENDERING_MODE_CFG_ZS_CLEAR_VALUES, clear) {
		clear.z_clear_value = 1.0f;
		clear.stencil_clear_value = 0;
	};

	cl_emit(job->rcl, TILE_LIST_INITIAL_BLOCK_SIZE, init) {
		init.use_auto_chained_tile_lists = true;
		init.size_of_first_block_in_chained_tile_lists = TILE_ALLOCATION_BLOCK_SIZE_64B;
	}

	cl_emit(job->rcl, MULTICORE_RENDERING_TILE_LIST_SET_BASE, list) {
		list.address = v3d_cl_address(job->tile_alloc, 0);
	}

	cl_emit(job->rcl, MULTICORE_RENDERING_SUPERTILE_CFG, config) {
		config.number_of_bin_tile_lists = 1;
		config.total_frame_width_in_tiles = 1;
		config.total_frame_height_in_tiles = 1;
		config.supertile_width_in_tiles = 1;
		config.supertile_height_in_tiles = 1;
		config.total_frame_width_in_supertiles = 1;
		config.total_frame_height_in_supertiles = 1;
	}

	tile_list_start = v3d_cl_get_address(job->icl);

	cl_emit(job->icl, TILE_COORDINATES_IMPLICIT, coords);

	cl_emit(job->icl, END_OF_LOADS, end);

	cl_emit(job->icl, BRANCH_TO_IMPLICIT_TILE_LIST, branch);

	cl_emit(job->icl, STORE_TILE_BUFFER_GENERAL, store) {
		store.buffer_to_store = NONE;
	}

	cl_emit(job->icl, END_OF_TILE_MARKER, end);

	cl_emit(job->icl, RETURN_FROM_SUB_LIST, ret);

	cl_emit(job->rcl, START_ADDRESS_OF_GENERIC_TILE_LIST, branch) {
		branch.start = tile_list_start;
		branch.end = v3d_cl_get_address(job->icl);
	}

	cl_emit(job->rcl, SUPERTILE_COORDINATES, coords) {
		coords.column_number_in_supertiles = 0;
		coords.row_number_in_supertiles = 0;
	}

	cl_emit(job->rcl, END_OF_RENDERING, end);

	job->submit = calloc(1, sizeof(*job->submit));

	job->submit->bcl_start = job->bcl->bo->offset;
	job->submit->bcl_end = job->bcl->bo->offset + v3d_cl_offset(job->bcl);
	job->submit->rcl_start = job->rcl->bo->offset;
	job->submit->rcl_end = job->rcl->bo->offset + v3d_cl_offset(job->rcl);

	job->submit->qma = job->tile_alloc->offset;
	job->submit->qms = job->tile_alloc->size;
	job->submit->qts = job->tile_state->offset;

	job->submit->bo_handle_count = 5;
	bos = malloc(sizeof(*bos) * job->submit->bo_handle_count);

	bos[0] = job->bcl->bo->handle;
	bos[1] = job->tile_alloc->handle;
	bos[2] = job->tile_state->handle;
	bos[3] = job->rcl->bo->handle;
	bos[4] = job->icl->bo->handle;

	job->submit->bo_handles = to_user_pointer(bos);

	return job;
}

void igt_v3d_free_cl_job(int fd, struct v3d_cl_job *job)
{
	free(from_user_pointer(job->submit->bo_handles));
	igt_v3d_free_bo(fd, job->tile_alloc);
	igt_v3d_free_bo(fd, job->tile_state);
	v3d_cl_destroy(fd, job->bcl);
	v3d_cl_destroy(fd, job->rcl);
	v3d_cl_destroy(fd, job->icl);
	free(job->submit);
	free(job);
}

/**
 * igt_v3d_empty_shader:
 * @fd: device file descriptor
 *
 * This helper returns a simple compute dispatch job. It sets the
 * configurations (cfg) needed for the job and has the assembled instructions
 * necessary to process an empty shader.
 */
struct v3d_csd_job *igt_v3d_empty_shader(int fd)
{
	struct v3d_csd_job *job;
	uint32_t *bos;

	/* Reproduce an empty shader */
	const uint32_t assembly[] = { 0xbb800000, 0x3c203186,
				      0xbb800000, 0x3c003186,
				      0xbb800000, 0x3c003186 };
	const uint32_t group_count_x = 1, group_count_y = 1, group_count_z = 1;
	const uint32_t num_batches = 1, wgs_per_sg = 1, batches_per_sg = 1, wg_size = 1;

	job = calloc(1, sizeof(*job));

	job->shader_assembly = igt_v3d_create_bo(fd, PAGE_SIZE);
	job->cl = igt_v3d_create_bo(fd, PAGE_SIZE);
	job->submit = calloc(1, sizeof(*job->submit));

	igt_v3d_bo_mmap(fd, job->shader_assembly);
	igt_v3d_bo_mmap(fd, job->cl);

	memset(job->shader_assembly->map, 0, sizeof(*job->shader_assembly->map));
	memcpy(job->shader_assembly->map, assembly, sizeof(assembly));
	memset(job->cl->map, 0, sizeof(*job->cl->map));

	job->submit->bo_handle_count = 2;
	bos = malloc(sizeof(*bos) * job->submit->bo_handle_count);
	bos[0] = job->shader_assembly->handle;
	bos[1] = job->cl->handle;

	job->submit->bo_handles = to_user_pointer(bos);

	job->submit->cfg[0] |= group_count_x << V3D_CSD_CFG012_WG_COUNT_SHIFT;
	job->submit->cfg[1] |= group_count_y << V3D_CSD_CFG012_WG_COUNT_SHIFT;
	job->submit->cfg[2] |= group_count_z << V3D_CSD_CFG012_WG_COUNT_SHIFT;

	job->submit->cfg[3] |= (wgs_per_sg & 0xf) << V3D_CSD_CFG3_WGS_PER_SG_SHIFT;
	job->submit->cfg[3] |= (batches_per_sg - 1) << V3D_CSD_CFG3_BATCHES_PER_SG_M1_SHIFT;
	job->submit->cfg[3] |= (wg_size & 0xff) << V3D_CSD_CFG3_WG_SIZE_SHIFT;

	job->submit->cfg[4] = num_batches - 1;

	job->submit->cfg[5] = job->shader_assembly->offset | V3D_CSD_CFG5_PROPAGATE_NANS;
	job->submit->cfg[5] |= V3D_CSD_CFG5_SINGLE_SEG;
	job->submit->cfg[5] |= V3D_CSD_CFG5_THREADING;

	job->submit->cfg[6] = job->cl->offset;

	return job;
}

/**
 * igt_v3d_free_csd_job:
 * @fd: device file descriptor
 * @job: a compute shader dispatch job
 *
 * This helper frees all the fields of the struct v3d_csd_job and the
 * alocatted job itself.
 */
void igt_v3d_free_csd_job(int fd, struct v3d_csd_job *job)
{
	free(from_user_pointer(job->submit->bo_handles));
	igt_v3d_free_bo(fd, job->shader_assembly);
	igt_v3d_free_bo(fd, job->cl);
	free(job->submit);
	free(job);
}
