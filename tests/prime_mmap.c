/*
 * Copyright © 2014 Intel Corporation
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
 *    Rob Bradford <rob at linux.intel.com>
 *    Tiago Vignatti <tiago.vignatti at intel.com>
 *
 */

/*
 * Testcase: Check whether mmap()ing dma-buf works
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include "drm.h"
#include "drmtest.h"
#include "igt.h"
#include "igt_collection.h"
#include "i915_drm.h"
#include "i915/gem_create.h"
#include "i915/gem_mman.h"
#include "igt_debugfs.h"
#include "ioctl_wrappers.h"
#include "i915/intel_memory_region.h"

#define BO_SIZE (16*1024)

static int fd;

char pattern[] = {0xff, 0x00, 0x00, 0x00,
	0x00, 0xff, 0x00, 0x00,
	0x00, 0x00, 0xff, 0x00,
	0x00, 0x00, 0x00, 0xff};

static void
fill_bo(uint32_t handle, size_t size)
{
	off_t i;
	for (i = 0; i < size; i+=sizeof(pattern))
	{
		gem_write(fd, handle, i, pattern, sizeof(pattern));
	}
}

static void
fill_bo_cpu(char *ptr, size_t size)
{
	off_t i;
	for (i = 0; i < size; i += sizeof(pattern))
	{
		memcpy(ptr + i, pattern, sizeof(pattern));
	}
}

static void
test_correct(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr1, *ptr2;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);

	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);

	/* Check correctness vs GEM_MMAP */
	ptr1 = gem_mmap__device_coherent(fd, handle, 0, size, PROT_READ);
	ptr2 = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr1 != MAP_FAILED);
	igt_assert(ptr2 != MAP_FAILED);
	igt_assert(memcmp(ptr1, ptr2, size) == 0);

	/* Check pattern correctness */
	igt_assert(memcmp(ptr2, pattern, sizeof(pattern)) == 0);

	munmap(ptr1, size);
	munmap(ptr2, size);
	close(dma_buf_fd);
	gem_close(fd, handle);
}

static void
test_map_unmap(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);

	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);

	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);

	/* Unmap and remap */
	munmap(ptr, size);
	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);

	munmap(ptr, size);
	close(dma_buf_fd);
	gem_close(fd, handle);
}

/* prime and then unprime and then prime again the same handle */
static void
test_reprime(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);

	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);

	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);

	close (dma_buf_fd);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);
	munmap(ptr, size);

	dma_buf_fd = prime_handle_to_fd(fd, handle);
	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);

	munmap(ptr, size);
	close(dma_buf_fd);
	gem_close(fd, handle);
}

/* map from another process */
static void
test_forked(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);

	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);

	igt_fork(childno, 1) {
		ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
		igt_assert(ptr != MAP_FAILED);
		igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);
		munmap(ptr, size);
		close(dma_buf_fd);
	}
	close(dma_buf_fd);
	igt_waitchildren();
	gem_close(fd, handle);
}

/* test simple CPU write */
static void
test_correct_cpu_write(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);

	dma_buf_fd = prime_handle_to_fd_for_mmap(fd, handle);

	/* Skip if DRM_RDWR is not supported */
	igt_skip_on(errno == EINVAL);

	/* Check correctness of map using write protection (PROT_WRITE) */
	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);

	/* Fill bo using CPU */
	fill_bo_cpu(ptr, BO_SIZE);

	/* Check pattern correctness */
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);

	munmap(ptr, size);
	close(dma_buf_fd);
	gem_close(fd, handle);
}

/* map from another process and then write using CPU */
static void
test_forked_cpu_write(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);

	dma_buf_fd = prime_handle_to_fd_for_mmap(fd, handle);

	/* Skip if DRM_RDWR is not supported */
	igt_skip_on(errno == EINVAL);

	igt_fork(childno, 1) {
		ptr = mmap(NULL, size, PROT_READ | PROT_WRITE , MAP_SHARED, dma_buf_fd, 0);
		igt_assert(ptr != MAP_FAILED);
		fill_bo_cpu(ptr, BO_SIZE);

		igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);
		munmap(ptr, size);
		close(dma_buf_fd);
	}
	close(dma_buf_fd);
	igt_waitchildren();
	gem_close(fd, handle);
}

static void
test_refcounting(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);

	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);
	/* Close gem object before mapping */
	gem_close(fd, handle);

	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);
	munmap(ptr, size);
	close (dma_buf_fd);
}

