/*
 * Copyright © 2011 Intel Corporation
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
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include <unistd.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "drm.h"

#include "i915/gem.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "igt_x86.h"
#include "sw_sync.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define abs(x) ((x) >= 0 ? (x) : -(x))

static int OBJECT_SIZE = 16*1024*1024;

static void
set_domain_gtt(int fd, uint32_t handle)
{
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
}

static void *
mmap_bo(int fd, uint32_t handle, uint64_t size)
{
	void *ptr;

	ptr = gem_mmap__gtt(fd, handle, size, PROT_READ | PROT_WRITE);

	return ptr;
}

static void *
create_pointer_size(int fd, uint64_t size, int tiling)
{
	uint32_t handle;
	void *ptr;

	handle = gem_create(fd, size);
	if (tiling)
		gem_set_tiling(fd, handle, tiling, 1024);

	ptr = mmap_bo(fd, handle, size);

	gem_close(fd, handle);

	return ptr;
}

static void *
create_pointer(int fd)
{
	return create_pointer_size(fd, OBJECT_SIZE, I915_TILING_NONE);
}

static void
test_access(int fd)
{
	uint32_t handle, flink, handle2;
	struct drm_i915_gem_mmap_gtt mmap_arg;
	int fd2;

	handle = gem_create(fd, OBJECT_SIZE);
	igt_assert(handle);

	fd2 = drm_open_driver(DRIVER_INTEL);

	/* Check that fd1 can mmap. */
	mmap_arg.handle = handle;
	do_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &mmap_arg);

	igt_assert(mmap64(0, OBJECT_SIZE, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, mmap_arg.offset));

	/* Check that the same offset on the other fd doesn't work. */
	igt_assert(mmap64(0, OBJECT_SIZE, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd2, mmap_arg.offset) == MAP_FAILED);
	igt_assert(errno == EACCES);

	flink = gem_flink(fd, handle);
	igt_assert(flink);
	handle2 = gem_open(fd2, flink);
	igt_assert(handle2);

	/* Recheck that it works after flink. */
	/* Check that the same offset on the other fd doesn't work. */
	igt_assert(mmap64(0, OBJECT_SIZE, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd2, mmap_arg.offset));
}

static void
test_zero_extend(int i915)
{
	struct drm_i915_gem_mmap_gtt arg = {};
	uint64_t redzone[16];

	memset(redzone, 0xc5, sizeof(redzone));
	arg.handle = gem_create(i915, 4096);

	igt_assert_eq(ioctl(i915, DRM_IOCTL_I915_GEM_MMAP_GTT, &arg), 0);

	gem_close(i915, arg.handle);
}

static void
test_short(int fd)
{
	struct drm_i915_gem_mmap_gtt mmap_arg;
	int pages, p;

	mmap_arg.handle = gem_create(fd, OBJECT_SIZE);
	igt_assert(mmap_arg.handle);

	do_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &mmap_arg);
	for (pages = 1; pages <= OBJECT_SIZE / PAGE_SIZE; pages <<= 1) {
		uint8_t *r, *w;

		w = mmap64(0, pages * PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, mmap_arg.offset);
		igt_assert(w != MAP_FAILED);

		r = mmap64(0, pages * PAGE_SIZE, PROT_READ,
			   MAP_SHARED, fd, mmap_arg.offset);
		igt_assert(r != MAP_FAILED);

		for (p = 0; p < pages; p++) {
			w[p*PAGE_SIZE] = r[p*PAGE_SIZE];
			w[p*PAGE_SIZE+(PAGE_SIZE-1)] =
				r[p*PAGE_SIZE+(PAGE_SIZE-1)];
		}

		munmap(r, pages * PAGE_SIZE);
		munmap(w, pages * PAGE_SIZE);
	}
	gem_close(fd, mmap_arg.handle);
}

static void
test_copy(int fd)
{
	void *src, *dst;

	/* copy from a fresh src to fresh dst to force pagefault on both */
	src = create_pointer(fd);
	dst = create_pointer(fd);

	memcpy(dst, src, OBJECT_SIZE);
	memcpy(src, dst, OBJECT_SIZE);

	munmap(dst, OBJECT_SIZE);
	munmap(src, OBJECT_SIZE);
}

enum test_read_write {
	READ_BEFORE_WRITE,
	READ_AFTER_WRITE,
};

static void
test_read_write(int fd, enum test_read_write order)
{
	uint32_t handle;
	void *ptr;
	volatile uint32_t val = 0;

	handle = gem_create(fd, OBJECT_SIZE);

	ptr = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_READ | PROT_WRITE);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	if (order == READ_BEFORE_WRITE) {
		val = *(uint32_t *)ptr;
		*(uint32_t *)ptr = val;
	} else {
		*(uint32_t *)ptr = val;
		val = *(uint32_t *)ptr;
	}

	gem_close(fd, handle);
	munmap(ptr, OBJECT_SIZE);
}

static void
test_read_write2(int fd, enum test_read_write order)
{
	uint32_t handle;
	void *r, *w;
	volatile uint32_t val = 0;

	handle = gem_create(fd, OBJECT_SIZE);

	r = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_READ);
	w = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_READ | PROT_WRITE);

	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	if (order == READ_BEFORE_WRITE) {
		val = *(uint32_t *)r;
		*(uint32_t *)w = val;
	} else {
		*(uint32_t *)w = val;
		val = *(uint32_t *)r;
	}

	gem_close(fd, handle);
	munmap(r, OBJECT_SIZE);
	munmap(w, OBJECT_SIZE);
}

