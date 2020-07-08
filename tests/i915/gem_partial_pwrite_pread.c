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
 */

#define PAGE_SIZE 4096
#define BO_SIZE (4*4096)

struct intel_bb *ibb;
struct intel_buf *scratch_buf;
struct intel_buf *staging_buf;

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
} data_t;

static void *__try_gtt_map_first(data_t *data, struct intel_buf *buf,
				int write_enable)
{
	uint8_t *ptr;
	unsigned prot = PROT_READ | (write_enable ? PROT_WRITE : 0);

	ptr = __gem_mmap__gtt(data->drm_fd, buf->handle, buf->surface[0].size, prot);
	if (!ptr) {
		ptr = gem_mmap__device_coherent(data->drm_fd, buf->handle,
					  0, buf->surface[0].size,  prot);
	}
	return ptr;
}

static void copy_bo(struct intel_buf *src, struct intel_buf *dst)
{
	bool has_64b_reloc;

	has_64b_reloc = ibb->gen >= 8;

	intel_bb_out(ibb,
		     XY_SRC_COPY_BLT_CMD |
		     XY_SRC_COPY_BLT_WRITE_ALPHA |
		     XY_SRC_COPY_BLT_WRITE_RGB |
		     (6 + 2 * has_64b_reloc));
	intel_bb_out(ibb, 3 << 24 | 0xcc << 16 | 4096);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, (BO_SIZE/4096) << 16 | 1024);
	intel_bb_emit_reloc_fenced(ibb, dst->handle,
				   I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER,
				   0, 0x0);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, 4096);
	intel_bb_emit_reloc_fenced(ibb, src->handle,
				   I915_GEM_DOMAIN_RENDER,
				   0, 0, 0x0);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_flush_blit(ibb);
	intel_bb_sync(ibb);
}

static void
blt_bo_fill(data_t *data, struct intel_buf *tmp_bo,
		struct intel_buf *bo, uint8_t val)
{
	uint8_t *gtt_ptr;
	int i;

	gtt_ptr = __try_gtt_map_first(data, tmp_bo, 1);

	for (i = 0; i < BO_SIZE; i++)
		gtt_ptr[i] = val;

	munmap(gtt_ptr, tmp_bo->surface[0].size);

	igt_drop_caches_set(data->drm_fd, DROP_BOUND);

	copy_bo(tmp_bo, bo);
}

#define MAX_BLT_SIZE 128
#define ROUNDS 1000
uint8_t tmp[BO_SIZE];

static void get_range(int *start, int *len)
{
	*start = random() % (BO_SIZE - 1);
	*len = random() % (BO_SIZE - *start - 1) + 1;
}

static void test_partial_reads(data_t *data)
{
	int i, j;

	igt_info("checking partial reads\n");
	for (i = 0; i < ROUNDS; i++) {
		uint8_t val = i;
		int start, len;

		blt_bo_fill(data, staging_buf, scratch_buf, val);

		get_range(&start, &len);
		gem_read(data->drm_fd, scratch_buf->handle, start, tmp, len);

		for (j = 0; j < len; j++) {
			igt_assert_f(tmp[j] == val,
				     "mismatch at %i [%i + %i], got: %i, expected: %i\n",
				     j, start, len, tmp[j], val);
		}

		igt_progress("partial reads test: ", i, ROUNDS);
	}
}

static void test_partial_writes(data_t *data)
{
	int i, j;
	uint8_t *gtt_ptr;

	igt_info("checking partial writes\n");
	for (i = 0; i < ROUNDS; i++) {
		uint8_t val = i;
		int start, len;

		blt_bo_fill(data, staging_buf, scratch_buf, val);

		memset(tmp, i + 63, BO_SIZE);

		get_range(&start, &len);
		gem_write(data->drm_fd, scratch_buf->handle, start, tmp, len);

		copy_bo(scratch_buf, staging_buf);
		gtt_ptr = __try_gtt_map_first(data, staging_buf, 0);

		for (j = 0; j < start; j++) {
			igt_assert_f(gtt_ptr[j] == val,
				     "mismatch at %i (start=%i), got: %i, expected: %i\n",
				     j, start, tmp[j], val);
		}
		for (; j < start + len; j++) {
			igt_assert_f(gtt_ptr[j] == tmp[0],
				     "mismatch at %i (%i/%i), got: %i, expected: %i\n",
				     j, j-start, len, tmp[j], i);
		}
		for (; j < BO_SIZE; j++) {
			igt_assert_f(gtt_ptr[j] == val,
				     "mismatch at %i (end=%i), got: %i, expected: %i\n",
				     j, start+len, tmp[j], val);
		}
		munmap(gtt_ptr, staging_buf->surface[0].size);

		igt_progress("partial writes test: ", i, ROUNDS);
	}
}

