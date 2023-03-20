// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#include <string.h>

#include "drmtest.h"
#include "igt.h"
#include "igt_core.h"
#include "igt_syncobj.h"
#include "intel_reg.h"
#include "xe_ioctl.h"
#include "xe_spin.h"

/**
 * xe_spin_init:
 * @spin: pointer to mapped bo in which spinner code will be written
 * @addr: offset of spinner within vm
 * @preempt: allow spinner to be preempted or not
 */
void xe_spin_init(struct xe_spin *spin, uint64_t addr, bool preempt)
{
	uint64_t batch_offset = (char *)&spin->batch - (char *)spin;
	uint64_t batch_addr = addr + batch_offset;
	uint64_t start_offset = (char *)&spin->start - (char *)spin;
	uint64_t start_addr = addr + start_offset;
	uint64_t end_offset = (char *)&spin->end - (char *)spin;
	uint64_t end_addr = addr + end_offset;
	int b = 0;

	spin->start = 0;
	spin->end = 0xffffffff;

	spin->batch[b++] = MI_STORE_DWORD_IMM;
	spin->batch[b++] = start_addr;
	spin->batch[b++] = start_addr >> 32;
	spin->batch[b++] = 0xc0ffee;

	if (preempt)
		spin->batch[b++] = (0x5 << 23);

	spin->batch[b++] = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | 2;
	spin->batch[b++] = 0;
	spin->batch[b++] = end_addr;
	spin->batch[b++] = end_addr >> 32;

	spin->batch[b++] = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	spin->batch[b++] = batch_addr;
	spin->batch[b++] = batch_addr >> 32;

	igt_assert(b <= ARRAY_SIZE(spin->batch));
}

/**
 * xe_spin_started:
 * @spin: pointer to spinner mapped bo
 *
 * Returns: true if spinner is running, othwerwise false.
 */
bool xe_spin_started(struct xe_spin *spin)
{
	return spin->start != 0;
}

/**
 * xe_spin_wait_started:
 * @spin: pointer to spinner mapped bo
 *
 * Wait in userspace code until spinner won't start.
 */
void xe_spin_wait_started(struct xe_spin *spin)
{
	while(!xe_spin_started(spin));
}

void xe_spin_end(struct xe_spin *spin)
{
	spin->end = 0;
}

void xe_cork_init(int fd, struct drm_xe_engine_class_instance *hwe,
		  struct xe_cork *cork)
{
	uint64_t addr = xe_get_default_alignment(fd);
	size_t bo_size = xe_get_default_alignment(fd);
	uint32_t vm, bo, engine, syncobj;
	struct xe_spin *spin;
	struct drm_xe_sync sync = {
		.flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};

	vm = xe_vm_create(fd, 0, 0);

	bo = xe_bo_create(fd, hwe->gt_id, vm, bo_size);
	spin = xe_bo_map(fd, bo, 0x1000);

	xe_vm_bind_sync(fd, vm, bo, 0, addr, bo_size);

	engine = xe_engine_create(fd, vm, hwe, 0);
	syncobj = syncobj_create(fd, 0);

	xe_spin_init(spin, addr, true);
	exec.engine_id = engine;
	exec.address = addr;
	sync.handle = syncobj;
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec), 0);

	cork->spin = spin;
	cork->fd = fd;
	cork->vm = vm;
	cork->bo = bo;
	cork->engine = engine;
	cork->syncobj = syncobj;
}

bool xe_cork_started(struct xe_cork *cork)
{
	return xe_spin_started(cork->spin);
}

void xe_cork_wait_started(struct xe_cork *cork)
{
	xe_spin_wait_started(cork->spin);
}

void xe_cork_end(struct xe_cork *cork)
{
	xe_spin_end(cork->spin);
}

void xe_cork_wait_done(struct xe_cork *cork)
{
	igt_assert(syncobj_wait(cork->fd, &cork->syncobj, 1, INT64_MAX, 0,
				NULL));
}

void xe_cork_fini(struct xe_cork *cork)
{
	syncobj_destroy(cork->fd, cork->syncobj);
	xe_engine_destroy(cork->fd, cork->engine);
	xe_vm_destroy(cork->fd, cork->vm);
	gem_close(cork->fd, cork->bo);
}

uint32_t xe_cork_sync_handle(struct xe_cork *cork)
{
	return cork->syncobj;
}
