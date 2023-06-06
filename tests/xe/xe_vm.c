// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Check if VMA functionality is working
 * Category: Software building block
 * Sub-category: VMA
 * Test category: functionality test
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>

static uint32_t
addr_low(uint64_t addr)
{
	return addr;
}

static uint32_t
addr_high(int fd, uint64_t addr)
{
	uint32_t va_bits = xe_va_bits(fd);
	uint32_t leading_bits = 64 - va_bits;

	igt_assert_eq(addr >> va_bits, 0);
	return (int64_t)(addr << leading_bits) >> (32 + leading_bits);
}

static uint32_t
hash_addr(uint64_t addr)
{
	return (addr * 7229) ^ ((addr >> 32) * 5741);
}

static void
write_dwords(int fd, uint32_t vm, int n_dwords, uint64_t *addrs)
{
	uint32_t batch_size, batch_bo, *batch_map, engine;
	uint64_t batch_addr = 0x1a0000;
	int i, b = 0;

	batch_size = (n_dwords * 4 + 1) * sizeof(uint32_t);
	batch_size = ALIGN(batch_size + xe_cs_prefetch_size(fd),
			   xe_get_default_alignment(fd));
	batch_bo = xe_bo_create(fd, 0, vm, batch_size);
	batch_map = xe_bo_map(fd, batch_bo, batch_size);

	for (i = 0; i < n_dwords; i++) {
		/* None of the addresses can land in our batch */
		igt_assert(addrs[i] + sizeof(uint32_t) <= batch_addr ||
			   batch_addr + batch_size <= addrs[i]);

		batch_map[b++] = MI_STORE_DWORD_IMM_GEN4;
		batch_map[b++] = addr_low(addrs[i]);
		batch_map[b++] = addr_high(fd, addrs[i]);
		batch_map[b++] = hash_addr(addrs[i]);

	}
	batch_map[b++] = MI_BATCH_BUFFER_END;
	igt_assert_lte(&batch_map[b] - batch_map, batch_size);
	munmap(batch_map, batch_size);

	xe_vm_bind_sync(fd, vm, batch_bo, 0, batch_addr, batch_size);
	engine = xe_engine_create_class(fd, vm, DRM_XE_ENGINE_CLASS_COPY);
	xe_exec_wait(fd, engine, batch_addr);
	xe_vm_unbind_sync(fd, vm, 0, batch_addr, batch_size);

	gem_close(fd, batch_bo);
	xe_engine_destroy(fd, engine);
}

/**
 * SUBTEST: scratch
 * Description: Test scratch page creation and write
 * Functionality: scratch page
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void
test_scratch(int fd)
{
	uint32_t vm = xe_vm_create(fd, DRM_XE_VM_CREATE_SCRATCH_PAGE, 0);
	uint64_t addrs[] = {
		0x000000000000ull,
		0x7ffdb86402d8ull,
		0x7ffffffffffcull,
		0x800000000000ull,
		0x3ffdb86402d8ull,
		0xfffffffffffcull,
	};

	write_dwords(fd, vm, ARRAY_SIZE(addrs), addrs);

	xe_vm_destroy(fd, vm);
}

static void
__test_bind_one_bo(int fd, uint32_t vm, int n_addrs, uint64_t *addrs)
{
	uint32_t bo, bo_size = xe_get_default_alignment(fd);
	uint32_t *vms;
	void *map;
	int i;

	if (!vm) {
		vms = malloc(sizeof(*vms) * n_addrs);
		igt_assert(vms);
	}
	bo = xe_bo_create(fd, 0, vm, bo_size);
	map = xe_bo_map(fd, bo, bo_size);
	memset(map, 0, bo_size);

	for (i = 0; i < n_addrs; i++) {
		uint64_t bind_addr = addrs[i] & ~(uint64_t)(bo_size - 1);

		if (!vm)
			vms[i] = xe_vm_create(fd, DRM_XE_VM_CREATE_SCRATCH_PAGE,
					      0);
		igt_debug("Binding addr %"PRIx64"\n", addrs[i]);
		xe_vm_bind_sync(fd, vm ? vm : vms[i], bo, 0,
				bind_addr, bo_size);
	}

	if (vm)
		write_dwords(fd, vm, n_addrs, addrs);
	else
		for (i = 0; i < n_addrs; i++)
			write_dwords(fd, vms[i], 1, addrs + i);

	for (i = 0; i < n_addrs; i++) {
		uint32_t *dw = map + (addrs[i] & (bo_size - 1));
		uint64_t bind_addr = addrs[i] & ~(uint64_t)(bo_size - 1);

		igt_debug("Testing addr %"PRIx64"\n", addrs[i]);
		igt_assert_eq(*dw, hash_addr(addrs[i]));

		xe_vm_unbind_sync(fd, vm ? vm : vms[i], 0,
				  bind_addr, bo_size);

		/* clear dw, to ensure same execbuf after unbind fails to write */
		*dw = 0;
	}

	if (vm)
		write_dwords(fd, vm, n_addrs, addrs);
	else
		for (i = 0; i < n_addrs; i++)
			write_dwords(fd, vms[i], 1, addrs + i);

	for (i = 0; i < n_addrs; i++) {
		uint32_t *dw = map + (addrs[i] & (bo_size - 1));

		igt_debug("Testing unbound addr %"PRIx64"\n", addrs[i]);
		igt_assert_eq(*dw, 0);
	}

	munmap(map, bo_size);

	gem_close(fd, bo);
	if (vm) {
		xe_vm_destroy(fd, vm);
	} else {
		for (i = 0; i < n_addrs; i++)
			xe_vm_destroy(fd, vms[i]);
		free(vms);
	}
}

uint64_t addrs_48b[] = {
	0x000000000000ull,
	0x0000b86402d4ull,
	0x0001b86402d8ull,
	0x7ffdb86402dcull,
	0x7fffffffffecull,
	0x800000000004ull,
	0x3ffdb86402e8ull,
	0xfffffffffffcull,
};

uint64_t addrs_57b[] = {
	0x000000000000ull,
	0x0000b86402d4ull,
	0x0001b86402d8ull,
	0x7ffdb86402dcull,
	0x7fffffffffecull,
	0x800000000004ull,
	0x3ffdb86402e8ull,
	0xfffffffffffcull,
	0x100000000000008ull,
	0xfffffdb86402e0ull,
	0x1fffffffffffff4ull,
};

