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

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "drm.h"

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_x86.h"

IGT_TEST_DESCRIPTION("Basic MMAP_OFFSET IOCTL tests for mem regions\n");

static int mmap_offset_ioctl(int i915, struct drm_i915_gem_mmap_offset *arg)
{
	int err = 0;

	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_MMAP_OFFSET, arg)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void *
__mmap_offset(int i915, uint32_t handle, uint64_t offset, uint64_t size,
	      unsigned int prot, uint64_t flags)
{
	struct drm_i915_gem_mmap_offset arg = {
		.handle = handle,
		.flags = flags,
	};
	void *ptr;

	if (mmap_offset_ioctl(i915, &arg))
		return NULL;

	ptr = mmap64(0, size, prot, MAP_SHARED, i915, arg.offset + offset);
	if (ptr == MAP_FAILED)
		ptr = NULL;
	else
		errno = 0;

	return ptr;
}

static void bad_object(int i915)
{
	uint32_t real_handle;
	uint32_t handles[20];
	int i = 0;

	real_handle = gem_create(i915, 4096);

	handles[i++] = 0xdeadbeef;
	for (int bit = 0; bit < 16; bit++)
		handles[i++] = real_handle | (1 << (bit + 16));
	handles[i] = real_handle + 1;

	for (; i >= 0; i--) {
		struct drm_i915_gem_mmap_offset arg = {
			.handle = handles[i],
			.flags = I915_MMAP_OFFSET_WB,
		};

		igt_debug("Trying MMAP IOCTL with handle %x\n",
			  handles[i]);
		igt_assert_eq(mmap_offset_ioctl(i915, &arg),
			      -ENOENT);
	}

	gem_close(i915, real_handle);
}

static void bad_flags(int i915)
{
	struct drm_i915_gem_mmap_offset arg = {
		.handle = gem_create(i915, 4096),
		.flags = -1ull,
	};

	igt_assert_eq(mmap_offset_ioctl(i915, &arg), -EINVAL);
	gem_close(i915, arg.handle);
}

static void bad_extensions(int i915)
{
	struct i915_user_extension ext;
	struct drm_i915_gem_mmap_offset arg = {
		.handle = gem_create(i915, 4096),
		.extensions = -1ull,
	};

	igt_assert_eq(mmap_offset_ioctl(i915, &arg), -EFAULT);
	arg.extensions = to_user_pointer(&ext);

	ext.name = -1;
	igt_assert_eq(mmap_offset_ioctl(i915, &arg), -EINVAL);

	gem_close(i915, arg.handle);
}

static void basic_uaf(int i915)
{
	const uint32_t obj_size = 4096;

	for_each_mmap_offset_type(i915, t) {
		uint32_t handle = gem_create(i915, obj_size);
		uint8_t *expected, *buf, *addr;

		addr = __mmap_offset(i915, handle, 0, obj_size,
				     PROT_READ | PROT_WRITE,
				     t->type);
		if (!addr) {
			gem_close(i915, handle);
			continue;
		}

		expected = calloc(obj_size, sizeof(*expected));
		gem_set_domain(i915, handle, t->domain, 0);
		igt_assert_f(memcmp(addr, expected, obj_size) == 0,
			     "mmap(%s) not clear on gem_create()\n",
			     t->name);
		free(expected);

		buf = calloc(obj_size, sizeof(*buf));
		memset(buf + 1024, 0x01, 1024);
		gem_write(i915, handle, 0, buf, obj_size);
		gem_set_domain(i915, handle, t->domain, 0);
		igt_assert_f(memcmp(buf, addr, obj_size) == 0,
			     "mmap(%s) not coherent with gem_write()\n",
			     t->name);

		gem_set_domain(i915, handle, t->domain, t->domain);
		memset(addr + 2048, 0xff, 1024);
		gem_read(i915, handle, 0, buf, obj_size);
		gem_set_domain(i915, handle, t->domain, 0);
		igt_assert_f(memcmp(buf, addr, obj_size) == 0,
			     "mmap(%s) not coherent with gem_read()\n",
			     t->name);

		gem_close(i915, handle);
		igt_assert_f(memcmp(buf, addr, obj_size) == 0,
			     "mmap(%s) not resident after gem_close()\n",
			     t->name);
		free(buf);

		igt_debug("Testing unmapping\n");
		munmap(addr, obj_size);
	}
}

