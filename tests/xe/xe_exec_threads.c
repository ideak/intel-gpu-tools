// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
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

#define MAX_N_ENGINES	16
#define MAX_INSTANCE	9
#define USERPTR		(0x1 << 0)
#define REBIND		(0x1 << 1)
#define INVALIDATE	(0x1 << 2)
#define RACE		(0x1 << 3)
#define SHARED_VM	(0x1 << 4)
#define FD		(0x1 << 5)
#define COMPUTE_MODE	(0x1 << 6)
#define MIXED_MODE	(0x1 << 7)
#define BALANCER	(0x1 << 8)
#define PARALLEL	(0x1 << 9)
#define VIRTUAL		(0x1 << 10)
#define HANG		(0x1 << 11)
#define REBIND_ERROR	(0x1 << 12)
#define BIND_ENGINE	(0x1 << 13)

pthread_barrier_t barrier;

static void
test_balancer(int fd, int gt, uint32_t vm, uint64_t addr, uint64_t userptr,
	      int class, int n_engines, int n_execs, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_sync sync_all[MAX_N_ENGINES];
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
	bool owns_vm = false, owns_fd = false;

	igt_assert(n_engines <= MAX_N_ENGINES);

	if (!fd) {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
		owns_fd = true;
	}

	if (!vm) {
		vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
		owns_vm = true;
	}

	for_each_hw_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[num_placements++] = *hwe;
	}
	igt_assert(num_placements > 1);

	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	if (flags & USERPTR) {
		if (flags & INVALIDATE) {
			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
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

	memset(sync_all, 0, sizeof(sync_all));
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
		sync_all[i].flags = DRM_XE_SYNC_SYNCOBJ;
		sync_all[i].handle = syncobjs[i];
	};
	exec.num_batch_buffer = flags & PARALLEL ? num_placements : 1;

	pthread_barrier_wait(&barrier);

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

		if (flags & REBIND && i && !(i & 0x1f)) {
			xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size,
					   sync_all, n_engines);

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

		if (flags & INVALIDATE && i && !(i & 0x1f)) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				for (j = 0; j < n_engines; ++j)
					igt_assert(syncobj_wait(fd,
								&syncobjs[j], 1,
								INT64_MAX, 0,
								NULL));
				igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		}
	}

	for (i = 0; i < n_engines; i++)
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
	if (owns_vm)
		xe_vm_destroy(fd, vm);
	if (owns_fd) {
		xe_device_put(fd);
		close(fd);
	}
}

static void
test_compute_mode(int fd, uint32_t vm, uint64_t addr, uint64_t userptr,
		  struct drm_xe_engine_class_instance *eci,
		  int n_engines, int n_execs, unsigned int flags)
{
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
	int i, j, b;
	int map_fd = -1;
	bool owns_vm = false, owns_fd = false;

	igt_assert(n_engines <= MAX_N_ENGINES);

	if (!fd) {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
		owns_fd = true;
	}

	if (!vm) {
		vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
				  XE_ENGINE_SET_PROPERTY_COMPUTE_MODE, 0);
		owns_vm = true;
	}

	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	if (flags & USERPTR) {
		if (flags & INVALIDATE) {
			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
	} else {
		bo = xe_bo_create(fd, eci->gt_id, 0, bo_size);
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

		engines[i] = xe_engine_create(fd, vm, eci,
					      to_user_pointer(&ext));
	};

	pthread_barrier_wait(&barrier);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	if (bo)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(data), addr,
					 bo_size, sync, 1);
