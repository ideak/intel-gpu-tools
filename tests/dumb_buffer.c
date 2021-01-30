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
 *
 * Authors:
 *    Ramalingam C <ramalingam.c@intel.com>
 *
 */

/** @file dumb_buffer.c
 *
 * The goal is to simply ensure that basics work and invalid input
 * combinations are rejected.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <drm.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "ioctl_wrappers.h"

IGT_TEST_DESCRIPTION("This is a test for the generic dumb buffer interface.");

static int __dumb_create(int fd, struct drm_mode_create_dumb *create)
{
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, create)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void dumb_create(int fd, struct drm_mode_create_dumb *create)
{
	igt_assert_eq(__dumb_create(fd, create), 0);
}

static void *__dumb_map(int fd, uint32_t handle, uint64_t size, unsigned prot)
{
	struct drm_mode_map_dumb arg = { handle };
	void *ptr;

	if (igt_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg))
		return NULL;

	ptr = mmap(NULL, size, prot, MAP_SHARED, fd, arg.offset);
	if (ptr == MAP_FAILED)
		return NULL;

	return ptr;
}

static void *dumb_map(int fd, uint32_t handle, uint64_t size, unsigned prot)
{
	void *ptr;

	ptr = __dumb_map(fd, handle, size, prot);
	igt_assert(ptr);

	return ptr;
}

static int __dumb_destroy(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb arg = { handle };
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void dumb_destroy(int fd, uint32_t handle)
{
	igt_assert_eq(__dumb_destroy(fd, handle), 0);
}

static void invalid_dimensions_test(int fd)
{
	struct drm_mode_create_dumb create;

	memset(&create, 0, sizeof(create));
	create.width = 4032;
	create.height = 2016;
	create.bpp = 24;
	igt_assert_eq(__dumb_create(fd, &create), -EINVAL);

	create.bpp = 32;
	create.width = 0;
	igt_assert_eq(__dumb_create(fd, &create), -EINVAL);

	create.width = 4032;
	create.height = 0;
	igt_assert_eq(__dumb_create(fd, &create), -EINVAL);
}

static void valid_dumb_creation_test(int fd)
{
	struct drm_mode_create_dumb create = {
		.width = 4032,
		.height = 2016,
		.bpp = 32,
	};

	dumb_create(fd, &create);
	dumb_destroy(fd, create.handle);
}

static void valid_map(int fd)
{
	struct drm_mode_create_dumb create = {
		.width = 4032,
		.height = 2016,
		.bpp = 32,
	};

	dumb_create(fd, &create);
	munmap(dumb_map(fd, create.handle, create.size, PROT_READ),
	       create.size);
	dumb_destroy(fd, create.handle);
}

static void uaf_map(int fd)
{
	struct drm_mode_create_dumb create = {
		.width = 4032,
		.height = 2016,
		.bpp = 32,
	};
	uint32_t *ptr;

	dumb_create(fd, &create);
	ptr = dumb_map(fd, create.handle, create.size, PROT_READ),
	dumb_destroy(fd, create.handle);

	igt_assert_eq(*ptr, 0);
	munmap(ptr, create.size);
}

static void invalid_size_map(int fd)
{
	struct drm_mode_map_dumb arg = {};
	struct drm_mode_create_dumb create = {
		.width = 4032,
		.height = 2016,
		.bpp = 32,
	};
	void *ptr;

	dumb_create(fd, &create);

	arg.handle = create.handle;
	do_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);

	ptr = mmap(NULL, create.size + 1, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, arg.offset);
	igt_assert(ptr == MAP_FAILED);

	dumb_destroy(fd, create.handle);
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
		try = 1 + npages % ((max / 2) ? (max / 2) : 1);
		max -= try;
	} while ((max = atomic_compare_swap_u64(global, old, max)) != old);

	return try;
}

struct thread_clear {
	_Atomic(uint64_t) max;
	uint64_t page_size;
	int timeout;
	int fd;
};

#define MAX_PAGE_TO_REQUEST	102400

static void *thread_clear(void *data)
{
	struct thread_clear *arg = data;
	unsigned long checked = 0;
	int fd = arg->fd;
	void *ptr;

	igt_until_timeout(arg->timeout) {
		struct drm_mode_create_dumb create = {};
		uint64_t npages;

		npages = random();
		npages = npages << 32;
		npages |= random();
		npages = get_npages(&arg->max, npages);

		for (uint64_t _npages = npages; npages > 0; npages -= _npages) {
			create.bpp = 32;
			create.width = arg->page_size / (create.bpp / 8);
			_npages = npages <= MAX_PAGE_TO_REQUEST ? npages :
				  MAX_PAGE_TO_REQUEST;
			create.height = _npages;

			dumb_create(fd, &create);
			ptr = dumb_map(fd,
				       create.handle, create.size,
				       PROT_WRITE);

			for (uint64_t page = 0; page < create.height; page++) {
				uint64_t x;

				memcpy(&x,
				       ptr + page * 4096 + page % (4096 - sizeof(x)),
				       sizeof(x));
				igt_assert_eq_u64(x, 0);
			}

			munmap(ptr, create.size);

			dumb_destroy(fd, create.handle);
			atomic_fetch_add(&arg->max, _npages);
				checked += _npages;
		}
	}

	return (void *)(uintptr_t)checked;
}

static jmp_buf sigjmp;

__noreturn static void sigprobe(int sig)
{
	longjmp(sigjmp, sig);
}

static uint64_t estimate_largest_dumb_buffer(int fd)
{
	sighandler_t old_sigbus = signal(SIGBUS, sigprobe);
	sighandler_t old_sigsegv = signal(SIGSEGV, sigprobe);
	struct drm_mode_create_dumb create = {
		.bpp = 32,
		.width = 1 << 18, /* in pixels */
		.height = 1, /* in rows */
	};
	const unsigned long max_rows =
		intel_get_total_ram_mb() / 2; /* leave some spare */
	volatile uint64_t largest = 0;
	char * volatile ptr = NULL;

	if (setjmp(sigjmp)) {
		if (ptr)
			munmap(ptr, create.size);

		signal(SIGBUS, old_sigbus);
		signal(SIGSEGV, old_sigsegv);

		igt_info("Largest dumb buffer sucessfully created: %'"PRIu64" bytes\n",
			 largest);
		return largest;
	}

	for (create.height = 1; create.height < max_rows; create.height *= 2) {
		if (__dumb_create(fd, &create))
			longjmp(sigjmp, SIGABRT);

		ptr = __dumb_map(fd, create.handle, create.size, PROT_READ);
		dumb_destroy(fd, create.handle);
		if (!ptr)
			longjmp(sigjmp, SIGABRT);

		if (!*ptr)
			largest = create.size;

		munmap(ptr, create.size);
		ptr = NULL;
	}

	longjmp(sigjmp, SIGABRT);
}

