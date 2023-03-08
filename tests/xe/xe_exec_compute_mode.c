// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf compute machine functionality
 * Category: Hardware building block
 * Sub-category: execbuf
 * Functionality: compute machine
 * Test category: functionality test
 */

#include <fcntl.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include <string.h>

#define MAX_N_ENGINES 16
#define USERPTR		(0x1 << 0)
#define REBIND		(0x1 << 1)
#define INVALIDATE	(0x1 << 2)
#define RACE		(0x1 << 3)
#define BIND_ENGINE	(0x1 << 4)
#define VM_FOR_BO	(0x1 << 5)
#define ENGINE_EARLY	(0x1 << 6)

/**
 * SUBTEST: twice-%s
 * Description: Run %arg[1] compute machine test twice
 * Run type: BAT
 *
 * SUBTEST: once-%s
 * Description: Run %arg[1] compute machine test only once
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: many-%s
 * Description: Run %arg[1] compute machine test many times
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @basic:				basic
 * @preempt-fence-early:		preempt fence early
 * @userptr:				userptr
 * @rebind:				rebind
 * @userptr-rebind:			userptr rebind
 * @userptr-invalidate:			userptr invalidate
 * @userptr-invalidate-race:		userptr invalidate race
 * @bindengine:				bindengine
 * @bindengine-userptr:			bindengine userptr
 * @bindengine-rebind:			bindengine rebind
 * @bindengine-userptr-rebind:		bindengine userptr rebind
 * @bindengine-userptr-invalidate:	bindengine userptr invalidate
 * @bindengine-userptr-invalidate-race:	bindengine-userptr invalidate race
 */

/**
 *
 * SUBTEST: many-engines-%s
 * Description: Run %arg[1] compute machine test on many engines
 *
 * arg[1]:
 *
 * @basic:				basic
 * @preempt-fence-early:		preempt fence early
 * @userptr:				userptr
 * @rebind:				rebind
 * @userptr-rebind:			userptr rebind
 * @userptr-invalidate:			userptr invalidate
 * @bindengine:				bindengine
 * @bindengine-userptr:			bindengine userptr
 * @bindengine-rebind:			bindengine rebind
 * @bindengine-userptr-rebind:		bindengine userptr rebind
 * @bindengine-userptr-invalidate:	bindengine userptr invalidate
 */
static void
test_exec(int fd, struct drm_xe_engine_class_instance *eci,
	  int n_engines, int n_execs, unsigned int flags)
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
	uint32_t bind_engines[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint64_t exec_sync;
		uint32_t data;
	} *data;
	int i, j, b;
	int map_fd = -1;

	igt_assert(n_engines <= MAX_N_ENGINES);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
			  DRM_XE_VM_CREATE_COMPUTE_MODE, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	for (i = 0; (flags & ENGINE_EARLY) && i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property ext = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_COMPUTE_MODE,
			.value = 1,
		};

		engines[i] = xe_engine_create(fd, vm, eci,
					      to_user_pointer(&ext));
		if (flags & BIND_ENGINE)
			bind_engines[i] =
				xe_bind_engine_create(fd, vm, 0);
		else
			bind_engines[i] = 0;
	};

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
		bo = xe_bo_create(fd, eci->gt_id, flags & VM_FOR_BO ? vm : 0,
				  bo_size);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	for (i = 0; !(flags & ENGINE_EARLY) && i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property ext = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_COMPUTE_MODE,
			.value = 1,
		};

		engines[i] = xe_engine_create(fd, vm, eci,
					      to_user_pointer(&ext));
		if (flags & BIND_ENGINE)
			bind_engines[i] =
				xe_bind_engine_create(fd, vm, 0);
		else
			bind_engines[i] = 0;
	};

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	if (bo)
		xe_vm_bind_async(fd, vm, bind_engines[0], bo, 0, addr,
				 bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, bind_engines[0],
					 to_user_pointer(data), addr,
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
		data[i].batch[b++] = MI_STORE_DWORD_IMM;
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
			xe_vm_unbind_async(fd, vm, bind_engines[e], 0,
					   addr, bo_size, NULL, 0);

			sync[0].addr = to_user_pointer(&data[0].vm_sync);
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, bind_engines[e], bo,
						 0, addr, bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm,
							 bind_engines[e],
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

	j = flags & INVALIDATE ? n_execs - 1 : 0;
	for (i = j; i < n_execs; i++)
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE, NULL,
			       ONE_SEC);

	/* Wait for all execs to complete */
	if (flags & INVALIDATE)
		usleep(250000);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, bind_engines[0], 0, addr, bo_size,
			   sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, ONE_SEC);

	for (i = j; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_engines; i++) {
		xe_engine_destroy(fd, engines[i]);
		if (bind_engines[i])
			xe_engine_destroy(fd, bind_engines[i]);
	}

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	xe_vm_destroy(fd, vm);
	if (map_fd != -1)
		close(map_fd);
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "preempt-fence-early", VM_FOR_BO | ENGINE_EARLY },
		{ "userptr", USERPTR },
		{ "rebind", REBIND },
		{ "userptr-rebind", USERPTR | REBIND },
		{ "userptr-invalidate", USERPTR | INVALIDATE },
		{ "userptr-invalidate-race", USERPTR | INVALIDATE | RACE },
		{ "bindengine", BIND_ENGINE },
		{ "bindengine-userptr", BIND_ENGINE | USERPTR },
		{ "bindengine-rebind",  BIND_ENGINE | REBIND },
		{ "bindengine-userptr-rebind",  BIND_ENGINE | USERPTR |
			REBIND },
		{ "bindengine-userptr-invalidate",  BIND_ENGINE | USERPTR |
			INVALIDATE },
		{ "bindengine-userptr-invalidate-race", BIND_ENGINE | USERPTR |
			INVALIDATE | RACE },
		{ NULL },
	};
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("once-%s", s->name)
			for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1, 1, s->flags);

		igt_subtest_f("twice-%s", s->name)
			for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1, 2, s->flags);

		igt_subtest_f("many-%s", s->name)
			for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 128,
					  s->flags);

		if (s->flags & RACE)
			continue;

		igt_subtest_f("many-engines-%s", s->name)
			for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 16,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 128,
					  s->flags);
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