/**
 * SUBTEST: bind-once
 * Description: bind once on one BO
 * Functionality: bind BO
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void
test_bind_once(int fd)
{
	uint64_t addr = 0x7ffdb86402d8ull;

	__test_bind_one_bo(fd,
			   xe_vm_create(fd, DRM_XE_VM_CREATE_SCRATCH_PAGE, 0),
			   1, &addr);
}

/**
 * SUBTEST: bind-one-bo-many-times
 * Description: bind many times on one BO
 * Functionality: bind BO
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void
test_bind_one_bo_many_times(int fd)
{
	uint32_t va_bits = xe_va_bits(fd);
	uint64_t *addrs = (va_bits == 57) ? addrs_57b : addrs_48b;
	uint64_t addrs_size = (va_bits == 57) ? ARRAY_SIZE(addrs_57b) :
						ARRAY_SIZE(addrs_48b);

	__test_bind_one_bo(fd,
			   xe_vm_create(fd, DRM_XE_VM_CREATE_SCRATCH_PAGE, 0),
			   addrs_size, addrs);
}

/**
 * SUBTEST: bind-one-bo-many-times-many-vm
 * Description: Test bind many times and many VM on one BO
 * Functionality: bind BO
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void
test_bind_one_bo_many_times_many_vm(int fd)
{
	uint32_t va_bits = xe_va_bits(fd);
	uint64_t *addrs = (va_bits == 57) ? addrs_57b : addrs_48b;
	uint64_t addrs_size = (va_bits == 57) ? ARRAY_SIZE(addrs_57b) :
						ARRAY_SIZE(addrs_48b);

	__test_bind_one_bo(fd, 0, addrs_size, addrs);
}

/**
 * SUBTEST: unbind-all-%d-vmas
 * Description: Test unbind all with %arg[1] VMAs
 * Functionality: unbind
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1].values: 2, 8
 */

static void unbind_all(int fd, int n_vmas)
{
	uint32_t bo, bo_size = xe_get_default_alignment(fd);
	uint64_t addr = 0x1a0000;
	uint32_t vm;
	int i;
	struct drm_xe_sync sync[1] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo = xe_bo_create(fd, 0, vm, bo_size);

	for (i = 0; i < n_vmas; ++i)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr + i * bo_size,
				 bo_size, NULL, 0);

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_unbind_all_async(fd, vm, 0, bo, sync, 1);

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	syncobj_destroy(fd, sync[0].handle);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define	MAP_ADDRESS	0x00007fadeadbe000

/**
 * SUBTEST: userptr-invalid
 * Description:
 *	Verifies that mapping an invalid userptr returns -EFAULT,
 *	and that it is correctly handled.
 * Functionality: userptr
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void userptr_invalid(int fd)
{
	size_t size = xe_get_default_alignment(fd);
	uint32_t vm;
	void *data;
	int ret;

	data = mmap((void *)MAP_ADDRESS, size, PROT_READ |
		    PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
	igt_assert(data != MAP_FAILED);

	vm = xe_vm_create(fd, 0, 0);
	munmap(data, size);
	ret = __xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
			   size, XE_VM_BIND_OP_MAP_USERPTR, NULL, 0, 0, 0);
	igt_assert(ret == -EFAULT);

	xe_vm_destroy(fd, vm);
}

struct vm_thread_data {
	pthread_t thread;
	struct drm_xe_vm_bind_op_error_capture *capture;
	int fd;
	int vm;
	uint32_t bo;
	size_t bo_size;
	bool destroy;
};

/**
 * SUBTEST: vm-async-ops-err
 * Description: Test VM async ops error
 * Functionality: VM
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: vm-async-ops-err-destroy
 * Description: Test VM async ops error destroy
 * Functionality: VM
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void *vm_async_ops_err_thread(void *data)
{
	struct vm_thread_data *args = data;
	int fd = args->fd;
	uint64_t addr = 0x201a0000;
	int num_binds = 0;
	int ret;

	struct drm_xe_wait_user_fence wait = {
		.vm_id = args->vm,
		.op = DRM_XE_UFENCE_WAIT_NEQ,
		.flags = DRM_XE_UFENCE_WAIT_VM_ERROR,
		.mask = DRM_XE_UFENCE_WAIT_U32,
		.timeout = 1000,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_WAIT_USER_FENCE,
				&wait), 0);
	if (args->destroy) {
		usleep(5000);	/* Wait other binds to queue up */
		xe_vm_destroy(fd, args->vm);
		return NULL;
	}

	while (!ret) {
		struct drm_xe_vm_bind bind = {
			.vm_id = args->vm,
			.num_binds = 1,
			.bind.op = XE_VM_BIND_OP_RESTART,
		};

		/* VM sync ops should work */
		if (!(num_binds++ % 2)) {
			xe_vm_bind_sync(fd, args->vm, args->bo, 0, addr,
					args->bo_size);
		} else {
			xe_vm_unbind_sync(fd, args->vm, 0, addr,
					  args->bo_size);
			addr += args->bo_size * 2;
		}

		/* Restart and wait for next error */
		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_VM_BIND,
					&bind), 0);
		args->capture->error = 0;
		ret = igt_ioctl(fd, DRM_IOCTL_XE_WAIT_USER_FENCE, &wait);
	}

	return NULL;
}

static void vm_async_ops_err(int fd, bool destroy)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync = {
		.flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL,
	};