static void isolation(int i915)
{
	for_each_mmap_offset_type(i915, t) {
		struct drm_i915_gem_mmap_offset mmap_arg = {
			.flags = t->type
		};
		int A = gem_reopen_driver(i915);
		int B = gem_reopen_driver(i915);
		uint64_t offset_a, offset_b;
		uint32_t a, b;
		void *ptr;

		a = gem_create(A, 4096);
		b = gem_open(B, gem_flink(A, a));

		mmap_arg.handle = a;
		if (mmap_offset_ioctl(A, &mmap_arg)) {
			close(A);
			close(B);
			continue;
		}
		offset_a = mmap_arg.offset;

		mmap_arg.handle = b;
		igt_assert_eq(mmap_offset_ioctl(B, &mmap_arg), 0);
		offset_b = mmap_arg.offset;

		igt_info("A[%s]: {fd:%d, handle:%d, offset:%"PRIx64"}\n",
			 t->name, A, a, offset_a);
		igt_info("B[%s]: {fd:%d, handle:%d, offset:%"PRIx64"}\n",
			 t->name, B, b, offset_b);

		errno = 0;
		ptr = mmap64(0, 4096, PROT_READ, MAP_SHARED, i915, offset_a);
		igt_assert(ptr == MAP_FAILED);
		igt_assert_eq(errno, EACCES);

		errno = 0;
		ptr = mmap64(0, 4096, PROT_READ, MAP_SHARED, i915, offset_b);
		igt_assert(ptr == MAP_FAILED);
		igt_assert_eq(errno, EACCES);

		if (offset_a != offset_b) {
			errno = 0;
			ptr = mmap(0, 4096, PROT_READ, MAP_SHARED, B, offset_a);
			igt_assert(ptr == MAP_FAILED);
			igt_assert_eq(errno, EACCES);

			errno = 0;
			ptr = mmap(0, 4096, PROT_READ, MAP_SHARED, A, offset_b);
			igt_assert(ptr == MAP_FAILED);
			igt_assert_eq(errno, EACCES);
		}

		close(B);

		ptr = mmap64(0, 4096, PROT_READ, MAP_SHARED, A, offset_a);
		igt_assert(ptr != MAP_FAILED);
		munmap(ptr, 4096);

		close(A);

		ptr = mmap64(0, 4096, PROT_READ, MAP_SHARED, A, offset_a);
		igt_assert(ptr == MAP_FAILED);
	}
}

static void pf_nonblock(int i915)
{
	igt_spin_t *spin = igt_spin_new(i915);

	for_each_mmap_offset_type(i915, t) {
		uint32_t *ptr;

		ptr = __mmap_offset(i915, spin->handle, 0, 4096,
				    PROT_READ | PROT_WRITE,
				    t->type);
		if (!ptr)
			continue;

		igt_set_timeout(1, t->name);
		/* no set-domain as we want to verify the pagefault is async */
		ptr[256] = 0;
		igt_reset_timeout();

		munmap(ptr, 4096);
	}

	igt_spin_free(i915, spin);
}

static void *memchr_inv(const void *s, int c, size_t n)
{
	const uint8_t *us = s;
	const uint8_t uc = c;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
	while (n--) {
		if (*us != uc)
			return (void *) us;
		us++;
	}
#pragma GCC diagnostic pop

	return NULL;
}

static void test_ptrace(int i915)
{
	const unsigned int SZ = 3 * 4096;
	unsigned long *ptr, *cpy;
	unsigned long AA, CC;
	uint32_t bo;

	memset(&AA, 0xaa, sizeof(AA));
	memset(&CC, 0x55, sizeof(CC));

	cpy = malloc(SZ);
	bo = gem_create(i915, SZ);

	for_each_mmap_offset_type(i915, t) {
		ptr = __mmap_offset(i915, bo, 0, SZ,
				    PROT_READ | PROT_WRITE,
				    t->type);
		if (!ptr)
			continue;

		igt_dynamic_f("%s", t->name) {
			pid_t pid;

			memset(cpy, AA, SZ);
			memset(ptr, CC, SZ);

			igt_assert(!memchr_inv(ptr, CC, SZ));
			igt_assert(!memchr_inv(cpy, AA, SZ));

			igt_fork(child, 1) {
				ptrace(PTRACE_TRACEME, 0, NULL, NULL);
				raise(SIGSTOP);
			}

			/* Wait for the child to ready themselves [SIGSTOP] */
			pid = wait(NULL);

			ptrace(PTRACE_ATTACH, pid, NULL, NULL);
			for (int i = 0; i < SZ / sizeof(long); i++) {
				long ret;

				ret = ptrace(PTRACE_PEEKDATA, pid, ptr + i);
				igt_assert_eq_u64(ret, CC);
				cpy[i] = ret;

				ret = ptrace(PTRACE_POKEDATA, pid, ptr + i, AA);
				igt_assert_eq(ret, 0l);
			}
			ptrace(PTRACE_DETACH, pid, NULL, NULL);

			/* Wakeup the child for it to exit */
			kill(SIGCONT, pid);
			igt_waitchildren();

			/* The two buffers should now be swapped */
			igt_assert(!memchr_inv(ptr, AA, SZ));
			igt_assert(!memchr_inv(cpy, CC, SZ));
		}

		munmap(ptr, SZ);
	}

	gem_close(i915, bo);
	free(cpy);
}