static void
test_write(int fd)
{
	void *src;
	uint32_t dst;

	/* copy from a fresh src to fresh dst to force pagefault on both */
	src = create_pointer(fd);
	dst = gem_create(fd, OBJECT_SIZE);

	gem_write(fd, dst, 0, src, OBJECT_SIZE);

	gem_close(fd, dst);
	munmap(src, OBJECT_SIZE);
}

static void
test_wc(int fd)
{
	unsigned long gtt_reads, gtt_writes, cpu_writes;
	uint32_t handle;
	void *gtt, *cpu;

	handle = gem_create(fd, 4096);
	cpu = gem_mmap__cpu(fd, handle, 0, 4096, PROT_READ | PROT_WRITE);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	gem_close(fd, handle);

	handle = gem_create(fd, 4096);
	gtt = gem_mmap__gtt(fd, handle, 4096, PROT_READ | PROT_WRITE);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, handle);

	gtt_reads = 0;
	igt_for_milliseconds(200) {
		memcpy(cpu, gtt, 4096);
		gtt_reads++;
	}
	igt_debug("%lu GTT reads in 200us\n", gtt_reads);

	gtt_writes = 0;
	igt_for_milliseconds(200) {
		memcpy(gtt, cpu, 4096);
		gtt_writes++;
	}
	igt_debug("%lu GTT writes in 200us\n", gtt_writes);

	if (igt_setup_clflush()) {
		cpu_writes = 0;
		igt_for_milliseconds(200) {
			igt_clflush_range(cpu, 4096);
			cpu_writes++;
		}
		igt_debug("%lu CPU writes in 200us\n", cpu_writes);
	} else
		cpu_writes = gtt_writes;

	munmap(cpu, 4096);
	munmap(gtt, 4096);

	igt_assert_f(gtt_writes > 2*gtt_reads,
		     "Write-Combined writes are expected to be much faster than reads: read=%.2fMiB/s, write=%.2fMiB/s\n",
		     5*gtt_reads/256., 5*gtt_writes/256.);

	igt_assert_f(gtt_writes > cpu_writes/2,
		     "Write-Combined writes are expected to be roughly equivalent to WB writes: WC (gtt)=%.2fMiB/s, WB (cpu)=%.2fMiB/s\n",
		     5*gtt_writes/256., 5*cpu_writes/256.);
}

static int mmap_gtt_version(int i915)
{
	int val = 0;
	struct drm_i915_getparam gp = {
		gp.param = 40, /* MMAP_GTT_VERSION */
		gp.value = &val,
	};

	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return val;
}

static void
test_pf_nonblock(int i915)
{
	igt_spin_t *spin;
	uint32_t *ptr;

	igt_require(mmap_gtt_version(i915) >= 3);

	spin = igt_spin_new(i915);

	igt_set_timeout(1, "initial pagefaulting did not complete within 1s");

	ptr = gem_mmap__gtt(i915, spin->handle, 4096, PROT_WRITE);
	ptr[256] = 0;
	munmap(ptr, 4096);

	igt_reset_timeout();

	igt_spin_free(i915, spin);
}

static void
test_isolation(int i915)
{
	struct drm_i915_gem_mmap_gtt mmap_arg;
	int A = gem_reopen_driver(i915);
	int B = gem_reopen_driver(i915);
	uint64_t offset_a, offset_b;
	uint32_t a, b;
	void *ptr;

	a = gem_create(A, 4096);
	b = gem_open(B, gem_flink(A, a));

	mmap_arg.handle = a;
	do_ioctl(A, DRM_IOCTL_I915_GEM_MMAP_GTT, &mmap_arg);
	offset_a = mmap_arg.offset;

	mmap_arg.handle = b;
	do_ioctl(B, DRM_IOCTL_I915_GEM_MMAP_GTT, &mmap_arg);
	offset_b = mmap_arg.offset;

	igt_info("A: {fd:%d, handle:%d, offset:%"PRIx64"}\n",
		 A, a, offset_a);
	igt_info("B: {fd:%d, handle:%d, offset:%"PRIx64"}\n",
		 B, b, offset_b);

	close(B);

	ptr = mmap64(0, 4096, PROT_READ, MAP_SHARED, A, offset_a);
	igt_assert(ptr != MAP_FAILED);
	munmap(ptr, 4096);

	close(A);

	ptr = mmap64(0, 4096, PROT_READ, MAP_SHARED, A, offset_a);
	igt_assert(ptr == MAP_FAILED);
}