/* dup before mmap */
static void
test_dup(uint32_t region, uint64_t size)
{
	int dma_buf_fd;
	char *ptr;
	uint32_t handle;

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);

	dma_buf_fd = dup(prime_handle_to_fd(fd, handle));
	igt_assert(errno == 0);

	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);
	munmap(ptr, size);
	gem_close(fd, handle);
	close (dma_buf_fd);
}

/* Used for error case testing to avoid wrapper */
static int prime_handle_to_fd_no_assert(uint32_t handle, int flags, int *fd_out)
{
	struct drm_prime_handle args;
	int ret;

	args.handle = handle;
	args.flags = flags;
	args.fd = -1;

	ret = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
	if (ret)
		ret = errno;
	*fd_out = args.fd;

	return ret;
}

static bool has_userptr(void)
{
	uint32_t handle = 0;
	void *ptr;

	igt_assert(posix_memalign(&ptr, 4096, 4096) == 0);
	if ( __gem_userptr(fd, ptr, 4096, 0, 0, &handle) == 0)
		gem_close(fd, handle);
	free(ptr);

	return handle;
}

/* test for mmap(dma_buf_export(userptr)) */
static void
test_userptr(uint32_t region, uint64_t size)
{
	int ret, dma_buf_fd;
	void *ptr;
	uint32_t handle;

	/* create userptr bo */
	ret = posix_memalign(&ptr, 4096, size);
	igt_assert_eq(ret, 0);

	/* we are not allowed to export unsynchronized userptr. Just create a normal
	 * one */
	gem_userptr(fd, (uint32_t *)ptr, size, 0, 0, &handle);

	/* export userptr */
	ret = prime_handle_to_fd_no_assert(handle, DRM_CLOEXEC, &dma_buf_fd);
	if (ret) {
		igt_assert(ret == EINVAL || ret == ENODEV);
		goto free_userptr;
	} else {
		igt_assert_eq(ret, 0);
		igt_assert_lte(0, dma_buf_fd);
	}

	/* a userptr doesn't have the obj->base.filp, but can be exported via
	 * dma-buf, so make sure it fails here */
	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr == MAP_FAILED && errno == ENODEV);
free_userptr:
	gem_close(fd, handle);
	close(dma_buf_fd);
}

static void
test_errors(uint32_t region, uint64_t size)
{
	int i, dma_buf_fd;
	char *ptr;
	uint32_t handle;
	int invalid_flags[] = {DRM_CLOEXEC - 1, DRM_CLOEXEC + 1,
	                       DRM_RDWR - 1, DRM_RDWR + 1};

	/* Test for invalid flags */
	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	for (i = 0; i < ARRAY_SIZE(invalid_flags); i++) {
		prime_handle_to_fd_no_assert(handle, invalid_flags[i], &dma_buf_fd);
		igt_assert_eq(errno, EINVAL);
		errno = 0;
	}
	gem_close(fd, handle);

	/* Close gem object before priming */
	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);
	gem_close(fd, handle);
	prime_handle_to_fd_no_assert(handle, DRM_CLOEXEC, &dma_buf_fd);
	igt_assert(dma_buf_fd == -1 && errno == ENOENT);
	errno = 0;

	/* close fd before mapping */
	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);
	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);
	close(dma_buf_fd);
	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr == MAP_FAILED && errno == EBADF);
	errno = 0;
	gem_close(fd, handle);

	/* Map too big */
	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	fill_bo(handle, size);
	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);
	ptr = mmap(NULL, size * 2, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
	igt_assert(ptr == MAP_FAILED && errno == EINVAL);
	errno = 0;
	close(dma_buf_fd);
	gem_close(fd, handle);

	/* Overlapping the end of the buffer */
	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	dma_buf_fd = prime_handle_to_fd(fd, handle);
	igt_assert(errno == 0);
	ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_buf_fd, size / 2);
	igt_assert(ptr == MAP_FAILED && errno == EINVAL);
	errno = 0;
	close(dma_buf_fd);
	gem_close(fd, handle);
}

/* Test for invalid flags on sync ioctl */
static void
test_invalid_sync_flags(uint32_t region, uint64_t size)
{
	int i, dma_buf_fd;
	uint32_t handle;
	struct local_dma_buf_sync sync;
	int invalid_flags[] = {-1,
	                       0x00,
	                       LOCAL_DMA_BUF_SYNC_RW + 1,
	                       LOCAL_DMA_BUF_SYNC_VALID_FLAGS_MASK + 1};

	igt_assert(__gem_create_in_memory_regions(fd, &handle, &size, region) == 0);
	dma_buf_fd = prime_handle_to_fd(fd, handle);
	for (i = 0; i < sizeof(invalid_flags) / sizeof(invalid_flags[0]); i++) {
		memset(&sync, 0, sizeof(sync));
		sync.flags = invalid_flags[i];

		drmIoctl(dma_buf_fd, LOCAL_DMA_BUF_IOCTL_SYNC, &sync);
		igt_assert_eq(errno, EINVAL);
		errno = 0;
	}
}

