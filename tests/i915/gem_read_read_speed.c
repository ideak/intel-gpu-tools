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
 */

/** @file gem_read_read_speed.c
 *
 * This is a test of performance with multiple readers from the same source.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <drm.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_sysfs.h"

IGT_TEST_DESCRIPTION("Test speed of concurrent reads between engines.");

#define BBSIZE 4096
igt_render_copyfunc_t rendercopy;
int width, height;

static void set_to_gtt_domain(struct intel_buf *buf, int writing)
{
	int i915 = buf_ops_get_fd(buf->bops);

	gem_set_domain(i915, buf->handle, I915_GEM_DOMAIN_GTT,
		       writing ? I915_GEM_DOMAIN_GTT : 0);
}

static struct intel_bb *rcs_copy_bo(struct intel_buf *dst,
				    struct intel_buf *src)
{
	int i915 = buf_ops_get_fd(dst->bops);
	struct intel_bb *ibb = intel_bb_create(i915, BBSIZE);

	/* enforce batch won't be recreated after execution */
	intel_bb_ref(ibb);

	rendercopy(ibb,
		   src, 0, 0,
		   width, height,
		   dst, 0, 0);

	return ibb;
}

static struct intel_bb *bcs_copy_bo(struct intel_buf *dst,
				    struct intel_buf *src)
{
	int i915 = buf_ops_get_fd(dst->bops);
	struct intel_bb *ibb = intel_bb_create(i915, BBSIZE);

	intel_bb_ref(ibb);

	intel_bb_blt_copy(ibb,
			  src, 0, 0, 4*width,
			  dst, 0, 0, 4*width,
			  width, height, 32);

	return ibb;
}

static void set_bo(struct intel_buf *buf, uint32_t val)
{
	int size = width * height;
	uint32_t *vaddr;

	vaddr = intel_buf_device_map(buf, true);
	while (size--)
		*vaddr++ = val;
	intel_buf_unmap(buf);
}

static double elapsed(const struct timespec *start,
		      const struct timespec *end,
		      int loop)
{
	return (1e6*(end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec)/1000)/loop;
}

static struct intel_buf *create_bo(struct buf_ops *bops, const char *name)
{
	uint32_t tiling_mode = I915_TILING_X;
	struct intel_buf *buf;

	buf = intel_buf_create(bops, width, height, 32, 0, tiling_mode,
			       I915_COMPRESSION_NONE);
	intel_buf_set_name(buf, name);

	return buf;
}

static void run(struct buf_ops *bops, int _width, int _height,
		bool write_bcs, bool write_rcs)
{
	struct intel_buf *src = NULL, *bcs = NULL, *rcs = NULL;
	struct intel_bb *bcs_ibb = NULL, *rcs_ibb = NULL;
	struct timespec start, end;
	int loops = 1;

	width = _width;
	height = _height;

	igt_info("width: %d, height: %d\n", width, height);

	src = create_bo(bops, "src");
	bcs = create_bo(bops, "bcs");
	rcs = create_bo(bops, "rcs");

	set_bo(src, 0xdeadbeef);

	if (write_bcs) {
		bcs_ibb = bcs_copy_bo(src, bcs);
	} else {
		bcs_ibb = bcs_copy_bo(bcs, src);
	}
	if (write_rcs) {
		rcs_ibb = rcs_copy_bo(src, rcs);
	} else {
		rcs_ibb = rcs_copy_bo(rcs, src);
	}

	set_to_gtt_domain(src, true);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < loops; i++) {
		intel_bb_exec(rcs_ibb, intel_bb_offset(rcs_ibb),
			      I915_EXEC_RENDER, false);
		intel_bb_exec(bcs_ibb, intel_bb_offset(bcs_ibb),
			      I915_EXEC_BLT, false);
	}

	set_to_gtt_domain(src, true);
	clock_gettime(CLOCK_MONOTONIC, &end);

	igt_info("Time to %s-%s %dx%d [%dk]:		%7.3fµs\n",
		 write_bcs ? "write" : "read",
		 write_rcs ? "write" : "read",
		 width, height, 4*width*height/1024,
		 elapsed(&start, &end, loops));

	intel_bb_unref(rcs_ibb);
	intel_bb_destroy(rcs_ibb);
	intel_bb_unref(bcs_ibb);
	intel_bb_destroy(bcs_ibb);
	intel_buf_destroy(src);
	intel_buf_destroy(rcs);
	intel_buf_destroy(bcs);
}

igt_main
{
	const int sizes[] = {128, 256, 512, 1024, 2048, 4096, 8192, 0};
	struct buf_ops *bops = NULL;
	int fd, i;

	igt_fixture {
		int devid;

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		devid = intel_get_drm_devid(fd);
		igt_require(intel_gen(devid) >= 6);

		rendercopy = igt_get_render_copyfunc(devid);
		igt_require(rendercopy);

		bops = buf_ops_create(fd);

		gem_submission_print_method(fd);
	}

	for (i = 0; sizes[i] != 0; i++) {
		igt_subtest_f("read-read-%dx%d", sizes[i], sizes[i])
			run(bops, sizes[i], sizes[i], false, false);
		igt_subtest_f("read-write-%dx%d", sizes[i], sizes[i])
			run(bops, sizes[i], sizes[i], false, true);
		igt_subtest_f("write-read-%dx%d", sizes[i], sizes[i])
			run(bops, sizes[i], sizes[i], true, false);
		igt_subtest_f("write-write-%dx%d", sizes[i], sizes[i])
			run(bops, sizes[i], sizes[i], true, true);
	}

	igt_fixture {
		buf_ops_destroy(bops);
		close(fd);
	}
}
