// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Check VMA eviction
 * Category: Software building block
 * Sub-category: VMA
 * Functionality: evict
 * GPU requirements: GPU needs to have dedicated VRAM
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include <string.h>

#define MAX_N_ENGINES 16
#define MULTI_VM	(0x1 << 0)
#define THREADED	(0x1 << 1)
#define MIXED_THREADS	(0x1 << 2)
#define LEGACY_THREAD	(0x1 << 3)
#define COMPUTE_THREAD	(0x1 << 4)
#define EXTERNAL_OBJ	(0x1 << 5)
#define BIND_ENGINE	(0x1 << 6)

static void
test_evict(int fd, struct drm_xe_engine_class_instance *eci,
	   int n_engines, int n_execs, size_t bo_size,
	   unsigned long flags, pthread_barrier_t *barrier)
{
	uint32_t vm, vm2, vm3;
	uint32_t bind_engines[3] = { 0, 0, 0 };
	uint64_t addr = 0x100000000, base_addr = 0x100000000;
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};
	uint32_t engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	uint32_t *bo;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert(n_engines <= MAX_N_ENGINES);

	bo = calloc(n_execs / 2, sizeof(*bo));
	igt_assert(bo);

	fd = drm_open_driver(DRIVER_XE);
	xe_device_get(fd);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	if (flags & BIND_ENGINE)
		bind_engines[0] = xe_bind_engine_create(fd, vm, 0);
	if (flags & MULTI_VM) {
		vm2 = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
		vm3 = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
		if (flags & BIND_ENGINE) {
			bind_engines[1] = xe_bind_engine_create(fd, vm2, 0);
			bind_engines[2] = xe_bind_engine_create(fd, vm3, 0);
		}
	}

	for (i = 0; i < n_engines; i++) {
		if (flags & MULTI_VM)
			engines[i] = xe_engine_create(fd, i & 1 ? vm2 : vm ,
						      eci, 0);
		else
			engines[i] = xe_engine_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint32_t __bo;
		int e = i % n_engines;

		if (i < n_execs / 2) {
                        uint32_t _vm = (flags & EXTERNAL_OBJ) &&
                                i < n_execs / 8 ? 0 : vm;

			if (flags & MULTI_VM) {
				__bo = bo[i] = xe_bo_create(fd, eci->gt_id, 0,
							    bo_size);
			} else if (flags & THREADED) {
				__bo = bo[i] = xe_bo_create(fd, eci->gt_id, vm,
							    bo_size);
			} else {
				__bo = bo[i] = xe_bo_create_flags(fd, _vm,
								  bo_size,
								  vram_memory(fd, eci->gt_id) |
								  system_memory(fd));
			}
		} else {
			__bo = bo[i % (n_execs / 2)];
		}
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));

		if (i < n_execs / 2) {
			sync[0].flags |= DRM_XE_SYNC_SIGNAL;
			sync[0].handle = syncobj_create(fd, 0);
			if (flags & MULTI_VM) {
				xe_vm_bind_async(fd, vm3, bind_engines[2], __bo,
						 0, addr,
						 bo_size, sync, 1);
				igt_assert(syncobj_wait(fd, &sync[0].handle, 1,
							INT64_MAX, 0, NULL));
				xe_vm_bind_async(fd, i & 1 ? vm2 : vm,
						 i & 1 ? bind_engines[1] :
						 bind_engines[0], __bo,
						 0, addr, bo_size, sync, 1);
			} else {
				xe_vm_bind_async(fd, vm, bind_engines[0],
						 __bo, 0, addr, bo_size,
						 sync, 1);
			}
		}
		addr += bo_size;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		if (i >= n_engines)
			syncobj_reset(fd, &syncobjs[e], 1);
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec), 0);

		if (i + 1 == n_execs / 2) {
			addr = base_addr;
			exec.num_syncs = 1;
			exec.syncs = to_user_pointer(sync + 1);
			if (barrier)
				pthread_barrier_wait(barrier);
		}
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	for (i = 0; i < n_engines; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++) {
		uint32_t __bo;

		__bo = bo[i % (n_execs / 2)];
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));
		igt_assert_eq(data[i].data, 0xc0ffee);
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	for (i = 0; i < 3; i++)
		if (bind_engines[i])
			xe_engine_destroy(fd, bind_engines[i]);

	for (i = 0; i < n_execs / 2; i++)
		gem_close(fd, bo[i]);

	xe_vm_destroy(fd, vm);
	if (flags & MULTI_VM) {
		xe_vm_destroy(fd, vm2);
		xe_vm_destroy(fd, vm3);
	}
	xe_device_put(fd);
	close(fd);
}

