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

static void many(int fd, int timeout)
{
	struct drm_i915_gem_exec_object2 *execobj;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint64_t gtt_size, ram_size;
	uint64_t max_alignment;
	unsigned long count, i;

	gtt_size = gem_aperture_size(fd);
	if (!gem_uses_full_ppgtt(fd))
		gtt_size /= 2; /* We have to *share* our GTT! */

	ram_size = intel_get_total_ram_mb();
	ram_size *= 1024 * 1024;

	count = ram_size / 4096;
	if (count > file_max()) /* vfs cap */
		count = file_max();

	max_alignment = find_last_bit(gtt_size / count);
	if (max_alignment <= 13)
		max_alignment = 4096;
	else
		max_alignment = 1ull << (max_alignment - 1);
	count = gtt_size / max_alignment / 2;

	igt_info("gtt_size=%lld MiB, max-alignment=%lld, count=%lu\n",
		 (long long)gtt_size/1024/1024,
		 (long long)max_alignment,
		 count);
	intel_require_memory(count, 4096, CHECK_RAM);

	execobj = calloc(sizeof(*execobj), count);
	igt_assert(execobj);

	for (i = 0; i < count; i++) {
		execobj[i].handle = batch_create(fd, 4096);
		if ((gtt_size - 1) >> 32)
			execobj[i].flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(execobj);
	execbuf.buffer_count = count;
	igt_require(__gem_execbuf(fd, &execbuf) == 0);

	set_timeout((uint64_t)timeout * NSEC_PER_SEC);
	for (uint64_t alignment = 8192;
	     alignment < gtt_size && !READ_ONCE(timed_out);
	     alignment <<= 1) {
		unsigned long max;

		max = count;
		if (alignment > max_alignment)
			max = max * max_alignment / alignment;
		igt_debug("Testing alignment:%" PRIx64", max_count:%lu\n",
			  alignment, max);

		for (i = 0; i < max; i++)
			execobj[i].alignment = alignment;

		for (i = 2; i < max; i <<= 1) {
			struct timespec tv = {};
			int err;

			execbuf.buffer_count = i;

			igt_nsec_elapsed(&tv);
			err = __execbuf(fd, &execbuf);
			igt_debug("Testing %lu x alignment=%#llx [%db], took %'"PRIu64"ns\n",
				  i,
				  (long long)alignment,
				  find_last_bit(alignment) - 1,
				  igt_nsec_elapsed(&tv));
			if (READ_ONCE(timed_out))
				break;
			igt_assert_eq(err, 0);

			for (unsigned long j = 0; j < i; j++) {
				igt_assert_eq_u64(execobj[j].alignment, alignment);
				igt_assert_eq_u64(execobj[j].offset % alignment, 0);
			}
		}
	}
	reset_timeout();

	for (i = 0; i < count; i++)
		gem_close(fd, execobj[i].handle);
	free(execobj);
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
}
