/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <sys/ioctl.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

#define MAX_ENGINES 64

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

static uint32_t batch_create(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle = gem_create(i915, 4096);
	gem_write(i915, handle, 0, &bbe, sizeof(bbe));
	return handle;
}

struct supervisor {
	int device;
	uint32_t handle;
	uint32_t context;

	uint32_t *map;
	uint32_t *semaphore;
	uint32_t *terminate;
	uint64_t *dispatch;
};

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static uint32_t __supervisor_create_context(int i915,
					    const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_context_create_ext_setparam p_ring = {
		{
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = 0
		},
		{
			.param = I915_CONTEXT_PARAM_RINGSIZE,
			.value = 4096,
		},
	};
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 2) = {
		.engines = {
			{ e->class, e->instance },
			{ e->class, e->instance },
		}
	};
	struct drm_i915_gem_context_create_ext_setparam p_engines = {
		{
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = to_user_pointer(&p_ring)
		},
		{
			.param = I915_CONTEXT_PARAM_ENGINES,
			.value = to_user_pointer(&engines),
			.size = sizeof(engines),
		},
	};
	struct drm_i915_gem_context_create_ext_setparam p_persistence = {
		{
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = to_user_pointer(&p_engines)
		},
		{
			.param = I915_CONTEXT_PARAM_PERSISTENCE,
			.value = 0
		},
	};
	struct drm_i915_gem_context_create_ext create = {
		.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
		.extensions = to_user_pointer(&p_persistence),
	};

	ioctl(i915, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &create);
	return create.ctx_id;
}

static void __supervisor_create(int i915,
				const struct intel_execution_engine2 *e,
				struct supervisor *sv)
{
	sv->device = i915;
	sv->context = __supervisor_create_context(i915, e);
	igt_require(sv->context);

	sv->handle = gem_create(i915, 4096);
	sv->map = gem_mmap__device_coherent(i915, sv->handle,
					    0, 4096, PROT_WRITE);
}

static void __supervisor_run(struct supervisor *sv)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = sv->handle,
		.flags = EXEC_OBJECT_PINNED
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.rsvd1 = sv->context,
	};
	uint32_t *cs = sv->map;

	sv->semaphore = cs + 1000;

	*cs++ = MI_SEMAPHORE_WAIT |
		MI_SEMAPHORE_POLL |
		MI_SEMAPHORE_SAD_EQ_SDD |
		(4 - 2);
	*cs++ = 1;
	*cs++ = offset_in_page(sv->semaphore);
	*cs++ = 0;

	sv->terminate = cs;
	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = offset_in_page(sv->semaphore);
	*cs++ = 0;
	*cs++ = 0;

	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	sv->dispatch = (uint64_t *)cs; /* to be filled in later */

	gem_execbuf(sv->device, &execbuf);
	igt_assert_eq_u64(obj.offset, 0);
}

static void supervisor_open(int i915,
			    const struct intel_execution_engine2 *e,
			    struct supervisor *sv)
{
	__supervisor_create(i915, e, sv);
	__supervisor_run(sv);
}

static void supervisor_dispatch(struct supervisor *sv, uint64_t addr)
{
	/* XXX How strongly ordered are WC writes to different cachelines? */
	WRITE_ONCE(*sv->dispatch, addr);
	WRITE_ONCE(*sv->semaphore, 1);
	__sync_synchronize();
}

static void legacy_supervisor_bind(struct supervisor *sv, uint32_t handle, uint64_t addr)
{
	struct drm_i915_gem_exec_object2 obj[2] = {
		{
			.handle = handle,
			.offset = addr,
			.flags = EXEC_OBJECT_PINNED
		},
		{
			.handle = batch_create(sv->device)
		}
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.rsvd1 = sv->context,
		.flags = 1, /* legacy bind engine */
	};

	gem_execbuf(sv->device, &execbuf);
	gem_close(sv->device, obj[1].handle);

	gem_sync(sv->device, handle); /* must wait for async binds */
}

static void emit_bbe_chain(uint32_t *cs)
{
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	*cs++ = 0;
	*cs++ = 0;
}

static void supervisor_close(struct supervisor *sv)
{
	WRITE_ONCE(*sv->terminate, MI_BATCH_BUFFER_END);
	WRITE_ONCE(*sv->semaphore, 1);
	__sync_synchronize();
	munmap(sv->map, 4096);

	gem_sync(sv->device, sv->handle);
	gem_close(sv->device, sv->handle);

	gem_context_destroy(sv->device, sv->context);
}