static void
test_close_race(int i915)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	_Atomic uint32_t *handles;

	handles = mmap64(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(handles != MAP_FAILED);

	igt_fork(child, ncpus + 1) {
		do {
			struct drm_i915_gem_mmap_gtt mmap_arg = {};
			const int i = 1 + random() % ncpus;
			uint32_t old;

			mmap_arg.handle = gem_create(i915, 4096);
			old = atomic_exchange(&handles[i], mmap_arg.handle);
			ioctl(i915, DRM_IOCTL_GEM_CLOSE, &old);

			if (ioctl(i915,
				  DRM_IOCTL_I915_GEM_MMAP_GTT,
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

	sleep(20);
	handles[0] = 1;
	igt_waitchildren();

	for (int i = 1; i <= ncpus; i++)
		ioctl(i915, DRM_IOCTL_GEM_CLOSE, handles[i]);
	munmap(handles, 4096);
}

static void
test_flink_race(int i915)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	_Atomic uint32_t *handles;

	handles = mmap64(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(handles != MAP_FAILED);

	igt_fork(child, ncpus + 1) {
		int fd = gem_reopen_driver(i915);

		do {
			struct drm_i915_gem_mmap_gtt mmap_arg = {};
			const int i = 1 + random() % ncpus;
			uint32_t old;

			old = atomic_exchange(&handles[i],
					      gem_create(i915, 4096));
			if (!old)
				continue;

			mmap_arg.handle =
				gem_open(fd, gem_flink(i915, old));
			gem_close(i915, old);

			if (ioctl(fd,
				  DRM_IOCTL_I915_GEM_MMAP_GTT,
				  &mmap_arg) != -1) {
				void *ptr;

				ptr = mmap64(0, 4096,
					     PROT_WRITE, MAP_SHARED, fd,
					     mmap_arg.offset);
				if (ptr != MAP_FAILED) {
					*(volatile uint32_t *)ptr = 0;
					munmap(ptr, 4096);
				}
			}

			gem_close(fd, mmap_arg.handle);
		} while (!READ_ONCE(handles[0]));
	}

	sleep(20);
	handles[0] = 1;
	igt_waitchildren();

	for (int i = 1; i <= ncpus; i++)
		ioctl(i915, DRM_IOCTL_GEM_CLOSE, handles[i]);
	munmap(handles, 4096);
}

static void
test_write_gtt(int fd)
{
	uint32_t dst;
	char *dst_gtt;
	void *src;

	dst = gem_create(fd, OBJECT_SIZE);

	/* prefault object into gtt */
	dst_gtt = mmap_bo(fd, dst, OBJECT_SIZE);
	set_domain_gtt(fd, dst);
	memset(dst_gtt, 0, OBJECT_SIZE);
	munmap(dst_gtt, OBJECT_SIZE);

	src = create_pointer(fd);

	gem_write(fd, dst, 0, src, OBJECT_SIZE);

	gem_close(fd, dst);
	munmap(src, OBJECT_SIZE);
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

static void
test_ptrace(int fd)
{
	unsigned long sz = 16 * 4096;
	unsigned long AA, CC;
	unsigned long *gtt, *cpy;
	uint32_t bo;
	pid_t pid;

	memset(&AA, 0xaa, sizeof(AA));
	memset(&CC, 0x55, sizeof(CC));

	cpy = malloc(sz);
	memset(cpy, AA, sz);

	bo = gem_create(fd, sz);
	gtt = mmap_bo(fd, bo, sz);
	memset(gtt, CC, sz);
	gem_close(fd, bo);

	igt_assert(!memchr_inv(gtt, CC, sz));
	igt_assert(!memchr_inv(cpy, AA, sz));

	igt_fork(child, 1) {
		ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		raise(SIGSTOP);
	}

	/* Wait for the child to ready themselves [SIGSTOP] */
	pid = wait(NULL);

	ptrace(PTRACE_ATTACH, pid, NULL, NULL);
	for (int i = 0; i < sz / sizeof(long); i++) {
		long ret;

		ret = ptrace(PTRACE_PEEKDATA, pid, gtt + i);
		igt_assert_eq_u64(ret, CC);
		cpy[i] = ret;

		ret = ptrace(PTRACE_POKEDATA, pid, gtt + i, AA);
		igt_assert_eq(ret, 0l);
	}
	ptrace(PTRACE_DETACH, pid, NULL, NULL);

	/* Wakeup the child for it to exit */
	kill(SIGCONT, pid);
	igt_waitchildren();

	/* The contents of the two buffers should now be swapped */
	igt_assert(!memchr_inv(gtt, AA, sz));
	igt_assert(!memchr_inv(cpy, CC, sz));

	munmap(gtt, sz);
	free(cpy);
}

static bool is_coherent(int i915)
{
	int val = 1; /* by default, we assume GTT is coherent, hence the test */
	struct drm_i915_getparam gp = {
		gp.param = 52, /* GTT_COHERENT */
		gp.value = &val,
	};

	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return val;
}

static void
test_coherency(int fd)
{
	uint32_t handle;
	uint32_t *gtt, *cpu;
	int i;

	igt_require(is_coherent(fd));
	igt_require(igt_setup_clflush());

	handle = gem_create(fd, OBJECT_SIZE);

	gtt = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_READ | PROT_WRITE);
	cpu = gem_mmap__cpu(fd, handle, 0, OBJECT_SIZE, PROT_READ | PROT_WRITE);
	set_domain_gtt(fd, handle);

	/* On byt/bsw/bxt this detects an interesting behaviour where the
	 * CPU cannot flush the iobar and so the read may bypass the write.
	 * https://bugs.freedesktop.org/show_bug.cgi?id=94314
	 */
	for (i = 0; i < OBJECT_SIZE / 64; i++) {
		int x = 16*i + (i%16);
		gtt[x] = i;
		igt_clflush_range(&cpu[x], sizeof(cpu[x]));
		igt_assert_eq(cpu[x], i);
	}

	munmap(cpu, OBJECT_SIZE);
	munmap(gtt, OBJECT_SIZE);
	gem_close(fd, handle);
}

static void
test_clflush(int fd)
{
	uint32_t handle;
	uint32_t *gtt;

	igt_require(igt_setup_clflush());

	handle = gem_create(fd, OBJECT_SIZE);

	gtt = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_READ | PROT_WRITE);
	set_domain_gtt(fd, handle);

	igt_clflush_range(gtt, OBJECT_SIZE);

	munmap(gtt, OBJECT_SIZE);
	gem_close(fd, handle);
}

static void
test_hang(int fd)
{
	const uint32_t patterns[] = {
		0, 0xaaaaaaaa, 0x55555555, 0xcccccccc,
	};
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct {
		bool done;
		bool error;
	} *control;
	unsigned long count;
	igt_hang_t hang;
	int dir;

	hang = igt_allow_hang(fd, 0, 0);
	igt_require(igt_params_set(fd, "reset", "1")); /* global */

	control = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(control != MAP_FAILED);

	igt_fork(child, ncpus) {
		int last_pattern = 0;
		int next_pattern = 1;
		uint32_t *gtt[2];

		for (int i = 0; i < ARRAY_SIZE(gtt); i++) {
			uint32_t handle;

			handle = gem_create(fd, OBJECT_SIZE);
			gem_set_tiling(fd, handle, I915_TILING_X + i, 2048);

			gtt[i] = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_WRITE);
			set_domain_gtt(fd, handle);
			gem_close(fd, handle);
		}

		while (!READ_ONCE(control->done)) {
			for (int i = 0; i < OBJECT_SIZE / 64; i++) {
				const unsigned int x = 16 * i + ( i% 16);
				uint32_t expected = patterns[last_pattern];
				uint32_t found[2];

				found[0] = READ_ONCE(gtt[0][x]);
				found[1] = READ_ONCE(gtt[1][x]);

				if (found[0] != expected ||
				    found[1] != expected) {
					igt_warn("child[%d] found (%x, %x), expecting %x\n",
						 child,
						 found[0], found[1],
						 expected);
					control->error = true;
					exit(0);
				}

				gtt[0][x] = patterns[next_pattern];
				gtt[1][x] = patterns[next_pattern];

				if (READ_ONCE(control->done))
					break;
			}

			last_pattern = next_pattern;
			next_pattern = (next_pattern + 1) % ARRAY_SIZE(patterns);
		}
	}

	count = 0;
	dir = igt_debugfs_dir(fd);
	igt_until_timeout(5) {
		igt_sysfs_printf(dir, "i915_wedged", "%llu", -1ull);
		if (READ_ONCE(control->error))
			break;
		count++;
	}
	close(dir);
	igt_info("%lu resets\n", count);

	control->done = true;
	igt_waitchildren_timeout(2, NULL);

	igt_assert(!control->error);
	munmap(control, 4096);

	igt_disallow_hang(fd, hang);
}

static void
test_hang_busy(int i915)
{
	uint32_t *ptr, *tile, *x;
	igt_spin_t *spin;
	igt_hang_t hang;
	uint32_t handle;

	hang = igt_allow_hang(i915, 0, 0);
	igt_require(igt_params_set(i915, "reset", "1")); /* global */

	spin = igt_spin_new(i915, .flags = IGT_SPIN_POLL_RUN | IGT_SPIN_FENCE_OUT | IGT_SPIN_NO_PREEMPTION);
	igt_spin_busywait_until_started(spin);
	igt_assert(spin->execbuf.buffer_count == 2);

	handle = gem_create(i915, 2 << 20);
	gem_set_tiling(i915, handle, I915_TILING_X, 512);

	spin->obj[0].handle = handle;
	spin->obj[0].flags = EXEC_OBJECT_WRITE;
	gem_execbuf(i915, &spin->execbuf);

	/* Fault in the busy objects */
	igt_debug("Faulting in busy batch\n");
	ptr = gem_mmap__gtt(i915, spin->handle, 4096, PROT_READ);
	x = ptr + ((uintptr_t)spin->condition & 4095) / sizeof(*ptr);
	igt_assert_eq_u32(READ_ONCE(*x), spin->cmd_precondition);

	igt_debug("Faulting in busy tile\n");
	tile = gem_mmap__gtt(i915, handle, 2 << 20, PROT_WRITE);
	*tile = 0xdeadbeef;

	igt_debug("Resetting GPU\n");
	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_assert(gem_bo_busy(i915, handle));
	igt_force_gpu_reset(i915);

	/* Check we reset the busy mmap */
	igt_debug("Sync\n");
	gem_sync(i915, spin->handle);
	igt_assert_eq(sync_fence_status(spin->out_fence), -5);

	igt_debug("Refault and verify\n");
	igt_assert_eq_u32(READ_ONCE(*tile), 0xdeadbeef);
	gem_close(i915, handle);
	munmap(tile, 2 << 20);

	igt_assert_eq_u32(READ_ONCE(*x), spin->cmd_precondition);
	munmap(ptr, 4096);

	igt_spin_free(i915, spin);
	igt_disallow_hang(i915, hang);
}

static void
test_hang_user(int i915)
{
	uint32_t *ptr, *mem, *x;
	igt_spin_t *spin;
	igt_hang_t hang;
	uint32_t handle;

	hang = igt_allow_hang(i915, 0, 0);
	igt_require(igt_params_set(i915, "reset", "1")); /* global */

	spin = igt_spin_new(i915, .flags = IGT_SPIN_POLL_RUN | IGT_SPIN_FENCE_OUT | IGT_SPIN_NO_PREEMPTION);
	igt_spin_busywait_until_started(spin);
	igt_assert(spin->execbuf.buffer_count == 2);

	igt_assert(posix_memalign((void **)&mem, 4096, 2 << 20) == 0);
	gem_userptr(i915, mem, 2 << 20, false, 0, &handle);

	spin->obj[0].handle = handle;
	spin->obj[0].flags = EXEC_OBJECT_WRITE;
	gem_execbuf(i915, &spin->execbuf);

	/* Fault in the busy objects */
	igt_debug("Faulting in busy batch\n");
	ptr = gem_mmap__gtt(i915, spin->handle, 4096, PROT_READ);
	x = ptr + ((uintptr_t)spin->condition & 4095) / sizeof(*ptr);
	igt_assert_eq_u32(READ_ONCE(*x), spin->cmd_precondition);

	igt_debug("Faulting in busy userptr\n");
	*mem = 0xdeadbeef;

	igt_debug("Resetting GPU\n");
	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_assert(gem_bo_busy(i915, handle));
	igt_force_gpu_reset(i915);

	/* Check we reset the busy mmap */
	igt_debug("Cancelling busy userptr\n");
	munmap(mem, 2 << 20);
	gem_close(i915, handle);

	gem_sync(i915, spin->handle);
	igt_assert_eq(sync_fence_status(spin->out_fence), -5);
	igt_assert_eq_u32(READ_ONCE(*x), spin->cmd_precondition);
	munmap(ptr, 4096);

	igt_spin_free(i915, spin);
	igt_disallow_hang(i915, hang);
}

static int min_tile_width(uint32_t devid, int tiling)
{
	if (tiling < 0) {
		if (intel_gen(devid) >= 4)
			return 4096 - min_tile_width(devid, -tiling);
		else
			return 1024;

	}

	if (intel_gen(devid) == 2)
		return 128;
	else if (tiling == I915_TILING_X)
		return 512;
	else if (IS_915(devid))
		return 512;
	else
		return 128;
}

static int max_tile_width(uint32_t devid, int tiling)
{
	if (tiling < 0) {
		if (intel_gen(devid) >= 4)
			return 4096 + min_tile_width(devid, -tiling);
		else
			return 2048;
	}

	if (intel_gen(devid) >= 7)
		return 256 << 10;
	else if (intel_gen(devid) >= 4)
		return 128 << 10;
	else
		return 8 << 10;
}

static bool known_swizzling(int fd, uint32_t handle)
{
	struct drm_i915_gem_get_tiling2 {
		uint32_t handle;
		uint32_t tiling_mode;
		uint32_t swizzle_mode;
		uint32_t phys_swizzle_mode;
	} arg = {
		.handle = handle,
	};
#define DRM_IOCTL_I915_GEM_GET_TILING2	DRM_IOWR (DRM_COMMAND_BASE + DRM_I915_GEM_GET_TILING, struct drm_i915_gem_get_tiling2)

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING2, &arg))
		return false;

	return arg.phys_swizzle_mode == arg.swizzle_mode;
}