static void test_partial_read_writes(data_t *data)
{
	int i, j;
	uint8_t *gtt_ptr;

	igt_info("checking partial writes after partial reads\n");
	for (i = 0; i < ROUNDS; i++) {
		uint8_t val = i;
		int start, len;

		blt_bo_fill(data, staging_buf, scratch_buf, val);

		/* partial read */
		get_range(&start, &len);
		gem_read(data->drm_fd, scratch_buf->handle, start, tmp, len);
		for (j = 0; j < len; j++) {
			igt_assert_f(tmp[j] == val,
				     "mismatch in read at %i [%i + %i], got: %i, expected: %i\n",
				     j, start, len, tmp[j], val);
		}

		/* Change contents through gtt to make the pread cachelines
		 * stale. */
		val += 17;
		blt_bo_fill(data, staging_buf, scratch_buf, val);

		/* partial write */
		memset(tmp, i + 63, BO_SIZE);

		get_range(&start, &len);
		gem_write(data->drm_fd, scratch_buf->handle, start, tmp, len);

		copy_bo(scratch_buf, staging_buf);
		gtt_ptr = __try_gtt_map_first(data, staging_buf, 0);

		for (j = 0; j < start; j++) {
			igt_assert_f(gtt_ptr[j] == val,
				     "mismatch at %i (start=%i), got: %i, expected: %i\n",
				     j, start, tmp[j], val);
		}
		for (; j < start + len; j++) {
			igt_assert_f(gtt_ptr[j] == tmp[0],
				     "mismatch at %i (%i/%i), got: %i, expected: %i\n",
				     j, j - start, len, tmp[j], tmp[0]);
		}
		for (; j < BO_SIZE; j++) {
			igt_assert_f(gtt_ptr[j] == val,
				     "mismatch at %i (end=%i), got: %i, expected: %i\n",
				     j, start + len, tmp[j], val);
		}
		munmap(gtt_ptr, staging_buf->surface[0].size);

		igt_progress("partial read/writes test: ", i, ROUNDS);
	}
}

static void do_tests(data_t *data, int cache_level, const char *suffix)
{
	igt_fixture {
		if (cache_level != -1)
			gem_set_caching(data->drm_fd, scratch_buf->handle, cache_level);
	}

	igt_subtest_f("reads%s", suffix)
		test_partial_reads(data);

	igt_subtest_f("write%s", suffix)
		test_partial_writes(data);

	igt_subtest_f("writes-after-reads%s", suffix)
		test_partial_read_writes(data);
}

igt_main
{
	data_t data = {0, };
	srandom(0xdeadbeef);

	igt_fixture {
		data.drm_fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(data.drm_fd);
		gem_require_blitter(data.drm_fd);

		data.devid = intel_get_drm_devid(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);

		ibb = intel_bb_create(data.drm_fd, PAGE_SIZE);

		/* overallocate the buffers we're actually using because */	
		scratch_buf = intel_buf_create(data.bops, BO_SIZE/4, 1, 32, 0, I915_TILING_NONE, 0);
		staging_buf = intel_buf_create(data.bops, BO_SIZE/4, 1, 32, 0, I915_TILING_NONE, 0);
	}

	do_tests(&data, -1, "");

	/* Repeat the tests using different levels of snooping */
	do_tests(&data, 0, "-uncached");
	do_tests(&data, 1, "-snoop");
	do_tests(&data, 2, "-display");

	igt_fixture {
		intel_bb_destroy(ibb);
		intel_buf_destroy(scratch_buf);
		intel_buf_destroy(staging_buf);
		buf_ops_destroy(data.bops);
		close(data.drm_fd);
	}
}