#define N_BINDS		32
	struct drm_xe_vm_bind_op_error_capture capture = {};
	struct drm_xe_ext_vm_set_property ext = {
		.base.next_extension = 0,
		.base.name = XE_VM_EXTENSION_SET_PROPERTY,
		.property = XE_VM_PROPERTY_BIND_OP_ERROR_CAPTURE_ADDRESS,
		.value = to_user_pointer(&capture),
	};
	struct vm_thread_data thread = {};
	uint32_t syncobjs[N_BINDS];
	size_t bo_size = 0x1000 * 32;
	uint32_t bo;
	int i, j;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS,
			  to_user_pointer(&ext));
	bo = xe_bo_create(fd, 0, vm, bo_size);

	thread.capture = &capture;
	thread.fd = fd;
	thread.vm = vm;
	thread.bo = bo;
	thread.bo_size = bo_size;
	thread.destroy = destroy;
	pthread_create(&thread.thread, 0, vm_async_ops_err_thread, &thread);

	for (i = 0; i < N_BINDS; i++)
		syncobjs[i] = syncobj_create(fd, 0);

	for (j = 0, i = 0; i < N_BINDS / 4; i++, j++) {
		sync.handle = syncobjs[j];
#define INJECT_ERROR	(0x1 << 31)
		if (i == N_BINDS / 8)	/* Inject error on this bind */
			__xe_vm_bind_assert(fd, vm, 0, bo, 0,
					    addr + i * bo_size * 2,
					    bo_size, XE_VM_BIND_OP_MAP |
					    XE_VM_BIND_FLAG_ASYNC |
					    INJECT_ERROR, &sync, 1, 0, 0);
		else
			xe_vm_bind_async(fd, vm, 0, bo, 0,
					 addr + i * bo_size * 2,
					 bo_size, &sync, 1);
	}

	for (i = 0; i < N_BINDS / 4; i++, j++) {
		sync.handle = syncobjs[j];
		if (i == N_BINDS / 8)
			__xe_vm_bind_assert(fd, vm, 0, 0, 0,
					    addr + i * bo_size * 2,
					    bo_size, XE_VM_BIND_OP_UNMAP |
					    XE_VM_BIND_FLAG_ASYNC |
					    INJECT_ERROR, &sync, 1, 0, 0);
		else
			xe_vm_unbind_async(fd, vm, 0, 0,
					   addr + i * bo_size * 2,
					   bo_size, &sync, 1);
	}

	for (i = 0; i < N_BINDS / 4; i++, j++) {
		sync.handle = syncobjs[j];
		if (i == N_BINDS / 8)
			__xe_vm_bind_assert(fd, vm, 0, bo, 0,
					    addr + i * bo_size * 2,
					    bo_size, XE_VM_BIND_OP_MAP |
					    XE_VM_BIND_FLAG_ASYNC |
					    INJECT_ERROR, &sync, 1, 0, 0);
		else
			xe_vm_bind_async(fd, vm, 0, bo, 0,
					 addr + i * bo_size * 2,
					 bo_size, &sync, 1);
	}

	for (i = 0; i < N_BINDS / 4; i++, j++) {
		sync.handle = syncobjs[j];
		if (i == N_BINDS / 8)
			__xe_vm_bind_assert(fd, vm, 0, 0, 0,
					    addr + i * bo_size * 2,
					    bo_size, XE_VM_BIND_OP_UNMAP |
					    XE_VM_BIND_FLAG_ASYNC |
					    INJECT_ERROR, &sync, 1, 0, 0);
		else
			xe_vm_unbind_async(fd, vm, 0, 0,
					   addr + i * bo_size * 2,
					   bo_size, &sync, 1);
	}

	for (i = 0; i < N_BINDS; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));

	if (!destroy)
		xe_vm_destroy(fd, vm);

	pthread_join(thread.thread, NULL);
}

/**
 * SUBTEST: shared-%s-page
 * Description: Test shared arg[1] page
 * Run type: BAT
 *
 * Functionality: %arg[1] page
 * arg[1].values: pte, pde, pde2, pde3
 */


struct shared_pte_page_data {
	uint32_t batch[16];
	uint64_t pad;
	uint32_t data;
};

#define MAX_N_ENGINES 4

static void
shared_pte_page(int fd, struct drm_xe_engine_class_instance *eci, int n_bo,
		uint64_t addr_stride)
{
	uint32_t vm;
	uint64_t addr = 0x1000 * 512;
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_sync sync_all[MAX_N_ENGINES + 1];
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t *bo;
	struct shared_pte_page_data **data;
	int n_engines = n_bo, n_execs = n_bo;
	int i, b;

	igt_assert(n_engines <= MAX_N_ENGINES);

	bo = malloc(sizeof(*bo) * n_bo);
	igt_assert(bo);

	data = malloc(sizeof(*data) * n_bo);
	igt_assert(data);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(struct shared_pte_page_data);
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	for (i = 0; i < n_bo; ++i) {
		bo[i] = xe_bo_create(fd, eci->gt_id, vm, bo_size);
		data[i] = xe_bo_map(fd, bo[i], bo_size);
	}

	memset(sync_all, 0, sizeof(sync_all));
	for (i = 0; i < n_engines; i++) {
		engines[i] = xe_engine_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
		sync_all[i].flags = DRM_XE_SYNC_SYNCOBJ;
		sync_all[i].handle = syncobjs[i];
	};

	sync[0].handle = syncobj_create(fd, 0);
	for (i = 0; i < n_bo; ++i)
		xe_vm_bind_async(fd, vm, 0, bo[i], 0, addr + i * addr_stride,
				 bo_size, sync, i == n_bo - 1 ? 1 : 0);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i]->batch -
			(char *)data[i];
		uint64_t batch_addr = addr + i * addr_stride + batch_offset;
		uint64_t sdi_offset = (char *)&data[i]->data - (char *)data[i];
		uint64_t sdi_addr = addr + i * addr_stride + sdi_offset;
		int e = i % n_engines;

		b = 0;
		data[i]->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i]->batch[b++] = sdi_addr;
		data[i]->batch[b++] = sdi_addr >> 32;
		data[i]->batch[b++] = 0xc0ffee;
		data[i]->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i]->batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);
	}

	for (i = 0; i < n_bo; ++i) {
		if (i % 2)
			continue;

		sync_all[n_execs].flags = DRM_XE_SYNC_SIGNAL;
		sync_all[n_execs].handle = sync[0].handle;
		xe_vm_unbind_async(fd, vm, 0, 0, addr + i * addr_stride,
				   bo_size, sync_all, n_execs + 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0,
					NULL));
	}

	for (i = 0; i < n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i]->data, 0xc0ffee);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i]->batch -
			(char *)data[i];
		uint64_t batch_addr = addr + i * addr_stride + batch_offset;
		uint64_t sdi_offset = (char *)&data[i]->data - (char *)data[i];
		uint64_t sdi_addr = addr + i * addr_stride + sdi_offset;
		int e = i % n_engines;

		if (!(i % 2))
			continue;

		b = 0;
		memset(data[i], 0, sizeof(struct shared_pte_page_data));
		data[i]->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i]->batch[b++] = sdi_addr;
		data[i]->batch[b++] = sdi_addr >> 32;
		data[i]->batch[b++] = 0xc0ffee;
		data[i]->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i]->batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);
	}

	for (i = 0; i < n_bo; ++i) {
		if (!(i % 2))
			continue;

		sync_all[n_execs].flags = DRM_XE_SYNC_SIGNAL;
		sync_all[n_execs].handle = sync[0].handle;
		xe_vm_unbind_async(fd, vm, 0, 0, addr + i * addr_stride,
				   bo_size, sync_all, n_execs + 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0,
					NULL));
	}

	for (i = 0; i < n_execs; i++) {
		if (!(i % 2))
			continue;
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	}
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i]->data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	for (i = 0; i < n_bo; ++i) {
		munmap(data[i], bo_size);
		gem_close(fd, bo[i]);
	}
	free(data);
	xe_vm_destroy(fd, vm);
}


