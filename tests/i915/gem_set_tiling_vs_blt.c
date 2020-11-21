/*
 * Copyright © 2012 Intel Corporation
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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

/** @file gem_set_tiling_vs_blt.c
 *
 * Testcase: Check for proper synchronization of tiling changes vs. tiled gpu
 * access
 *
 * The blitter on gen3 and earlier needs properly set up fences. Which also
 * means that for untiled blits we may not set up a fence before that blt has
 * finished.
 *
 * Current kernels have a bug there, but it's pretty hard to hit because you
 * need:
 * - a blt on an untiled object which is aligned correctly for tiling.
 * - a set_tiling to switch that object to tiling
 * - another blt without any intervening cpu access that uses this object.
 *
 * Testcase has been extended to also check tiled->untiled and tiled->tiled
 * transitions (i.e. changing stride).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdbool.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Check for proper synchronization of tiling changes vs."
		     " tiled gpu access.");

#define TEST_SIZE (1024*1024)
#define TEST_STRIDE (4*1024)
#define TEST_HEIGHT(stride)	(TEST_SIZE/(stride))
#define TEST_WIDTH(stride)	((stride)/4)

uint32_t data[TEST_SIZE/4];

static void __set_tiling(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t tiling, unsigned stride)
{
	int ret;

	ret = __gem_set_tiling(buf_ops_get_fd(bops), buf->handle, tiling, stride);

	buf->surface[0].stride = stride;
	buf->surface[0].size = buf->surface[0].stride * TEST_HEIGHT(stride);
	buf->tiling = tiling;

	igt_assert_eq(ret, 0);
}

static void do_test(struct buf_ops *bops, uint32_t tiling, unsigned stride,
		    uint32_t tiling_after, unsigned stride_after)
{
	struct intel_buf *test_buf, *target_buf;
	struct intel_bb *ibb;
	igt_spin_t *busy;
	int i, fd = buf_ops_get_fd(bops);
	uint32_t *ptr;
	uint32_t blt_stride, blt_bits;
	uint32_t ring = I915_EXEC_DEFAULT;
	bool tiling_changed = false;

	ibb = intel_bb_create_with_relocs(fd, 4096);

	igt_info("filling ring\n");
	if (HAS_BLT_RING(ibb->devid))
		ring = I915_EXEC_BLT;
	busy = igt_spin_new(fd, .engine = ring);

	igt_info("playing tricks .. ");
	/* first allocate the target so it gets out of the way of playing funky
	 * tricks */
	target_buf = intel_buf_create(bops, TEST_WIDTH(TEST_STRIDE),
				      TEST_HEIGHT(TEST_STRIDE), 32, 0,
				      I915_TILING_NONE, I915_COMPRESSION_NONE);

	intel_bb_add_intel_buf(ibb, target_buf, true);
	/* allocate buffer with parameters _after_ transition we want to check
	 * and touch it, so that it's properly aligned in the gtt. */
	test_buf = intel_buf_create(bops, TEST_WIDTH(stride_after),
				    TEST_HEIGHT(stride_after), 32, 0,
				    tiling_after, I915_COMPRESSION_NONE);

	ptr = gem_mmap__gtt(fd, test_buf->handle, TEST_SIZE,
			    PROT_READ | PROT_WRITE);
	gem_set_domain(fd, test_buf->handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	*ptr = 0;
	gem_munmap(ptr, TEST_SIZE);

	/* Reuse previously aligned in the gtt object */
	intel_buf_init_using_handle(bops, test_buf->handle, test_buf,
				    TEST_WIDTH(stride), TEST_HEIGHT(stride), 32,
				    0, tiling, I915_COMPRESSION_NONE);
	igt_assert_eq_u32(intel_buf_bo_size(test_buf), TEST_SIZE);
	intel_buf_set_ownership(test_buf, true);
	intel_bb_add_intel_buf(ibb, test_buf, false);

	if (tiling == I915_TILING_NONE) {
		gem_write(fd, test_buf->handle, 0, data, TEST_SIZE);
	} else {
		ptr = gem_mmap__gtt(fd, test_buf->handle, TEST_SIZE,
				    PROT_READ | PROT_WRITE);
		gem_set_domain(fd, test_buf->handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

		memcpy(ptr, data, TEST_SIZE);
		gem_munmap(ptr, TEST_SIZE);
		ptr = NULL;
	}

	blt_stride = stride;
	blt_bits = 0;
	if (intel_gen(ibb->devid) >= 4 && tiling != I915_TILING_NONE) {
		blt_stride /= 4;
		blt_bits = XY_SRC_COPY_BLT_SRC_TILED;
	}

	intel_bb_blit_start(ibb, blt_bits);
	intel_bb_out(ibb, (3 << 24) | /* 32 bits */
		  (0xcc << 16) | /* copy ROP */
		  stride);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, (TEST_HEIGHT(stride)) << 16 | (TEST_WIDTH(stride)));
	intel_bb_emit_reloc_fenced(ibb, target_buf->handle,
				   I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER, 0, 0);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, blt_stride);
	intel_bb_emit_reloc_fenced(ibb, test_buf->handle,
				   I915_GEM_DOMAIN_RENDER, 0, 0, 0);
	intel_bb_flush_blit(ibb);

	__set_tiling(bops, test_buf, tiling_after, stride_after);
	intel_bb_reset(ibb, true);
	intel_bb_add_intel_buf(ibb, test_buf, true);

	/* Note: We don't care about gen4+ here because the blitter doesn't use
	 * fences there. So not setting tiling flags on the tiled buffer is ok.
	 */

	intel_bb_blit_start(ibb, 0);
	intel_bb_out(ibb, (3 << 24) | /* 32 bits */
		  (0xcc << 16) | /* copy ROP */
		  stride_after);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, (1) << 16 | (1));
	intel_bb_emit_reloc_fenced(ibb, test_buf->handle,
				   I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER, 0, 0);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, stride_after);
	intel_bb_emit_reloc_fenced(ibb, test_buf->handle,
				   I915_GEM_DOMAIN_RENDER, 0, 0, 0);
	intel_bb_flush_blit(ibb);
	igt_spin_free(fd, busy);
	/* Now try to trick the kernel the kernel into changing up the fencing
	 * too early. */
	igt_info("checking .. ");
	memset(data, 0, TEST_SIZE);

	gem_read(fd, target_buf->handle, 0, data, TEST_SIZE);
	for (i = 0; i < TEST_SIZE/4; i++)
		igt_assert(data[i] == i);

	/* check whether tiling on the test_buf actually changed. */
	ptr = gem_mmap__gtt(fd, test_buf->handle, TEST_SIZE,
			    PROT_WRITE | PROT_READ);
	gem_set_domain(fd, test_buf->handle, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < TEST_SIZE/4; i++)
		if (ptr[i] != data[i])
			tiling_changed = true;

	gem_munmap(ptr, TEST_SIZE);
	igt_assert(tiling_changed);

	intel_buf_destroy(test_buf);
	intel_buf_destroy(target_buf);

	igt_info("done\n");
}