static void close_race(int i915, int timeout)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	_Atomic uint32_t *handles;
	size_t len = ALIGN((ncpus + 1) * sizeof(uint32_t), 4096);

	handles = mmap64(0, len, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(handles != MAP_FAILED);

	igt_fork(child, ncpus + 1) {
		do {
			struct drm_i915_gem_mmap_offset mmap_arg = {};
			const int i = 1 + random() % ncpus;
			uint32_t old;

			mmap_arg.handle = gem_create(i915, 4096);
			mmap_arg.flags = I915_MMAP_OFFSET_WB;
			old = atomic_exchange(&handles[i], mmap_arg.handle);
			ioctl(i915, DRM_IOCTL_GEM_CLOSE, &old);

			if (ioctl(i915,
				  DRM_IOCTL_I915_GEM_MMAP_OFFSET,
				  &mmap_arg) != -1) {
				void *ptr;

				ptr = mmap64(0, 4096,
					     PROT_WRITE, MAP_SHARED, i915,
					     mmap_arg.offset);
				if (ptr != MAP_FAILED) {
					*(volatile uint32_t *)ptr = 0;
					munmap(ptr, 4096);
				}
			}

		} while (!READ_ONCE(handles[0]));
	}

	sleep(timeout);
	handles[0] = 1;
	igt_waitchildren();

	for (int i = 1; i <= ncpus; i++)
		ioctl(i915, DRM_IOCTL_GEM_CLOSE, handles[i]);
	munmap(handles, len);
}

static void open_flood(int i915, int timeout)
{
	unsigned long count;
	uint32_t handle;
	int dmabuf;
	int *ctl;

	ctl = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ctl != MAP_FAILED);

	handle = gem_create(i915, 4096);
	dmabuf = prime_handle_to_fd(i915, handle);

	for_each_mmap_offset_type(i915, t) {
		struct drm_i915_gem_mmap_offset arg = {
			.handle = handle,
			.flags = t->type,
		};

		if (mmap_offset_ioctl(i915, &arg))
			continue;

		igt_fork(child, 1) {
			i915 = gem_reopen_driver(i915);
			arg.handle = prime_fd_to_handle(i915, dmabuf);

			do {
				igt_assert_eq(mmap_offset_ioctl(i915, &arg), 0);
			} while (!READ_ONCE(*ctl));
		}
	}
	gem_close(i915, handle);

	count = 0;
	igt_until_timeout(timeout) {
		int tmp;

		tmp = gem_reopen_driver(i915);
		handle = prime_fd_to_handle(i915, dmabuf);

		for_each_mmap_offset_type(i915, t) {
			struct drm_i915_gem_mmap_offset arg = {
				.handle = handle,
				.flags = t->type,
			};

			mmap_offset_ioctl(i915, &arg);
		}

		close(tmp);
		count++;
	}

	igt_info("Completed %lu cycles\n", count);
	close(dmabuf);

	*ctl = 1;
	igt_waitchildren();
	munmap(ctl, 4096);
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

static int create_ioctl(int i915, struct drm_i915_gem_create *create)
{
	int err = 0;

	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_CREATE, create)) {
		err = -errno;
		igt_assume(err != 0);
	}

	errno = 0;
	return err;
}

