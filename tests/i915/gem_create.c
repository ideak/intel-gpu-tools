/*
 * Copyright © 2015 Intel Corporation
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
 *
 * Authors:
 *    Ankitprasad Sharma <ankitprasad.r.sharma at intel.com>
 *
 */

/** @file gem_create.c
 *
 * This is a test for the gem_create ioctl. The goal is to simply ensure that
 * basics work and invalid input combinations are rejected.
 */

#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <signal.h>

#include "drm.h"
#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "intel_batchbuffer.h"
#include "intel_io.h"
#include "intel_chipset.h"
#include "igt_aux.h"
#include "igt_dummyload.h"
#include "igt_types.h"
#include "igt_x86.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"
#include "i915/intel_memory_region.h"
#include "i915_drm.h"

IGT_TEST_DESCRIPTION("Ensure that basic gem_create and gem_create_ext works"
		     " and that invalid input combinations are rejected.");

#define PAGE_SIZE 4096

static int create_ioctl(int fd, struct drm_i915_gem_create *create)
{
        int err = 0;

        if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, create)) {
                err = -errno;
                igt_assume(err != 0);
        }

        errno = 0;
        return err;
}

static void invalid_size_test(int fd)
{
	struct drm_i915_gem_create create = {};

	create.size = 0; /* zero-sized objects are not allowed */
	igt_assert_eq(create_ioctl(fd, &create), -EINVAL);

	create.size = -1ull; /* will wrap to 0 on aligning to page */
	igt_assert_eq(create_ioctl(fd, &create), -EINVAL);

	igt_assert_eq(create.handle, 0);
}

static void massive_test(int fd)
{
	struct drm_i915_gem_create create = {};

	/* No system has this much memory... Yet small enough not to wrap */
	create.size = -1ull << 32;
	igt_assert_eq(create_ioctl(fd, &create), -E2BIG);

	igt_assert_eq(create.handle, 0);
}

/*
 * Creating an object with non-aligned size request and assert the buffer is
 * page aligned. And test the write into the padded extra memory.
 */
static void valid_nonaligned_size(int fd)
{
	struct drm_i915_gem_create create = {
		.size = PAGE_SIZE / 2,
	};
	char buf[PAGE_SIZE];

	igt_assert_eq(create_ioctl(fd, &create), 0);
	igt_assert(create.size >= PAGE_SIZE);

	gem_write(fd, create.handle, PAGE_SIZE / 2, buf, PAGE_SIZE / 2);

	gem_close(fd, create.handle);
}

static uint64_t atomic_compare_swap_u64(_Atomic(uint64_t) *ptr,
					uint64_t oldval, uint64_t newval)
{
	atomic_compare_exchange_strong(ptr, &oldval, newval);
	return oldval;
}

static uint64_t get_npages(_Atomic(uint64_t) *global, uint64_t npages)
{
	uint64_t try, old, max;

	max = *global;
	do {
		old = max;
		try = 1 + npages % (max / 2);
		max -= try;
	} while ((max = atomic_compare_swap_u64(global, old, max)) != old);

	return try;
}

struct thread_clear {
	_Atomic(uint64_t) max;
	struct drm_i915_gem_memory_class_instance region;
	int timeout;
	int i915;
};

static uint32_t batch_create(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle = gem_create(i915, sizeof(bbe));

	gem_write(i915, handle, 0, &bbe, sizeof(bbe));
	return handle;
}

static void make_resident(int i915, uint32_t batch, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 obj[2] = {
		[0] = {
			.handle = handle,
			.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS,
		},
		[1] = {
			.handle = batch ?: batch_create(i915),
			.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS,
		},
	};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
	};
	int err;

	err = __gem_execbuf(i915, &eb);
	if (obj[1].handle != batch)
		gem_close(i915, obj[1].handle);

	igt_assert(err == 0 || err == -E2BIG || err == -ENOSPC);
}

