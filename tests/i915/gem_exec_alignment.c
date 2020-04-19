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
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

/* Exercises the basic execbuffer using object alignments */

#include "igt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sched.h>

#include "drm.h"

IGT_TEST_DESCRIPTION("Exercises the basic execbuffer using object alignments");

static uint32_t find_last_bit(uint64_t x)
{
	uint32_t i = 0;
	while (x) {
		x >>= 1;
		i++;
	}
	return i;
}

static uint32_t file_max(void)
{
	static uint32_t max;
	if (max == 0) {
		FILE *file = fopen("/proc/sys/fs/file-max", "r");
		max = 80000;
		if (file) {
			igt_assert(fscanf(file, "%d", &max) == 1);
			fclose(file);
		}
		max /= 2;
	}
	return max;
}

static bool timed_out;

static void alarm_handler(int signal)
{
	timed_out = true;
}

static void set_timeout(uint64_t timeout_ns)
{
	struct sigaction sa = {
		.sa_handler = alarm_handler,
	};
	struct itimerval itv = {
		.it_value.tv_sec = timeout_ns / NSEC_PER_SEC,
		.it_value.tv_usec = timeout_ns % NSEC_PER_SEC / 1000,
	};

	timed_out = false;
	sigaction(SIGALRM, &sa, NULL);
	setitimer(ITIMER_REAL, &itv, NULL);
}

static void reset_timeout(void)
{
	struct itimerval itv = {};

	sigaction(SIGALRM, NULL, NULL);
	setitimer(ITIMER_REAL, &itv, NULL);
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;

	if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	return err;
}

static uint32_t batch_create(int i915, unsigned long sz)
{
	uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, sz);
	gem_write(i915, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static unsigned long create_batch(int i915,
				  struct drm_i915_gem_exec_object2 *obj,
				  unsigned long from, unsigned long to,
				  unsigned int flags)
{
	for (unsigned long i = from; i < to; i++) {
		obj[i].handle = batch_create(i915, 4096);
		obj[i].flags = flags;
	}

	return to;
}

static void sighandler(int sig)
{
}

static void
naughty_child(int i915, int link, uint32_t shared, unsigned int flags)
#define SHARED 0x1
#define ISOLATED 0x2
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 *obj;
	uint64_t gtt_size, ram_size, count;
	struct sigaction act = {
		.sa_handler = sighandler,
	};
	struct timespec tv = {};
	int err;

	if (flags & ISOLATED)
		i915 = gem_reopen_driver(i915);

	if (!(flags & SHARED))
		shared = 0;

	gtt_size = gem_aperture_size(i915);
	if (!gem_uses_full_ppgtt(i915))
		gtt_size /= 2; /* We have to *share* our GTT! */

	ram_size = min(intel_get_total_ram_mb(), 4096);
	ram_size *= 1024 * 1024;

	count = min(gtt_size, ram_size) / 16384;
	if (count > file_max()) /* vfs cap */
		count = file_max();
	intel_require_memory(count, 4096, CHECK_RAM);

	flags = 0;
	if ((gtt_size - 1) >> 32)
		flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	/* Fill the low-priority address space */
	obj = calloc(sizeof(*obj), count);
	igt_assert(obj);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = create_batch(i915, obj, 0, 1, flags);
	execbuf.rsvd1 = gem_context_create(i915);
	gem_execbuf(i915, &execbuf);

	igt_seconds_elapsed(memset(&tv, 0, sizeof(tv)));
	for (unsigned long i = 1; i < count; i *= 2) {
		execbuf.buffer_count =
			create_batch(i915, obj, execbuf.buffer_count, i, flags);
		gem_execbuf(i915, &execbuf);
		if (igt_seconds_elapsed(&tv) > 8) {
			count = i;
			break;
		}
	}
	if (shared) {
		gem_close(i915, obj[0].handle);
		obj[0].handle = shared;
	}
	execbuf.buffer_count =
		create_batch(i915, obj, execbuf.buffer_count, count, flags);
	gem_execbuf(i915, &execbuf);
	igt_debug("Created %lu buffers ready for delay\n", count);

	/* Calibrate a long execbuf() */
	memset(&tv, 0, sizeof(tv));
	for (unsigned long i = 0; i < count; i++)
		obj[i].alignment = 8192;

	execbuf.buffer_count = 2;
	while (igt_seconds_elapsed(&tv) < 4) {
		gem_execbuf(i915, &execbuf);
		execbuf.buffer_count <<= 1;
		if (execbuf.buffer_count > count) {
			execbuf.buffer_count = count;
			break;
		}
	}
	igt_debug("Using %u buffers to delay execbuf\n", execbuf.buffer_count);

	for (unsigned long i = 0; i < count; i++)
		obj[i].alignment = 16384;

	write(link, &tv, sizeof(tv));

	sigaction(SIGINT, &act, NULL);
	igt_debug("Executing naughty execbuf\n");
	igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));
	err = __execbuf(i915, &execbuf); /* this should take over 2s */
	igt_info("Naughty client took %'"PRIu64"ns, result %d\n",
		 igt_nsec_elapsed(&tv), err);
	igt_assert(igt_nsec_elapsed(&tv) > NSEC_PER_SEC / 2 || err == -EINTR);

	gem_context_destroy(i915, execbuf.rsvd1);
	for (unsigned long i = !!shared; i < count; i++)
		gem_close(i915, obj[i].handle);
	free(obj);
}

