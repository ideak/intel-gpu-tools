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
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION(
   "pwrite to a snooped bo then make it uncached and check that the GPU sees the data.");

static int fd;
static struct buf_ops *bops;

static void blit(struct intel_buf *dst, struct intel_buf *src,
		 unsigned int width, unsigned int height,
		 unsigned int dst_pitch, unsigned int src_pitch)
{
	struct intel_bb *ibb;

	ibb = intel_bb_create(fd, 4096);
	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_blit_start(ibb, 0);
	intel_bb_out(ibb, (3 << 24) | /* 32 bits */
		  (0xcc << 16) | /* copy ROP */
		  dst_pitch);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, height << 16 | width);
	intel_bb_emit_reloc_fenced(ibb, dst->handle, I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER, 0, dst->addr.offset);
	intel_bb_out(ibb, 0 << 16 | 0);
	intel_bb_out(ibb, src_pitch);
	intel_bb_emit_reloc_fenced(ibb, src->handle, I915_GEM_DOMAIN_RENDER,
				   0, 0, src->addr.offset);

	if (ibb->gen >= 6) {
		intel_bb_out(ibb, XY_SETUP_CLIP_BLT_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
	}

	intel_bb_flush_blit(ibb);
	intel_bb_destroy(ibb);
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

static void test(int w, int h)
{
	int object_size = w * h * 4;
	struct intel_buf *src, *dst;
	void *buf;

	src = intel_buf_create(bops, w, h, 32, 0, I915_TILING_NONE,
			       I915_COMPRESSION_NONE);
	dst = intel_buf_create(bops, w, h, 32, 0, I915_TILING_NONE,
			       I915_COMPRESSION_NONE);

	buf = malloc(object_size);
	igt_assert(buf);
	memset(buf, 0xff, object_size);

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);

	gem_set_caching(fd, src->handle, I915_CACHING_CACHED);

	gem_write(fd, src->handle, 0, buf, object_size);

	gem_set_caching(fd, src->handle, I915_CACHING_NONE);

	blit(dst, src, w, h, w * 4, h * 4);

	memset(buf, 0x00, object_size);
	gem_read(fd, dst->handle, 0, buf, object_size);

	igt_assert(memchr_inv(buf, 0xff, object_size) == NULL);

	intel_buf_destroy(src);
	intel_buf_destroy(dst);
}

igt_simple_main
{
	fd = drm_open_driver(DRIVER_INTEL);
	igt_require_gem(fd);
	gem_require_blitter(fd);
	gem_require_pread_pwrite(fd);

	bops = buf_ops_create(fd);

	test(256, 256);

	buf_ops_destroy(bops);
	close(fd);
}
