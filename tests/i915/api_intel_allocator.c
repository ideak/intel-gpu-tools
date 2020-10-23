// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <stdatomic.h>
#include "i915/gem.h"
#include "igt.h"
#include "igt_aux.h"
#include "intel_allocator.h"

#define OBJ_SIZE 1024

struct test_obj {
	uint32_t handle;
	uint64_t offset;
	uint64_t size;
};

static _Atomic(uint32_t) next_handle;

static inline uint32_t gem_handle_gen(void)
{
	return atomic_fetch_add(&next_handle, 1);
}

static void alloc_simple(int fd)
{
	uint64_t ahnd;
	uint64_t offset0, offset1, size = 0x1000, align = 0x1000, start, end;
	bool is_allocated, freed;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);

	offset0 = intel_allocator_alloc(ahnd, 1, size, align);
	offset1 = intel_allocator_alloc(ahnd, 1, size, align);
	igt_assert(offset0 == offset1);

	is_allocated = intel_allocator_is_allocated(ahnd, 1, size, offset0);
	igt_assert(is_allocated);

	freed = intel_allocator_free(ahnd, 1);
	igt_assert(freed);

	is_allocated = intel_allocator_is_allocated(ahnd, 1, size, offset0);
	igt_assert(!is_allocated);

	freed = intel_allocator_free(ahnd, 1);
	igt_assert(!freed);

	intel_allocator_get_address_range(ahnd, &start, &end);
	offset0 = intel_allocator_alloc(ahnd, 1, end - start, 0);
	offset1 = __intel_allocator_alloc(ahnd, 2, 4096, 0);
	igt_assert(offset1 == ALLOC_INVALID_ADDRESS);
	intel_allocator_free(ahnd, 1);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void reserve_simple(int fd)
{
	uint64_t ahnd, start, size = 0x1000;
	bool reserved, unreserved;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	intel_allocator_get_address_range(ahnd, &start, NULL);

	reserved = intel_allocator_reserve(ahnd, 0, size, start);
	igt_assert(reserved);

	reserved = intel_allocator_is_reserved(ahnd, size, start);
	igt_assert(reserved);

	reserved = intel_allocator_reserve(ahnd, 0, size, start);
	igt_assert(!reserved);

	unreserved = intel_allocator_unreserve(ahnd, 0, size, start);
	igt_assert(unreserved);

	reserved = intel_allocator_is_reserved(ahnd, size, start);
	igt_assert(!reserved);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void reserve(int fd, uint8_t type)
{
	struct test_obj obj;
	uint64_t ahnd, offset = 0x40000, size = 0x1000;

	ahnd = intel_allocator_open(fd, 0, type);

	igt_assert_eq(intel_allocator_reserve(ahnd, 0, size, offset), true);
	/* try overlapping won't succeed */
	igt_assert_eq(intel_allocator_reserve(ahnd, 0, size, offset + size/2), false);

	obj.handle = gem_handle_gen();
	obj.size = OBJ_SIZE;
	obj.offset = intel_allocator_alloc(ahnd, obj.handle, obj.size, 0);

	igt_assert_eq(intel_allocator_reserve(ahnd, 0, obj.size, obj.offset), false);
	intel_allocator_free(ahnd, obj.handle);
	igt_assert_eq(intel_allocator_reserve(ahnd, 0, obj.size, obj.offset), true);

	igt_assert_eq(intel_allocator_unreserve(ahnd, 0, obj.size, obj.offset), true);
	igt_assert_eq(intel_allocator_unreserve(ahnd, 0, size, offset), true);
	igt_assert_eq(intel_allocator_reserve(ahnd, 0, size, offset + size/2), true);
	igt_assert_eq(intel_allocator_unreserve(ahnd, 0, size, offset + size/2), true);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static bool overlaps(struct test_obj *buf1, struct test_obj *buf2)
{
	uint64_t begin1 = buf1->offset;
	uint64_t end1 = buf1->offset + buf1->size;
	uint64_t begin2 = buf2->offset;
	uint64_t end2 = buf2->offset + buf2->size;

	return (end1 > begin2 && end2 > end1) || (end2 > begin1 && end1 > end2);
}

static void basic_alloc(int fd, int cnt, uint8_t type)
{
	struct test_obj *obj;
	uint64_t ahnd;
	int i, j;

	ahnd = intel_allocator_open(fd, 0, type);
	obj = malloc(sizeof(struct test_obj) * cnt);

	for (i = 0; i < cnt; i++) {
		igt_progress("allocating objects: ", i, cnt);
		obj[i].handle = gem_handle_gen();
		obj[i].size = OBJ_SIZE;
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
						      obj[i].size, 4096);
		igt_assert_eq(obj[i].offset % 4096, 0);
	}

	for (i = 0; i < cnt; i++) {
		igt_progress("check overlapping: ", i, cnt);

		if (type == INTEL_ALLOCATOR_RANDOM)
			continue;

		for (j = 0; j < cnt; j++) {
			if (j == i)
				continue;
				igt_assert(!overlaps(&obj[i], &obj[j]));
		}
	}

	for (i = 0; i < cnt; i++) {
		igt_progress("freeing objects: ", i, cnt);
		intel_allocator_free(ahnd, obj[i].handle);
	}

	free(obj);
	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void reuse(int fd, uint8_t type)
{
	struct test_obj obj[128], tmp;
	uint64_t ahnd, prev_offset;
	int i;

	ahnd = intel_allocator_open(fd, 0, type);

	for (i = 0; i < 128; i++) {
		obj[i].handle = gem_handle_gen();
		obj[i].size = OBJ_SIZE;
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
						      obj[i].size, 0x40);
	}

	/* check simple reuse */
	for (i = 0; i < 128; i++) {
		prev_offset = obj[i].offset;
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
						      obj[i].size, 0);
		igt_assert(prev_offset == obj[i].offset);
	}
	i--;

	/* free previously allocated bo */
	intel_allocator_free(ahnd, obj[i].handle);
	/* alloc different buffer to fill freed hole */
	tmp.handle = gem_handle_gen();
	tmp.offset = intel_allocator_alloc(ahnd, tmp.handle, OBJ_SIZE, 0);
	igt_assert(prev_offset == tmp.offset);

	obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
					      obj[i].size, 0);
	igt_assert(prev_offset != obj[i].offset);
	intel_allocator_free(ahnd, tmp.handle);

	for (i = 0; i < 128; i++)
		intel_allocator_free(ahnd, obj[i].handle);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