static uint64_t probe_page_size(int fd)
{
	struct drm_mode_create_dumb create = {
		.bpp = 32,
		.width = 1, /* in pixels */
		.height = 1, /* in rows */
	};

	dumb_create(fd, &create);
	dumb_destroy(fd, create.handle);

	return create.size;
}

static void always_clear(int fd, int timeout)
{
	struct thread_clear arg = {
		.fd = fd,
		.timeout = timeout,
		.page_size = probe_page_size(fd),
		.max = estimate_largest_dumb_buffer(fd),
	};
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned long checked;
	pthread_t thread[ncpus];
	void *result;

	arg.max /= arg.page_size;
	for (int i = 0; i < ncpus; i++)
		pthread_create(&thread[i], NULL, thread_clear, &arg);

	checked = 0;
	for (int i = 0; i < ncpus; i++) {
		pthread_join(thread[i], &result);
		checked += (uintptr_t)result;
	}
	igt_info("Checked %'lu page allocations\n", checked);
}

igt_main
{
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_ANY);
	}

	igt_subtest("invalid-bpp")
		invalid_dimensions_test(fd);

	igt_subtest("create-valid-dumb")
		valid_dumb_creation_test(fd);

	igt_subtest("map-valid")
		valid_map(fd);

	igt_subtest("map-uaf")
		uaf_map(fd);

	igt_subtest("map-invalid-size")
		invalid_size_map(fd);

	igt_subtest("create-clear")
		always_clear(fd, 30);
}
