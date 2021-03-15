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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
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

IGT_TEST_DESCRIPTION("Test pwrite/pread consistency when touching partial"
		     " cachelines.");

/*
 * Testcase: pwrite/pread consistency when touching partial cachelines
 *
 * Some fancy new pwrite/pread optimizations clflush in-line while
 * reading/writing. Check whether all required clflushes happen.
 *
 * Unfortunately really old mesa used unaligned pread/pwrite for s/w fallback
 * rendering, so we need to check whether this works on tiled buffers, too.
 *
 */

static struct buf_ops *bops;

struct intel_buf *scratch_buf;
struct intel_buf *staging_buf;
struct intel_buf *tiled_staging_buf;
#define BO_SIZE (32*4096)
int fd;

static void
copy_bo(struct intel_bb *ibb, struct intel_buf *src, int src_tiled,
	struct intel_buf *dst, int dst_tiled)
{
	unsigned long dst_pitch = dst->surface[0].stride;
	unsigned long src_pitch = src->surface[0].stride;
	unsigned long scratch_pitch = src->surface[0].stride;
	uint32_t cmd_bits = 0;

	/* dst is tiled ... */
	if (ibb->gen >= 4 && dst_tiled) {
		dst_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_DST_TILED;
	}

	if (ibb->gen >= 4 && src_tiled) {
		src_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
	}

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_blit_start(ibb, cmd_bits);
	intel_bb_out(ibb, (3 << 24) | /* 32 bits */
		  (0xcc << 16) | /* copy ROP */
		  dst_pitch);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, BO_SIZE/scratch_pitch << 16 | 1024);
	intel_bb_emit_reloc_fenced(ibb, dst->handle, I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER, 0, dst->addr.offset);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, src_pitch);
	intel_bb_emit_reloc_fenced(ibb, src->handle, I915_GEM_DOMAIN_RENDER, 0,
				   0, src->addr.offset);

	intel_bb_flush_blit(ibb);
}

static void
blt_bo_fill(struct intel_bb *ibb, struct intel_buf *tmp_buf,
	    struct intel_buf *buf, int val)
{
	uint8_t *gtt_ptr;
	int i;