struct ial_thread_args {
	uint64_t ahnd;
	pthread_t thread;
	uint32_t *handles;
	uint64_t *offsets;
	uint32_t count;
	int threads;
	int idx;
};

static void *alloc_bo_in_thread(void *arg)
{
	struct ial_thread_args *a = arg;
	int i;

	for (i = a->idx; i < a->count; i += a->threads) {
		a->handles[i] = gem_handle_gen();
		a->offsets[i] = intel_allocator_alloc(a->ahnd, a->handles[i], OBJ_SIZE,
						      1UL << ((random() % 20) + 1));
	}

	return NULL;
}

static void *free_bo_in_thread(void *arg)
{
	struct ial_thread_args *a = arg;
	int i;

	for (i = (a->idx + 1) % a->threads; i < a->count; i += a->threads)
		intel_allocator_free(a->ahnd, a->handles[i]);

	return NULL;
}

#define THREADS 6

static void parallel_one(int fd, uint8_t type)
{
	struct ial_thread_args a[THREADS];
	uint32_t *handles;
	uint64_t ahnd, *offsets;
	int count, i;

	srandom(0xdeadbeef);
	ahnd = intel_allocator_open(fd, 0, type);
	count = 1UL << 12;

	handles = malloc(sizeof(uint32_t) * count);
	offsets = calloc(1, sizeof(uint64_t) * count);

	for (i = 0; i < THREADS; i++) {
		a[i].ahnd = ahnd;
		a[i].handles = handles;
		a[i].offsets = offsets;
		a[i].count = count;
		a[i].threads = THREADS;
		a[i].idx = i;
		pthread_create(&a[i].thread, NULL, alloc_bo_in_thread, &a[i]);
	}

	for (i = 0; i < THREADS; i++)
		pthread_join(a[i].thread, NULL);

	/* Check if all objects are allocated */
	for (i = 0; i < count; i++) {
		/* Reloc + random allocators don't have state. */
		if (type == INTEL_ALLOCATOR_RELOC || type == INTEL_ALLOCATOR_RANDOM)
			break;

		igt_assert_eq(offsets[i],
			      intel_allocator_alloc(a->ahnd, handles[i], OBJ_SIZE, 0));
	}

	for (i = 0; i < THREADS; i++)
		pthread_create(&a[i].thread, NULL, free_bo_in_thread, &a[i]);

	for (i = 0; i < THREADS; i++)
		pthread_join(a[i].thread, NULL);

	free(handles);
	free(offsets);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

#define SIMPLE_GROUP_ALLOCS 8
static void __simple_allocs(int fd)
{
	uint32_t handles[SIMPLE_GROUP_ALLOCS];
	uint64_t ahnd;
	uint32_t ctx;
	int i;

	ctx = rand() % 2;
	ahnd = intel_allocator_open(fd, ctx, INTEL_ALLOCATOR_SIMPLE);

	for (i = 0; i < SIMPLE_GROUP_ALLOCS; i++) {
		uint32_t size;

		size = (rand() % 4 + 1) * 0x1000;
		handles[i] = gem_create(fd, size);
		intel_allocator_alloc(ahnd, handles[i], size, 0x1000);
	}

	for (i = 0; i < SIMPLE_GROUP_ALLOCS; i++) {
		igt_assert_f(intel_allocator_free(ahnd, handles[i]) == 1,
			     "Error freeing handle: %u\n", handles[i]);
		gem_close(fd, handles[i]);
	}

	intel_allocator_close(ahnd);
}

static void fork_simple_once(int fd)
{
	intel_allocator_multiprocess_start();

	igt_fork(child, 1)
		__simple_allocs(fd);

	igt_waitchildren();

	intel_allocator_multiprocess_stop();
}

#define SIMPLE_TIMEOUT 5
static void *__fork_simple_thread(void *data)
{
	int fd = (int) (long) data;

	igt_until_timeout(SIMPLE_TIMEOUT) {
		__simple_allocs(fd);
	}

	return NULL;
}

static void fork_simple_stress(int fd, bool two_level_inception)
{
	pthread_t thread0, thread1;
	uint64_t ahnd0, ahnd1;
	bool are_empty;

	__intel_allocator_multiprocess_prepare();

	igt_fork(child, 8) {
		if (two_level_inception) {
			pthread_create(&thread0, NULL, __fork_simple_thread,
				       (void *) (long) fd);
			pthread_create(&thread1, NULL, __fork_simple_thread,
				       (void *) (long) fd);
		}

		igt_until_timeout(SIMPLE_TIMEOUT) {
			__simple_allocs(fd);
		}

		if (two_level_inception) {
			pthread_join(thread0, NULL);
			pthread_join(thread1, NULL);
		}
	}

	pthread_create(&thread0, NULL, __fork_simple_thread, (void *) (long) fd);
	pthread_create(&thread1, NULL, __fork_simple_thread, (void *) (long) fd);

	ahnd0 = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	ahnd1 = intel_allocator_open(fd, 1, INTEL_ALLOCATOR_SIMPLE);

	__intel_allocator_multiprocess_start();

	igt_waitchildren();

	pthread_join(thread0, NULL);
	pthread_join(thread1, NULL);

	are_empty = intel_allocator_close(ahnd0);
	are_empty &= intel_allocator_close(ahnd1);

	intel_allocator_multiprocess_stop();

	igt_assert_f(are_empty, "Allocators were not emptied\n");
}

static void __reopen_allocs(int fd1, int fd2, bool check)
{
	uint64_t ahnd0, ahnd1, ahnd2;

	ahnd0 = intel_allocator_open(fd1, 0, INTEL_ALLOCATOR_SIMPLE);
	ahnd1 = intel_allocator_open(fd2, 0, INTEL_ALLOCATOR_SIMPLE);
	ahnd2 = intel_allocator_open(fd2, 0, INTEL_ALLOCATOR_SIMPLE);
	igt_assert(ahnd0 != ahnd1);
	igt_assert(ahnd1 != ahnd2);

	/* in fork mode we can have more references, so skip check */
	if (!check) {
		intel_allocator_close(ahnd0);
		intel_allocator_close(ahnd1);
		intel_allocator_close(ahnd2);
	} else {
		igt_assert_eq(intel_allocator_close(ahnd0), true);
		igt_assert_eq(intel_allocator_close(ahnd1), false);
		igt_assert_eq(intel_allocator_close(ahnd2), true);
	}
}

static void reopen(int fd)
{
	int fd2;

	igt_require_gem(fd);

	fd2 = gem_reopen_driver(fd);

	__reopen_allocs(fd, fd2, true);

	close(fd2);
}

#define REOPEN_TIMEOUT 3
static void reopen_fork(int fd)
{
	int fd2;

	igt_require_gem(fd);

	intel_allocator_multiprocess_start();

	fd2 = gem_reopen_driver(fd);

	igt_fork(child, 2) {
		igt_until_timeout(REOPEN_TIMEOUT)
			__reopen_allocs(fd, fd2, false);
	}
	igt_until_timeout(REOPEN_TIMEOUT)
		__reopen_allocs(fd, fd2, false);

	igt_waitchildren();

	/* Check references at the end */
	__reopen_allocs(fd, fd2, true);

	close(fd2);

	intel_allocator_multiprocess_stop();
}

static void open_vm(int fd)
{
	uint64_t ahnd[4], offset[4], size = 0x1000;
	int i, n = ARRAY_SIZE(ahnd);

	ahnd[0] = intel_allocator_open_vm(fd, 1, INTEL_ALLOCATOR_SIMPLE);
	ahnd[1] = intel_allocator_open_vm(fd, 1, INTEL_ALLOCATOR_SIMPLE);
	ahnd[2] = intel_allocator_open_vm_as(ahnd[1], 2);
	ahnd[3] = intel_allocator_open(fd, 3, INTEL_ALLOCATOR_SIMPLE);

	offset[0] = intel_allocator_alloc(ahnd[0], 1, size, 0);
	offset[1] = intel_allocator_alloc(ahnd[1], 2, size, 0);
	igt_assert(offset[0] != offset[1]);

	offset[2] = intel_allocator_alloc(ahnd[2], 3, size, 0);
	igt_assert(offset[0] != offset[2] && offset[1] != offset[2]);

	offset[3] = intel_allocator_alloc(ahnd[3], 1, size, 0);
	igt_assert(offset[0] == offset[3]);

	/*
	 * As ahnd[0-2] lead to same allocator check can we free all handles
	 * using selected ahnd.
	 */
	intel_allocator_free(ahnd[0], 1);
	intel_allocator_free(ahnd[0], 2);
	intel_allocator_free(ahnd[0], 3);
	intel_allocator_free(ahnd[3], 1);

	for (i = 0; i < n - 1; i++)
		igt_assert_eq(intel_allocator_close(ahnd[i]), (i == n - 2));
	igt_assert_eq(intel_allocator_close(ahnd[n-1]), true);
}

struct allocators {
	const char *name;
	uint8_t type;
} als[] = {
	{"simple", INTEL_ALLOCATOR_SIMPLE},
	{"reloc",  INTEL_ALLOCATOR_RELOC},
	{"random", INTEL_ALLOCATOR_RANDOM},
	{NULL, 0},
};

igt_main
{
	int fd;
	struct allocators *a;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		atomic_init(&next_handle, 1);
		srandom(0xdeadbeef);
	}

	igt_subtest_f("alloc-simple")
		alloc_simple(fd);

	igt_subtest_f("reserve-simple")
		reserve_simple(fd);

	igt_subtest_f("reuse")
		reuse(fd, INTEL_ALLOCATOR_SIMPLE);

	igt_subtest_f("reserve")
		reserve(fd, INTEL_ALLOCATOR_SIMPLE);

	for (a = als; a->name; a++) {
		igt_subtest_with_dynamic_f("%s-allocator", a->name) {
			igt_dynamic("basic")
				basic_alloc(fd, 1UL << 8, a->type);

			igt_dynamic("parallel-one")
				parallel_one(fd, a->type);

			igt_dynamic("print")
				basic_alloc(fd, 1UL << 2, a->type);

			if (a->type == INTEL_ALLOCATOR_SIMPLE) {
				igt_dynamic("reuse")
					reuse(fd, a->type);

				igt_dynamic("reserve")
					reserve(fd, a->type);
			}
		}
	}

	igt_subtest_f("fork-simple-once")
		fork_simple_once(fd);

	igt_subtest_f("fork-simple-stress")
		fork_simple_stress(fd, false);

	igt_subtest_f("fork-simple-stress-signal") {
		igt_fork_signal_helper();
		fork_simple_stress(fd, false);
		igt_stop_signal_helper();
	}

	igt_subtest_f("two-level-inception")
		fork_simple_stress(fd, true);

	igt_subtest_f("two-level-inception-interruptible") {
		igt_fork_signal_helper();
		fork_simple_stress(fd, true);
		igt_stop_signal_helper();
	}

	igt_subtest_f("reopen")
		reopen(fd);

	igt_subtest_f("reopen-fork")
		reopen_fork(fd);

	igt_subtest_f("open-vm")
		open_vm(fd);

	igt_fixture
		close(fd);
}
