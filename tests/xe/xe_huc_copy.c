// SPDX-License-Identifier: MIT
/*
 * Copyright Â© 2022 Intel Corporation
 */

/**
 * TEST: Test HuC copy firmware.
 * Category: Firmware building block
 * Sub-category: HuC
 * Functionality: HuC copy
 * Test category: functionality test
 * TODO: make the test more generic, getting rid of the PCI ID list
 * GPU requirements: This test currently requires TGL, and runs only if the
 *	PCI ID is 0x9A60, 0x9A68, 0x9A70, 0x9A40, 0x9A49, 0x9A59, 0x9A78,
 *	0x9AC0, 0x9AC9, 0x9AD9 or 0x9AF8
 */

#include <string.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define SIZE_DATA           0x1000
#define SIZE_BATCH          0x1000
#define SIZE_BUFFER_INPUT   SIZE_DATA
#define SIZE_BUFFER_OUTPUT  SIZE_DATA
#define ADDR_INPUT          0x200000
#define ADDR_OUTPUT         0x400000
#define ADDR_BATCH          0x600000

#define PARALLEL_VIDEO_PIPE     (0x3<<29)
#define HUC_MFX_WAIT            (PARALLEL_VIDEO_PIPE|(0x1<<27)|(0x1<<8))
#define HUC_IMEM_STATE          (PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|(0x1<<16)|0x3)
#define HUC_PIPE_MODE_SELECT    (PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|0x1)
#define HUC_START               (PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|(0x21<<16))
#define HUC_VIRTUAL_ADDR_STATE  (PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|(0x4<<16)|0x2f)
#define HUC_VIRTUAL_ADDR_REGION_NUM 16
#define HUC_VIRTUAL_ADDR_REGION_SRC 0
#define HUC_VIRTUAL_ADDR_REGION_DST 14

struct bo_dict_entry {
	uint64_t addr;
	uint32_t size;
	void *data;
};

static void
gen12_emit_huc_virtual_addr_state(uint64_t src_addr,
	uint64_t dst_addr,
	uint32_t *batch,
	int *i) {
	batch[(*i)++] = HUC_VIRTUAL_ADDR_STATE;

	for (int j = 0; j < HUC_VIRTUAL_ADDR_REGION_NUM; j++) {
		if (j == HUC_VIRTUAL_ADDR_REGION_SRC) {
			batch[(*i)++] = src_addr;
		} else if (j == HUC_VIRTUAL_ADDR_REGION_DST) {
			batch[(*i)++] = dst_addr;
		} else {
			batch[(*i)++] = 0;
		}
		batch[(*i)++] = 0;
		batch[(*i)++] = 0;
	}
}

static void
gen12_create_batch_huc_copy(uint32_t *batch,
	uint64_t src_addr,
	uint64_t dst_addr) {
	int i = 0;

	batch[i++] = HUC_IMEM_STATE;
	batch[i++] = 0;
	batch[i++] = 0;
	batch[i++] = 0;
	batch[i++] = 0x3;

	batch[i++] = HUC_MFX_WAIT;
	batch[i++] = HUC_MFX_WAIT;

	batch[i++] = HUC_PIPE_MODE_SELECT;
	batch[i++] = 0;
	batch[i++] = 0;

	batch[i++] = HUC_MFX_WAIT;

	gen12_emit_huc_virtual_addr_state(src_addr, dst_addr, batch, &i);

	batch[i++] = HUC_START;
	batch[i++] = 1;

	batch[i++] = MI_BATCH_BUFFER_END;
}

/**
 * SUBTEST: huc_copy
 * Run type: BAT
 * Description:
 *	Loads the HuC copy firmware to copy the content of
 *	the source buffer to the destination buffer. *
 */

static void
test_huc_copy(int fd)
{
	uint32_t vm, engine;
	char *dinput;
	struct drm_xe_sync sync = { 0 };

#define BO_DICT_ENTRIES 3
	struct bo_dict_entry bo_dict[BO_DICT_ENTRIES] = {
		{ .addr = ADDR_INPUT, .size = SIZE_BUFFER_INPUT }, // input
		{ .addr = ADDR_OUTPUT, .size = SIZE_BUFFER_OUTPUT }, // output
		{ .addr = ADDR_BATCH, .size = SIZE_BATCH }, // batch
	};

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	engine = xe_engine_create_class(fd, vm, DRM_XE_ENGINE_CLASS_VIDEO_DECODE);
	sync.flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL;
	sync.handle = syncobj_create(fd, 0);

	for(int i = 0; i < BO_DICT_ENTRIES; i++) {
		bo_dict[i].data = aligned_alloc(xe_get_default_alignment(fd), bo_dict[i].size);
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(bo_dict[i].data), bo_dict[i].addr, bo_dict[i].size, &sync, 1);
		syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL);
		memset(bo_dict[i].data, 0, bo_dict[i].size);
	}
	dinput = (char *)bo_dict[0].data;
	srand(time(NULL));
	for(int i=0; i < SIZE_DATA; i++) {
		((char*) dinput)[i] = rand()/256;
	}
	gen12_create_batch_huc_copy(bo_dict[2].data, bo_dict[0].addr, bo_dict[1].addr);

	xe_exec_wait(fd, engine, ADDR_BATCH);
	for(int i = 0; i < SIZE_DATA; i++) {
		igt_assert(((char*) bo_dict[1].data)[i] == ((char*) bo_dict[0].data)[i]);
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
is_huc_running(int fd)
{
	char buf[4096];
	char *s;
	int gt;

	xe_for_each_gt(fd, gt) {
		char name[256];

		sprintf(name, "gt%d/uc/huc_info", gt);
		igt_debugfs_read(fd, name, buf);
		s = strstr(buf, "RUNNING");

		if (s)
			return true;
	}
	return false;
}

igt_main
{
	int xe;

	igt_fixture {
		xe = drm_open_driver(DRIVER_XE);
		xe_device_get(xe);
	}

	igt_subtest("huc_copy") {
		/*
		 * TODO: eventually need to differentiate huc failed to load vs
		 * platform doesnt have huc
		 */
		igt_skip_on(!is_huc_running(xe));
		test_huc_copy(xe);
	}

	igt_fixture {
		xe_device_put(xe);
		close(xe);
	}
}
