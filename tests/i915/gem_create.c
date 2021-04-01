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

#include "drm.h"
#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "intel_batchbuffer.h"
#include "intel_io.h"
#include "intel_chipset.h"
#include "igt_aux.h"
#include "igt_dummyload.h"
#include "igt_x86.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"
#include "i915_drm.h"

IGT_TEST_DESCRIPTION("This is a test for the gem_create ioctl,"
		     " where the goal is to simply ensure that basics work"
		     " and invalid input combinations are rejected.");

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
	int timeout;
	int i915;
};

static void *thread_clear(void *data)
{
	struct thread_clear *arg = data;
	unsigned long checked = 0, total = 0;
	enum { PRW, GTT, WC, WB, __LAST__ } mode = PRW;
	int i915 = arg->i915;

	igt_until_timeout(arg->timeout) {
		struct drm_i915_gem_create create = {};
		uint64_t npages;
		void *ptr;

		npages = random();
		npages <<= 32;
		npages |= random();
		npages = get_npages(&arg->max, npages);
		create.size = npages << 12;

		create_ioctl(i915, &create);
		switch (mode) {
		case __LAST__:
		case PRW:
			ptr = NULL;
			break;
		case WB:
			ptr = __gem_mmap__cpu(i915, create.handle,
					      0, create.size, PROT_READ);
			break;
		case WC:
			ptr = __gem_mmap__wc(i915, create.handle,
					     0, create.size, PROT_READ);
			break;
		case GTT:
			ptr = __gem_mmap__gtt(i915, create.handle,
					      create.size, PROT_READ);
			break;
		}
		/* No set-domains as we are being as naughty as possible */

		for (uint64_t page = 0; page < npages; page += 1 + random() % (npages - page)) {
			uint64_t x[8] = {
				page * 4096 +
				sizeof(x) * ((page % (4096 - sizeof(x)) / sizeof(x)))
			};

			if (!ptr)
				gem_read(i915, create.handle, x[0], x, sizeof(x));
			else if (page & 1)
				igt_memcpy_from_wc(x, ptr + x[0], sizeof(x));
			else
				memcpy(x, ptr + x[0], sizeof(x));

			for (int i = 0; i < ARRAY_SIZE(x); i++)
				igt_assert_eq_u64(x[i], 0);

			checked++;
		}
		if (ptr)
			munmap(ptr, create.size);
		gem_close(i915, create.handle);

		total += npages;
		atomic_fetch_add(&arg->max, npages);

		if (++mode == __LAST__)
			mode = PRW;
	}

	igt_info("Checked %'lu / %'lu pages\n", checked, total);
	return (void *)(uintptr_t)checked;
}

static void always_clear(int i915, int timeout)
{
	struct thread_clear arg = {
		.i915 = i915,
		.timeout = timeout,
		.max = intel_get_avail_ram_mb() << (20 - 12), /* in pages */
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

static void busy_create(int i915, int timeout)
{
	struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	igt_spin_t *spin[I915_EXEC_RING_MASK + 1];
	unsigned long count = 0;

	ctx = intel_ctx_create_all_physical(i915);

	igt_fork_hang_detector(i915);
	for_each_ctx_engine(i915, ctx, e)
		spin[e->flags] = igt_spin_new(i915, .ctx = ctx,
					      .engine = e->flags);

	igt_until_timeout(timeout) {
		for_each_ctx_engine(i915, ctx, e) {
			uint32_t handle;
			igt_spin_t *next;

			handle = gem_create(i915, 4096);
			next = igt_spin_new(i915, .ctx = ctx,
					    .engine = e->flags,
					    .dependency = handle,
					    .flags = IGT_SPIN_SOFTDEP);
			gem_close(i915, handle);

			igt_spin_free(i915, spin[e->flags]);
			spin[e->flags] = next;

			count++;
		}
	}

	intel_ctx_destroy(i915, ctx);

	igt_info("Created %ld objects while busy\n", count);

	gem_quiescent_gpu(i915);
	igt_stop_hang_detector();
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
	uint64_t size;
	uint32_t handle;
	int i;

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	/*
	 * extensions should be optional, giving us the normal gem_create
	 * behaviour.
	 */
	size = PAGE_SIZE;
	igt_assert_eq(__gem_create_ext(fd, &size, &handle, 0), 0);
	gem_close(fd, handle);

	/* Try some uncreative invalid combinations */
	setparam_region.regions = to_user_pointer(&region_smem);
	setparam_region.num_regions = 0;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_smem);
	setparam_region.num_regions = regions->num_regions + 1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_smem);
	setparam_region.num_regions = -1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_invalid);
	setparam_region.num_regions = 1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
					&setparam_region.base), 0);

	setparam_region.regions = to_user_pointer(&region_invalid);
	setparam_region.num_regions = 0;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
					&setparam_region.base), 0);

	uregions = calloc(regions->num_regions + 1, sizeof(uint32_t));

	for (i = 0; i < regions->num_regions; i++)
		uregions[i] = regions->regions[i].region;

	setparam_region.regions = to_user_pointer(uregions);
	setparam_region.num_regions = regions->num_regions + 1;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
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
			igt_assert_neq(__gem_create_ext(fd, &size, &handle,
							&setparam_region.base), 0);
		}
	}

	uregions[rand() % regions->num_regions].memory_class = -1;
	uregions[rand() % regions->num_regions].memory_instance = -1;
	setparam_region.regions = to_user_pointer(uregions);
	setparam_region.num_regions = regions->num_regions;
	size = PAGE_SIZE;
	igt_assert_neq(__gem_create_ext(fd, &size, &handle,
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
		igt_assert_neq(__gem_create_ext(fd, &size, &handle,
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
	igt_assert_eq(__gem_create_ext(fd, &size, &handle,
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
		igt_assert_eq(__gem_create_ext(fd, &size, &handle,
					       &setparam_region.base), 0);
		gem_close(fd, handle);
	}

	free(regions);
}

igt_main
{
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
	}

	igt_subtest("create-invalid-size")
		invalid_size_test(fd);

	igt_subtest("create-massive")
		massive_test(fd);

	igt_subtest("create-valid-nonaligned")
		valid_nonaligned_size(fd);

	igt_subtest("create-size-update")
		size_update(fd);

	igt_subtest("create-clear")
		always_clear(fd, 30);

	igt_subtest("busy-create")
		busy_create(fd, 30);

	igt_subtest("create-ext-placement-sanity-check")
		create_ext_placement_sanity_check(fd);

	igt_subtest("create-ext-placement-each")
		create_ext_placement_each(fd);

	igt_subtest("create-ext-placement-all")
		create_ext_placement_all(fd);

}
