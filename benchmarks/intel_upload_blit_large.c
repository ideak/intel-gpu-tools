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

/**
 * Roughly simulates repeatedly uploading frames of images, by uploading
 * the data all at once with pwrite, and then blitting it to another buffer.
 *
 * You might think of this like a movie player, but that wouldn't be entirely
 * accurate, since the access patterns of the memory would be different
 * (generally, smaller source image, upscaled, an thus different memory access
 * pattern in both texel fetch for the stretching and the destination writes).
 * However, some things like swfdec would be doing something like this since
 * they compute their data in host memory and upload the full sw rendered
 * frame.
 *
 * Additionally, those applications should be rendering at the screen refresh
 * rate, while this test has no limits, and so can get itself into the
 * working set larger than aperture size performance disaster.
 *
 * The current workload doing this path is pixmap upload for non-KMS.
 */

#include "igt.h"
#include "i915/gem_create.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <drm.h>
#include <i915_drm.h>

#define OBJECT_WIDTH	1280
#define OBJECT_HEIGHT	720

static double
get_time_in_secs(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (double)tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void
do_render(int i915, uint32_t dst_handle)
{
	struct drm_i915_gem_execbuffer2 exec = {};
	struct drm_i915_gem_exec_object2 obj[3] = {};
	struct drm_i915_gem_relocation_entry reloc[2];
	static uint32_t seed = 1;
	uint32_t data[OBJECT_WIDTH * OBJECT_HEIGHT];
	uint64_t size = OBJECT_WIDTH * OBJECT_HEIGHT * 4, bb_size = 4096;
	uint32_t src_handle, bb_handle, *bb;
	uint32_t gen = intel_gen(intel_get_drm_devid(i915));
	const bool has_64b_reloc = gen >= 8;
	int i;

	bb_handle = gem_create_from_pool(i915, &bb_size, REGION_SMEM);
	src_handle = gem_create_from_pool(i915, &size, REGION_SMEM);

	for (i = 0; i < OBJECT_WIDTH * OBJECT_HEIGHT; i++)
		data[i] = seed++;

	gem_write(i915, src_handle, 0, data, sizeof(data));

	/* Render the junk to the dst. */
	bb = gem_mmap__device_coherent(i915, bb_handle, 0, bb_size, PROT_WRITE);
	i = 0;
	bb[i++] = XY_SRC_COPY_BLT_CMD |
		  XY_SRC_COPY_BLT_WRITE_ALPHA |
		  XY_SRC_COPY_BLT_WRITE_RGB |
		  (6 + 2*(gen >= 8));
	bb[i++] = (3 << 24) | /* 32 bits */
		  (0xcc << 16) | /* copy ROP */
		  (OBJECT_WIDTH * 4) /* dst pitch */;
	bb[i++] = 0; /* dst x1,y1 */
	bb[i++] = (OBJECT_HEIGHT << 16) | OBJECT_WIDTH; /* dst x2,y2 */

	obj[0].handle = dst_handle;
	obj[0].offset = dst_handle * size;
	reloc[0].target_handle = dst_handle;
	reloc[0].presumed_offset = obj[0].offset;
	reloc[0].offset = sizeof(uint32_t) * i;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	bb[i++] = obj[0].offset;
	if (has_64b_reloc)
		bb[i++] = obj[0].offset >> 32;

	bb[i++] = 0; /* src x1,y1 */
	bb[i++] = OBJECT_WIDTH * 4; /* src pitch */

	obj[1].handle = src_handle;
	obj[1].offset = src_handle * size;
	reloc[1].target_handle = src_handle;
	reloc[1].presumed_offset = obj[1].offset;
	reloc[1].offset = sizeof(uint32_t) * i;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[1].write_domain = 0;
	bb[i++] = obj[1].offset;
	if (has_64b_reloc)
		bb[i++] = obj[1].offset >> 32;

	obj[2].handle = bb_handle;
	obj[2].relocs_ptr = to_user_pointer(reloc);
	obj[2].relocation_count = 2;

	bb[i++] = MI_BATCH_BUFFER_END;
	gem_munmap(bb, bb_size);

	exec.buffers_ptr = to_user_pointer(obj);
	exec.buffer_count = 3;
	exec.flags = gen >= 6 ? I915_EXEC_BLT : 0 | I915_EXEC_NO_RELOC;

	gem_execbuf(i915, &exec);
}

int main(int argc, char **argv)
{
	double start_time, end_time;
	uint32_t dst_handle;
	int i915, i;

	i915 = drm_open_driver(DRIVER_INTEL);
	dst_handle = gem_create(i915, OBJECT_WIDTH * OBJECT_HEIGHT * 4);

	/* Prep loop to get us warmed up. */
	for (i = 0; i < 60; i++)
		do_render(i915, dst_handle);
	gem_sync(i915, dst_handle);

	/* Do the actual timing. */
	start_time = get_time_in_secs();
	for (i = 0; i < 200; i++)
		do_render(i915, dst_handle);
	gem_sync(i915, dst_handle);

	end_time = get_time_in_secs();

	printf("%d iterations in %.03f secs: %.01f MB/sec\n", i,
	       end_time - start_time,
	       (double)i * OBJECT_WIDTH * OBJECT_HEIGHT * 4 / 1024.0 / 1024.0 /
	       (end_time - start_time));

	close(i915);
}