	gtt_ptr = gem_mmap__gtt(fd, tmp_buf->handle, tmp_buf->surface[0].size,
				PROT_WRITE);
	gem_set_domain(fd, tmp_buf->handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	for (i = 0; i < BO_SIZE; i++)
		gtt_ptr[i] = val;

	gem_munmap(gtt_ptr, tmp_buf->surface[0].size);

	igt_drop_caches_set(fd, DROP_BOUND);

	copy_bo(ibb, tmp_buf, 0, buf, 1);
}

#define MAX_BLT_SIZE 128
#define ROUNDS 200
uint8_t tmp[BO_SIZE];
uint8_t compare_tmp[BO_SIZE];

static void test_partial_reads(void)
{
	struct intel_bb *ibb;
	int i, j;

	ibb = intel_bb_create(fd, 4096);
	for (i = 0; i < ROUNDS; i++) {
		int start, len;
		int val = i % 256;

		blt_bo_fill(ibb, staging_buf, scratch_buf, i);

		start = random() % BO_SIZE;
		len = random() % (BO_SIZE-start) + 1;

		gem_read(fd, scratch_buf->handle, start, tmp, len);
		for (j = 0; j < len; j++) {
			igt_assert_f(tmp[j] == val,
				     "mismatch at %i, got: %i, expected: %i\n",
				     start + j, tmp[j], val);
		}

		igt_progress("partial reads test: ", i, ROUNDS);
	}

	intel_bb_destroy(ibb);
}

static void test_partial_writes(void)
{
	struct intel_bb *ibb;
	int i, j;

	ibb = intel_bb_create(fd, 4096);
	for (i = 0; i < ROUNDS; i++) {
		int start, len;
		int val = i % 256;

		blt_bo_fill(ibb, staging_buf, scratch_buf, i);

		start = random() % BO_SIZE;
		len = random() % (BO_SIZE-start) + 1;

		memset(tmp, i + 63, BO_SIZE);

		gem_write(fd, scratch_buf->handle, start, tmp, len);

		copy_bo(ibb, scratch_buf, 1, tiled_staging_buf, 1);
		gem_read(fd, tiled_staging_buf->handle, 0, compare_tmp, BO_SIZE);

		for (j = 0; j < start; j++) {
			igt_assert_f(compare_tmp[j] == val,
				     "mismatch at %i, got: %i, expected: %i\n",
				     j, tmp[j], val);
		}
		for (; j < start + len; j++) {
			igt_assert_f(compare_tmp[j] == tmp[0],
				     "mismatch at %i, got: %i, expected: %i\n",
				     j, tmp[j], i);
		}
		for (; j < BO_SIZE; j++) {
			igt_assert_f(compare_tmp[j] == val,
				     "mismatch at %i, got: %i, expected: %i\n",
				     j, tmp[j], val);
		}

		igt_progress("partial writes test: ", i, ROUNDS);
	}

	intel_bb_destroy(ibb);
}

static void test_partial_read_writes(void)
{
	struct intel_bb *ibb;
	int i, j;

	ibb = intel_bb_create(fd, 4096);
	for (i = 0; i < ROUNDS; i++) {
		int start, len;
		int val = i % 256;

		blt_bo_fill(ibb, staging_buf, scratch_buf, i);

		/* partial read */
		start = random() % BO_SIZE;
		len = random() % (BO_SIZE-start) + 1;

		gem_read(fd, scratch_buf->handle, start, tmp, len);
		for (j = 0; j < len; j++) {
			igt_assert_f(tmp[j] == val,
				     "mismatch in read at %i, got: %i, expected: %i\n",
				     start + j, tmp[j], val);
		}

		/* Change contents through gtt to make the pread cachelines
		 * stale. */
		val = (i + 17) % 256;
		blt_bo_fill(ibb, staging_buf, scratch_buf, val);

		/* partial write */
		start = random() % BO_SIZE;
		len = random() % (BO_SIZE-start) + 1;

		memset(tmp, i + 63, BO_SIZE);

		gem_write(fd, scratch_buf->handle, start, tmp, len);

		copy_bo(ibb, scratch_buf, 1, tiled_staging_buf, 1);
		gem_read(fd, tiled_staging_buf->handle, 0, compare_tmp, BO_SIZE);

		for (j = 0; j < start; j++) {
			igt_assert_f(compare_tmp[j] == val,
				     "mismatch at %i, got: %i, expected: %i\n",
				     j, tmp[j], val);
		}
		for (; j < start + len; j++) {
			igt_assert_f(compare_tmp[j] == tmp[0],
				     "mismatch at %i, got: %i, expected: %i\n",
				     j, tmp[j], tmp[0]);
		}
		for (; j < BO_SIZE; j++) {
			igt_assert_f(compare_tmp[j] == val,
				     "mismatch at %i, got: %i, expected: %i\n",
				     j, tmp[j], val);
		}

		igt_progress("partial read/writes test: ", i, ROUNDS);
	}

	intel_bb_destroy(ibb);
}

static bool known_swizzling(uint32_t handle)
{
	struct drm_i915_gem_get_tiling arg = {
		.handle = handle,
	};

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING, &arg))
		return false;

	return arg.phys_swizzle_mode == arg.swizzle_mode;
}

igt_main
{
	srandom(0xdeadbeef);

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_mappable_ggtt(fd);
		gem_require_blitter(fd);
		gem_require_pread_pwrite(fd);

		bops = buf_ops_create(fd);

		/* overallocate the buffers we're actually using because */
		scratch_buf = intel_buf_create(bops, 1024, BO_SIZE/4096, 32, 0,
					       I915_TILING_X,
					       I915_COMPRESSION_NONE);

		/*
		 * As we want to compare our template tiled pattern against
		 * the target bo, we need consistent swizzling on both.
		 */
		igt_require(known_swizzling(scratch_buf->handle));
		staging_buf = intel_buf_create(bops, 1024, BO_SIZE/4096, 32,
					       4096, I915_TILING_NONE,
					       I915_COMPRESSION_NONE);

		tiled_staging_buf = intel_buf_create(bops, 1024, BO_SIZE/4096,
						     32, 0, I915_TILING_X,
						     I915_COMPRESSION_NONE);
	}

	igt_subtest("reads")
		test_partial_reads();

	igt_subtest("writes")
		test_partial_writes();

	igt_subtest("writes-after-reads")
		test_partial_read_writes();

	igt_fixture {
		intel_buf_destroy(scratch_buf);
		intel_buf_destroy(staging_buf);
		intel_buf_destroy(tiled_staging_buf);
		buf_ops_destroy(bops);

		close(fd);
	}
}