static void *thread_clear(void *data)
{
	struct thread_clear *arg = data;
	unsigned long checked = 0, total = 0;
	enum { PRW, GTT, WC, WB, __LAST__, FIXED } mode = PRW;
	int i915 = arg->i915;
	uint32_t batch = batch_create(i915);

	if (__gem_write(i915, 0, 0, 0, 0) == -EOPNOTSUPP)
		mode = FIXED;

	igt_until_timeout(arg->timeout) {
		uint64_t npages, size;
		uint32_t handle;
		void *ptr;

		npages = random();
		npages <<= 32;
		npages |= random();
		npages = get_npages(&arg->max, npages);
		size = npages << 12;

		igt_assert_eq(__gem_create_in_memory_region_list(i915, &handle, &size, 0, &arg->region, 1), 0);
		if (random() & 1)
			make_resident(i915, batch, handle);

		switch (mode) {
		case FIXED:
			ptr = __gem_mmap_offset__fixed(i915, handle, 0, size, PROT_READ);
			break;
		case __LAST__:
		case PRW:
			ptr = NULL;
			break;
		case WB:
			ptr = __gem_mmap__cpu(i915, handle, 0, size, PROT_READ);
			break;
		case WC:
			ptr = __gem_mmap__wc(i915, handle, 0, size, PROT_READ);
			break;
		case GTT:
			ptr = __gem_mmap__gtt(i915, handle, size, PROT_READ);
			break;
		}
		/* No set-domains as we are being as naughty as possible */

		for (uint64_t page = 0; page < npages; page += 1 + random() % (npages - page)) {
			uint64_t x[8] = {
				page * 4096 +
				sizeof(x) * ((page % (4096 - sizeof(x)) / sizeof(x)))
			};

			if (!ptr)
				gem_read(i915, handle, x[0], x, sizeof(x));
			else if (page & 1)
				igt_memcpy_from_wc(x, ptr + x[0], sizeof(x));
			else
				memcpy(x, ptr + x[0], sizeof(x));

			for (int i = 0; i < ARRAY_SIZE(x); i++)
				igt_assert_eq_u64(x[i], 0);

			checked++;
		}
		if (ptr)
			munmap(ptr, size);
		gem_close(i915, handle);

		total += npages;
		atomic_fetch_add(&arg->max, npages);

		if (mode < __LAST__ && ++mode == __LAST__)
			mode = PRW;
	}
	gem_close(i915, batch);

	igt_info("Checked %'lu / %'lu pages\n", checked, total);
	return (void *)(uintptr_t)checked;
}

static void always_clear(int i915, const struct gem_memory_region *r, int timeout)
{
	struct thread_clear arg = {
		.i915 = i915,
		.region = r->ci,
		.max = r->cpu_size / 2 >> 12, /* in pages */
		.timeout = timeout,
	};
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned long checked;
	pthread_t thread[ncpus];
	void *result;

	for (int i = 0; i < ncpus; i++)
		pthread_create(&thread[i], NULL, thread_clear, &arg);

	checked = 0;
	for (int i = 0; i < ncpus; i++) {
		pthread_join(thread[i], &result);
		checked += (uintptr_t)result;
	}
	igt_info("Checked %'lu page allocations\n", checked);
}

static void busy_create(int i915, const struct gem_memory_region *r, int timeout, unsigned int flags)
#define BUSY_HOG 0x1
{
	struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	igt_spin_t *spin[I915_EXEC_RING_MASK + 1];
	unsigned long count = 0;
	uint64_t ahnd;

	ctx = intel_ctx_create_all_physical(i915);
	ahnd = get_reloc_ahnd(i915, ctx->id);

	for_each_ctx_engine(i915, ctx, e) {
		spin[e->flags] =
			igt_spin_new(i915,
				     .ahnd = ahnd,
				     .ctx = ctx,
				     .engine = e->flags,
				     .flags = flags & BUSY_HOG ? IGT_SPIN_NO_PREEMPTION : 0);
	}

	igt_until_timeout(timeout) {
		for_each_ctx_engine(i915, ctx, e) {
			uint32_t handle;
			igt_spin_t *next;

			handle = gem_create_in_memory_region_list(i915, 4096, 0, &r->ci, 1);
			next = __igt_spin_new(i915,
					      .ahnd = ahnd,
					      .ctx = ctx,
					      .engine = e->flags,
					      .dependency = handle,
					      .flags = ((flags & BUSY_HOG ? IGT_SPIN_NO_PREEMPTION : 0) |
							IGT_SPIN_SOFTDEP));
			gem_close(i915, handle);

			igt_spin_free(i915, spin[e->flags]);
			spin[e->flags] = next;

			count++;
		}
	}

	intel_ctx_destroy(i915, ctx);
	put_ahnd(ahnd);

	igt_info("Created %ld objects while busy\n", count);

	gem_quiescent_gpu(i915);
}