static void kill_children(int sig)
{
	signal(sig, SIG_IGN);
	kill(-getpgrp(), SIGINT);
	signal(sig, SIG_DFL);
}

static void prio_inversion(int i915, unsigned int flags)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915, 4095)
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	struct timespec tv;
	uint64_t elapsed;
	int link[2];

	/*
	 * First low priority client create mass of holes in their
	 * own address space, then launch a batch with oodles of object with
	 * alignment that doesn't match previous one. While lp execbufer
	 * is performing we want to start high priority task
	 * and we expect it will not be blocked.
	 */

	igt_require(gem_uses_full_ppgtt(i915));
	igt_assert(pipe(link) == 0);

	/* Prime our prestine context */
	gem_execbuf(i915, &execbuf);

	igt_fork(child, 1)
		naughty_child(i915, link[1], obj.handle, flags);

	igt_debug("Waiting for naughty client\n");
	read(link[0], &tv, sizeof(tv));
	igt_debug("Ready...\n");
	usleep(250 * 1000); /* let the naughty execbuf begin */
	igt_debug("Go!\n");

	igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));
	gem_execbuf(i915, &execbuf);
	elapsed = igt_nsec_elapsed(&tv);
	igt_info("Normal client took %'"PRIu64"ns\n", elapsed);

	kill_children(SIGINT);
	igt_waitchildren();
	gem_close(i915, obj.handle);

	igt_assert(elapsed < NSEC_PER_SEC / 2);
	close(link[0]);
	close(link[1]);
}

static void __many(int fd, int timeout,
		   struct drm_i915_gem_exec_object2 *execobj,
		   unsigned long count)
{
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(execobj),
		.buffer_count = count,
	};

	set_timeout((uint64_t)timeout * NSEC_PER_SEC);
	for (uint64_t align = 8192; !READ_ONCE(timed_out); align <<= 1) {
		unsigned long i, j;

		for (i = 0; i < count; i++)
			execobj[i].alignment = align;

		for (i = 2; i < count; i <<= 1) {
			struct timespec tv = {};
			int err;

			execbuf.buffer_count = i;

			igt_nsec_elapsed(&tv);
			err = __execbuf(fd, &execbuf);
			igt_debug("Testing %lu x alignment=%#llx [%db], took %'"PRIu64"ns\n",
				  i,
				  (long long)align,
				  find_last_bit(align) - 1,
				  igt_nsec_elapsed(&tv));
			if (READ_ONCE(timed_out))
				break;
			igt_assert_eq(err, 0);

			for (j = 0; j < i; j++) {
				igt_assert_eq_u64(execobj[j].alignment, align);
				igt_assert_eq_u64(execobj[j].offset % align, 0);
			}
		}

		count >>= 1;
		if (!count)
			break;
	}
	reset_timeout();
}

static struct drm_i915_gem_exec_object2 *
setup_many(int i915, unsigned long *out)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 *obj;
	uint64_t gtt_size, ram_size;
	struct timespec tv = {};
	unsigned long count;
	unsigned int flags;

	gtt_size = gem_aperture_size(i915);
	if (!gem_uses_full_ppgtt(i915))
		gtt_size /= 2; /* We have to *share* our GTT! */

	ram_size = min(intel_get_total_ram_mb(), 4096);
	ram_size *= 1024 * 1024;

	count = min(gtt_size, ram_size) / 16384;
	if (count > file_max()) /* vfs cap */
		count = file_max();
	intel_require_memory(count, 4096, CHECK_RAM);

	obj = calloc(sizeof(*obj), count);
	igt_assert(obj);

	flags = 0;
	if ((gtt_size - 1) >> 32)
		flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	/* Instantiating all the objects may take awhile, so limit to 20s */
	igt_seconds_elapsed(&tv);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count =
		create_batch(i915, obj, execbuf.buffer_count, 1, flags);
	igt_require(__gem_execbuf(i915, &execbuf) == 0);

	for (unsigned long i = 2; i < count; i *= 2) {
		execbuf.buffer_count =
			create_batch(i915, obj, execbuf.buffer_count, i, flags);
		gem_execbuf(i915, &execbuf);
		if (igt_seconds_elapsed(&tv) > 10) { /* NB doubling each time */
			count = i;
			break;
		}
	}

	execbuf.buffer_count =
		create_batch(i915, obj, execbuf.buffer_count, count, flags);
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj[0].handle);

	igt_info("Setup %'lu 4KiB objects in %.1fms\n",
		 count, igt_nsec_elapsed(&tv) * 1e-6);

	*out = count;
	return obj;
}

