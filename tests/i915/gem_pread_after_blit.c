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

/** @file gem_pread_after_blit.c
 *
 * This is a test of pread's behavior when getting values out of just-drawn-to
 * buffers.
 *
 * The goal is to catch failure in the whole-buffer-flush or
 * ranged-buffer-flush paths in the kernel.
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

IGT_TEST_DESCRIPTION("Test pread behavior when getting values out of"
		     " just-drawn-to buffers.");
static const int width = 512, height = 512;
static const int size = 1024 * 1024;

#define PAGE_SIZE 4096

static struct intel_buf *
create_bo(struct buf_ops *bops, uint32_t val)
{
	struct intel_buf *buf;
	uint32_t *vaddr;
	int i;

	buf = intel_buf_create(bops, width, height, 32, 0, I915_TILING_NONE,
			       I915_COMPRESSION_NONE);

	/* Fill the BO with dwords starting at start_val */
	intel_buf_cpu_map(buf, 1);
	vaddr = buf->ptr;

	for (i = 0; i < 1024 * 1024 / 4; i++)
		vaddr[i] = val++;

	intel_buf_unmap(buf);

	return buf;
}

static void
verify_large_read(int fd, struct intel_buf *buf, uint32_t val)
{
	uint32_t tmp[size / 4];
	int i;

	gem_read(fd, buf->handle, 0, tmp, size);

	for (i = 0; i < size / 4; i++) {
		igt_assert_f(tmp[i] == val,
			     "Unexpected value 0x%08x instead of "
			     "0x%08x at offset 0x%08x (%p)\n",
			     tmp[i], val, i * 4, tmp);
		val++;
	}
}

/** This reads at the size that Mesa usees for software fallbacks. */
static void
verify_small_read(int fd, struct intel_buf *buf, uint32_t val)
{
	uint32_t tmp[4096 / 4];
	int offset, i;

	for (i = 0; i < 4096 / 4; i++)
		tmp[i] = 0x00c0ffee;

	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		gem_read(fd, buf->handle, offset, tmp, PAGE_SIZE);

		for (i = 0; i < PAGE_SIZE; i += 4) {
			igt_assert_f(tmp[i / 4] == val,
				     "Unexpected value 0x%08x instead of "
				     "0x%08x at offset 0x%08x\n",
				     tmp[i / 4], val, i * 4);
			val++;
		}
	}
}

typedef igt_hang_t (*do_hang)(int fd, struct intel_bb *ibb);

static igt_hang_t no_hang(int fd, struct intel_bb *ibb)
{
	return (igt_hang_t){0};
}

static igt_hang_t bcs_hang(int fd, struct intel_bb *ibb)
{
	return igt_hang_ring(fd, ibb->gen >= 6 ? I915_EXEC_BLT : I915_EXEC_DEFAULT);
}

static void do_test(struct buf_ops *bops, int cache_level,
		    struct intel_buf *src[2],
		    const uint32_t start[2],
		    struct intel_buf *tmp[2],
		    int loop, do_hang do_hang_func)
{
	struct intel_bb *ibb;
	igt_hang_t hang;
	int fd = buf_ops_get_fd(bops);

	ibb = intel_bb_create(fd, 4096);

	if (cache_level != -1) {
		gem_set_caching(fd, tmp[0]->handle, cache_level);
		gem_set_caching(fd, tmp[1]->handle, cache_level);
	}

	do {
		/* First, do a full-buffer read after blitting */
		intel_bb_copy_intel_buf(ibb, src[0], tmp[0], size);
		hang = do_hang_func(fd, ibb);
		verify_large_read(fd, tmp[0], start[0]);
		igt_post_hang_ring(fd, hang);
		intel_bb_copy_intel_buf(ibb, src[1], tmp[0], size);
		hang = do_hang_func(fd, ibb);
		verify_large_read(fd, tmp[0], start[1]);
		igt_post_hang_ring(fd, hang);

		intel_bb_copy_intel_buf(ibb, src[0], tmp[0], size);
		hang = do_hang_func(fd, ibb);
		verify_small_read(fd, tmp[0], start[0]);
		igt_post_hang_ring(fd, hang);
		intel_bb_copy_intel_buf(ibb, src[1], tmp[0], size);
		hang = do_hang_func(fd, ibb);
		verify_small_read(fd, tmp[0], start[1]);
		igt_post_hang_ring(fd, hang);

		intel_bb_copy_intel_buf(ibb, src[0], tmp[0], size);
		hang = do_hang_func(fd, ibb);
		verify_large_read(fd, tmp[0], start[0]);
		igt_post_hang_ring(fd, hang);

		intel_bb_copy_intel_buf(ibb, src[0], tmp[0], size);
		intel_bb_copy_intel_buf(ibb, src[1], tmp[1], size);
		hang = do_hang_func(fd, ibb);
		verify_large_read(fd, tmp[0], start[0]);
		verify_large_read(fd, tmp[1], start[1]);
		igt_post_hang_ring(fd, hang);

		intel_bb_copy_intel_buf(ibb, src[0], tmp[0], size);
		intel_bb_copy_intel_buf(ibb, src[1], tmp[1], size);
		hang = do_hang_func(fd, ibb);
		verify_large_read(fd, tmp[1], start[1]);
		verify_large_read(fd, tmp[0], start[0]);
		igt_post_hang_ring(fd, hang);

		intel_bb_copy_intel_buf(ibb, src[0], tmp[1], size);
		intel_bb_copy_intel_buf(ibb, src[1], tmp[0], size);
		hang = do_hang_func(fd, ibb);
		verify_large_read(fd, tmp[0], start[1]);
		verify_large_read(fd, tmp[1], start[0]);
		igt_post_hang_ring(fd, hang);
	} while (--loop);

	intel_bb_destroy(ibb);
}

igt_main
{
	const uint32_t start[2] = {0, 1024 * 1024 / 4};
	const struct {
		const char *name;
		int cache;
	} tests[] = {
		{ "default", -1 },
		{ "uncached", 0 },
		{ "snooped", 1 },
		{ "display", 2 },
		{ NULL, -1 },
	}, *t;
	struct intel_buf *src[2], *dst[2];
	struct buf_ops *bops;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		bops = buf_ops_create(fd);

		src[0] = create_bo(bops, start[0]);
		src[1] = create_bo(bops, start[1]);

		dst[0] = intel_buf_create(bops, width, height, 32, 4096,
					  I915_TILING_NONE, I915_COMPRESSION_NONE);
		dst[1] = intel_buf_create(bops, width, height, 32, 4096,
					  I915_TILING_NONE, I915_COMPRESSION_NONE);
	}

	for (t = tests; t->name; t++) {
		igt_subtest_f("%s-normal", t->name)
			do_test(bops, t->cache, src, start, dst, 1, no_hang);

		igt_fork_signal_helper();
		igt_subtest_f("%s-interruptible", t->name)
			do_test(bops, t->cache, src, start, dst, 100, no_hang);
		igt_stop_signal_helper();

		igt_subtest_f("%s-hang", t->name)
			do_test(bops, t->cache, src, start, dst, 1, bcs_hang);
	}

	igt_fixture {
		intel_buf_destroy(src[0]);
		intel_buf_destroy(src[1]);
		intel_buf_destroy(dst[0]);
		intel_buf_destroy(dst[1]);

		buf_ops_destroy(bops);
	}

	igt_fixture
		close(fd);
}