static void size_update(int fd)
{
	int size_initial_nonaligned = 15;

	struct drm_i915_gem_create create = {
		.size = size_initial_nonaligned,
	};

	igt_assert_eq(create_ioctl(fd, &create), 0);
	igt_assert_neq(create.size, size_initial_nonaligned);
}

static void create_ext_placement_sanity_check(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_gem_create_ext_memory_regions setparam_region = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
	};
	struct drm_i915_gem_memory_class_instance *uregions;
	struct drm_i915_gem_memory_class_instance region_smem = {
		.memory_class = I915_MEMORY_CLASS_SYSTEM,
		.memory_instance = 0,
	};
	struct drm_i915_gem_memory_class_instance region_invalid = {
		.memory_class = -1,
		.memory_instance = -1,
	};
	uint32_t handle, create_ext_supported_flags;
	uint64_t size;
	int i;

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	/*
	 * extensions should be optional, giving us the normal gem_create
	 * behaviour.
	 */
	size = PAGE_SIZE;
	igt_assert_eq(__gem_create_ext(fd, &size, 0, &handle, 0), 0);
	gem_close(fd, handle);

	/* Try some uncreative invalid combinations */
	create_ext_supported_flags =
		I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS;
	igt_assert_neq(__gem_create_ext(fd, &size, ~create_ext_supported_flags,
					&handle, 0), 0);

	setparam_region.regions = to_user_pointer(&region_smem);
	setparam_region.num_regions = 0;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_smem);
	setparam_region.num_regions = regions->num_regions + 1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_smem);
	setparam_region.num_regions = -1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_invalid);
	setparam_region.num_regions = 1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_invalid);
	setparam_region.num_regions = 0;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	uregions = calloc(regions->num_regions + 1, sizeof(uint32_t));

	for (i = 0; i < regions->num_regions; i++)
		uregions[i] = regions->regions[i].region;

	setparam_region.regions = to_user_pointer(uregions);
	setparam_region.num_regions = regions->num_regions + 1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	if (regions->num_regions > 1)  {
		for (i = 0; i < regions->num_regions; i++) {
			struct drm_i915_gem_memory_class_instance dups[] = {
				regions->regions[i].region,
				regions->regions[i].region,
			};

			setparam_region.regions = to_user_pointer(dups);
			setparam_region.num_regions = 2;
			size = PAGE_SIZE;
			igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
							&setparam_region.base), 0);
		}
	}

	uregions[rand() % regions->num_regions].memory_class = -1;
	uregions[rand() % regions->num_regions].memory_instance = -1;
	setparam_region.regions = to_user_pointer(uregions);
	setparam_region.num_regions = regions->num_regions;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
					&setparam_region.base), 0);

	free(uregions);

	{
		struct drm_i915_gem_create_ext_memory_regions setparam_region_next;

		setparam_region.regions = to_user_pointer(&region_smem);
		setparam_region.num_regions = 1;

		setparam_region_next = setparam_region;
		setparam_region.base.next_extension =
				to_user_pointer(&setparam_region_next);

		size = PAGE_SIZE;
		igt_assert_neq(__gem_create_ext(fd, &size, 0, &handle,
						&setparam_region.base), 0);
		setparam_region.base.next_extension = 0;
	}

	free(regions);
}

