/*
 * Copyright © 2009 Intel Corporation
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
 *    Eric Anholt <eric@anholt.net>
 *
 */

/** @file gem_tiled_blits.c
 *
 * This is a test of doing many tiled blits, with a working set
 * larger than the aperture size.
 *
 * The goal is to catch a couple types of failure;
 * - Fence management problems on pre-965.
 * - A17 or L-shaped memory tiling workaround problems in acceleration.
 *
 * The model is to fill a collection of 1MB objects in a way that can't trip
 * over A6 swizzling -- upload data to a non-tiled object, blit to the tiled
 * object.  Then, copy the 1MB objects randomly between each other for a while.
 * Finally, download their data through linear objects again and see what
 * resulted.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <drm.h>

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Test doing many tiled blits, with a working set larger"
		     " than the aperture size.");

static int width = 512, height = 512;

static void
copy_buf(struct intel_bb *ibb, struct intel_buf *src, struct intel_buf *dst)
{
	intel_bb_blt_copy(ibb,
			  src, 0, 0, src->surface[0].stride,
			  dst, 0, 0, dst->surface[0].stride,
			  width, height, 32);
}

static struct intel_buf *
create_bo(struct buf_ops *bops, struct intel_bb *ibb, uint32_t x)
{
	struct intel_buf *buf, *linear_buf;
	uint32_t *linear;
	int i;

	buf = intel_buf_create(bops, width, height, 32, 0,
			       I915_TILING_X, I915_COMPRESSION_NONE);

	linear_buf = intel_buf_create(bops, width, height, 32, 0,
				      I915_TILING_NONE, I915_COMPRESSION_NONE);

	/* Fill the BO with dwords starting at start_val */
	linear = intel_buf_cpu_map(linear_buf, 1);
	for (i = 0; i < width * height; i++)
		linear[i] = x++;
	intel_buf_unmap(linear_buf);

	copy_buf(ibb, linear_buf, buf);

	intel_buf_destroy(linear_buf);

	return buf;
}

static void
check_bo(struct intel_buf *buf, uint32_t val, struct intel_bb *ibb)
{
	struct intel_buf *linear_buf;
	uint32_t *linear;
	int num_errors;
	int i;

	linear_buf = intel_buf_create(buf->bops, width, height, 32, 0,
				      I915_TILING_NONE, I915_COMPRESSION_NONE);

	copy_buf(ibb, buf, linear_buf);

	linear = intel_buf_cpu_map(linear_buf, 0);
	num_errors = 0;
	for (i = 0; i < width * height; i++) {
		if (linear[i] != val && num_errors++ < 32)
			igt_warn("[%08x] Expected 0x%08x, found 0x%08x (difference 0x%08x)\n",
				 i * 4, val, linear[i], val ^ linear[i]);
		val++;
	}
	igt_assert_eq(num_errors, 0);
	intel_buf_unmap(linear_buf);

	intel_buf_destroy(linear_buf);
}

static void run_test(int fd, int count)
{
	struct intel_bb *ibb;
	struct buf_ops *bops;
	struct intel_buf **bo;
	uint32_t *bo_start_val;
	uint32_t start = 0;
	int i;

	bops = buf_ops_create(fd);
	ibb = intel_bb_create(fd, 4096);

	bo = malloc(sizeof(struct intel_buf *)*count);
	bo_start_val = malloc(sizeof(uint32_t)*count);

	for (i = 0; i < count; i++) {
		bo[i] = create_bo(bops, ibb, start);
		bo_start_val[i] = start;
		start += 1024 * 1024 / 4;
	}

	for (i = 0; i < count + 1; i++) {
		int src = random() % count;
		int dst = random() % count;

		if (src == dst)
			continue;

		copy_buf(ibb, bo[src], bo[dst]);

		bo_start_val[dst] = bo_start_val[src];
	}

	for (i = 0; i < count; i++) {
		check_bo(bo[i], bo_start_val[i], ibb);
		intel_buf_destroy(bo[i]);
	}

	free(bo_start_val);
	free(bo);

	intel_bb_destroy(ibb);
	buf_ops_destroy(bops);
}

#define MAX_32b ((1ull << 32) - 4096)

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint64_t count = 0;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		gem_require_mappable_ggtt(fd);

		count = gem_aperture_size(fd);
		if (count >> 32)
			count = MAX_32b;
		count = 3 + count / (1024 * 1024);
		igt_require(count > 1);
		intel_require_memory(count, 1024 * 1024 , CHECK_RAM);

		igt_debug("Using %'"PRIu64" 1MiB buffers\n", count);
		count = (count + ncpus - 1) / ncpus;
	}

	igt_subtest("basic")
		run_test(fd, 2);

	igt_subtest("normal") {
		igt_fork(child, ncpus)
			run_test(fd, count);
		igt_waitchildren();
	}

	igt_subtest("interruptible") {
		igt_fork_signal_helper();
		igt_fork(child, ncpus)
			run_test(fd, count);
		igt_waitchildren();
		igt_stop_signal_helper();
	}

	igt_fixture {
		close(fd);
	}
}