/**
 * SUBTEST: bind-engines-independent
 * Description: Test independent bind engines
 * Functionality: bind engines
 * Run type: BAT
 */

static void
test_bind_engines_independent(int fd, struct drm_xe_engine_class_instance *eci)
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
		.syncs = to_user_pointer(sync),
	};
#define N_ENGINES	2
	uint32_t engines[N_ENGINES];
	uint32_t bind_engines[N_ENGINES];
	uint32_t syncobjs[N_ENGINES + 1];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * N_ENGINES;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));
	bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < N_ENGINES; i++) {
		engines[i] = xe_engine_create(fd, vm, eci, 0);
		bind_engines[i] = xe_bind_engine_create(fd, vm, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	}
	syncobjs[N_ENGINES] = syncobj_create(fd, 0);

	/* Initial bind, needed for spinner */
	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, bind_engines[0], bo, 0, addr, bo_size,
			 sync, 1);

	for (i = 0; i < N_ENGINES; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t spin_addr = addr + spin_offset;
		int e = i;

		if (i == 0) {
			/* Cork 1st engine with a spinner */
			xe_spin_init(&data[i].spin, spin_addr, true);
			exec.engine_id = engines[e];
			exec.address = spin_addr;
			sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
			sync[1].flags |= DRM_XE_SYNC_SIGNAL;
			sync[1].handle = syncobjs[e];
			xe_exec(fd, &exec);
			xe_spin_wait_started(&data[i].spin);

			/* Do bind to 1st engine blocked on cork */
			addr += bo_size;
			sync[1].flags &= ~DRM_XE_SYNC_SIGNAL;
			sync[1].handle = syncobjs[e];
			xe_vm_bind_async(fd, vm, bind_engines[e], bo, 0, addr,
					 bo_size, sync + 1, 1);
			addr += bo_size;
		} else {
			/* Do bind to 2nd engine which blocks write below */
			sync[0].flags |= DRM_XE_SYNC_SIGNAL;
			xe_vm_bind_async(fd, vm, bind_engines[e], bo, 0, addr,
					 bo_size, sync, 1);
		}

		/*
		 * Write to either engine, 1st blocked on spinner + bind, 2nd
		 * just blocked on bind. The 2nd should make independent
		 * progress.
		 */
		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[!i ? N_ENGINES : e];

		exec.num_syncs = 2;
		exec.engine_id = engines[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);
	}

	/* Verify initial bind, bind + write to 2nd engine done */
	igt_assert(syncobj_wait(fd, &syncobjs[1], 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data[1].data, 0xc0ffee);

	/* Verify bind + write to 1st engine still inflight */
	igt_assert(!syncobj_wait(fd, &syncobjs[0], 1, 1, 0, NULL));
	igt_assert(!syncobj_wait(fd, &syncobjs[N_ENGINES], 1, 1, 0, NULL));

	/* Verify bind + write to 1st engine done after ending spinner */
	xe_spin_end(&data[0].spin);
	igt_assert(syncobj_wait(fd, &syncobjs[0], 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &syncobjs[N_ENGINES], 1, INT64_MAX, 0,
				NULL));
	igt_assert_eq(data[0].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < N_ENGINES; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
		xe_engine_destroy(fd, bind_engines[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define BIND_ARRAY_BIND_ENGINE_FLAG	(0x1 << 0)


/**
 * SUBTEST: bind-array-twice
 * Description: Test bind array twice
 * Functionality: bind engines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: bind-array-many
 * Description: Test bind array many times
 * Functionality: bind engines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: bind-array-engine-twice
 * Description: Test bind array engine twice
 * Functionality: bind engines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: bind-array-engine-many
 * Description: Test bind array engine many times
 * Functionality: bind engines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void
test_bind_array(int fd, struct drm_xe_engine_class_instance *eci, int n_execs,
		unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000, base_addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.syncs = to_user_pointer(sync),
	};
	uint32_t engine, bind_engine = 0;
#define BIND_ARRAY_MAX_N_EXEC	16
	struct drm_xe_vm_bind_op bind_ops[BIND_ARRAY_MAX_N_EXEC];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert(n_execs <= BIND_ARRAY_MAX_N_EXEC);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
	data = xe_bo_map(fd, bo, bo_size);

	if (flags & BIND_ARRAY_BIND_ENGINE_FLAG)
		bind_engine = xe_bind_engine_create(fd, vm, 0);
	engine = xe_engine_create(fd, vm, eci, 0);

	for (i = 0; i < n_execs; ++i) {
		bind_ops[i].obj = bo;
		bind_ops[i].obj_offset = 0;
		bind_ops[i].range = bo_size;
		bind_ops[i].addr = addr;
		bind_ops[i].gt_mask = 0x1 << eci->gt_id;
		bind_ops[i].op = XE_VM_BIND_OP_MAP | XE_VM_BIND_FLAG_ASYNC;
		bind_ops[i].region = 0;
		bind_ops[i].reserved[0] = 0;
		bind_ops[i].reserved[1] = 0;

		addr += bo_size;
	}

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_array(fd, vm, bind_engine, bind_ops, n_execs, sync, 1);

	addr = base_addr;
	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		if (i == n_execs - 1) {
			sync[1].handle = syncobj_create(fd, 0);
			exec.num_syncs = 2;
		} else {
			exec.num_syncs = 1;
		}

		exec.engine_id = engine;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += bo_size;
	}

	for (i = 0; i < n_execs; ++i) {
		bind_ops[i].obj = 0;
		bind_ops[i].op = XE_VM_BIND_OP_UNMAP | XE_VM_BIND_FLAG_ASYNC;
	}

	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	sync[1].flags &= ~DRM_XE_SYNC_SIGNAL;
	xe_vm_bind_array(fd, vm, bind_engine, bind_ops, n_execs, sync, 2);

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	xe_engine_destroy(fd, engine);
	if (bind_engine)
		xe_engine_destroy(fd, bind_engine);

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define LARGE_BIND_FLAG_MISALIGNED	(0x1 << 0)
#define LARGE_BIND_FLAG_SPLIT		(0x1 << 1)
#define LARGE_BIND_FLAG_USERPTR		(0x1 << 2)

/**
 * SUBTEST: %s-%ld
 * Description: Test %arg[1] with %arg[2] bind size
 * Functionality: bind
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @large-binds: large-binds
 * @large-split-binds: large-split-binds
 * @large-misaligned-binds: large-misaligned-binds
 * @large-split-misaligned-binds: large-split-misaligned-binds
 *
 * arg[2].values: 2097152, 4194304, 8388608, 16777216, 33554432
 * arg[2].values: 67108864, 134217728, 268435456, 536870912, 1073741824
 * arg[2].values: 2147483648
 */

/**
 * SUBTEST: %s-%ld
 * Description: Test %arg[1] with %arg[2] bind size
 * Functionality: userptr bind
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @large-userptr-binds: large-userptr-binds
 * @large-userptr-split-binds: large-userptr-split-binds
 * @large-userptr-misaligned-binds: large-userptr-misaligned-binds
 * @large-userptr-split-misaligned-binds: large-userptr-split-misaligned-binds
 *
 * arg[2].values: 2097152, 4194304, 8388608, 16777216, 33554432
 * arg[2].values: 67108864, 134217728, 268435456, 536870912, 1073741824
 * arg[2].values: 2147483648
 */

/**
 *
 * SUBTEST: %s-%ld
 * Description: Test %arg[1] with %arg[2] bind size
 * Functionality: mixed bind
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @mixed-binds: mixed-binds
 * @mixed-misaligned-binds: mixed-misaligned-binds
 *
 * arg[2].values: 3145728, 1611661312
 */

/**
 *
 * SUBTEST: %s-%ld
 * Description: Test %arg[1] with %arg[2] bind size
 * Functionality: mixed bind
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @mixed-userptr-binds: mixed-userptr-binds
 * @mixed-userptr-misaligned-binds: mixed-userptr-misaligned-binds
 * @mixed-userptr-binds: mixed-userptr-binds
 *
 * arg[2].values: 3145728, 1611661312
 */

static void
test_large_binds(int fd, struct drm_xe_engine_class_instance *eci,
		 int n_engines, int n_execs, size_t bo_size,
		 unsigned int flags)
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
	uint64_t addr = 0x1ull << 30, base_addr = 0x1ull << 30;
	uint32_t vm;
	uint32_t engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	uint32_t bo = 0;
	void *map;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	if (flags & LARGE_BIND_FLAG_MISALIGNED) {
		addr -= xe_get_default_alignment(fd);
		base_addr -= xe_get_default_alignment(fd);
	}

	igt_assert(n_engines <= MAX_N_ENGINES);
	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);

	if (flags & LARGE_BIND_FLAG_USERPTR) {
		map = aligned_alloc(xe_get_default_alignment(fd), bo_size);
		igt_assert(map);
	} else {
		bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
		map = xe_bo_map(fd, bo, bo_size);
	}

	for (i = 0; i < n_engines; i++) {
		engines[i] = xe_engine_create(fd, vm, eci, 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};

	sync[0].handle = syncobj_create(fd, 0);
	if (flags & LARGE_BIND_FLAG_USERPTR) {
		if (flags & LARGE_BIND_FLAG_SPLIT) {
			xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(map),
						 addr, bo_size / 2, NULL, 0);
			xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(map) + bo_size / 2,
						 addr + bo_size / 2, bo_size / 2,
						 sync, 1);
		} else {
			xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(map),
						 addr, bo_size, sync, 1);
		}
	} else {
		if (flags & LARGE_BIND_FLAG_SPLIT) {
			xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size / 2, NULL, 0);
			xe_vm_bind_async(fd, vm, 0, bo, bo_size / 2, addr + bo_size / 2,
					 bo_size / 2, sync, 1);
		} else {
			xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
		}
	}

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		int e = i % n_engines;

		data = map + (addr - base_addr);
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

		if (i != e)
			syncobj_reset(fd, &sync[1].handle, 1);

		exec.engine_id = engines[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		if (i + 1 != n_execs)
			addr += bo_size / n_execs;
		else
			addr = base_addr + bo_size - 0x1000;
	}

	for (i = 0; i < n_engines; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	if (flags & LARGE_BIND_FLAG_SPLIT) {
		xe_vm_unbind_async(fd, vm, 0, 0, base_addr,
				   bo_size / 2, NULL, 0);
		xe_vm_unbind_async(fd, vm, 0, 0, base_addr + bo_size / 2,
				   bo_size / 2, sync, 1);
	} else {
		xe_vm_unbind_async(fd, vm, 0, 0, base_addr, bo_size,
				   sync, 1);
	}
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	addr = base_addr;
	for (i = 0; i < n_execs; i++) {
		data = map + (addr - base_addr);
		igt_assert_eq(data[i].data, 0xc0ffee);

		if (i + 1 != n_execs)
			addr += bo_size / n_execs;
		else
			addr = base_addr + bo_size - 0x1000;
	}

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_engine_destroy(fd, engines[i]);
	}

	if (bo) {
		munmap(map, bo_size);
		gem_close(fd, bo);
	} else {
		free(map);
	}
	xe_vm_destroy(fd, vm);
}