static void create_ext_placement_all(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_gem_create_ext_memory_regions setparam_region = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
	};
	struct drm_i915_gem_memory_class_instance *uregions;
	uint64_t size;
	uint32_t handle;
	int i;

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	uregions = calloc(regions->num_regions, sizeof(*uregions));

	for (i = 0; i < regions->num_regions; i++)
		uregions[i] = regions->regions[i].region;

	setparam_region.regions = to_user_pointer(uregions);
	setparam_region.num_regions = regions->num_regions;

	size = PAGE_SIZE;
	igt_assert_eq(__gem_create_ext(fd, &size, 0, &handle,
				       &setparam_region.base), 0);
	gem_close(fd, handle);
	free(uregions);
	free(regions);
}

static void create_ext_placement_each(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_gem_create_ext_memory_regions setparam_region = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
	};
	int i;

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_gem_memory_class_instance region =
			regions->regions[i].region;
		uint64_t size;
		uint32_t handle;

		setparam_region.regions = to_user_pointer(&region);
		setparam_region.num_regions = 1;

		size = PAGE_SIZE;
		igt_assert_eq(__gem_create_ext(fd, &size, 0, &handle,
					       &setparam_region.base), 0);
		gem_close(fd, handle);
	}

	free(regions);
}

static bool supports_needs_cpu_access(int fd)
{
	struct drm_i915_gem_memory_class_instance regions[] = {
		{ I915_MEMORY_CLASS_DEVICE, },
		{ I915_MEMORY_CLASS_SYSTEM, },
	};
	struct drm_i915_gem_create_ext_memory_regions setparam_region = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
		.regions = to_user_pointer(&regions),
		.num_regions = ARRAY_SIZE(regions),
	};
	uint64_t size = PAGE_SIZE;
	uint32_t handle;
	int ret;

	ret = __gem_create_ext(fd, &size,
			       I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS,
			       &handle, &setparam_region.base);
	if (!ret) {
		gem_close(fd, handle);
		igt_assert(gem_has_lmem(fd)); /* Should be dgpu only */
	}

	return ret == 0;
}

static uint32_t batch_create_size(int fd, uint64_t size)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, size);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static int upload(int fd, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 exec[2] = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&exec),
		.buffer_count = 2,
	};

	/*
	 * To be reasonably sure that we are not being swindled, let's make
	 * sure to 'touch' the pages from the GPU first to ensure the object is
	 * for sure placed in one of requested regions.
	 */
	exec[0].handle = handle;
	exec[0].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	exec[1].handle = batch_create_size(fd, PAGE_SIZE);
	exec[1].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	return __gem_execbuf(fd, &execbuf);
}

static int alloc_lmem(int fd, uint32_t *handle,
		      struct drm_i915_gem_memory_class_instance ci,
		      uint64_t size, bool cpu_access, bool do_upload)
{
	struct drm_i915_gem_memory_class_instance regions[] = {
		ci, { I915_MEMORY_CLASS_SYSTEM, },
	};
	struct drm_i915_gem_create_ext_memory_regions setparam_region = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
		.regions = to_user_pointer(&regions),
	};
	uint32_t flags;

	igt_assert_eq(ci.memory_class, I915_MEMORY_CLASS_DEVICE);

	flags = 0;
	setparam_region.num_regions = 1;
	if (cpu_access) {
		flags = I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS,
		setparam_region.num_regions = 2;
	}

	*handle = gem_create_ext(fd, size, flags, &setparam_region.base);

	if (do_upload)
		return upload(fd, *handle);

	return 0;
}

