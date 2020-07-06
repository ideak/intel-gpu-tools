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
 *    Chris Wilson <chris@chris-wilson.co.uk>
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

IGT_TEST_DESCRIPTION("Test snoop consistency when touching partial"
		     " cachelines.");

/*
 * Testcase: snoop consistency when touching partial cachelines
 *
 */

#define BO_SIZE (4*4096)
#define PAGE_SIZE 4096

typedef struct {
	int fd;
	uint32_t devid;
	struct buf_ops *bops;
} data_t;


static void *__try_gtt_map_first(data_t *data, struct intel_buf *buf,
				 int write_enable)
{
	uint8_t *ptr;
	unsigned int prot = PROT_READ | (write_enable ? PROT_WRITE : 0);

	ptr = __gem_mmap__gtt(data->fd, buf->handle, buf->surface[0].size, prot);
	if (!ptr) {
		ptr = gem_mmap__device_coherent(data->fd, buf->handle,
					  0, buf->surface[0].size,  prot);
	}
	return ptr;
}

static void
copy_bo(struct intel_bb *ibb, struct intel_buf *src, struct intel_buf *dst)
{
	bool has_64b_reloc;

	has_64b_reloc = ibb->gen >= 8;

	intel_bb_out(ibb,
		     XY_SRC_COPY_BLT_CMD |
		     XY_SRC_COPY_BLT_WRITE_ALPHA |
		     XY_SRC_COPY_BLT_WRITE_RGB |
		     (6 + 2 * has_64b_reloc));

	intel_bb_out(ibb, (3 << 24) | /* 32 bits */
		     (0xcc << 16) | /* copy ROP */
		     4096);
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

	 /* Mark the end of the buffer. */
	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_flush_blit(ibb);
	intel_bb_sync(ibb);
}

static void
blt_bo_fill(data_t *data, struct intel_bb *ibb, struct intel_buf *tmp_bo,
	    struct intel_buf *bo, uint8_t val)
{
	uint8_t *gtt_ptr;
	int i;

	gtt_ptr = __try_gtt_map_first(data, tmp_bo, 1);

	for (i = 0; i < BO_SIZE; i++)
		gtt_ptr[i] = val;

	munmap(gtt_ptr, tmp_bo->surface[0].size);

	igt_drop_caches_set(data->fd, DROP_BOUND);

	copy_bo(ibb, tmp_bo, bo);
}