static void
test_aperture_limit(uint32_t region, uint64_t size)
{
	int dma_buf_fd1, dma_buf_fd2;
	char *ptr1, *ptr2;
	uint32_t handle1, handle2;
	/* Two buffers the sum of which > mappable aperture */
	uint64_t size1 = (gem_mappable_aperture_size(fd) * 7) / 8;
	uint64_t size2 = (gem_mappable_aperture_size(fd) * 3) / 8;

	handle1 = gem_create(fd, size1);
	dma_buf_fd1 = prime_handle_to_fd_for_mmap(fd, handle1);
	igt_assert(errno == 0);
	ptr1 = mmap(NULL, size1, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd1, 0);
	igt_assert(ptr1 != MAP_FAILED);
	fill_bo_cpu(ptr1, size);
	igt_assert(memcmp(ptr1, pattern, sizeof(pattern)) == 0);

	handle2 = gem_create(fd, size1);
	dma_buf_fd2 = prime_handle_to_fd_for_mmap(fd, handle2);
	igt_assert(errno == 0);
	ptr2 = mmap(NULL, size2, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd2, 0);
	igt_assert(ptr2 != MAP_FAILED);
	fill_bo_cpu(ptr2, size);
	igt_assert(memcmp(ptr2, pattern, sizeof(pattern)) == 0);

	igt_assert(memcmp(ptr1, ptr2, size) == 0);

	munmap(ptr1, size1);
	munmap(ptr2, size2);
	close(dma_buf_fd1);
	close(dma_buf_fd2);
	gem_close(fd, handle1);
	gem_close(fd, handle2);
}

#define SKIP_LMEM (1 << 0)
#define SKIP_USERPTR (1 << 1)

/*
 * true skips the test
 */
static bool check_skip(uint32_t skip, uint32_t region)
{
	if ((skip & SKIP_LMEM) && IS_DEVICE_MEMORY_REGION(region))
		return true;

	if (skip & SKIP_USERPTR)
		return !has_userptr();

	return false;
}

igt_main
{
	struct igt_collection *set, *regions, *dma_buf_set;
	struct drm_i915_query_memory_regions *query_info;
	struct {
		const char *name;
		void (*fn)(uint32_t, uint64_t);
		uint32_t skip;
	} tests[] = {
		{ "test_correct", test_correct },
		{ "test_map_unmap", test_map_unmap },
		{ "test_reprime", test_reprime },
		{ "test_forked", test_forked },
		{ "test_correct_cpu_write", test_correct_cpu_write },
		{ "test_forked_cpu_write", test_forked_cpu_write },
		{ "test_refcounting", test_refcounting },
		{ "test_dup", test_dup },
		{ "test_userptr", test_userptr, SKIP_LMEM | SKIP_USERPTR },
		{ "test_errors", test_errors },
		{ "test_invalid_sync_flags", test_invalid_sync_flags },
		{ "test_aperture_limit", test_aperture_limit, SKIP_LMEM },
	};
	uint32_t region;
	char *ext;
	int i;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);

		query_info = gem_get_query_memory_regions(fd);
		igt_assert(query_info);

		set = get_memory_region_set(query_info, I915_SYSTEM_MEMORY,
					    I915_DEVICE_MEMORY);

		dma_buf_set = get_dma_buf_mmap_supported_set(fd, set);
		igt_require_f(dma_buf_set, "No dma-buf region supported\n");
		errno = 0;
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++)
		igt_subtest_with_dynamic(tests[i].name) {
			for_each_combination(regions, 1, dma_buf_set) {
				region = igt_collection_get_value(regions, 0);
				if (check_skip(tests[i].skip, region))
					continue;
				ext = memregion_dynamic_subtest_name(regions);
				igt_dynamic_f("%s-%s", tests[i].name, ext)
					tests[i].fn(region, BO_SIZE);
				free(ext);
			}
		}

	igt_fixture {
		free(query_info);
		igt_collection_destroy(set);
		igt_collection_destroy(dma_buf_set);
		close(fd);
	}
}
