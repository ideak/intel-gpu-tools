// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>

static void test_spin(int fd, struct drm_xe_engine_class_instance *eci)
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
	uint32_t engine;
	uint32_t syncobj;
	size_t bo_size;
	uint32_t bo = 0;
	struct xe_spin *spin;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*spin);
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
	spin = xe_bo_map(fd, bo, bo_size);

	engine = xe_engine_create(fd, vm, eci, 0);
	syncobj = syncobj_create(fd, 0);

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	xe_spin_init(spin, addr, false);

	sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_SIGNAL;
	sync[1].handle = syncobj;

	exec.engine_id = engine;
	exec.address = addr;
	xe_exec(fd, &exec);

	xe_spin_wait_started(spin);
	usleep(50000);
	igt_assert(!syncobj_wait(fd, &syncobj, 1, 1, 0, NULL));
	xe_spin_end(spin);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobj);
	xe_engine_destroy(fd, engine);

	munmap(spin, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define MAX_N_ENGINES 16
#define MAX_INSTANCE 9
#define CANCEL		(0x1 << 0)
#define ENGINE_RESET	(0x1 << 1)
#define GT_RESET	(0x1 << 2)
#define CLOSE_FD	(0x1 << 3)
#define CLOSE_ENGINES	(0x1 << 4)
#define VIRTUAL		(0x1 << 5)
#define PARALLEL	(0x1 << 6)
#define CAT_ERROR	(0x1 << 7)

static void
test_balancer(int fd, int gt, int class, int n_engines, int n_execs,
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
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	int i, j, b, num_placements = 0, bad_batches = 1;

	igt_assert(n_engines <= MAX_N_ENGINES);

	if (flags & CLOSE_FD) {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	for_each_hw_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;

		eci[num_placements++] = *hwe;
	}
	if (num_placements < 2)
		return;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	bo = xe_bo_create(fd, gt, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property job_timeout = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_JOB_TIMEOUT,
			.value = 50,
		};
		struct drm_xe_ext_engine_set_property preempt_timeout = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_PREEMPTION_TIMEOUT,
			.value = 1000,
		};
		struct drm_xe_engine_create create = {
			.vm_id = vm,
			.width = flags & PARALLEL ? num_placements : 1,
			.num_placements = flags & PARALLEL ? 1 : num_placements,
			.instances = to_user_pointer(eci),
		};

		if (flags & CANCEL)
			create.extensions = to_user_pointer(&job_timeout);
		else if (flags & ENGINE_RESET)
			create.extensions = to_user_pointer(&preempt_timeout);

		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_ENGINE_CREATE,
					&create), 0);
		engines[i] = create.engine_id;
		syncobjs[i] = syncobj_create(fd, 0);
	};
	exec.num_batch_buffer = flags & PARALLEL ? num_placements : 1;

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	if (flags & VIRTUAL && (flags & CAT_ERROR || flags & ENGINE_RESET ||
				flags & GT_RESET))
		bad_batches = num_placements;

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = flags & CAT_ERROR && i < bad_batches ?
			addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t spin_addr = base_addr + spin_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		uint64_t batches[MAX_INSTANCE];
		int e = i % n_engines;

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = batch_addr;

		if (i < bad_batches) {
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

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = exec_addr;

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = flags & PARALLEL ?
			to_user_pointer(batches) : exec_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);
	}

	if (flags & GT_RESET)
		xe_force_gt_reset(fd, gt);

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_ENGINES) {
			for (i = 0; i < n_engines; i++)
				xe_engine_destroy(fd, engines[i]);
		}
		xe_device_put(fd);
		close(fd);
		/* FIXME: wait for idle */
		usleep(150000);
		return;
	}

	for (i = 0; i < n_engines && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = bad_batches; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void
test_legacy_mode(int fd, struct drm_xe_engine_class_instance *eci,
		 int n_engines, int n_execs, unsigned int flags)
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
	uint32_t engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert(n_engines <= MAX_N_ENGINES);

	if (flags & CLOSE_FD) {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property job_timeout = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_JOB_TIMEOUT,
			.value = 50,
		};
		struct drm_xe_ext_engine_set_property preempt_timeout = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_PREEMPTION_TIMEOUT,
			.value = 1000,
		};
		uint64_t ext = 0;

		if (flags & CANCEL)
			ext = to_user_pointer(&job_timeout);
		else if (flags & ENGINE_RESET)
			ext = to_user_pointer(&preempt_timeout);

		engines[i] = xe_engine_create(fd, vm, eci, ext);
		syncobjs[i] = syncobj_create(fd, 0);
	};

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = flags & CAT_ERROR && !i ?
			addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t spin_addr = base_addr + spin_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		int e = i % n_engines;

		if (!i) {
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
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);
	}

	if (flags & GT_RESET)
		xe_force_gt_reset(fd, eci->gt_id);

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_ENGINES) {
			for (i = 0; i < n_engines; i++)
				xe_engine_destroy(fd, engines[i]);
		}
		xe_device_put(fd);
		close(fd);
		/* FIXME: wait for idle */
		usleep(150000);
		return;
	}

	for (i = 0; i < n_engines && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 1; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void
test_compute_mode(int fd, struct drm_xe_engine_class_instance *eci,
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
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint64_t exec_sync;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert(n_engines <= MAX_N_ENGINES);

	if (flags & CLOSE_FD) {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
			  DRM_XE_VM_CREATE_COMPUTE_MODE, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);
	memset(data, 0, bo_size);

	for (i = 0; i < n_engines; i++) {
		struct drm_xe_ext_engine_set_property compute = {
			.base.next_extension = 0,
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_COMPUTE_MODE,
			.value = 1,
		};
		struct drm_xe_ext_engine_set_property preempt_timeout = {
			.base.next_extension = to_user_pointer(&compute),
			.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY,
			.property = XE_ENGINE_SET_PROPERTY_PREEMPTION_TIMEOUT,
			.value = 1000,
		};
		uint64_t ext = 0;

		if (flags & ENGINE_RESET)
			ext = to_user_pointer(&preempt_timeout);
		else
			ext = to_user_pointer(&compute);

		engines[i] = xe_engine_create(fd, vm, eci, ext);
	};

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

#define THREE_SEC	3000
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, THREE_SEC);
	data[0].vm_sync = 0;

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = flags & CAT_ERROR && !i ?
			addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t spin_addr = base_addr + spin_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		int e = i % n_engines;

		if (!i) {
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

		sync[0].addr = base_addr +
			(char *)&data[i].exec_sync - (char *)data;

		exec.engine_id = engines[e];
		exec.address = exec_addr;
		xe_exec(fd, &exec);
	}

	if (flags & GT_RESET)
		xe_force_gt_reset(fd, eci->gt_id);

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_ENGINES) {
			for (i = 0; i < n_engines; i++)
				xe_engine_destroy(fd, engines[i]);
		}
		xe_device_put(fd);
		close(fd);
		/* FIXME: wait for idle */
		usleep(150000);
		return;
	}

	for (i = 1; i < n_execs; i++)
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
			       NULL, THREE_SEC);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, NULL, THREE_SEC);

	for (i = 1; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_engines; i++)
		xe_engine_destroy(fd, engines[i]);

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