igt_main
{
	int fd, i;
	uint32_t tiling, tiling_after;
	struct buf_ops *bops;

	igt_fixture {
		for (i = 0; i < 1024*256; i++)
			data[i] = i;

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		igt_require(gem_available_fences(fd) > 0);
		bops = buf_ops_create(fd);
	}

	igt_subtest("untiled-to-tiled") {
		tiling = I915_TILING_NONE;
		tiling_after = I915_TILING_X;
		do_test(bops, tiling, TEST_STRIDE, tiling_after, TEST_STRIDE);
		igt_assert(tiling == I915_TILING_NONE);
		igt_assert(tiling_after == I915_TILING_X);
	}

	igt_subtest("tiled-to-untiled") {
		tiling = I915_TILING_X;
		tiling_after = I915_TILING_NONE;
		do_test(bops, tiling, TEST_STRIDE, tiling_after, TEST_STRIDE);
		igt_assert(tiling == I915_TILING_X);
		igt_assert(tiling_after == I915_TILING_NONE);
	}

	igt_subtest("tiled-to-tiled") {
		tiling = I915_TILING_X;
		tiling_after = I915_TILING_X;
		do_test(bops, tiling, TEST_STRIDE/2, tiling_after, TEST_STRIDE);
		igt_assert(tiling == I915_TILING_X);
		igt_assert(tiling_after == I915_TILING_X);
	}

	igt_fixture{
		buf_ops_destroy(bops);
		close(fd);
	}
}