struct thread_data {
	pthread_t thread;
	pthread_barrier_t *barrier;
	int fd;
	uint32_t vm;
	uint64_t addr;
	struct drm_xe_engine_class_instance *eci;
	void *map;
	int *exit;
};

static void *hammer_thread(void *tdata)
{
	struct thread_data *t = tdata;
	struct drm_xe_sync sync[1] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data = t->map;
	uint32_t engine = xe_engine_create(t->fd, t->vm, t->eci, 0);
	int b;
	int i = 0;

	sync[0].handle = syncobj_create(t->fd, 0);
	pthread_barrier_wait(t->barrier);

	while (!*t->exit) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = t->addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = t->addr + sdi_offset;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data->batch));

		exec.engine_id = engine;
		exec.address = batch_addr;
		if (i % 32) {
			exec.num_syncs = 0;
			xe_exec(t->fd, &exec);
		} else {
			exec.num_syncs = 1;
			xe_exec(t->fd, &exec);
			igt_assert(syncobj_wait(t->fd, &sync[0].handle, 1,
						INT64_MAX, 0, NULL));
			syncobj_reset(t->fd, &sync[0].handle, 1);
		}
		++i;
	}

	syncobj_destroy(t->fd, sync[0].handle);
	xe_engine_destroy(t->fd, engine);

	return NULL;
}