static void
test_huge_bo(int fd, int huge, int tiling)
{
	uint32_t bo;
	char *ptr;
	char *tiled_pattern;
	char *linear_pattern;
	uint64_t size, last_offset;
	uint32_t devid = intel_get_drm_devid(fd);
	int pitch = min_tile_width(devid, tiling);
	int i;

	switch (huge) {
	case -1:
		size = gem_mappable_aperture_size() / 2;

		/* Power of two fence size, natural fence
		 * alignment, and the guard page at the end
		 * gtt means that if the entire gtt is
		 * mappable, we can't usually fit in a tiled
		 * object half the size of the gtt. Let's use
		 * a quarter size one instead.
		 */
		if (tiling &&
		    intel_gen(intel_get_drm_devid(fd)) < 4 &&
		    size >= gem_global_aperture_size(fd) / 2)
			size /= 2;
		break;
	case 0:
		size = gem_mappable_aperture_size() + PAGE_SIZE;
		break;
	default:
		size = gem_global_aperture_size(fd) + PAGE_SIZE;
		break;
	}
	intel_require_memory(1, size, CHECK_RAM);

	last_offset = size - PAGE_SIZE;

	/* Create pattern */
	bo = gem_create(fd, PAGE_SIZE);
	if (tiling)
		igt_require(__gem_set_tiling(fd, bo, tiling, pitch) == 0);
	igt_require(known_swizzling(fd, bo));

	linear_pattern = gem_mmap__gtt(fd, bo, PAGE_SIZE,
				       PROT_READ | PROT_WRITE);
	for (i = 0; i < PAGE_SIZE; i++)
		linear_pattern[i] = i;
	tiled_pattern = gem_mmap__cpu(fd, bo, 0, PAGE_SIZE, PROT_READ);

	gem_set_domain(fd, bo, I915_GEM_DOMAIN_CPU | I915_GEM_DOMAIN_GTT, 0);
	gem_close(fd, bo);

	bo = gem_create(fd, size);
	if (tiling)
		igt_require(__gem_set_tiling(fd, bo, tiling, pitch) == 0);

	/* Initialise first/last page through CPU mmap */
	ptr = gem_mmap__cpu(fd, bo, 0, size, PROT_READ | PROT_WRITE);
	memcpy(ptr, tiled_pattern, PAGE_SIZE);
	memcpy(ptr + last_offset, tiled_pattern, PAGE_SIZE);
	munmap(ptr, size);

	/* Obtain mapping for the object through GTT. */
	ptr = __gem_mmap__gtt(fd, bo, size, PROT_READ | PROT_WRITE);
	igt_require_f(ptr, "Huge BO GTT mapping not supported.\n");

	set_domain_gtt(fd, bo);

	/* Access through GTT should still provide the CPU written values. */
	igt_assert(memcmp(ptr              , linear_pattern, PAGE_SIZE) == 0);
	igt_assert(memcmp(ptr + last_offset, linear_pattern, PAGE_SIZE) == 0);

	gem_set_tiling(fd, bo, I915_TILING_NONE, 0);

	igt_assert(memcmp(ptr              , tiled_pattern, PAGE_SIZE) == 0);
	igt_assert(memcmp(ptr + last_offset, tiled_pattern, PAGE_SIZE) == 0);

	munmap(ptr, size);

	gem_close(fd, bo);
	munmap(tiled_pattern, PAGE_SIZE);
	munmap(linear_pattern, PAGE_SIZE);
}

