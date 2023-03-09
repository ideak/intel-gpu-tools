// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf functionality for virtual and parallel engines
 * Category: Hardware building block
 * Sub-category: execbuf
 * Functionality: virtual and parallel engines
 * Test category: functionality test
 */

#include <fcntl.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>

#define MAX_INSTANCE 9

/**
 * SUBTEST: virtual-all-active
 * Description:
 * 	Run a test to check if virtual engines can be running on all instances
 *	of a class simultaneously
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void test_all_active(int fd, int gt, int class)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};
	uint32_t engines[MAX_INSTANCE];
	uint32_t syncobjs[MAX_INSTANCE];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
	} *data;
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	int i, num_placements = 0;

	for_each_hw_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[num_placements++] = *hwe;
	}
	if (num_placements < 2)
		return;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * num_placements;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd), xe_get_default_alignment(fd));

	bo = xe_bo_create(fd, gt, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < num_placements; i++) {
		struct drm_xe_engine_create create = {
			.vm_id = vm,
			.width = 1,
			.num_placements = num_placements,
			.instances = to_user_pointer(eci),
		};

		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_ENGINE_CREATE,
					&create), 0);
		engines[i] = create.engine_id;
		syncobjs[i] = syncobj_create(fd, 0);
	};

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	for (i = 0; i < num_placements; i++) {
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t spin_addr = addr + spin_offset;

		xe_spin_init(&data[i].spin, spin_addr, false);
		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[i];

		exec.engine_id = engines[i];
		exec.address = spin_addr;
		xe_exec(fd, &exec);
		xe_spin_wait_started(&data[i].spin);
	}

	for (i = 0; i < num_placements; i++) {
		xe_spin_end(&data[i].spin);
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	}
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < num_placements; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define MAX_N_ENGINES 16
#define USERPTR		(0x1 << 0)
#define REBIND		(0x1 << 1)
#define INVALIDATE	(0x1 << 2)
#define RACE		(0x1 << 3)
#define VIRTUAL		(0x1 << 4)
#define PARALLEL	(0x1 << 5)

/**
 * SUBTEST: once-%s
 * Description: Run %arg[1] test only once
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: many-%s
 * Description: Run %arg[1] test many times
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: many-engines-%s
 * Description: Run %arg[1] test on many engines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: twice-%s
 * Description: Run %arg[1] test twice
 * Run type: BAT
 *
 * SUBTEST: no-exec-%s
 * Description: Run no-exec %arg[1] test
 * Run type: BAT
 *
 * arg[1]:
 *
 * @virtual-basic:			virtual basic
 * @virtual-userptr:			virtual userptr
 * @virtual-rebind:			virtual rebind
 * @virtual-userptr-rebind:		virtual userptr -rebind
 * @virtual-userptr-invalidate:		virtual userptr invalidate
 * @virtual-userptr-invalidate-race:	virtual userptr invalidate racy
 * @parallel-basic:			parallel basic
 * @parallel-userptr:			parallel userptr
 * @parallel-rebind:			parallel rebind
 * @parallel-userptr-rebind:		parallel userptr rebind
 * @parallel-userptr-invalidate:	parallel userptr invalidate
 * @parallel-userptr-invalidate-race:	parallel userptr invalidate racy
 */