static void create_ext_cpu_access_sanity_check(int fd)
{
	struct drm_i915_gem_create_ext_memory_regions setparam_region = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
	};
	struct drm_i915_query_memory_regions *regions;
	uint64_t size = PAGE_SIZE;
	uint32_t handle;
	int i;

	/*
	 * The ABI is that FLAG_NEEDS_CPU_ACCESS can only be applied to LMEM +
	 * SMEM objects. Make sure the kernel follows that, while also checking
	 * the basic CPU faulting behavour.
	 */

	/* Implicit placement; should fail */
	igt_assert_eq(__gem_create_ext(fd, &size,
				       I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS,
				       &handle, NULL), -EINVAL);

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_gem_memory_class_instance ci_regions[2] = {
			regions->regions[i].region,
			{ I915_MEMORY_CLASS_SYSTEM, },
		};
		uint32_t *ptr;

		setparam_region.regions = to_user_pointer(ci_regions);
		setparam_region.num_regions = 1;

		/* Single explicit placement; should fail */
		igt_assert_eq(__gem_create_ext(fd, &size,
					       I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS,
					       &handle, &setparam_region.base),
			      -EINVAL);

		if (ci_regions[0].memory_class == I915_MEMORY_CLASS_SYSTEM)
			continue;

		/*
		 * Now combine with system memory; should pass. We should also
		 * be able to fault it.
		 */
		setparam_region.num_regions = 2;
		igt_assert_eq(__gem_create_ext(fd, &size,
					       I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS,
					       &handle, &setparam_region.base),
			      0);
		upload(fd, handle);
		ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
					     PROT_READ | PROT_WRITE);
		ptr[0] = 0xdeadbeaf;
		gem_close(fd, handle);

		/*
		 * It should also work just fine without the flag, where in the
		 * worst case we need to migrate it when faulting it.
		 */
		igt_assert_eq(__gem_create_ext(fd, &size,
					       0,
					       &handle, &setparam_region.base),
			      0);
		upload(fd, handle);
		ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
					     PROT_READ | PROT_WRITE);
		ptr[0] = 0xdeadbeaf;
		gem_close(fd, handle);
	}

	free(regions);
}

static jmp_buf jmp;

__noreturn static void sigtrap(int sig)
{
	siglongjmp(jmp, sig);
}

static void trap_sigbus(uint32_t *ptr)
{
	sighandler_t old_sigbus;

	old_sigbus = signal(SIGBUS, sigtrap);
	switch (sigsetjmp(jmp, SIGBUS)) {
	case SIGBUS:
		break;
	case 0:
		*ptr = 0xdeadbeaf;
	default:
		igt_assert(!"reached");
		break;
	}
	signal(SIGBUS, old_sigbus);
}

static void create_ext_cpu_access_big(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	int i;

	/*
	 * Sanity check that we can still CPU map an overly large object, even
	 * if it happens to be larger the CPU visible portion of LMEM. Also
	 * check that an overly large allocation, which can't be spilled into
	 * system memory does indeed fail.
	 */

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_memory_region_info qmr = regions->regions[i];
		struct drm_i915_gem_memory_class_instance ci = qmr.region;
		uint64_t size, visible_size, lmem_size;
		uint32_t handle;
		uint32_t *ptr;

		if (ci.memory_class == I915_MEMORY_CLASS_SYSTEM)
			continue;

		lmem_size = qmr.probed_size;
		visible_size = qmr.probed_cpu_visible_size;
		igt_assert_neq_u64(visible_size, 0);

		if (visible_size <= (0.70 * lmem_size)) {
			/*
			 * Too big. We should still be able to allocate it just
			 * fine, but faulting should result in tears.
			 */
			size = visible_size;
			igt_assert_eq(alloc_lmem(fd, &handle, ci, size, false, true), 0);
			ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
						     PROT_READ | PROT_WRITE);
			trap_sigbus(ptr);
			gem_close(fd, handle);

			/*
			 * Too big again, but this time we can spill to system
			 * memory when faulting the object.
			 */
			size = visible_size;
			igt_assert_eq(alloc_lmem(fd, &handle, ci, size, true, true), 0);
			ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
						     PROT_READ | PROT_WRITE);
			ptr[0] = 0xdeadbeaf;
			gem_close(fd, handle);

			/*
			 * Let's also move the upload to after faulting the
			 * pages. The current behaviour is that the pages are
			 * only allocated in device memory when initially
			 * touched by the GPU. With this in mind we should also
			 * make sure that the pages are indeed migrated, as
			 * expected.
			 */
			size = visible_size;
			igt_assert_eq(alloc_lmem(fd, &handle, ci, size, false, false), 0);
			ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
						     PROT_READ | PROT_WRITE);
			ptr[0] = 0xdeadbeaf; /* temp system memory */
			igt_assert_eq(upload(fd, handle), 0);
			trap_sigbus(ptr); /* non-mappable device memory */
			gem_close(fd, handle);
		}

		/*
		 * Should fit. We likely need to migrate to the mappable portion
		 * on fault though, if this device has a small BAR, given how
		 * large the initial allocation is.
		 */
		size = visible_size >> 1;
		igt_assert_eq(alloc_lmem(fd, &handle, ci, size, false, true), 0);
		ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
					     PROT_READ | PROT_WRITE);
		ptr[0] = 0xdeadbeaf;
		gem_close(fd, handle);

		/*
		 * And then with the CPU_ACCESS flag enabled; should also be no
		 * surprises here.
		 */
		igt_assert_eq(alloc_lmem(fd, &handle, ci, size, true, true), 0);
		ptr = gem_mmap_offset__fixed(fd, handle, 0, size,
					     PROT_READ | PROT_WRITE);
		ptr[0] = 0xdeadbeaf;
		gem_close(fd, handle);
	}

	free(regions);
}