static void *thread_clear(void *data)
{
	struct thread_clear *arg = data;
	const struct mmap_offset *t;
	unsigned long checked = 0;
	int i915 = arg->i915;

	t = mmap_offset_types;
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
		ptr = __mmap_offset(i915, create.handle, 0, create.size,
				    PROT_READ | PROT_WRITE,
				    t->type);
		/* No set-domains as we are being as naughty as possible */
		for (uint64_t page = 0; ptr && page < npages; page++) {
			uint64_t x[8] = {
				page * 4096 +
					sizeof(x) * ((page % (4096 - sizeof(x)) / sizeof(x)))
			};

			if (page & 1)
				igt_memcpy_from_wc(x, ptr + x[0], sizeof(x));
			else
				memcpy(x, ptr + x[0], sizeof(x));

			for (int i = 0; i < ARRAY_SIZE(x); i++)
				igt_assert_eq_u64(x[i], 0);
		}
		if (ptr)
			munmap(ptr, create.size);
		gem_close(i915, create.handle);
		checked += npages;

		atomic_fetch_add(&arg->max, npages);

		if (!(++t)->name)
			t = mmap_offset_types;
	}

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

static struct intel_buf *create_bo(struct buf_ops *bops, uint32_t value,
				   uint32_t width, uint32_t height)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_buf *buf;
	uint32_t *v, size;

	buf = intel_buf_create(bops, width, height, 32, 0, I915_TILING_NONE, 0);
	size = buf->surface[0].size;
	v = gem_mmap__cpu_coherent(i915, buf->handle, 0, size, PROT_WRITE);

	gem_set_domain(i915, buf->handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	for (int i = 0; i < 64 / sizeof(*v); i++)
		v[i] = value;

	munmap(v, size);

	return buf;
}

static void blt_coherency(int i915)
{
	struct buf_ops *bops;
	struct intel_buf *src, *dst;
	struct intel_bb *ibb;
	uint32_t width = 512;
	uint32_t height = 512;
	uint32_t *psrc, *pdst, size;
	bool compare_ok;
	int i;

	bops = buf_ops_create(i915);
	ibb = intel_bb_create(i915, 4096);

	src = create_bo(bops, 2, width, height);
	dst = create_bo(bops, 1, width, height);
	size = src->surface[0].size;

	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, dst, true);

	intel_bb_blt_copy(ibb,
			  src, 0, 0, src->surface[0].stride,
			  dst, 0, 0, dst->surface[0].stride,
			  intel_buf_width(dst),
			  intel_buf_height(dst), dst->bpp);

	psrc = gem_mmap__cpu_coherent(i915, src->handle, 0, size, PROT_READ);
	gem_set_domain(i915, src->handle, I915_GEM_DOMAIN_CPU, 0);

	pdst = gem_mmap__cpu_coherent(i915, dst->handle, 0, size, PROT_READ);
	gem_set_domain(i915, dst->handle, I915_GEM_DOMAIN_CPU, 0);

	for (i = 0; i < 16; i++)
		igt_debug("[%2d] %08x <> %08x\n", i, psrc[i], pdst[i]);

	compare_ok = psrc[0] == pdst[0];

	munmap(psrc, size);
	munmap(pdst, size);

	intel_buf_destroy(src);
	intel_buf_destroy(dst);

	intel_bb_destroy(ibb);
	buf_ops_destroy(bops);

	igt_assert_f(compare_ok, "Problem with coherency, flush is too late\n");
}

static int mmap_gtt_version(int i915)
{
	int gtt_version = -1;
	struct drm_i915_getparam gp = {
		.param = I915_PARAM_MMAP_GTT_VERSION,
		.value = &gtt_version,
	};
	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);

	return gtt_version;
}

static bool has_mmap_offset(int i915)
{
	return mmap_gtt_version(i915) >= 4;
}

igt_main
{
	int i915;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require(has_mmap_offset(i915));
	}

	igt_describe("Verify mapping to invalid gem objects won't be created");
	igt_subtest_f("bad-object")
		bad_object(i915);
	igt_subtest_f("bad-flags")
		bad_flags(i915);
	igt_subtest_f("bad-extensions")
		bad_extensions(i915);

	igt_describe("Check buffer object mapping persists after gem_close");
	igt_subtest_f("basic-uaf")
		basic_uaf(i915);

	igt_subtest_f("isolation")
		isolation(i915);
	igt_subtest_f("pf-nonblock")
		pf_nonblock(i915);

	igt_subtest_with_dynamic("ptrace")
		test_ptrace(i915);

	igt_describe("Check race between close and mmap offset between threads");
	igt_subtest_f("close-race")
		close_race(i915, 20);

	igt_subtest_f("open-flood")
		open_flood(i915, 20);

	igt_subtest_f("clear")
		always_clear(i915, 20);

	igt_subtest_f("blt-coherency")
		blt_coherency(i915);

	igt_fixture {
		close(i915);
	}
}