#define THREE_SEC	3000
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, THREE_SEC);
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

		if (flags & REBIND && i && !(i & 0x1f)) {
			for (j = i - 0x20; j <= i; ++j)
				xe_wait_ufence(fd, &data[j].exec_sync,
					       USER_FENCE_VALUE,
					       NULL, THREE_SEC);
			xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size,
					   NULL, 0);

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
				       NULL, THREE_SEC);
			data[0].vm_sync = 0;
		}

		if (flags & INVALIDATE && i && !(i & 0x1f)) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				for (j = i == 0x20 ? 0 : i - 0x1f; j <= i; ++j)
					xe_wait_ufence(fd, &data[j].exec_sync,
						       USER_FENCE_VALUE,
						       NULL, THREE_SEC);
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
				data = mmap(from_user_pointer(userptr), bo_size,
					    PROT_READ | PROT_WRITE,
					    MAP_SHARED | MAP_FIXED,
					    map_fd, 0);
			} else {
				data = mmap(from_user_pointer(userptr), bo_size,
					    PROT_READ | PROT_WRITE,
					    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
					    -1, 0);
			}
			igt_assert(data != MAP_FAILED);
		}
	}

	j = flags & INVALIDATE ?
		(flags & RACE ? n_execs / 2 + 1 : n_execs - 1) : 0;
	for (i = j; i < n_execs; i++)
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE, NULL,
			       THREE_SEC);

	/* Wait for all execs to complete */
	if (flags & INVALIDATE)
		sleep(1);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, THREE_SEC);

	for (i = j; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_engines; i++)
		xe_engine_destroy(fd, engines[i]);

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	if (map_fd != -1)
		close(map_fd);
	if (owns_vm)
		xe_vm_destroy(fd, vm);
	if (owns_fd) {
		xe_device_put(fd);
		close(fd);
	}
}

static void
test_legacy_mode(int fd, uint32_t vm, uint64_t addr, uint64_t userptr,
		 struct drm_xe_engine_class_instance *eci, int n_engines,
		 int n_execs, int rebind_error_inject, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_sync sync_all[MAX_N_ENGINES];
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};
	uint32_t engines[MAX_N_ENGINES];
	uint32_t bind_engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, j, b, hang_engine = n_engines / 2;
	bool owns_vm = false, owns_fd = false;

	igt_assert(n_engines <= MAX_N_ENGINES);

	if (!fd) {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
		owns_fd = true;
	}

	if (!vm) {
		vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
		owns_vm = true;
	}

	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	if (flags & USERPTR) {
		if (flags & INVALIDATE) {
			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
	} else {
		bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	memset(sync_all, 0, sizeof(sync_all));
	for (i = 0; i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property preempt_timeout = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_PREEMPTION_TIMEOUT,
			.value = 1000,
		};
		uint64_t ext = to_user_pointer(&preempt_timeout);

		if (flags & HANG && i == hang_engine)
			engines[i] = xe_engine_create(fd, vm, eci, ext);
		else
			engines[i] = xe_engine_create(fd, vm, eci, 0);
		if (flags & BIND_ENGINE)
			bind_engines[i] = xe_bind_engine_create(fd, vm, 0);
		else
			bind_engines[i] = 0;
		syncobjs[i] = syncobj_create(fd, 0);
		sync_all[i].flags = DRM_XE_SYNC_SYNCOBJ;
		sync_all[i].handle = syncobjs[i];
	};

	pthread_barrier_wait(&barrier);

	sync[0].handle = syncobj_create(fd, 0);
	if (bo)
		xe_vm_bind_async(fd, vm, bind_engines[0], bo, 0, addr,
				 bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, bind_engines[0],
					 to_user_pointer(data), addr,
					 bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t spin_addr = addr + spin_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t exec_addr;
		int e = i % n_engines;

		if (flags & HANG && e == hang_engine && i == e) {
			xe_spin_init(&data[i].spin, spin_addr, false);
			exec_addr = spin_addr;
		} else {
			b = 0;
			data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data[i].batch[b++] = sdi_addr;
			data[i].batch[b++] = sdi_addr >> 32;
			data[i].batch[b++] = 0xc0ffee;
			data[i].batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			exec_addr = batch_addr;
		}

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = exec_addr;
		if (e != i && !(flags & HANG))
			 syncobj_reset(fd, &syncobjs[e], 1);
		if ((flags & HANG && e == hang_engine) ||
		    rebind_error_inject > 0) {
			int err;

			do {
				err = igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec);
			} while (err && errno == ENOMEM);
		} else {
			xe_exec(fd, &exec);
		}

		if (flags & REBIND && i &&
		    (!(i & 0x1f) || rebind_error_inject == i)) {
#define INJECT_ERROR	(0x1 << 31)
			if (rebind_error_inject == i)
				__xe_vm_bind_assert(fd, vm, bind_engines[e],
						    0, 0, addr, bo_size,
						    XE_VM_BIND_OP_UNMAP |
						    XE_VM_BIND_FLAG_ASYNC |
						    INJECT_ERROR, sync_all,
						    n_engines, 0, 0);
			else
				xe_vm_unbind_async(fd, vm, bind_engines[e],
						   0, addr, bo_size,
						   sync_all, n_engines);

			sync[0].flags |= DRM_XE_SYNC_SIGNAL;
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, bind_engines[e],
						 bo, 0, addr, bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm,
							 bind_engines[e],
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
		}

		if (flags & INVALIDATE && i && !(i & 0x1f)) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				for (j = 0; j < n_engines; ++j)
					igt_assert(syncobj_wait(fd,
								&syncobjs[j], 1,
								INT64_MAX, 0,
								NULL));
				if (!(flags & HANG && e == hang_engine))
					igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		}
	}

	for (i = 0; i < n_engines; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, bind_engines[0], 0, addr,
			   bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = flags & INVALIDATE ? n_execs - 1 : 0;
	     i < n_execs; i++) {
		int e = i % n_engines;

		if (flags & HANG && e == hang_engine)
			igt_assert_eq(data[i].data, 0x0);
		else
			igt_assert_eq(data[i].data, 0xc0ffee);
	}

	syncobj_destroy(fd, sync[0].handle);
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
	if (owns_vm)
		xe_vm_destroy(fd, vm);
	if (owns_fd) {
		xe_device_put(fd);
		close(fd);
	}
}

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	uint64_t addr;
	uint64_t userptr;
	int class;
	int fd;
	int gt;
	uint32_t vm_legacy_mode;
	uint32_t vm_compute_mode;
	struct drm_xe_engine_class_instance *eci;
	int n_engine;
	int n_exec;
	int flags;
	int rebind_error_inject;
	bool *go;
};