igt_main
{
	igt_fd_t(fd);

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
	}

	igt_describe("Try to create a gem object of invalid size 0"
		     " and check if ioctl returns error.");
	igt_subtest("create-invalid-size")
		invalid_size_test(fd);

	igt_describe("Exercise creation of buffer object with impossible"
		     " size and check for the expected error.");
	igt_subtest("create-massive")
		massive_test(fd);

	igt_describe("Try to create an object with non-aligned size, check"
		     " we got one with size aligned up to page size and test"
		     " we can write into the padded extra memory.");
	igt_subtest("create-valid-nonaligned")
		valid_nonaligned_size(fd);

	igt_describe("Try to create a gem object with size 15"
		     " and check actual created size.");
	igt_subtest("create-size-update")
		size_update(fd);

	igt_describe("Verify that all new objects are clear.");
	igt_subtest_with_dynamic("create-clear") {
		for_each_memory_region(r, fd) {
			igt_dynamic_f("%s", r->name)
				always_clear(fd, r, 30);
		}
	}

	igt_describe("Create buffer objects while GPU is busy.");
	igt_subtest_group {
		igt_fixture
			igt_fork_hang_detector(fd);

		igt_subtest_with_dynamic("busy-create") {
			for_each_memory_region(r, fd) {
				igt_dynamic_f("%s", r->name)
					busy_create(fd, r, 30, 0);
			}
		}

		igt_subtest_with_dynamic("hog-create") {
			for_each_memory_region(r, fd) {
				igt_dynamic_f("%s", r->name)
					busy_create(fd, r, 30, BUSY_HOG);
			}
		}

		igt_fixture
			igt_stop_hang_detector();
	}

	igt_describe("Exercise create_ext placements extension.");
	igt_subtest("create-ext-placement-sanity-check")
		create_ext_placement_sanity_check(fd);

	igt_describe("Create one object with memory pieces in each"
		     " memory region using create_ext.");
	igt_subtest("create-ext-placement-each")
		create_ext_placement_each(fd);

	igt_describe("Create objects in every  memory region using"
		     " create_ext.");
	igt_subtest("create-ext-placement-all")
		create_ext_placement_all(fd);

	igt_describe("Verify the basic functionally and expected ABI contract around I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS");
	igt_subtest("create-ext-cpu-access-sanity-check") {
		igt_require(supports_needs_cpu_access(fd));
		create_ext_cpu_access_sanity_check(fd);
	}

	igt_describe("Verify the extreme cases with very large objects and I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS");
	igt_subtest("create-ext-cpu-access-big") {
		igt_require(supports_needs_cpu_access(fd));
		create_ext_cpu_access_big(fd);
	}
}
