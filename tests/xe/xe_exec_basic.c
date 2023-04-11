// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf functionality
 * Category: Hardware building block
 * Sub-category: execbuf
 * Test category: functionality test
 */

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
#define DEFER_ALLOC	(0x1 << 5)
#define DEFER_BIND	(0x1 << 6)

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
 * SUBTEST: many-engines-many-vm-%s
 * Description: Run %arg[1] test on many engines and many VMs
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
 * @basic:				basic
 * @basic-defer-mmap:			basic defer mmap
 * @basic-defer-bind:			basic defer bind
 * @userptr:				userptr
 * @rebind:				rebind
 * @userptr-rebind:			userptr rebind
 * @userptr-invalidate:			userptr invalidate
 * @userptr-invalidate-race:		userptr invalidate racy
 * @bindengine:				bind engine
 * @bindengine-userptr:			bind engine userptr description
 * @bindengine-rebind:			bind engine rebind description
 * @bindengine-userptr-rebind:		bind engine userptr rebind
 * @bindengine-userptr-invalidate:	bind engine userptr invalidate
 * @bindengine-userptr-invalidate-race:	bind engine userptr invalidate racy
 */

static void
test_exec(int fd, struct drm_xe_engine_class_instance *eci,
	  int n_engines, int n_execs, int n_vm, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t addr[MAX_N_ENGINES];
	uint32_t vm[MAX_N_ENGINES];
	uint32_t engines[MAX_N_ENGINES];
	uint32_t bind_engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	uint32_t bind_syncobjs[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert(n_engines <= MAX_N_ENGINES);
	igt_assert(n_vm <= MAX_N_ENGINES);

	for (i = 0; i < n_vm; ++i)
		vm[i] = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	addr[0] = 0x1a0000;
	for (i = 1; i < MAX_N_ENGINES; ++i)
		addr[i] = addr[i - 1] + (0x1ull << 32);

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
		if (flags & DEFER_ALLOC) {
			bo = xe_bo_create_flags(fd, n_vm == 1 ? vm[0] : 0,
						bo_size,
						vram_if_possible(fd, eci->gt_id) |
						XE_GEM_CREATE_FLAG_DEFER_BACKING);
		} else {
			bo = xe_bo_create(fd, eci->gt_id, n_vm == 1 ? vm[0] : 0,
					  bo_size);
		}
		if (!(flags & DEFER_BIND))
			data = xe_bo_map(fd, bo, bo_size);
	}

	for (i = 0; i < n_engines; i++) {
		uint32_t __vm = vm[i % n_vm];

		engines[i] = xe_engine_create(fd, __vm, eci, 0);
		if (flags & BIND_ENGINE)
			bind_engines[i] = xe_bind_engine_create(fd, __vm, 0);
		else
			bind_engines[i] = 0;
		syncobjs[i] = syncobj_create(fd, 0);
		bind_syncobjs[i] = syncobj_create(fd, 0);
	};

	for (i = 0; i < n_vm; ++i) {
		sync[0].handle = bind_syncobjs[i];
		if (bo)
			xe_vm_bind_async(fd, vm[i], bind_engines[i], bo, 0,
					 addr[i], bo_size, sync, 1);
		else
			xe_vm_bind_userptr_async(fd, vm[i], bind_engines[i],
						 to_user_pointer(data), addr[i],
						 bo_size, sync, 1);
	}

	if (flags & DEFER_BIND)
		data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_execs; i++) {
		int cur_vm = i % n_vm;
		uint64_t __addr = addr[cur_vm];
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = __addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = __addr + sdi_offset;
		int e = i % n_engines;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[0].handle = bind_syncobjs[cur_vm];
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);

		if (flags & REBIND && i + 1 != n_execs) {
			uint32_t __vm = vm[cur_vm];

			sync[1].flags &= ~DRM_XE_SYNC_SIGNAL;
			xe_vm_unbind_async(fd, __vm, bind_engines[e], 0,
					   __addr, bo_size, sync + 1, 1);

			sync[0].flags |= DRM_XE_SYNC_SIGNAL;
			addr[i % n_vm] += bo_size;
			__addr = addr[i % n_vm];
			if (bo)
				xe_vm_bind_async(fd, __vm, bind_engines[e], bo,
						 0, __addr, bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, __vm,
							 bind_engines[e],
							 to_user_pointer(data),
							 __addr, bo_size, sync,
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

	for (i = 0; i < n_vm; i++)
		igt_assert(syncobj_wait(fd, &bind_syncobjs[i], 1, INT64_MAX, 0,
					NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	for (i = 0; i < n_vm; ++i) {
		syncobj_reset(fd, &sync[0].handle, 1);
		xe_vm_unbind_async(fd, vm[i], bind_engines[i], 0, addr[i],
				   bo_size, sync, 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1,
					INT64_MAX, 0, NULL));
	}

	for (i = (flags & INVALIDATE && n_execs) ? n_execs - 1 : 0;
	     i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
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
	for (i = 0; i < n_vm; ++i) {
		syncobj_destroy(fd, bind_syncobjs[i]);
		xe_vm_destroy(fd, vm[i]);
	}
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "basic-defer-mmap", DEFER_ALLOC },
		{ "basic-defer-bind", DEFER_ALLOC | DEFER_BIND },
		{ "userptr", USERPTR },
		{ "rebind", REBIND },
		{ "userptr-rebind", USERPTR | REBIND },
		{ "userptr-invalidate", USERPTR | INVALIDATE },
		{ "userptr-invalidate-race", USERPTR | INVALIDATE | RACE },
		{ "bindengine", BIND_ENGINE },
		{ "bindengine-userptr", BIND_ENGINE | USERPTR },
		{ "bindengine-rebind", BIND_ENGINE | REBIND },
		{ "bindengine-userptr-rebind", BIND_ENGINE | USERPTR | REBIND },
		{ "bindengine-userptr-invalidate", BIND_ENGINE | USERPTR |
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
			xe_for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1, 1, 1, s->flags);

		igt_subtest_f("twice-%s", s->name)
			xe_for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1, 2, 1, s->flags);

		igt_subtest_f("many-%s", s->name)
			xe_for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 1024, 1,
					  s->flags);

		igt_subtest_f("many-engines-%s", s->name)
			xe_for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 16,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 1024, 1,
					  s->flags);

		igt_subtest_f("many-engines-many-vm-%s", s->name)
			xe_for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 16,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 1024, 16,
					  s->flags);

		igt_subtest_f("no-exec-%s", s->name)
			xe_for_each_hw_engine(fd, hwe)
				test_exec(fd, hwe, 1, 0, 1, s->flags);
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