static void
test_evict_cm(int fd, struct drm_xe_engine_class_instance *eci,
	      int n_engines, int n_execs, size_t bo_size, unsigned long flags,
	      pthread_barrier_t *barrier)
{
	uint32_t vm, vm2;
	uint32_t bind_engines[2] = { 0, 0 };
	uint64_t addr = 0x100000000, base_addr = 0x100000000;
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
	uint32_t *bo;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
		uint64_t vm_sync;
		uint64_t exec_sync;
	} *data;
	int i, b;

	igt_assert(n_engines <= MAX_N_ENGINES);

	bo = calloc(n_execs / 2, sizeof(*bo));
	igt_assert(bo);

	fd = drm_open_driver(DRIVER_XE);
	xe_device_get(fd);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
			  DRM_XE_VM_CREATE_COMPUTE_MODE, 0);
	if (flags & BIND_ENGINE)
		bind_engines[0] = xe_bind_engine_create(fd, vm, 0);
	if (flags & MULTI_VM) {
		vm2 = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
				   DRM_XE_VM_CREATE_COMPUTE_MODE, 0);
		if (flags & BIND_ENGINE)
			bind_engines[1] = xe_bind_engine_create(fd, vm2, 0);
	}

	for (i = 0; i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property ext = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_COMPUTE_MODE,
			.value = 1,
		};

		if (flags & MULTI_VM)
			engines[i] = xe_engine_create(fd, i & 1 ? vm2 : vm, eci,
						      to_user_pointer(&ext));
		else
			engines[i] = xe_engine_create(fd, vm, eci,
						      to_user_pointer(&ext));
	}

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint32_t __bo;
		int e = i % n_engines;

		if (i < n_execs / 2) {
                        uint32_t _vm = (flags & EXTERNAL_OBJ) &&
                                i < n_execs / 8 ? 0 : vm;

			if (flags & MULTI_VM) {
				__bo = bo[i] = xe_bo_create(fd, eci->gt_id,
							    0, bo_size);
			} else if (flags & THREADED) {
				__bo = bo[i] = xe_bo_create(fd, eci->gt_id,
							    vm, bo_size);
			} else {
				__bo = bo[i] = xe_bo_create_flags(fd, _vm,
								  bo_size,
								  vram_memory(fd, eci->gt_id) |
								  system_memory(fd));
			}
		} else {
			__bo = bo[i % (n_execs / 2)];
		}
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));
		if (i < n_execs / 2)
			memset(data, 0, ALIGN(sizeof(*data) * n_execs, 0x1000));

		if (i < n_execs / 2) {
			sync[0].addr = to_user_pointer(&data[i].vm_sync);
			if (flags & MULTI_VM) {
				xe_vm_bind_async(fd, i & 1 ? vm2 : vm,
						 i & 1 ? bind_engines[1] :
						 bind_engines[0], __bo,
						 0, addr, bo_size, sync, 1);
			} else {
				xe_vm_bind_async(fd, vm, bind_engines[0], __bo,
						 0, addr, bo_size, sync, 1);
			}
#define TWENTY_SEC	20000
			xe_wait_ufence(fd, &data[i].vm_sync, USER_FENCE_VALUE,
				       NULL, TWENTY_SEC);
		}
		sync[0].addr = addr + (char *)&data[i].exec_sync -
			(char *)data;
		addr += bo_size;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec), 0);

		if (i + 1 == n_execs / 2) {
			addr = base_addr;
			if (barrier)
				pthread_barrier_wait(barrier);
		}
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	for (i = 0; i < n_execs; i++) {
		uint32_t __bo;

		__bo = bo[i % (n_execs / 2)];
		if (i)
			munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));
		data = xe_bo_map(fd, __bo,
				 ALIGN(sizeof(*data) * n_execs, 0x1000));
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
			       NULL, TWENTY_SEC);
		igt_assert_eq(data[i].data, 0xc0ffee);
	}
	munmap(data, ALIGN(sizeof(*data) * n_execs, 0x1000));

	for (i = 0; i < n_engines; i++)
		xe_engine_destroy(fd, engines[i]);

	for (i = 0; i < 2; i++)
		if (bind_engines[i])
			xe_engine_destroy(fd, bind_engines[i]);

	for (i = 0; i < n_execs / 2; i++)
		gem_close(fd, bo[i]);

	xe_vm_destroy(fd, vm);
	if (flags & MULTI_VM)
		xe_vm_destroy(fd, vm2);
	xe_device_put(fd);
	close(fd);
}

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	pthread_barrier_t *barrier;
	int fd;
	struct drm_xe_engine_class_instance *eci;
	int n_engines;
	int n_execs;
	uint64_t bo_size;
	int flags;
	bool *go;
};