struct gt_thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int fd;
	int gt;
	int *go;
	int *exit;
	int *num_reset;
	bool do_reset;
};

static void do_resets(struct gt_thread_data *t)
{
	while (!*(t->exit)) {
		usleep(250000);	/* 250 ms */
		(*t->num_reset)++;
		xe_force_gt_reset(t->fd, t->gt);
	}
}

static void submit_jobs(struct gt_thread_data *t)
{
	int fd = t->fd;
	uint32_t vm = xe_vm_create(fd, 0, 0);
	uint64_t addr = 0x1a0000;
	size_t bo_size = xe_get_default_alignment(fd);
	uint32_t bo;
	uint32_t *data;

	bo = xe_bo_create(fd, 0, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);
	data[0] = MI_BATCH_BUFFER_END;

	xe_vm_bind_sync(fd, vm, bo, 0, addr, bo_size);

	while (!*(t->exit)) {
		struct drm_xe_engine_class_instance instance = {
			.engine_class = DRM_XE_ENGINE_CLASS_COPY,
			.engine_instance = 0,
			.gt_id = 0,
		};
		struct drm_xe_engine_create create = {
			.vm_id = vm,
			.width = 1,
			.num_placements = 1,
			.instances = to_user_pointer(&instance),
		};
		struct drm_xe_exec exec;
		int ret;

		/* GuC IDs can get exhausted */
		ret = igt_ioctl(fd, DRM_IOCTL_XE_ENGINE_CREATE, &create);
		if (ret)
			continue;

		exec.engine_id = create.engine_id;
		exec.address = addr;
		exec.num_batch_buffer = 1;
		xe_exec(fd, &exec);
		xe_engine_destroy(fd, create.engine_id);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void *gt_reset_thread(void *data)
{
	struct gt_thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->do_reset)
		do_resets(t);
	else
		submit_jobs(t);

	return NULL;
}

static void
gt_reset(int fd, int n_threads, int n_sec)
{
	struct gt_thread_data *threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int go = 0, exit = 0, num_reset = 0, i;

	threads = calloc(n_threads, sizeof(struct gt_thread_data));
	igt_assert(threads);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	for (i = 0; i < n_threads; ++i) {
		threads[i].mutex = &mutex;
		threads[i].cond = &cond;
		threads[i].fd = fd;
		threads[i].gt = 0;
		threads[i].go = &go;
		threads[i].exit = &exit;
		threads[i].num_reset = &num_reset;
		threads[i].do_reset = (i == 0);

		pthread_create(&threads[i].thread, 0, gt_reset_thread,
			       &threads[i]);
	}

	pthread_mutex_lock(&mutex);
	go = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	sleep(n_sec);
	exit = 1;

	for (i = 0; i < n_threads; i++)
		pthread_join(threads[i].thread, NULL);

	printf("number of resets %d\n", num_reset);

	free(threads);
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "virtual", VIRTUAL },
		{ "parallel", PARALLEL },
		{ NULL },
	};
	int gt;
	int class;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	igt_subtest("spin")
		for_each_hw_engine(fd, hwe)
			test_spin(fd, hwe);

	igt_subtest("cancel")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(fd, hwe, 1, 1, CANCEL);

	igt_subtest("engine-reset")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(fd, hwe, 2, 2, ENGINE_RESET);

	igt_subtest("cat-error")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(fd, hwe, 2, 2, CAT_ERROR);

	igt_subtest("gt-reset")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(fd, hwe, 2, 2, GT_RESET);

	igt_subtest("close-fd-no-exec")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(-1, hwe, 16, 0, CLOSE_FD);

	igt_subtest("close-fd")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(-1, hwe, 16, 256, CLOSE_FD);

	igt_subtest("close-engines-close-fd")
		for_each_hw_engine(fd, hwe)
			test_legacy_mode(-1, hwe, 16, 256, CLOSE_FD |
					 CLOSE_ENGINES);

	igt_subtest("cm-engine-reset")
		for_each_hw_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, ENGINE_RESET);

	igt_subtest("cm-cat-error")
		for_each_hw_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, CAT_ERROR);

	igt_subtest("cm-gt-reset")
		for_each_hw_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, GT_RESET);

	igt_subtest("cm-close-fd-no-exec")
		for_each_hw_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 0, CLOSE_FD);

	igt_subtest("cm-close-fd")
		for_each_hw_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256, CLOSE_FD);

	igt_subtest("cm-close-engines-close-fd")
		for_each_hw_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256, CLOSE_FD |
					  CLOSE_ENGINES);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("%s-cancel", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(fd, gt, class, 1, 1,
						      CANCEL | s->flags);

		igt_subtest_f("%s-engine-reset", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(fd, gt, class, MAX_INSTANCE + 1,
						      MAX_INSTANCE + 1,
						      ENGINE_RESET | s->flags);

		igt_subtest_f("%s-cat-error", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(fd, gt, class, MAX_INSTANCE + 1,
						      MAX_INSTANCE + 1,
						      CAT_ERROR | s->flags);

		igt_subtest_f("%s-gt-reset", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(fd, gt, class, MAX_INSTANCE + 1,
						      MAX_INSTANCE + 1,
						      GT_RESET | s->flags);

		igt_subtest_f("%s-close-fd-no-exec", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(-1, gt, class, 16, 0,
						      CLOSE_FD | s->flags);

		igt_subtest_f("%s-close-fd", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(-1, gt, class, 16, 256,
						      CLOSE_FD | s->flags);

		igt_subtest_f("%s-close-engines-close-fd", s->name)
			for_each_gt(fd, gt)
				for_each_hw_engine_class(class)
					test_balancer(-1, gt, class, 16, 256, CLOSE_FD |
						      CLOSE_ENGINES | s->flags);
	}

	igt_subtest("gt-reset-stress")
		gt_reset(fd, 4, 1);

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