static void *thread(void *data)
{
	struct thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->flags & PARALLEL || t->flags & VIRTUAL)
		test_balancer(t->fd, t->gt, t->vm_legacy_mode, t->addr,
			      t->userptr, t->class, t->n_engine, t->n_exec,
			      t->flags);
	else if (t->flags & COMPUTE_MODE)
		test_compute_mode(t->fd, t->vm_compute_mode, t->addr,
				  t->userptr, t->eci, t->n_engine, t->n_exec,
				  t->flags);
	else
		test_legacy_mode(t->fd, t->vm_legacy_mode, t->addr, t->userptr,
				 t->eci, t->n_engine, t->n_exec,
				 t->rebind_error_inject, t->flags);

	return NULL;
}

struct vm_thread_data {
	pthread_t thread;
	struct drm_xe_vm_bind_op_error_capture *capture;
	int fd;
	int vm;
};

static void *vm_async_ops_err_thread(void *data)
{
	struct vm_thread_data *args = data;
	int fd = args->fd;
	int ret;

	struct drm_xe_wait_user_fence wait = {
		.vm_id = args->vm,
		.op = DRM_XE_UFENCE_WAIT_NEQ,
		.flags = DRM_XE_UFENCE_WAIT_VM_ERROR,
		.mask = DRM_XE_UFENCE_WAIT_U32,
#define BASICALLY_FOREVER	0xffffffffffff
		.timeout = BASICALLY_FOREVER,
	};

	ret = igt_ioctl(fd, DRM_IOCTL_XE_WAIT_USER_FENCE, &wait);

	while (!ret) {
		struct drm_xe_vm_bind bind = {
			.vm_id = args->vm,
			.num_binds = 1,
			.bind.op = XE_VM_BIND_OP_RESTART,
		};

		/* Restart and wait for next error */
		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_VM_BIND,
					&bind), 0);
		args->capture->error = 0;
		ret = igt_ioctl(fd, DRM_IOCTL_XE_WAIT_USER_FENCE, &wait);
	}

	return NULL;
}