static void *thread(void *data)
{
	struct thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->flags & COMPUTE_THREAD)
		test_evict_cm(t->fd, t->eci, t->n_engines, t->n_execs,
			      t->bo_size, t->flags, t->barrier);
	else
		test_evict(t->fd, t->eci, t->n_engines, t->n_execs,
			   t->bo_size, t->flags, t->barrier);

	return NULL;
}

static void
threads(int fd, struct drm_xe_engine_class_instance *eci,
	int n_threads, int n_engines, int n_execs, size_t bo_size,
	unsigned long flags)
{
	pthread_barrier_t barrier;
	bool go = false;
	struct thread_data *threads_data;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int i;

	threads_data = calloc(n_threads, sizeof(*threads_data));
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);
	pthread_barrier_init(&barrier, NULL, n_threads);

	for (i = 0; i < n_threads; ++i) {
		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
		threads_data[i].barrier = &barrier;
		threads_data[i].fd = fd;
		threads_data[i].eci = eci;
		threads_data[i].n_engines = n_engines;
		threads_data[i].n_execs = n_execs;
		threads_data[i].bo_size = bo_size;
		threads_data[i].flags = flags;
		if ((i & 1 && flags & MIXED_THREADS) || flags & COMPUTE_THREAD)
			threads_data[i].flags |= COMPUTE_THREAD;
		else
			threads_data[i].flags |= LEGACY_THREAD;
		threads_data[i].go = &go;

		pthread_create(&threads_data[i].thread, 0, thread,
			       &threads_data[i]);
	}

	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < n_threads; ++i)
		pthread_join(threads_data[i].thread, NULL);
}

static uint64_t calc_bo_size(uint64_t vram_size, int mul, int div)
{
	return (ALIGN(vram_size, 0x40000000)  * mul) / div;
}

/**
 * SUBTEST: evict-%s
 * Description:  %arg[1] evict test.
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @small:			small
 * @small-external:		small external
 * @small-multi-vm:		small multi VM
 * @large:			large
 * @large-external:		large external
 * @large-multi-vm:		large multi VM
 * @beng-small:			small bind engine
 * @beng-small-external:	small external bind engine
 * @beng-small-multi-vm:	small multi VM bind ending
 * @beng-large:			large bind engine
 * @beng-large-external:	large external bind engine
 * @beng-large-multi-vm:	large multi VM bind engine
 *
 * @small-cm:			small compute machine
 * @small-external-cm:		small external compute machine
 * @small-multi-vm-cm:		small multi VM compute machine
 * @large-cm:			large compute machine
 * @large-external-cm:		large external compute machine
 * @large-multi-vm-cm:		large multi VM compute machine
 * @beng-small-cm:		small bind engine compute machine
 * @beng-small-external-cm:	small external bind engine compute machine
 * @beng-small-multi-vm-cm:	small multi VM bind ending compute machine
 * @beng-large-cm:		large bind engine compute machine
 * @beng-large-external-cm:	large external bind engine compute machine
 * @beng-large-multi-vm-cm:	large multi VM bind engine compute machine
 *
 * @threads-small:		threads small
 * @cm-threads-small:		compute mode threads small
 * @mixed-threads-small:	mixed threads small
 * @mixed-many-threads-small:	mixed many threads small
 * @threads-large:		threads large
 * @cm-threads-large:		compute mode threads large
 * @mixed-threads-large:	mixed threads large
 * @mixed-many-threads-large:	mixed many threads large
 * @threads-small-multi-vm:	threads small multi vm
 * @cm-threads-small-multi-vm:	compute mode threads small multi vm
 * @mixed-threads-small-multi-vm:
 * 				mixed threads small multi vm
 * @threads-large-multi-vm:	threads large multi vm
 * @cm-threads-large-multi-vm:	compute mode threads large multi vm
 * @mixed-threads-large-multi-vm:
 *				mixed threads large multi vm
 * @beng-threads-small:		bind engine threads small
 * @beng-cm-threads-small:	bind engine compute mode threads small
 * @beng-mixed-threads-small:	bind engine mixed threads small
 * @beng-mixed-many-threads-small:
 *				bind engine mixed many threads small
 * @beng-threads-large:		bind engine threads large
 * @beng-cm-threads-large:	bind engine compute mode threads large
 * @beng-mixed-threads-large:	bind engine mixed threads large
 * @beng-mixed-many-threads-large:
 *				bind engine mixed many threads large
 * @beng-threads-small-multi-vm:
 *				bind engine threads small multi vm
 * @beng-cm-threads-small-multi-vm:
 *				bind engine compute mode threads small multi vm
 * @beng-mixed-threads-small-multi-vm:
 *				bind engine mixed threads small multi vm
 * @beng-threads-large-multi-vm:
 *				bind engine threads large multi vm
 * @beng-cm-threads-large-multi-vm:
 *				bind engine compute mode threads large multi vm
 * @beng-mixed-threads-large-multi-vm:
 *				bind engine mixed threads large multi vm
 */

