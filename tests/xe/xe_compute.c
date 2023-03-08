// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

/**
 * TEST: Check compute-related functionality
 * Category: Hardware building block
 * Sub-category: compute
 * Test category: functionality test
 * Run type: BAT
 */

#include <string.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_compute.h"

#define MAX(X, Y)			(((X) > (Y)) ? (X) : (Y))
#define SIZE_DATA			64
#define SIZE_BATCH			0x1000
#define SIZE_KERNEL			0x1000
#define SIZE_BUFFER_INPUT		MAX(sizeof(float)*SIZE_DATA, 0x1000)
#define SIZE_BUFFER_OUTPUT		MAX(sizeof(float)*SIZE_DATA, 0x1000)
#define ADDR_BATCH			0x100000
#define ADDR_INPUT			(unsigned long)0x200000
#define ADDR_OUTPUT			(unsigned long)0x300000
#define ADDR_SURFACE_STATE_BASE		(unsigned long)0x400000
#define ADDR_DYNAMIC_STATE_BASE		(unsigned long)0x500000
#define ADDR_INDIRECT_OBJECT_BASE	0x800100000000
#define OFFSET_INDIRECT_DATA_START	0xFFFDF000
#define OFFSET_KERNEL			0xFFFEF000

struct bo_dict_entry {
	uint64_t addr;
	uint32_t size;
	void *data;
};

/**
 * SUBTEST: compute-square
 * GPU requirement: only works on TGL_GT2 with device ID: 0x9a49
 * Description:
 * 	This test shows how to create a batch to execute a
 * 	compute kernel. For now it supports tgllp only.
 * TODO: extend test to cover other platforms
 */
static void
test_compute_square(int fd)
{
	uint32_t vm, engine;
	float *dinput;
	struct drm_xe_sync sync = { 0 };

#define BO_DICT_ENTRIES 7
	struct bo_dict_entry bo_dict[BO_DICT_ENTRIES] = {
		{ .addr = ADDR_INDIRECT_OBJECT_BASE + OFFSET_KERNEL, .size = SIZE_KERNEL }, // kernel
		{ .addr = ADDR_DYNAMIC_STATE_BASE, .size =  0x1000}, // dynamic state
		{ .addr = ADDR_SURFACE_STATE_BASE, .size =  0x1000}, // surface state
		{ .addr = ADDR_INDIRECT_OBJECT_BASE + OFFSET_INDIRECT_DATA_START, .size =  0x10000}, // indirect data
		{ .addr = ADDR_INPUT, .size = SIZE_BUFFER_INPUT }, // input
		{ .addr = ADDR_OUTPUT, .size = SIZE_BUFFER_OUTPUT }, // output
		{ .addr = ADDR_BATCH, .size = SIZE_BATCH }, // batch
	};

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	engine = xe_engine_create_class(fd, vm, DRM_XE_ENGINE_CLASS_RENDER);
	sync.flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL;
	sync.handle = syncobj_create(fd, 0);

	for(int i = 0; i < BO_DICT_ENTRIES; i++) {
		bo_dict[i].data = aligned_alloc(xe_get_default_alignment(fd), bo_dict[i].size);
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(bo_dict[i].data), bo_dict[i].addr, bo_dict[i].size, &sync, 1);
		syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL);
		memset(bo_dict[i].data, 0, bo_dict[i].size);
	}
	memcpy(bo_dict[0].data, tgllp_kernel_square_bin, tgllp_kernel_square_length);
	tgllp_create_dynamic_state(bo_dict[1].data, OFFSET_KERNEL);
	tgllp_create_surface_state(bo_dict[2].data, ADDR_INPUT, ADDR_OUTPUT);
	tgllp_create_indirect_data(bo_dict[3].data, ADDR_INPUT, ADDR_OUTPUT);
	dinput = (float *)bo_dict[4].data;
	srand(time(NULL));
	for(int i=0; i < SIZE_DATA; i++) {
		((float*) dinput)[i] = rand()/(float)RAND_MAX;
	}
	tgllp_create_batch_compute(bo_dict[6].data, ADDR_SURFACE_STATE_BASE, ADDR_DYNAMIC_STATE_BASE, ADDR_INDIRECT_OBJECT_BASE, OFFSET_INDIRECT_DATA_START);

	xe_exec_wait(fd, engine, ADDR_BATCH);
	for(int i = 0; i < SIZE_DATA; i++) {
		igt_assert(((float*) bo_dict[5].data)[i] == ((float*) bo_dict[4].data)[i] * ((float*) bo_dict[4].data)[i]);
	}

	for(int i = 0; i < BO_DICT_ENTRIES; i++) {
		xe_vm_unbind_async(fd, vm, 0, 0, bo_dict[i].addr, bo_dict[i].size, &sync, 1);
		syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL);
		free(bo_dict[i].data);
	}

	syncobj_destroy(fd, sync.handle);
	xe_engine_destroy(fd, engine);
	xe_vm_destroy(fd, vm);
}

static bool
is_device_supported(int fd)
{
	struct drm_xe_query_config *config;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_CONFIG,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	config = malloc(query.size);
	igt_assert(config);

	query.data = to_user_pointer(config);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	return (config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff) == 0x9a49;
}

igt_main
{
	int xe;

	igt_fixture {
		xe = drm_open_driver(DRIVER_XE);
		xe_device_get(xe);
	}

	igt_subtest("compute-square") {
		igt_skip_on(!is_device_supported(xe));
		test_compute_square(xe);
	}

	igt_fixture {
		xe_device_put(xe);
		close(xe);
	}
}