static void
test_exec(int fd, int gt, int class, int n_engines, int n_execs,
	  unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};
	uint32_t engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	int i, j, b, num_placements = 0;

	igt_assert(n_engines <= MAX_N_ENGINES);

	for_each_hw_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[num_placements++] = *hwe;
	}
	if (num_placements < 2)
		return;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd), xe_get_default_alignment(fd));

	if (flags & USERPTR) {
#define	MAP_ADDRESS	0x00007fadeadbe000
		if (flags & INVALIDATE) {
			data = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				    PROT_WRITE, MAP_SHARED | MAP_FIXED |
				    MAP_ANONYMOUS, -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd), bo_size);
			igt_assert(data);
		}
		memset(data, 0, bo_size);
	} else {
		bo = xe_bo_create(fd, gt, vm, bo_size);
		data = xe_bo_map(fd, bo, bo_size);
	}

	for (i = 0; i < n_engines; i++) {
		struct drm_xe_engine_create create = {
			.vm_id = vm,
			.width = flags & PARALLEL ? num_placements : 1,
			.num_placements = flags & PARALLEL ? 1 : num_placements,
			.instances = to_user_pointer(eci),
		};

		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_ENGINE_CREATE,
					&create), 0);
		engines[i] = create.engine_id;
		syncobjs[i] = syncobj_create(fd, 0);
	};
	exec.num_batch_buffer = flags & PARALLEL ? num_placements : 1;

	sync[0].handle = syncobj_create(fd, 0);
	if (bo)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(data), addr,
					 bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t batches[MAX_INSTANCE];
		int e = i % n_engines;

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = batch_addr;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = flags & PARALLEL ?
			to_user_pointer(batches) : batch_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);

		if (flags & REBIND && i + 1 != n_execs) {
			sync[1].flags &= ~DRM_XE_SYNC_SIGNAL;
			xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size,
					   sync + 1, 1);

			sync[0].flags |= DRM_XE_SYNC_SIGNAL;
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, 0, bo, 0, addr,
						 bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm, 0,
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
		}

		if (flags & INVALIDATE && i + 1 != n_execs) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				igt_assert(syncobj_wait(fd, &syncobjs[e], 1,
							INT64_MAX, 0, NULL));
				igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			data = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				    PROT_WRITE, MAP_SHARED | MAP_FIXED |
				    MAP_ANONYMOUS, -1, 0);
			igt_assert(data != MAP_FAILED);
		}
	}

	for (i = 0; i < n_engines && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = (flags & INVALIDATE && n_execs) ? n_execs - 1 : 0;
	     i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: once-cm-%s
 * Description: Run compute mode virtual engine arg[1] test only once
 *
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: twice-cm-%s
 * Description: Run compute mode virtual engine arg[1] test twice
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: many-cm-%s
 * Description: Run compute mode virtual engine arg[1] test many times
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: many-engines-cm-%s
 * Description: Run compute mode virtual engine arg[1] test on many engines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: no-exec-cm-%s
 * Description: Run compute mode virtual engine arg[1] no-exec test
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @virtual-basic:			virtual basic
 * @virtual-userptr:			virtual userptr
 * @virtual-rebind:			virtual rebind
 * @virtual-userptr-rebind:		virtual userptr rebind
 * @virtual-userptr-invalidate:		virtual userptr invalidate
 * @virtual-userptr-invalidate-race:	virtual userptr invalidate racy
 */

static void
test_cm(int fd, int gt, int class, int n_engines, int n_execs,
	unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
	struct drm_xe_sync sync[1] = {
		{ .flags = DRM_XE_SYNC_USER_FENCE | DRM_XE_SYNC_SIGNAL,
	          .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	uint32_t engines[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint64_t exec_sync;
		uint32_t data;
	} *data;
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	int i, j, b, num_placements = 0;
	int map_fd = -1;

	igt_assert(n_engines <= MAX_N_ENGINES);

	for_each_hw_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[num_placements++] = *hwe;
	}
	if (num_placements < 2)
		return;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
			  DRM_XE_VM_CREATE_COMPUTE_MODE, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	if (flags & USERPTR) {
#define	MAP_ADDRESS	0x00007fadeadbe000
		if (flags & INVALIDATE) {
			data = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				    PROT_WRITE, MAP_SHARED | MAP_FIXED |
				    MAP_ANONYMOUS, -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
	} else {
		bo = xe_bo_create(fd, gt, vm, bo_size);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	for (i = 0; i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property ext = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_COMPUTE_MODE,
			.value = 1,
		};
		struct drm_xe_engine_create create = {
			.vm_id = vm,
			.width = 1,
			.num_placements = num_placements,
			.instances = to_user_pointer(eci),
			.extensions = to_user_pointer(&ext),
		};

		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_ENGINE_CREATE,
					&create), 0);
		engines[i] = create.engine_id;
	}

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	if (bo)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(data), addr,
					 bo_size, sync, 1);

#define ONE_SEC	1000
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, ONE_SEC);
	data[0].vm_sync = 0;

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		int e = i % n_engines;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].addr = addr + (char *)&data[i].exec_sync - (char *)data;

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		if (flags & REBIND && i + 1 != n_execs) {
			xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
				       NULL, ONE_SEC);
			xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, NULL,
					   0);

			sync[0].addr = to_user_pointer(&data[0].vm_sync);
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, 0, bo, 0, addr,
						 bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm, 0,
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
			xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE,
				       NULL, ONE_SEC);
			data[0].vm_sync = 0;
		}

		if (flags & INVALIDATE && i + 1 != n_execs) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				xe_wait_ufence(fd, &data[i].exec_sync,
					       USER_FENCE_VALUE, NULL, ONE_SEC);
				igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			if (flags & RACE) {
				map_fd = open("/tmp", O_TMPFILE | O_RDWR,
					      0x666);
				write(map_fd, data, bo_size);
				data = mmap((void *)MAP_ADDRESS, bo_size,
					    PROT_READ | PROT_WRITE, MAP_SHARED |
					    MAP_FIXED, map_fd, 0);
			} else {
				data = mmap((void *)MAP_ADDRESS, bo_size,
					    PROT_READ | PROT_WRITE, MAP_SHARED |
					    MAP_FIXED | MAP_ANONYMOUS, -1, 0);
			}
			igt_assert(data != MAP_FAILED);
		}
	}

	j = flags & INVALIDATE && n_execs ? n_execs - 1 : 0;
	for (i = j; i < n_execs; i++)
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE, NULL,
			       ONE_SEC);

	/* Wait for all execs to complete */
	if (flags & INVALIDATE)
		usleep(250000);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, ONE_SEC);

	for (i = (flags & INVALIDATE && n_execs) ? n_execs - 1 : 0;
	     i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_engines; i++)
		xe_engine_destroy(fd, engines[i]);

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	xe_vm_destroy(fd, vm);
}