static int read_timestamp_frequency(int i915)
{
	int value = 0;
	drm_i915_getparam_t gp = {
		.value = &value,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};
	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return value;
}

static int cmp_u32(const void *A, const void *B)
{
	const uint32_t *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static uint32_t trifilter(uint32_t *x)
{
	qsort(x, 5, sizeof(*x), cmp_u32);
	return (x[1] + 2 * x[2] + x[3]) / 4;
}

#define TIMESTAMP (0x358)
static void endless_dispatch(int i915, const struct intel_execution_engine2 *e)
{
	const uint32_t mmio_base = gem_engine_mmio_base(i915, e->name);
	const int cs_timestamp_freq = read_timestamp_frequency(i915);
	uint32_t handle, *cs, *map;
	struct supervisor sv;
	uint32_t latency[5];
	uint32_t *timestamp;
	uint32_t *result;

	/*
	 * Launch a supervisor bb.
	 * Wait on semaphore.
	 * Bind second bb.
	 * Write new address into MI_BB_START
	 * Release semaphore.
	 *
	 * Check we see the second bb execute.
	 *
	 * Chain MI_BB_START to supervisor bb (replacing BBE).
	 *
	 * Final dispatch is BBE.
	 */

	igt_require(gem_class_has_mutable_submission(i915, e->class));

	igt_require(mmio_base);
	timestamp = (void *)igt_global_mmio + mmio_base + TIMESTAMP;

	supervisor_open(i915, e, &sv);
	result = sv.semaphore + 1;

	handle = gem_create(i915, 4096);
	cs = map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);
	*cs++ = 0x24 << 23 | 2; /* SRM */
	*cs++ = mmio_base + TIMESTAMP;
	*cs++ = offset_in_page(result);
	*cs++ = 0;
	emit_bbe_chain(cs);
	munmap(map, 4096);
	legacy_supervisor_bind(&sv, handle, 64 << 10);

	for (int pass = 0; pass < ARRAY_SIZE(latency); pass++) {
		uint32_t start, end;

		WRITE_ONCE(*result, 0);
		start = READ_ONCE(*timestamp);
		supervisor_dispatch(&sv, 64 << 10);
		while (!(end = READ_ONCE(*result)))
			;

		igt_assert_eq(READ_ONCE(*sv.semaphore), 0);
		latency[pass] = end - start;
	}

	latency[0] = trifilter(latency);
	igt_info("Dispatch latency: %u cycles, %.0fns\n",
		 latency[0], latency[0] * 1e9 / cs_timestamp_freq);

	supervisor_close(&sv);

	gem_close(i915, handle);
}

#define test_each_engine(T, i915, e) \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		for_each_if(gem_class_can_store_dword(i915, (e)->class)) \
			igt_dynamic_f("%s", (e)->name)

static void pin_rps(int sysfs)
{
	unsigned int max;

	if (igt_sysfs_scanf(sysfs, "gt_RP0_freq_mhz", "%u", &max) != 1)
		return;

	igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", max);
	igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", max);
	igt_sysfs_printf(sysfs, "gt_boost_freq_mhz", "%u", max);
}

static void unpin_rps(int sysfs)
{
	unsigned int v;

	if (igt_sysfs_scanf(sysfs, "gt_RPn_freq_mhz", "%u", &v) == 1)
		igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", v);

	if (igt_sysfs_scanf(sysfs, "gt_RP0_freq_mhz", "%u", &v) == 1) {
		igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", v);
		igt_sysfs_printf(sysfs, "gt_boost_freq_mhz", "%u", v);
	}
}

igt_main
{
	const struct intel_execution_engine2 *e;
	int i915 = -1;

	igt_skip_on_simulation();

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
	}

	igt_subtest_group {
		struct intel_mmio_data mmio;
		int sysfs;

		igt_fixture {
			igt_require(gem_scheduler_enabled(i915));
			igt_require(gem_scheduler_has_preemption(i915));

			intel_register_access_init(&mmio,
						   igt_device_get_pci_device(i915),
						   false, i915);

			sysfs = igt_sysfs_open(i915);
			pin_rps(sysfs);
		}

		test_each_engine("dispatch", i915, e)
				endless_dispatch(i915, e);

		igt_fixture {
			unpin_rps(sysfs);
			close(sysfs);
			intel_register_access_fini(&mmio);
		}
	}
}