static void copy_wc_page(void *dst, const void *src)
{
	igt_memcpy_from_wc(dst, src, PAGE_SIZE);
}

static unsigned int tile_row_size(int tiling, unsigned int stride)
{
	if (tiling < 0)
		tiling = -tiling;

	return stride * (tiling == I915_TILING_Y ? 32 : 8);
}

#define rounddown(x, y) (x - (x%y))

static void
test_huge_copy(int fd, int huge, int tiling_a, int tiling_b, int ncpus)
{
	const uint32_t devid = intel_get_drm_devid(fd);
	uint64_t huge_object_size, i;
	unsigned mode = CHECK_RAM;

	switch (huge) {
	case -2:
		huge_object_size = gem_mappable_aperture_size() / 4;
		break;
	case -1:
		huge_object_size = gem_mappable_aperture_size() / 2;
		break;
	case 0:
		huge_object_size = gem_mappable_aperture_size() + PAGE_SIZE;
		break;
	case 1:
		huge_object_size = gem_global_aperture_size(fd) + PAGE_SIZE;
		break;
	default:
		huge_object_size = (intel_get_total_ram_mb() << 19) + PAGE_SIZE;
		mode |= CHECK_SWAP;
		break;
	}
	intel_require_memory(2*ncpus, huge_object_size, mode);

	igt_fork(child, ncpus) {
		uint64_t valid_size = huge_object_size;
		uint32_t bo[2];
		char *a, *b;

		bo[0] = gem_create(fd, huge_object_size);
		if (tiling_a) {
			igt_require(__gem_set_tiling(fd, bo[0], abs(tiling_a), min_tile_width(devid, tiling_a)) == 0);
			valid_size = rounddown(valid_size, tile_row_size(tiling_a, min_tile_width(devid, tiling_a)));
		}
		a = __gem_mmap__gtt(fd, bo[0], huge_object_size, PROT_READ | PROT_WRITE);
		igt_require(a);

		bo[1] = gem_create(fd, huge_object_size);
		if (tiling_b) {
			igt_require(__gem_set_tiling(fd, bo[1], abs(tiling_b), max_tile_width(devid, tiling_b)) == 0);
			valid_size = rounddown(valid_size, tile_row_size(tiling_b, max_tile_width(devid, tiling_b)));
		}
		b = __gem_mmap__gtt(fd, bo[1], huge_object_size, PROT_READ | PROT_WRITE);
		igt_require(b);

		gem_set_domain(fd, bo[0], I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		for (i = 0; i < valid_size / PAGE_SIZE; i++) {
			uint32_t *ptr = (uint32_t *)(a + PAGE_SIZE*i);
			for (int j = 0; j < PAGE_SIZE/4; j++)
				ptr[j] = i + j;
			igt_progress("Writing a ", i, valid_size / PAGE_SIZE);
		}

		gem_set_domain(fd, bo[1], I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		for (i = 0; i < valid_size / PAGE_SIZE; i++) {
			uint32_t *ptr = (uint32_t *)(b + PAGE_SIZE*i);
			for (int j = 0; j < PAGE_SIZE/4; j++)
				ptr[j] = ~(i + j);
			igt_progress("Writing b ", i, valid_size / PAGE_SIZE);
		}

		for (i = 0; i < valid_size / PAGE_SIZE; i++) {
			uint32_t *A = (uint32_t *)(a + PAGE_SIZE*i);
			uint32_t *B = (uint32_t *)(b + PAGE_SIZE*i);
			uint32_t A_tmp[PAGE_SIZE/sizeof(uint32_t)];
			uint32_t B_tmp[PAGE_SIZE/sizeof(uint32_t)];

			copy_wc_page(A_tmp, A);
			copy_wc_page(B_tmp, B);
			for (int j = 0; j < PAGE_SIZE/4; j++)
				if ((i +  j) & 1)
					A_tmp[j] = B_tmp[j];
				else
					B_tmp[j] = A_tmp[j];

			gem_set_domain(fd, bo[0], I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
			memcpy(A, A_tmp, PAGE_SIZE);

			gem_set_domain(fd, bo[1], I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
			memcpy(B, B_tmp, PAGE_SIZE);

			igt_progress("Copying a<->b ", i, valid_size / PAGE_SIZE);
		}

		gem_close(fd, bo[0]);
		gem_close(fd, bo[1]);

		for (i = 0; i < valid_size / PAGE_SIZE; i++) {
			uint32_t page[PAGE_SIZE/sizeof(uint32_t)];
			copy_wc_page(page, a + PAGE_SIZE*i);
			for (int j = 0; j < PAGE_SIZE/sizeof(uint32_t); j++)
				if ((i + j) & 1)
					igt_assert_eq_u32(page[j], ~(i + j));
				else
					igt_assert_eq_u32(page[j], i + j);
			igt_progress("Checking a ", i, valid_size / PAGE_SIZE);
		}
		munmap(a, huge_object_size);

		for (i = 0; i < valid_size / PAGE_SIZE; i++) {
			uint32_t page[PAGE_SIZE/sizeof(uint32_t)];
			copy_wc_page(page, b + PAGE_SIZE*i);
			for (int j = 0; j < PAGE_SIZE/sizeof(uint32_t); j++)
				if ((i + j) & 1)
					igt_assert_eq_u32(page[j], ~(i + j));
				else
					igt_assert_eq_u32(page[j], i + j);
			igt_progress("Checking b ", i, valid_size / PAGE_SIZE);
		}
		munmap(b, huge_object_size);
	}
	igt_waitchildren();
}

static void
test_read(int fd)
{
	void *dst;
	uint32_t src;

	/* copy from a fresh src to fresh dst to force pagefault on both */
	dst = create_pointer(fd);
	src = gem_create(fd, OBJECT_SIZE);

	gem_read(fd, src, 0, dst, OBJECT_SIZE);

	gem_close(fd, src);
	munmap(dst, OBJECT_SIZE);
}

static void
test_write_cpu_read_gtt(int fd)
{
	uint32_t handle;
	uint32_t *src, *dst;

	igt_require(gem_has_llc(fd));

	handle = gem_create(fd, OBJECT_SIZE);

	dst = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_READ);

	src = gem_mmap__cpu(fd, handle, 0, OBJECT_SIZE, PROT_WRITE);

	gem_close(fd, handle);

	memset(src, 0xaa, OBJECT_SIZE);
	igt_assert(memcmp(dst, src, OBJECT_SIZE) == 0);

	munmap(src, OBJECT_SIZE);
	munmap(dst, OBJECT_SIZE);
}

struct thread_fault_concurrent {
	pthread_t thread;
	int id;
	int *ctl;
	uint32_t **ptr;
};

static void *
thread_fault_concurrent(void *closure)
{
	struct thread_fault_concurrent *t = closure;
	uint32_t val = 0;
	int n;

	for (n = 0; READ_ONCE(*t->ctl); n++) {
		unsigned int id = (n + t->id) % 32;
		uint32_t sz = *t->ptr[id] - 1;
		int idx = rand() % sz + 1;

		igt_assert(t->ptr[id][0]);
		igt_assert(idx); /* avoid overwriting the size */

		if (n & 1)
			t->ptr[id][idx] = val;
		else
			val = t->ptr[id][idx];

		val++;
	}

	return NULL;
}

static void
test_fault_concurrent(int fd, int tiling)
{
	uint32_t *ptr[32];
	struct thread_fault_concurrent thread[64];
	int *ctl;
	int n;

	if (tiling != I915_TILING_NONE)
		igt_require(gem_available_fences(fd) > 0);

	ctl = create_pointer_size(fd, 4096, I915_TILING_NONE);
	*ctl = 1;

	for (n = 0; n < 32; n++) {
		uint32_t sz = (n + 1) << 19; /* 512KiB increments */

		ptr[n] = create_pointer_size(fd, sz, tiling);
		*ptr[n] = sz / sizeof(uint32_t); /* num_elems for convenience */
	}

	__sync_synchronize();
	for (n = 0; n < 64; n++) {
		thread[n].ptr = ptr;
		thread[n].id = n;
		thread[n].ctl = ctl;
		pthread_create(&thread[n].thread, NULL,
				thread_fault_concurrent, &thread[n]);
	}

	sleep(2);
	*ctl = 0;

	__sync_synchronize();
	for (n = 0; n < 64; n++)
		pthread_join(thread[n].thread, NULL);

	for (n = 0; n < 32; n++)
		munmap(ptr[n], *ptr[n] * sizeof(uint32_t));
	munmap(ctl, 4096);
}

static int mmap_ioctl(int i915, struct drm_i915_gem_mmap_gtt *arg)
{
	int err = 0;

	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_MMAP_GTT, arg))
		err = -errno;

	errno = 0;
	return err;
}

int fd;

igt_main
{
	if (igt_run_in_simulation())
		OBJECT_SIZE = 1 * 1024 * 1024;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);

		/*
		 * If the HW lacks or does not expose an aperture for indirect
		 * detiling access from untrusted userspace to the objects,
		 * the kernel does an early rejection of the mmap_gtt ioctl.
		 */
		gem_require_mappable_ggtt(fd);
	}

	igt_subtest("bad-object") {
		uint32_t real_handle = gem_create(fd, 4096);
		uint32_t handles[20];
		size_t i = 0, len;

		handles[i++] = 0xdeadbeef;
		for(int bit = 0; bit < 16; bit++)
			handles[i++] = real_handle | (1 << (bit + 16));
		handles[i++] = real_handle + 1;
		len = i;

		for (i = 0; i < len; ++i) {
			struct drm_i915_gem_mmap_gtt arg = {
				.handle = handles[i],
			};
			igt_assert_eq(mmap_ioctl(fd, &arg), -ENOENT);
		}

		gem_close(fd, real_handle);
	}

	igt_subtest("basic")
		test_access(fd);
	igt_subtest("basic-short")
		test_short(fd);
	igt_subtest("basic-copy")
		test_copy(fd);
	igt_subtest("basic-read")
		test_read(fd);
	igt_subtest("basic-write")
		test_write(fd);
	igt_subtest("basic-write-gtt")
		test_write_gtt(fd);
	igt_subtest("ptrace")
		test_ptrace(fd);
	igt_subtest("coherency")
		test_coherency(fd);
	igt_subtest("clflush")
		test_clflush(fd);
	igt_subtest("hang")
		test_hang(fd);
	igt_subtest("hang-busy")
		test_hang_busy(fd);
	igt_subtest("hang-user")
		test_hang_user(fd);
	igt_subtest("basic-read-write")
		test_read_write(fd, READ_BEFORE_WRITE);
	igt_subtest("basic-write-read")
		test_read_write(fd, READ_AFTER_WRITE);
	igt_subtest("basic-read-write-distinct")
		test_read_write2(fd, READ_BEFORE_WRITE);
	igt_subtest("basic-write-read-distinct")
		test_read_write2(fd, READ_AFTER_WRITE);
	igt_subtest("fault-concurrent")
		test_fault_concurrent(fd, I915_TILING_NONE);
	igt_subtest("fault-concurrent-X")
		test_fault_concurrent(fd, I915_TILING_X);
	igt_subtest("fault-concurrent-Y")
		test_fault_concurrent(fd, I915_TILING_Y);
	igt_subtest("basic-write-cpu-read-gtt")
		test_write_cpu_read_gtt(fd);
	igt_subtest("basic-wc")
		test_wc(fd);
	igt_subtest("isolation")
		test_isolation(fd);
	igt_subtest("zero-extend")
		test_zero_extend(fd);
	igt_subtest("close-race")
		test_close_race(fd);
	igt_subtest("flink-race")
		test_flink_race(fd);
	igt_subtest("pf-nonblock")
		test_pf_nonblock(fd);

	igt_subtest("basic-small-bo")
		test_huge_bo(fd, -1, I915_TILING_NONE);
	igt_subtest("basic-small-bo-tiledX")
		test_huge_bo(fd, -1, I915_TILING_X);
	igt_subtest("basic-small-bo-tiledY")
		test_huge_bo(fd, -1, I915_TILING_Y);

	igt_subtest("big-bo")
		test_huge_bo(fd, 0, I915_TILING_NONE);
	igt_subtest("big-bo-tiledX")
		test_huge_bo(fd, 0, I915_TILING_X);
	igt_subtest("big-bo-tiledY")
		test_huge_bo(fd, 0, I915_TILING_Y);

	igt_subtest("huge-bo")
		test_huge_bo(fd, 1, I915_TILING_NONE);
	igt_subtest("huge-bo-tiledX")
		test_huge_bo(fd, 1, I915_TILING_X);
	igt_subtest("huge-bo-tiledY")
		test_huge_bo(fd, 1, I915_TILING_Y);

	igt_subtest_group {
		const struct copy_size {
			const char *prefix;
			int size;
		} copy_sizes[] = {
			{ "basic-small", -2 },
			{ "medium", -1 },
			{ "big", 0 },
			{ "huge", 1 },
			{ "swap", 2 },
			{ }
		};
		const struct copy_mode {
			const char *suffix;
			int tiling_x, tiling_y;
		} copy_modes[] = {
			{ "", I915_TILING_NONE, I915_TILING_NONE},
			{ "-XY", I915_TILING_X, I915_TILING_Y},
			{ "-odd", -I915_TILING_X, -I915_TILING_Y},
			{}
		};
		const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

		for (const struct copy_size *s = copy_sizes; s->prefix; s++)
			for (const struct copy_mode *m = copy_modes; m->suffix; m++) {
				igt_subtest_f("%s-copy%s", s->prefix, m->suffix)
					test_huge_copy(fd,
						       s->size,
						       m->tiling_x,
						       m->tiling_y,
						       1);

				igt_subtest_f("cpuset-%s-copy%s", s->prefix, m->suffix) {
					cpu_set_t cpu, old;

					sched_getaffinity(0, sizeof(old), &old);

					CPU_ZERO(&cpu);
					CPU_SET(0, &cpu);
					igt_assert(sched_setaffinity(0, sizeof(cpu), &cpu) == 0);
					test_huge_copy(fd,
						       s->size,
						       m->tiling_x,
						       m->tiling_y,
						       2);

					igt_assert(sched_setaffinity(0, sizeof(old), &old) == 0);
				}

				igt_subtest_f("forked-%s-copy%s", s->prefix, m->suffix)
					test_huge_copy(fd,
						       s->size,
						       m->tiling_x,
						       m->tiling_y,
						       ncpus);
			}
	}


	igt_fixture
		close(fd);
}