igt_main
{
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "virtual-basic", VIRTUAL },
		{ "virtual-userptr", VIRTUAL | USERPTR },
		{ "virtual-rebind", VIRTUAL | REBIND },
		{ "virtual-userptr-rebind", VIRTUAL | USERPTR | REBIND },
		{ "virtual-userptr-invalidate", VIRTUAL | USERPTR |
			INVALIDATE },
		{ "virtual-userptr-invalidate-race", VIRTUAL | USERPTR |
			INVALIDATE | RACE },
		{ "parallel-basic", PARALLEL },
		{ "parallel-userptr", PARALLEL | USERPTR },
		{ "parallel-rebind", PARALLEL | REBIND },
		{ "parallel-userptr-rebind", PARALLEL | USERPTR | REBIND },
		{ "parallel-userptr-invalidate", PARALLEL | USERPTR |
			INVALIDATE },
		{ "parallel-userptr-invalidate-race", PARALLEL | USERPTR |
			INVALIDATE | RACE },
		{ NULL },
	};
	int gt;
	int class;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	igt_subtest("virtual-all-active")
		for_each_gt(fd, gt)
			for_each_hw_engine_class(class)
				test_all_active(fd, gt, class);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("once-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_exec(fd, gt, class, 1, 1,
						  s->flags);

		igt_subtest_f("twice-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_exec(fd, gt, class, 1, 2,
						  s->flags);

		igt_subtest_f("many-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_exec(fd, gt, class, 1,
						  s->flags & (REBIND | INVALIDATE) ?
						  64 : 1024,
						  s->flags);

		igt_subtest_f("many-engines-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_exec(fd, gt, class, 16,
						  s->flags & (REBIND | INVALIDATE) ?
						  64 : 1024,
						  s->flags);

		igt_subtest_f("no-exec-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_exec(fd, gt, class, 1, 0,
						  s->flags);

		if (s->flags & PARALLEL)
			continue;

		igt_subtest_f("once-cm-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_cm(fd, gt, class, 1, 1, s->flags);

		igt_subtest_f("twice-cm-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_cm(fd, gt, class, 1, 2, s->flags);

		igt_subtest_f("many-cm-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_cm(fd, gt, class, 1,
						s->flags & (REBIND | INVALIDATE) ?
						64 : 1024,
						s->flags);

		igt_subtest_f("many-engines-cm-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_cm(fd, gt, class, 16,
						s->flags & (REBIND | INVALIDATE) ?
						64 : 1024,
						s->flags);

		igt_subtest_f("no-exec-cm-%s", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_cm(fd, gt, class, 1, 0, s->flags);
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