#define MAX_BLT_SIZE 128
#define ROUNDS 1000
#define TEST_READ 0x1
#define TEST_WRITE 0x2
#define TEST_BOTH (TEST_READ | TEST_WRITE)
igt_main
{
	struct intel_buf *scratch_buf, *staging_buf;
	struct intel_bb *ibb;
	data_t data = {0, };
	unsigned flags = TEST_BOTH;
	int i, j;
	uint8_t *cpu_ptr;
	uint8_t *gtt_ptr;

	igt_fixture {
		srandom(0xdeadbeef);

		data.fd = drm_open_driver(DRIVER_INTEL);

		igt_require_gem(data.fd);
		gem_require_blitter(data.fd);
		gem_require_caching(data.fd);

		data.devid = intel_get_drm_devid(data.fd);
		if (IS_GEN2(data.devid)) /* chipset only handles cached -> uncached */
			flags &= ~TEST_READ;
		if (IS_BROADWATER(data.devid) || IS_CRESTLINE(data.devid)) {
			/* chipset is completely fubar */
			igt_info("coherency broken on i965g/gm\n");
			flags = 0;
		}
		data.bops = buf_ops_create(data.fd);
		ibb = intel_bb_create(data.fd, PAGE_SIZE);

		scratch_buf = intel_buf_create(data.bops, BO_SIZE/4, 1,
					       32, 0, I915_TILING_NONE, 0);

		gem_set_caching(data.fd, scratch_buf->handle, 1);

		staging_buf = intel_buf_create(data.bops, BO_SIZE/4, 1,
					       32, 0, I915_TILING_NONE, 0);
	}

	igt_subtest("reads") {
		igt_require(flags & TEST_READ);

		igt_info("checking partial reads\n");

		for (i = 0; i < ROUNDS; i++) {
			uint8_t val0 = i;
			int start, len;

			blt_bo_fill(&data, ibb, staging_buf, scratch_buf, i);

			start = random() % BO_SIZE;
			len = random() % (BO_SIZE-start) + 1;

			cpu_ptr = gem_mmap__cpu(data.fd, scratch_buf->handle,
						0, scratch_buf->surface[0].size,
						PROT_READ);
			for (j = 0; j < len; j++) {
				igt_assert_f(cpu_ptr[j] == val0,
					     "mismatch at %i, got: %i, expected: %i\n",
					     j, cpu_ptr[j], val0);
			}
			munmap(cpu_ptr, scratch_buf->surface[0].size);

			igt_progress("partial reads test: ", i, ROUNDS);
		}
	}

	igt_subtest("writes") {
		igt_require(flags & TEST_WRITE);

		igt_info("checking partial writes\n");

		for (i = 0; i < ROUNDS; i++) {
			uint8_t val0 = i, val1;
			int start, len;

			blt_bo_fill(&data, ibb, staging_buf, scratch_buf, val0);

			start = random() % BO_SIZE;
			len = random() % (BO_SIZE-start) + 1;

			val1 = val0 + 63;
			cpu_ptr = gem_mmap__cpu(data.fd, scratch_buf->handle,
						0, scratch_buf->surface[0].size,
						PROT_READ | PROT_WRITE);

			memset(cpu_ptr + start, val1, len);
			munmap(cpu_ptr, scratch_buf->surface[0].size);
			copy_bo(ibb, scratch_buf, staging_buf);
			gtt_ptr = __try_gtt_map_first(&data, staging_buf, 0);

			for (j = 0; j < start; j++) {
				igt_assert_f(gtt_ptr[j] == val0,
					     "mismatch at %i, partial=[%d+%d] got: %i, expected: %i\n",
					     j, start, len, gtt_ptr[j], val0);
			}
			for (; j < start + len; j++) {
				igt_assert_f(gtt_ptr[j] == val1,
					     "mismatch at %i, partial=[%d+%d] got: %i, expected: %i\n",
					     j, start, len, gtt_ptr[j], val1);
			}
			for (; j < BO_SIZE; j++) {
				igt_assert_f(gtt_ptr[j] == val0,
					     "mismatch at %i, partial=[%d+%d] got: %i, expected: %i\n",
					     j, start, len, gtt_ptr[j], val0);
			}
			munmap(gtt_ptr, staging_buf->surface[0].size);

			igt_progress("partial writes test: ", i, ROUNDS);
		}
	}

	igt_subtest("read-writes") {
		igt_require((flags & TEST_BOTH) == TEST_BOTH);

		igt_info("checking partial writes after partial reads\n");

		for (i = 0; i < ROUNDS; i++) {
			uint8_t val0 = i, val1, val2;
			int start, len;

			blt_bo_fill(&data, ibb, staging_buf, scratch_buf, val0);

			/* partial read */
			start = random() % BO_SIZE;
			len = random() % (BO_SIZE-start) + 1;

			cpu_ptr = gem_mmap__cpu(data.fd, scratch_buf->handle,
						0, scratch_buf->surface[0].size,
						PROT_READ);

			for (j = 0; j < len; j++) {
				igt_assert_f(cpu_ptr[j] == val0,
					     "mismatch in read at %i, got: %i, expected: %i\n",
					     j, cpu_ptr[j], val0);
			}
			munmap(cpu_ptr, scratch_buf->surface[0].size);

			/* Change contents through gtt to make the pread cachelines
			 * stale. */
			val1 = i + 17;
			blt_bo_fill(&data, ibb, staging_buf, scratch_buf, val1);

			/* partial write */
			start = random() % BO_SIZE;
			len = random() % (BO_SIZE-start) + 1;

			val2 = i + 63;
			cpu_ptr = gem_mmap__cpu(data.fd, scratch_buf->handle,
						0, scratch_buf->surface[0].size,
						PROT_READ);

			memset(cpu_ptr + start, val2, len);

			copy_bo(ibb, scratch_buf, staging_buf);
			gtt_ptr = __try_gtt_map_first(&data, staging_buf, 0);

			for (j = 0; j < start; j++) {
				igt_assert_f(gtt_ptr[j] == val1,
					     "mismatch at %i, partial=[%d+%d] got: %i, expected: %i\n",
					     j, start, len, gtt_ptr[j], val1);
			}
			for (; j < start + len; j++) {
				igt_assert_f(gtt_ptr[j] == val2,
					     "mismatch at %i, partial=[%d+%d] got: %i, expected: %i\n",
					     j, start, len, gtt_ptr[j], val2);
			}
			for (; j < BO_SIZE; j++) {
				igt_assert_f(gtt_ptr[j] == val1,
					     "mismatch at %i, partial=[%d+%d] got: %i, expected: %i\n",
					     j, start, len, gtt_ptr[j], val1);
			}
			munmap(cpu_ptr, scratch_buf->handle);
			munmap(gtt_ptr, staging_buf->handle);

			igt_progress("partial read/writes test: ", i, ROUNDS);
		}
	}

	igt_fixture {
		intel_bb_destroy(ibb);
		intel_buf_destroy(scratch_buf);
		intel_buf_destroy(staging_buf);
		buf_ops_destroy(data.bops);
		close(data.fd);
	}
}