static void threads(int fd, int flags)
{
	struct thread_data *threads_data;
	struct drm_xe_engine_class_instance *hwe;
	uint64_t addr = 0x1a0000;
	uint64_t userptr = 0x00007000eadbe000;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int n_hw_engines = 0, class;
	uint64_t i = 0;
	uint32_t vm_legacy_mode = 0, vm_compute_mode = 0;
	struct drm_xe_vm_bind_op_error_capture capture = {};
	struct vm_thread_data vm_err_thread = {};
	bool go = false;
	int n_threads = 0;
	int gt;

	for_each_hw_engine(fd, hwe)
		++n_hw_engines;

	if (flags & BALANCER) {
		for_each_gt(fd, gt)
			for_each_hw_engine_class(class) {
				int num_placements = 0;

				for_each_hw_engine(fd, hwe) {
					if (hwe->engine_class != class ||
					    hwe->gt_id != gt)
						continue;
					++num_placements;
				}

				if (num_placements > 1)
					n_hw_engines += 2;
			}
	}

	threads_data = calloc(n_hw_engines, sizeof(*threads_data));
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	if (flags & SHARED_VM) {
		struct drm_xe_ext_vm_set_property ext = {
			.base.next_extension = 0,
			.base.name = XE_VM_EXTENSION_SET_PROPERTY,
			.property =
				XE_VM_PROPERTY_BIND_OP_ERROR_CAPTURE_ADDRESS,
			.value = to_user_pointer(&capture),
		};

		vm_legacy_mode = xe_vm_create(fd,
					      DRM_XE_VM_CREATE_ASYNC_BIND_OPS,
					      to_user_pointer(&ext));
		vm_compute_mode = xe_vm_create(fd,
					       DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
					       XE_ENGINE_SET_PROPERTY_COMPUTE_MODE,
					       0);

		vm_err_thread.capture = &capture;
		vm_err_thread.fd = fd;
		vm_err_thread.vm = vm_legacy_mode;
		pthread_create(&vm_err_thread.thread, 0,
			       vm_async_ops_err_thread, &vm_err_thread);

	}

	for_each_hw_engine(fd, hwe) {
		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
#define ADDRESS_SHIFT	39
		threads_data[i].addr = addr | (i << ADDRESS_SHIFT);
		threads_data[i].userptr = userptr | (i << ADDRESS_SHIFT);
		if (flags & FD)
			threads_data[i].fd = 0;
		else
			threads_data[i].fd = fd;
		threads_data[i].vm_legacy_mode = vm_legacy_mode;
		threads_data[i].vm_compute_mode = vm_compute_mode;
		threads_data[i].eci = hwe;
#define N_ENGINE	16
		threads_data[i].n_engine = N_ENGINE;
#define N_EXEC		1024
		threads_data[i].n_exec = N_EXEC;
		if (flags & REBIND_ERROR)
			threads_data[i].rebind_error_inject =
				(N_EXEC / (n_hw_engines + 1)) * (i + 1);
		else
			threads_data[i].rebind_error_inject = -1;
		threads_data[i].flags = flags;
		if (flags & MIXED_MODE) {
			threads_data[i].flags &= ~MIXED_MODE;
			if (i & 1)
				threads_data[i].flags |= COMPUTE_MODE;
		}
		threads_data[i].go = &go;

		++n_threads;
		pthread_create(&threads_data[i].thread, 0, thread,
			       &threads_data[i]);
		++i;
	}

	if (flags & BALANCER) {
		for_each_gt(fd, gt)
			for_each_hw_engine_class(class) {
				int num_placements = 0;

				for_each_hw_engine(fd, hwe) {
					if (hwe->engine_class != class ||
					    hwe->gt_id != gt)
						continue;
					++num_placements;
				}

				if (num_placements > 1) {
					threads_data[i].mutex = &mutex;
					threads_data[i].cond = &cond;
					if (flags & SHARED_VM)
						threads_data[i].addr = addr |
							(i << ADDRESS_SHIFT);
					else
						threads_data[i].addr = addr;
					threads_data[i].userptr = userptr |
						(i << ADDRESS_SHIFT);
					if (flags & FD)
						threads_data[i].fd = 0;
					else
						threads_data[i].fd = fd;
					threads_data[i].gt = gt;
					threads_data[i].vm_legacy_mode =
						vm_legacy_mode;
					threads_data[i].class = class;
					threads_data[i].n_engine = N_ENGINE;
					threads_data[i].n_exec = N_EXEC;
					threads_data[i].flags = flags;
					threads_data[i].flags &= ~BALANCER;
					threads_data[i].flags |= VIRTUAL;
					threads_data[i].go = &go;

					++n_threads;
					pthread_create(&threads_data[i].thread, 0,
						       thread, &threads_data[i]);
					++i;

					threads_data[i].mutex = &mutex;
					threads_data[i].cond = &cond;
					if (flags & SHARED_VM)
						threads_data[i].addr = addr |
							(i << ADDRESS_SHIFT);
					else
						threads_data[i].addr = addr;
					threads_data[i].userptr = userptr |
						(i << ADDRESS_SHIFT);
					if (flags & FD)
						threads_data[i].fd = 0;
					else
						threads_data[i].fd = fd;
					threads_data[i].vm_legacy_mode =
						vm_legacy_mode;
					threads_data[i].class = class;
					threads_data[i].n_engine = N_ENGINE;
					threads_data[i].n_exec = N_EXEC;
					threads_data[i].flags = flags;
					threads_data[i].flags &= ~BALANCER;
					threads_data[i].flags |= PARALLEL;
					threads_data[i].go = &go;

					++n_threads;
					pthread_create(&threads_data[i].thread, 0,
						       thread, &threads_data[i]);
					++i;
				}
			}
	}

	pthread_barrier_init(&barrier, NULL, n_threads);

	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < n_hw_engines; ++i)
		pthread_join(threads_data[i].thread, NULL);

	if (vm_legacy_mode)
		xe_vm_destroy(fd, vm_legacy_mode);
	if (vm_compute_mode)
		xe_vm_destroy(fd, vm_compute_mode);
	free(threads_data);
	if (flags & SHARED_VM)
		pthread_join(vm_err_thread.thread, NULL);
	pthread_barrier_destroy(&barrier);
}