#define MUNMAP_FLAG_USERPTR		(0x1 << 0)
#define MUNMAP_FLAG_INVALIDATE		(0x1 << 1)
#define MUNMAP_FLAG_HAMMER_FIRST_PAGE	(0x1 << 2)


/**
 * SUBTEST: munmap-style-unbind-%s
 * Description: Test munmap style unbind with %arg[1]
 * Functionality: unbind
 *
 * arg[1]:
 *
 * @one-partial:			one partial
 * @end:				end
 * @front:				front
 * @userptr-one-partial:		userptr one partial
 * @userptr-end:			userptr end
 * @userptr-front:			userptr front
 * @userptr-inval-end:			userptr inval end
 * @userptr-inval-front:		userptr inval front
 * Run type: BAT
 */

/**
 * SUBTEST: munmap-style-unbind-%s
 * Description: Test munmap style unbind with %arg[1]
 * Functionality: unbind
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * arg[1]:
 *
 * @all:				all
 * @either-side-partial:		either side partial
 * @either-side-partial-hammer:		either side partial hammer
 * @either-side-full:			either side full
 * @many-all:				many all
 * @many-either-side-partial:		many either side partial
 * @many-either-side-partial-hammer:	many either side partial hammer
 * @many-either-side-full:		many either side full
 * @many-end:				many end
 * @many-front:				many front
 * @userptr-all:			userptr all
 * @userptr-either-side-partial:	userptr either side partial
 * @userptr-either-side-full:		userptr either side full
 * @userptr-many-all:			userptr many all
 * @userptr-many-either-side-full:	userptr many either side full
 * @userptr-many-end:			userptr many end
 * @userptr-many-front:			userptr many front
 * @userptr-inval-either-side-full:	userptr inval either side full
 * @userptr-inval-many-all:		userptr inval many all
 * @userptr-inval-many-either-side-partial:
 *					userptr inval many either side partial
 * @userptr-inval-many-either-side-full:
 *					userptr inval many either side full
 * @userptr-inval-many-end:		userptr inval many end
 * @userptr-inval-many-front:		userptr inval many front
 */

static void
test_munmap_style_unbind(int fd, struct drm_xe_engine_class_instance *eci,
			 int bo_n_pages, int n_binds,
			 int unbind_n_page_offfset, int unbind_n_pages,
			 unsigned int flags)
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
	uint64_t addr = 0x1a0000, base_addr = 0x1a0000;
	uint32_t vm;
	uint32_t engine;
	size_t bo_size;
	uint32_t bo = 0;
	uint64_t bind_size;
	uint64_t page_size = xe_get_default_alignment(fd);
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	void *map;
	int i, b;
	int invalidate = 0;
	struct thread_data t;
	pthread_barrier_t barrier;
	int exit = 0;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = page_size * bo_n_pages;

	if (flags & MUNMAP_FLAG_USERPTR) {
		map = mmap(from_user_pointer(addr), bo_size, PROT_READ |
			    PROT_WRITE, MAP_SHARED | MAP_FIXED |
			    MAP_ANONYMOUS, -1, 0);
		igt_assert(map != MAP_FAILED);
	} else {
		bo = xe_bo_create(fd, eci->gt_id, vm, bo_size);
		map = xe_bo_map(fd, bo, bo_size);
	}
	memset(map, 0, bo_size);

	engine = xe_engine_create(fd, vm, eci, 0);

	sync[0].handle = syncobj_create(fd, 0);
	sync[1].handle = syncobj_create(fd, 0);

	/* Do initial binds */
	bind_size = (page_size * bo_n_pages) / n_binds;
	for (i = 0; i < n_binds; ++i) {
		if (flags & MUNMAP_FLAG_USERPTR)
			xe_vm_bind_userptr_async(fd, vm, 0, addr, addr,
						 bind_size, sync, 1);
		else
			xe_vm_bind_async(fd, vm, 0, bo, i * bind_size,
					 addr, bind_size, sync, 1);
		addr += bind_size;
	}
	addr = base_addr;

	/*
	 * Kick a thread to write the first page continously to ensure we can't
	 * cause a fault if a rebind occurs during munmap style VM unbind
	 * (partial VMAs unbound).
	 */
	if (flags & MUNMAP_FLAG_HAMMER_FIRST_PAGE) {
		t.fd = fd;
		t.vm = vm;
#define PAGE_SIZE	4096
		t.addr = addr + PAGE_SIZE / 2;
		t.eci = eci;
		t.exit = &exit;
		t.map = map + PAGE_SIZE / 2;
		t.barrier = &barrier;
		pthread_barrier_init(&barrier, NULL, 2);
		pthread_create(&t.thread, 0, hammer_thread, &t);
		pthread_barrier_wait(&barrier);
	}

	/* Verify we can use every page */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		data = map + i * page_size;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		if (i)
			syncobj_reset(fd, &sync[1].handle, 1);
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;

		exec.engine_id = engine;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += page_size;
	}
	addr = base_addr;

	/* Unbind some of the pages */
	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	sync[1].flags &= ~DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0,
			   addr + unbind_n_page_offfset * page_size,
			   unbind_n_pages * page_size, sync, 2);

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages written */
	for (i = 0; i < n_binds; ++i) {
		data = map + i * page_size;
		igt_assert_eq(data->data, 0xc0ffee);
	}
	if (flags & MUNMAP_FLAG_HAMMER_FIRST_PAGE) {
		memset(map, 0, PAGE_SIZE / 2);
		memset(map + PAGE_SIZE, 0, bo_size - PAGE_SIZE);
	} else {
		memset(map, 0, bo_size);
	}