/*
 * Table driven test that attempts to cover all possible scenarios of eviction
 * (small / large objects, compute mode vs non-compute VMs, external BO or BOs
 * tied to VM, multiple VMs using over 51% of the VRAM, evicting BOs from your
 * own VM, and using a user bind or kernel VM engine to do the binds). All of
 * these options are attempted to be mixed via different table entries. Single
 * threaded sections exists for both compute and non-compute VMs, and thread
 * sections exists which cover multiple compute VM, multiple non-compute VMs,
 * and mixing of VMs.
 */
igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		int n_engines;
		int n_execs;
		int mul;
		int div;
		unsigned int flags;
	} sections[] = {
		{ "small", 16, 448, 1, 128, 0 },
		{ "small-external", 16, 448, 1, 128, EXTERNAL_OBJ },
		{ "small-multi-vm", 16, 256, 1, 128, MULTI_VM },
		{ "large", 4, 16, 1, 4, 0 },
		{ "large-external", 4, 16, 1, 4, EXTERNAL_OBJ },
		{ "large-multi-vm", 4, 8, 3, 8, MULTI_VM },
		{ "beng-small", 16, 448, 1, 128, BIND_ENGINE },
		{ "beng-small-external", 16, 448, 1, 128, BIND_ENGINE |
			EXTERNAL_OBJ },
		{ "beng-small-multi-vm", 16, 256, 1, 128, BIND_ENGINE |
			MULTI_VM },
		{ "beng-large", 4, 16, 1, 4, 0 },
		{ "beng-large-external", 4, 16, 1, 4, BIND_ENGINE |
			EXTERNAL_OBJ },
		{ "beng-large-multi-vm", 4, 8, 3, 8, BIND_ENGINE | MULTI_VM },
		{ NULL },
	};
	const struct section_cm {
		const char *name;
		int n_engines;
		int n_execs;
		int mul;
		int div;
		unsigned int flags;
	} sections_cm[] = {
		{ "small-cm", 16, 448, 1, 128, 0 },
		{ "small-external-cm", 16, 448, 1, 128, EXTERNAL_OBJ },
		{ "small-multi-vm-cm", 16, 256, 1, 128, MULTI_VM },
		{ "large-cm", 4, 16, 1, 4, 0 },
		{ "large-external-cm", 4, 16, 1, 4, EXTERNAL_OBJ },
		{ "large-multi-vm-cm", 4, 8, 3, 8, MULTI_VM },
		{ "beng-small-cm", 16, 448, 1, 128, BIND_ENGINE },
		{ "beng-small-external-cm", 16, 448, 1, 128, BIND_ENGINE |
			EXTERNAL_OBJ },
		{ "beng-small-multi-vm-cm", 16, 256, 1, 128, BIND_ENGINE |
			MULTI_VM },
		{ "beng-large-cm", 4, 16, 1, 4, BIND_ENGINE },
		{ "beng-large-external-cm", 4, 16, 1, 4, BIND_ENGINE |
			EXTERNAL_OBJ },
		{ "beng-large-multi-vm-cm", 4, 8, 3, 8, BIND_ENGINE |
			MULTI_VM },
		{ NULL },
	};
	const struct section_threads {
		const char *name;
		int n_threads;
		int n_engines;
		int n_execs;
		int mul;
		int div;
		unsigned int flags;
	} sections_threads[] = {
		{ "threads-small", 2, 16, 128, 1, 128,
			THREADED },
		{ "cm-threads-small", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | THREADED },
		{ "mixed-threads-small", 2, 16, 128, 1, 128,
			MIXED_THREADS | THREADED },
		{ "mixed-many-threads-small", 3, 16, 128, 1, 128,
			THREADED },
		{ "threads-large", 2, 2, 4, 3, 8,
			THREADED },
		{ "cm-threads-large", 2, 2, 4, 3, 8,
			COMPUTE_THREAD | THREADED },
		{ "mixed-threads-large", 2, 2, 4, 3, 8,
			MIXED_THREADS | THREADED },
		{ "mixed-many-threads-large", 3, 2, 4, 3, 8,
			THREADED },
		{ "threads-small-multi-vm", 2, 16, 128, 1, 128,
			MULTI_VM | THREADED },
		{ "cm-threads-small-multi-vm", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | MULTI_VM | THREADED },
		{ "mixed-threads-small-multi-vm", 2, 16, 128, 1, 128,
			MIXED_THREADS | MULTI_VM | THREADED },
		{ "threads-large-multi-vm", 2, 2, 4, 3, 8,
			MULTI_VM | THREADED },
		{ "cm-threads-large-multi-vm", 2, 2, 4, 3, 8,
			COMPUTE_THREAD | MULTI_VM | THREADED },
		{ "mixed-threads-large-multi-vm", 2, 2, 4, 3, 8,
			MIXED_THREADS | MULTI_VM | THREADED },
		{ "beng-threads-small", 2, 16, 128, 1, 128,
			THREADED | BIND_ENGINE },
		{ "beng-cm-threads-small", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | THREADED | BIND_ENGINE },
		{ "beng-mixed-threads-small", 2, 16, 128, 1, 128,
			MIXED_THREADS | THREADED | BIND_ENGINE },
		{ "beng-mixed-many-threads-small", 3, 16, 128, 1, 128,
			THREADED | BIND_ENGINE },
		{ "beng-threads-large", 2, 2, 4, 3, 8,
			THREADED | BIND_ENGINE },
		{ "beng-cm-threads-large", 2, 2, 4, 3, 8,
			COMPUTE_THREAD | THREADED | BIND_ENGINE },
		{ "beng-mixed-threads-large", 2, 2, 4, 3, 8,
			MIXED_THREADS | THREADED | BIND_ENGINE },
		{ "beng-mixed-many-threads-large", 3, 2, 4, 3, 8,
			THREADED | BIND_ENGINE },
		{ "beng-threads-small-multi-vm", 2, 16, 128, 1, 128,
			MULTI_VM | THREADED | BIND_ENGINE },
		{ "beng-cm-threads-small-multi-vm", 2, 16, 128, 1, 128,
			COMPUTE_THREAD | MULTI_VM | THREADED | BIND_ENGINE },
		{ "beng-mixed-threads-small-multi-vm", 2, 16, 128, 1, 128,
			MIXED_THREADS | MULTI_VM | THREADED | BIND_ENGINE },
		{ "beng-threads-large-multi-vm", 2, 2, 4, 3, 8,
			MULTI_VM | THREADED | BIND_ENGINE },
		{ "beng-cm-threads-large-multi-vm", 2, 2, 4, 3, 8,
			COMPUTE_THREAD | MULTI_VM | THREADED | BIND_ENGINE },
		{ "beng-mixed-threads-large-multi-vm", 2, 2, 4, 3, 8,
			MIXED_THREADS | MULTI_VM | THREADED | BIND_ENGINE },
		{ NULL },
	};
	uint64_t vram_size;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
		igt_require(xe_has_vram(fd));
		vram_size = xe_vram_size(fd, 0);
		igt_assert(vram_size);

		for_each_hw_engine(fd, hwe)
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COPY)
				break;
	}

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("evict-%s", s->name)
			test_evict(-1, hwe, s->n_engines, s->n_execs,
				   calc_bo_size(vram_size, s->mul, s->div),
				   s->flags, NULL);
	}

	for (const struct section_cm *s = sections_cm; s->name; s++) {
		igt_subtest_f("evict-%s", s->name)
			test_evict_cm(-1, hwe, s->n_engines, s->n_execs,
				      calc_bo_size(vram_size, s->mul, s->div),
				      s->flags, NULL);
	}

	for (const struct section_threads *s = sections_threads; s->name; s++) {
		igt_subtest_f("evict-%s", s->name)
			threads(-1, hwe, s->n_threads, s->n_engines,
				 s->n_execs,
				 calc_bo_size(vram_size, s->mul, s->div),
				 s->flags);
	}

	igt_fixture
		close(fd);
}