static void cleanup_many(int i915,
			 struct drm_i915_gem_exec_object2 *obj,
			 unsigned long count)
{
	for (unsigned long i = 0; i < count; i++)
		gem_close(i915, obj[i].handle);
	free(obj);
}

static void many(int fd, int timeout)
{
	struct drm_i915_gem_exec_object2 *obj;
	unsigned long count;

	obj = setup_many(fd, &count);

	__many(fd, timeout, obj, count);

	cleanup_many(fd, obj, count);
}

static void forked(int i915, int timeout)
{
	struct drm_i915_gem_exec_object2 *obj;
	unsigned long count;

	i915 = gem_reopen_driver(i915);
	igt_require(gem_uses_full_ppgtt(i915));

	obj = setup_many(i915, &count);
	for (unsigned long i = 0; i < count; i++)
		obj[i].handle = gem_flink(i915, obj[i].handle);

	igt_fork(child, sysconf(_SC_NPROCESSORS_ONLN)) {
		i915 = gem_reopen_driver(i915);
		for (unsigned long i = 0; i < count; i++)
			obj[i].handle = gem_open(i915, obj[i].handle);
		__many(i915, timeout, obj, count);
	}
	igt_waitchildren_timeout(3 * timeout, NULL);

	free(obj);
	close(i915);
}

static void single(int fd)
{
	struct drm_i915_gem_exec_object2 execobj;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch = MI_BATCH_BUFFER_END;
	uint64_t gtt_size;
	int non_pot;

	memset(&execobj, 0, sizeof(execobj));
	execobj.handle = gem_create(fd, 4096);
	execobj.flags = 1<<3; /* EXEC_OBJECT_SUPPORTS_48B_ADDRESS */
	gem_write(fd, execobj.handle, 0, &batch, sizeof(batch));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&execobj);
	execbuf.buffer_count = 1;

	gtt_size = gem_aperture_size(fd);
	if (__gem_execbuf(fd, &execbuf)) {
		execobj.flags = 0;
		gtt_size = 1ull << 32;
		gem_execbuf(fd, &execbuf);
	}

	execobj.alignment = 3*4096;
	non_pot = __gem_execbuf(fd, &execbuf) == 0;
	igt_debug("execbuffer() accepts non-power-of-two alignment? %s\n",
		  non_pot ? "yes" : "no");

	for (execobj.alignment = 4096;
	     execobj.alignment <= 64<<20;
	     execobj.alignment += 4096) {
		if (!non_pot && execobj.alignment & -execobj.alignment)
			continue;

		igt_debug("starting offset: %#llx, next alignment: %#llx\n",
			  (long long)execobj.offset,
			  (long long)execobj.alignment);
		gem_execbuf(fd, &execbuf);
		igt_assert_eq_u64(execobj.offset % execobj.alignment, 0);
	}

	for (execobj.alignment = 4096;
	     execobj.alignment < gtt_size;
	     execobj.alignment <<= 1) {
		igt_debug("starting offset: %#llx, next alignment: %#llx [%db]\n",
			  (long long)execobj.offset,
			  (long long)execobj.alignment,
			  find_last_bit(execobj.alignment)-1);
		gem_execbuf(fd, &execbuf);
		igt_assert_eq_u64(execobj.offset % execobj.alignment, 0);
	}

	gem_close(fd, execobj.handle);
}

igt_main
{
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
	}

	igt_subtest("single") /* basic! */
		single(fd);
	igt_subtest("many")
		many(fd, 20);
	igt_subtest("forked")
		forked(fd, 20);
	igt_subtest("pi")
		prio_inversion(fd, 0);
	igt_subtest("pi-shared")
		prio_inversion(fd, SHARED);
	igt_subtest("pi-isolated")
		prio_inversion(fd, ISOLATED);
}