try_again_after_invalidate:
	/* Verify we can use every page still bound */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;

		data = map + i * page_size;
		addr += page_size;

		if (i < unbind_n_page_offfset ||
		    i + 1 > unbind_n_page_offfset + unbind_n_pages) {
			b = 0;
			data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data->batch[b++] = sdi_addr;
			data->batch[b++] = sdi_addr >> 32;
			data->batch[b++] = 0xc0ffee;
			data->batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
			syncobj_reset(fd, &sync[1].handle, 1);
			sync[1].flags |= DRM_XE_SYNC_SIGNAL;

			exec.engine_id = engine;
			exec.address = batch_addr;
			xe_exec(fd, &exec);
		}
	}
	addr = base_addr;

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages still bound written */
	for (i = 0; i < n_binds; ++i) {
		if (i < unbind_n_page_offfset ||
		    i + 1 > unbind_n_page_offfset + unbind_n_pages) {
			data = map + i * page_size;
			igt_assert_eq(data->data, 0xc0ffee);
		}
	}
	if (flags & MUNMAP_FLAG_HAMMER_FIRST_PAGE) {
		memset(map, 0, PAGE_SIZE / 2);
		memset(map + PAGE_SIZE, 0, bo_size - PAGE_SIZE);
	} else {
		memset(map, 0, bo_size);
	}

	/*
	 * The munmap style VM unbind can create new VMAs, make sure those are
	 * in the bookkeeping for another rebind after a userptr invalidate.
	 */
	if (flags & MUNMAP_FLAG_INVALIDATE && !invalidate++) {
		map = mmap(from_user_pointer(addr), bo_size, PROT_READ |
			    PROT_WRITE, MAP_SHARED | MAP_FIXED |
			    MAP_ANONYMOUS, -1, 0);
		igt_assert(data != MAP_FAILED);
		goto try_again_after_invalidate;
	}

	/* Confirm unbound region can be rebound */
	syncobj_reset(fd, &sync[0].handle, 1);
	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	if (flags & MUNMAP_FLAG_USERPTR)
		xe_vm_bind_userptr_async(fd, vm, 0,
					 addr + unbind_n_page_offfset * page_size,
					 addr + unbind_n_page_offfset * page_size,
					 unbind_n_pages * page_size, sync, 1);
	else
		xe_vm_bind_async(fd, vm, 0, bo,
				 unbind_n_page_offfset * page_size,
				 addr + unbind_n_page_offfset * page_size,
				 unbind_n_pages * page_size, sync, 1);

	/* Verify we can use every page */
	for (i = 0; i < n_binds; ++i) {
		uint64_t batch_offset = (char *)&data->batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data->data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		data = map + i * page_size;

		b = 0;
		data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data->batch[b++] = sdi_addr;
		data->batch[b++] = sdi_addr >> 32;
		data->batch[b++] = 0xc0ffee;
		data->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		syncobj_reset(fd, &sync[1].handle, 1);
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;

		exec.engine_id = engine;
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		addr += page_size;
	}
	addr = base_addr;

	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert(syncobj_wait(fd, &sync[1].handle, 1, INT64_MAX, 0, NULL));

	/* Verify all pages written */
	for (i = 0; i < n_binds; ++i) {
		data = map + i * page_size;
		igt_assert_eq(data->data, 0xc0ffee);
	}

	if (flags & MUNMAP_FLAG_HAMMER_FIRST_PAGE) {
		exit = 1;
		pthread_join(t.thread, NULL);
		pthread_barrier_destroy(&barrier);
	}

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, sync[1].handle);
	xe_engine_destroy(fd, engine);
	munmap(map, bo_size);
	if (bo)
		gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe, *hwe_non_copy = NULL;
	uint64_t bind_size;
	int fd;
	const struct section {
		const char *name;
		int bo_n_pages;
		int n_binds;
		int unbind_n_page_offfset;
		int unbind_n_pages;
		unsigned int flags;
	} sections[] = {
		{ "all", 4, 2, 0, 4, 0 },
		{ "one-partial", 4, 1, 1, 2, 0 },
		{ "either-side-partial", 4, 2, 1, 2, 0 },
		{ "either-side-partial-hammer", 4, 2, 1, 2,
			MUNMAP_FLAG_HAMMER_FIRST_PAGE },
		{ "either-side-full", 4, 4, 1, 2, 0 },
		{ "end", 4, 2, 0, 3, 0 },
		{ "front", 4, 2, 1, 3, 0 },
		{ "many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8, 0 },
		{ "many-either-side-partial", 4 * 8, 2 * 8, 1, 4 * 8 - 2, 0 },
		{ "many-either-side-partial-hammer", 4 * 8, 2 * 8, 1, 4 * 8 - 2,
			MUNMAP_FLAG_HAMMER_FIRST_PAGE },
		{ "many-either-side-full", 4 * 8, 4 * 8, 1 * 8, 2 * 8, 0 },
		{ "many-end", 4 * 8, 4, 0 * 8, 3 * 8 + 2, 0 },
		{ "many-front", 4 * 8, 4, 1 * 8 - 2, 3 * 8 + 2, 0 },
		{ "userptr-all", 4, 2, 0, 4, MUNMAP_FLAG_USERPTR },
		{ "userptr-one-partial", 4, 1, 1, 2, MUNMAP_FLAG_USERPTR },
		{ "userptr-either-side-partial", 4, 2, 1, 2,
			MUNMAP_FLAG_USERPTR },
		{ "userptr-either-side-full", 4, 4, 1, 2,
			MUNMAP_FLAG_USERPTR },
		{ "userptr-end", 4, 2, 0, 3, MUNMAP_FLAG_USERPTR },
		{ "userptr-front", 4, 2, 1, 3, MUNMAP_FLAG_USERPTR },
		{ "userptr-many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8,
			MUNMAP_FLAG_USERPTR },
		{ "userptr-many-either-side-full", 4 * 8, 4 * 8, 1 * 8, 2 * 8,
			MUNMAP_FLAG_USERPTR },
		{ "userptr-many-end", 4 * 8, 4, 0 * 8, 3 * 8 + 2,
			MUNMAP_FLAG_USERPTR },
		{ "userptr-many-front", 4 * 8, 4, 1 * 8 - 2, 3 * 8 + 2,
			MUNMAP_FLAG_USERPTR },
		{ "userptr-inval-either-side-full", 4, 4, 1, 2,
			MUNMAP_FLAG_USERPTR | MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-end", 4, 2, 0, 3, MUNMAP_FLAG_USERPTR |
			MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-front", 4, 2, 1, 3, MUNMAP_FLAG_USERPTR |
			MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-all", 4 * 8, 2 * 8, 0 * 8, 4 * 8,
			MUNMAP_FLAG_USERPTR | MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-either-side-partial", 4 * 8, 2 * 8, 1,
			4 * 8 - 2, MUNMAP_FLAG_USERPTR |
				MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-either-side-full", 4 * 8, 4 * 8, 1 * 8,
			2 * 8, MUNMAP_FLAG_USERPTR | MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-end", 4 * 8, 4, 0 * 8, 3 * 8 + 2,
			MUNMAP_FLAG_USERPTR | MUNMAP_FLAG_INVALIDATE },
		{ "userptr-inval-many-front", 4 * 8, 4, 1 * 8 - 2, 3 * 8 + 2,
			MUNMAP_FLAG_USERPTR | MUNMAP_FLAG_INVALIDATE },
		{ NULL },
	};

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);

		xe_for_each_hw_engine(fd, hwe)
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COPY) {
				hwe_non_copy = hwe;
				break;
			}
	}

	igt_subtest("bind-once")
		test_bind_once(fd);

	igt_subtest("bind-one-bo-many-times")
		test_bind_one_bo_many_times(fd);

	igt_subtest("bind-one-bo-many-times-many-vm")
		test_bind_one_bo_many_times_many_vm(fd);

	igt_subtest("scratch")
		test_scratch(fd);

	igt_subtest("unbind-all-2-vmas")
		unbind_all(fd, 2);

	igt_subtest("unbind-all-8-vmas")
		unbind_all(fd, 8);

	igt_subtest("userptr-invalid")
		userptr_invalid(fd);

	igt_subtest("vm-async-ops-err")
		vm_async_ops_err(fd, false);

	igt_subtest("vm-async-ops-err-destroy")
		vm_async_ops_err(fd, true);

	igt_subtest("shared-pte-page")
		xe_for_each_hw_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4,
					xe_get_default_alignment(fd));

	igt_subtest("shared-pde-page")
		xe_for_each_hw_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4, 0x1000ul * 512);

	igt_subtest("shared-pde2-page")
		xe_for_each_hw_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4, 0x1000ul * 512 * 512);

	igt_subtest("shared-pde3-page")
		xe_for_each_hw_engine(fd, hwe)
			shared_pte_page(fd, hwe, 4, 0x1000ul * 512 * 512 * 512);

	igt_subtest("bind-engines-independent")
		xe_for_each_hw_engine(fd, hwe)
			test_bind_engines_independent(fd, hwe);

	igt_subtest("bind-array-twice")
		xe_for_each_hw_engine(fd, hwe)
			test_bind_array(fd, hwe, 2, 0);

	igt_subtest("bind-array-many")
		xe_for_each_hw_engine(fd, hwe)
			test_bind_array(fd, hwe, 16, 0);

	igt_subtest("bind-array-engine-twice")
		xe_for_each_hw_engine(fd, hwe)
			test_bind_array(fd, hwe, 2,
					BIND_ARRAY_BIND_ENGINE_FLAG);

	igt_subtest("bind-array-engine-many")
		xe_for_each_hw_engine(fd, hwe)
			test_bind_array(fd, hwe, 16,
					BIND_ARRAY_BIND_ENGINE_FLAG);

	for (bind_size = 0x1ull << 21; bind_size <= 0x1ull << 31;
	     bind_size = bind_size << 1) {
		igt_subtest_f("large-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size, 0);
				break;
			}
		igt_subtest_f("large-split-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT);
				break;
			}
		igt_subtest_f("large-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_MISALIGNED);
				break;
			}
		igt_subtest_f("large-split-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT |
						 LARGE_BIND_FLAG_MISALIGNED);
				break;
			}
		igt_subtest_f("large-userptr-binds-%lld", (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
		igt_subtest_f("large-userptr-split-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT |
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
		igt_subtest_f("large-userptr-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_MISALIGNED |
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
		igt_subtest_f("large-userptr-split-misaligned-binds-%lld",
			      (long long)bind_size)
			xe_for_each_hw_engine(fd, hwe) {
				test_large_binds(fd, hwe, 4, 16, bind_size,
						 LARGE_BIND_FLAG_SPLIT |
						 LARGE_BIND_FLAG_MISALIGNED |
						 LARGE_BIND_FLAG_USERPTR);
				break;
			}
	}

	bind_size = (0x1ull << 21) + (0x1ull << 20);
	igt_subtest_f("mixed-binds-%lld", (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size, 0);
			break;
		}

	igt_subtest_f("mixed-misaligned-binds-%lld", (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-binds-%lld", (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size, 0);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-misaligned-binds-%lld", (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED);
			break;
		}

	bind_size = (0x1ull << 21) + (0x1ull << 20);
	igt_subtest_f("mixed-userptr-binds-%lld", (long long) bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	igt_subtest_f("mixed-userptr-misaligned-binds-%lld",
		      (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED |
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-userptr-binds-%lld", (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	bind_size = (0x1ull << 30) + (0x1ull << 29) + (0x1ull << 20);
	igt_subtest_f("mixed-userptr-misaligned-binds-%lld",
		      (long long)bind_size)
		xe_for_each_hw_engine(fd, hwe) {
			test_large_binds(fd, hwe, 4, 16, bind_size,
					 LARGE_BIND_FLAG_MISALIGNED |
					 LARGE_BIND_FLAG_USERPTR);
			break;
		}

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("munmap-style-unbind-%s", s->name) {
			igt_require_f(hwe_non_copy,
				      "Requires non-copy engine to run\n");

			test_munmap_style_unbind(fd, hwe_non_copy,
						 s->bo_n_pages,
						 s->n_binds,
						 s->unbind_n_page_offfset,
						 s->unbind_n_pages,
						 s->flags);
		}
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