igt_main
{
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "userptr", USERPTR },
		{ "rebind", REBIND },
		{ "rebind-bindengine", REBIND | BIND_ENGINE },
		{ "userptr-rebind", USERPTR | REBIND },
		{ "userptr-invalidate", USERPTR | INVALIDATE },
		{ "userptr-invalidate-race", USERPTR | INVALIDATE | RACE },
		{ "shared-vm-basic", SHARED_VM },
		{ "shared-vm-userptr", SHARED_VM | USERPTR },
		{ "shared-vm-rebind", SHARED_VM | REBIND },
		{ "shared-vm-rebind-bindengine", SHARED_VM | REBIND |
			BIND_ENGINE },
		{ "shared-vm-userptr-rebind", SHARED_VM | USERPTR | REBIND },
		{ "shared-vm-rebind-err", SHARED_VM | REBIND | REBIND_ERROR },
		{ "shared-vm-userptr-rebind-err", SHARED_VM | USERPTR |
			REBIND | REBIND_ERROR},
		{ "shared-vm-userptr-invalidate", SHARED_VM | USERPTR |
			INVALIDATE },
		{ "shared-vm-userptr-invalidate-race", SHARED_VM | USERPTR |
			INVALIDATE | RACE },
		{ "fd-basic", FD },
		{ "fd-userptr", FD | USERPTR },
		{ "fd-rebind", FD | REBIND },
		{ "fd-userptr-rebind", FD | USERPTR | REBIND },
		{ "fd-userptr-invalidate", FD | USERPTR | INVALIDATE },
		{ "fd-userptr-invalidate-race", FD | USERPTR | INVALIDATE |
			RACE },
		{ "hang-basic", HANG | 0 },
		{ "hang-userptr", HANG | USERPTR },
		{ "hang-rebind", HANG | REBIND },
		{ "hang-userptr-rebind", HANG | USERPTR | REBIND },
		{ "hang-userptr-invalidate", HANG | USERPTR | INVALIDATE },
		{ "hang-userptr-invalidate-race", HANG | USERPTR | INVALIDATE |
			RACE },
		{ "hang-shared-vm-basic", HANG | SHARED_VM },
		{ "hang-shared-vm-userptr", HANG | SHARED_VM | USERPTR },
		{ "hang-shared-vm-rebind", HANG | SHARED_VM | REBIND },
		{ "hang-shared-vm-userptr-rebind", HANG | SHARED_VM | USERPTR |
			REBIND },
		{ "hang-shared-vm-rebind-err", HANG | SHARED_VM | REBIND |
			REBIND_ERROR },
		{ "hang-shared-vm-userptr-rebind-err", HANG | SHARED_VM |
			USERPTR | REBIND | REBIND_ERROR },
		{ "hang-shared-vm-userptr-invalidate", HANG | SHARED_VM |
			USERPTR | INVALIDATE },
		{ "hang-shared-vm-userptr-invalidate-race", HANG | SHARED_VM |
			USERPTR | INVALIDATE | RACE },
		{ "hang-fd-basic", HANG | FD },
		{ "hang-fd-userptr", HANG | FD | USERPTR },
		{ "hang-fd-rebind", HANG | FD | REBIND },
		{ "hang-fd-userptr-rebind", HANG | FD | USERPTR | REBIND },
		{ "hang-fd-userptr-invalidate", HANG | FD | USERPTR |
			INVALIDATE },
		{ "hang-fd-userptr-invalidate-race", HANG | FD | USERPTR |
			INVALIDATE | RACE },
		{ "bal-basic", BALANCER },
		{ "bal-userptr", BALANCER | USERPTR },
		{ "bal-rebind", BALANCER | REBIND },
		{ "bal-userptr-rebind", BALANCER | USERPTR | REBIND },
		{ "bal-userptr-invalidate", BALANCER | USERPTR | INVALIDATE },
		{ "bal-userptr-invalidate-race", BALANCER | USERPTR |
			INVALIDATE | RACE },
		{ "bal-shared-vm-basic", BALANCER | SHARED_VM },
		{ "bal-shared-vm-userptr", BALANCER | SHARED_VM | USERPTR },
		{ "bal-shared-vm-rebind", BALANCER | SHARED_VM | REBIND },
		{ "bal-shared-vm-userptr-rebind", BALANCER | SHARED_VM |
			USERPTR | REBIND },
		{ "bal-shared-vm-userptr-invalidate", BALANCER | SHARED_VM |
			USERPTR | INVALIDATE },
		{ "bal-shared-vm-userptr-invalidate-race", BALANCER |
			SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "bal-fd-basic", BALANCER | FD },
		{ "bal-fd-userptr", BALANCER | FD | USERPTR },
		{ "bal-fd-rebind", BALANCER | FD | REBIND },
		{ "bal-fd-userptr-rebind", BALANCER | FD | USERPTR | REBIND },
		{ "bal-fd-userptr-invalidate", BALANCER | FD | USERPTR |
			INVALIDATE },
		{ "bal-fd-userptr-invalidate-race", BALANCER | FD | USERPTR |
			INVALIDATE | RACE },
		{ "cm-basic", COMPUTE_MODE },
		{ "cm-userptr", COMPUTE_MODE | USERPTR },
		{ "cm-rebind", COMPUTE_MODE | REBIND },
		{ "cm-userptr-rebind", COMPUTE_MODE | USERPTR | REBIND },
		{ "cm-userptr-invalidate", COMPUTE_MODE | USERPTR |
			INVALIDATE },
		{ "cm-userptr-invalidate-race", COMPUTE_MODE | USERPTR |
			INVALIDATE | RACE },
		{ "cm-shared-vm-basic", COMPUTE_MODE | SHARED_VM },
		{ "cm-shared-vm-userptr", COMPUTE_MODE | SHARED_VM | USERPTR },
		{ "cm-shared-vm-rebind", COMPUTE_MODE | SHARED_VM | REBIND },
		{ "cm-shared-vm-userptr-rebind", COMPUTE_MODE | SHARED_VM |
			USERPTR | REBIND },
		{ "cm-shared-vm-userptr-invalidate", COMPUTE_MODE | SHARED_VM |
			USERPTR | INVALIDATE },
		{ "cm-shared-vm-userptr-invalidate-race", COMPUTE_MODE |
			SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "cm-fd-basic", COMPUTE_MODE | FD },
		{ "cm-fd-userptr", COMPUTE_MODE | FD | USERPTR },
		{ "cm-fd-rebind", COMPUTE_MODE | FD | REBIND },
		{ "cm-fd-userptr-rebind", COMPUTE_MODE | FD | USERPTR |
			REBIND },
		{ "cm-fd-userptr-invalidate", COMPUTE_MODE | FD |
			USERPTR | INVALIDATE },
		{ "cm-fd-userptr-invalidate-race", COMPUTE_MODE | FD |
			USERPTR | INVALIDATE | RACE },
		{ "mixed-basic", MIXED_MODE },
		{ "mixed-userptr", MIXED_MODE | USERPTR },
		{ "mixed-rebind", MIXED_MODE | REBIND },
		{ "mixed-userptr-rebind", MIXED_MODE | USERPTR | REBIND },
		{ "mixed-userptr-invalidate", MIXED_MODE | USERPTR |
			INVALIDATE },
		{ "mixed-userptr-invalidate-race", MIXED_MODE | USERPTR |
			INVALIDATE | RACE },
		{ "mixed-shared-vm-basic", MIXED_MODE | SHARED_VM },
		{ "mixed-shared-vm-userptr", MIXED_MODE | SHARED_VM |
			USERPTR },
		{ "mixed-shared-vm-rebind", MIXED_MODE | SHARED_VM | REBIND },
		{ "mixed-shared-vm-userptr-rebind", MIXED_MODE | SHARED_VM |
			USERPTR | REBIND },
		{ "mixed-shared-vm-userptr-invalidate", MIXED_MODE |
			SHARED_VM | USERPTR | INVALIDATE },
		{ "mixed-shared-vm-userptr-invalidate-race", MIXED_MODE |
			SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "mixed-fd-basic", MIXED_MODE | FD },
		{ "mixed-fd-userptr", MIXED_MODE | FD | USERPTR },
		{ "mixed-fd-rebind", MIXED_MODE | FD | REBIND },
		{ "mixed-fd-userptr-rebind", MIXED_MODE | FD | USERPTR |
			REBIND },
		{ "mixed-fd-userptr-invalidate", MIXED_MODE | FD |
			USERPTR | INVALIDATE },
		{ "mixed-fd-userptr-invalidate-race", MIXED_MODE | FD |
			USERPTR | INVALIDATE | RACE },
		{ "bal-mixed-basic", BALANCER | MIXED_MODE },
		{ "bal-mixed-userptr", BALANCER | MIXED_MODE | USERPTR },
		{ "bal-mixed-rebind", BALANCER | MIXED_MODE | REBIND },
		{ "bal-mixed-userptr-rebind", BALANCER | MIXED_MODE | USERPTR |
			REBIND },
		{ "bal-mixed-userptr-invalidate", BALANCER | MIXED_MODE |
			USERPTR | INVALIDATE },
		{ "bal-mixed-userptr-invalidate-race", BALANCER | MIXED_MODE |
			USERPTR | INVALIDATE | RACE },
		{ "bal-mixed-shared-vm-basic", BALANCER | MIXED_MODE |
			SHARED_VM },
		{ "bal-mixed-shared-vm-userptr", BALANCER | MIXED_MODE |
			SHARED_VM | USERPTR },
		{ "bal-mixed-shared-vm-rebind", BALANCER | MIXED_MODE |
			SHARED_VM | REBIND },
		{ "bal-mixed-shared-vm-userptr-rebind", BALANCER | MIXED_MODE |
			SHARED_VM | USERPTR | REBIND },
		{ "bal-mixed-shared-vm-userptr-invalidate", BALANCER |
			MIXED_MODE | SHARED_VM | USERPTR | INVALIDATE },
		{ "bal-mixed-shared-vm-userptr-invalidate-race", BALANCER |
			MIXED_MODE | SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "bal-mixed-fd-basic", BALANCER | MIXED_MODE | FD },
		{ "bal-mixed-fd-userptr", BALANCER | MIXED_MODE | FD |
			USERPTR },
		{ "bal-mixed-fd-rebind", BALANCER | MIXED_MODE | FD | REBIND },
		{ "bal-mixed-fd-userptr-rebind", BALANCER | MIXED_MODE | FD |
			USERPTR | REBIND },
		{ "bal-mixed-fd-userptr-invalidate", BALANCER | MIXED_MODE |
			FD | USERPTR | INVALIDATE },
		{ "bal-mixed-fd-userptr-invalidate-race", BALANCER |
			MIXED_MODE | FD | USERPTR | INVALIDATE | RACE },
		{ NULL },
	};
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("threads-%s", s->name)
			threads(fd, s->flags);
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
