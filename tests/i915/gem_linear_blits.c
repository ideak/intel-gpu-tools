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

/** @file gem_linear_blits.c
 *
 * This is a test of doing many blits, with a working set
 * larger than the aperture size.
 *
 * The goal is to simply ensure the basics work.
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

IGT_TEST_DESCRIPTION("Test doing many blits with a working set larger than the"
		     " aperture size.");

#define WIDTH 512
#define HEIGHT 512

/* We don't have alignment detection yet, so assume worst case scenario */
#define ALIGNMENT (2048*1024)

static uint32_t linear[WIDTH*HEIGHT];

static void copy(int fd, uint64_t ahnd, uint32_t dst, uint32_t src,
		 uint64_t dst_offset, uint64_t src_offset, bool do_relocs)
{
	uint32_t batch[12];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_execbuffer2 exec;
	int i = 0;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = dst;
	obj[0].offset = CANONICAL(dst_offset);
	obj[0].flags = EXEC_OBJECT_WRITE | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[1].handle = src;
	obj[1].offset = CANONICAL(src_offset);
	obj[1].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	obj[2].handle = gem_create(fd, 4096);
	obj[2].offset = intel_allocator_alloc(ahnd, obj[2].handle,
			4096, ALIGNMENT);
	obj[2].offset = CANONICAL(obj[2].offset);
	obj[2].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	batch[i++] = XY_SRC_COPY_BLT_CMD |
		  XY_SRC_COPY_BLT_WRITE_ALPHA |
		  XY_SRC_COPY_BLT_WRITE_RGB;
	if (intel_gen(intel_get_drm_devid(fd)) >= 8)
		batch[i - 1] |= 8;
	else
		batch[i - 1] |= 6;

	batch[i++] = (3 << 24) | /* 32 bits */
		  (0xcc << 16) | /* copy ROP */
		  WIDTH*4;
	batch[i++] = 0; /* dst x1,y1 */
	batch[i++] = (HEIGHT << 16) | WIDTH; /* dst x2,y2 */
	batch[i++] = obj[0].offset;
	if (intel_gen(intel_get_drm_devid(fd)) >= 8)
		batch[i++] = obj[0].offset >> 32;
	batch[i++] = 0; /* src x1,y1 */
	batch[i++] = WIDTH*4;
	batch[i++] = obj[1].offset;
	if (intel_gen(intel_get_drm_devid(fd)) >= 8)
		batch[i++] = obj[1].offset >> 32;
	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	gem_write(fd, obj[2].handle, 0, batch, i * sizeof(batch[0]));

	memset(reloc, 0, sizeof(reloc));
	reloc[0].target_handle = dst;
	reloc[0].delta = 0;
	reloc[0].offset = 4 * sizeof(batch[0]);
	reloc[0].presumed_offset = obj[0].offset;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;

	reloc[1].target_handle = src;
	reloc[1].delta = 0;
	reloc[1].offset = 7 * sizeof(batch[0]);
	if (intel_gen(intel_get_drm_devid(fd)) >= 8)
		reloc[1].offset += sizeof(batch[0]);
	reloc[1].presumed_offset = obj[1].offset;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[1].write_domain = 0;

	if (do_relocs) {
		obj[2].relocation_count = ARRAY_SIZE(reloc);
		obj[2].relocs_ptr = to_user_pointer(reloc);
	} else {
		obj[0].flags |= EXEC_OBJECT_PINNED;
		obj[1].flags |= EXEC_OBJECT_PINNED;
		obj[2].flags |= EXEC_OBJECT_PINNED;
	}

	memset(&exec, 0, sizeof(exec));
	exec.buffers_ptr = to_user_pointer(obj);
	exec.buffer_count = ARRAY_SIZE(obj);
	exec.batch_len = i * sizeof(batch[0]);
	exec.flags = gem_has_blt(fd) ? I915_EXEC_BLT : 0;
	gem_execbuf(fd, &exec);

	intel_allocator_free(ahnd, obj[2].handle);
	gem_close(fd, obj[2].handle);
}

static uint32_t
create_bo(int fd, uint32_t val)
{
	uint32_t handle;
	int i;

	handle = gem_create(fd, sizeof(linear));

	/* Fill the BO with dwords starting at val */
	for (i = 0; i < WIDTH*HEIGHT; i++)
		linear[i] = val++;
	gem_write(fd, handle, 0, linear, sizeof(linear));

	return handle;
}

static void
check_bo(int fd, uint32_t handle, uint32_t val)
{
	int num_errors;
	int i;

	gem_read(fd, handle, 0, linear, sizeof(linear));

	num_errors = 0;
	for (i = 0; i < WIDTH*HEIGHT; i++) {
		if (linear[i] != val && num_errors++ < 32)
			igt_warn("[%08x] Expected 0x%08x, found 0x%08x (difference 0x%08x)\n",
				 i * 4, val, linear[i], val ^ linear[i]);
		val++;
	}
	igt_assert_eq(num_errors, 0);
}

static void run_test(int fd, int count, bool do_relocs)
{
	uint32_t *handle, *start_val;
	uint64_t *offset, ahnd;
	uint32_t start = 0;
	int i;

	ahnd = intel_allocator_open(fd, 0, do_relocs ?
					    INTEL_ALLOCATOR_RELOC :
					    INTEL_ALLOCATOR_SIMPLE);

	handle = malloc(sizeof(uint32_t) * count * 2);
	offset = calloc(1, sizeof(uint64_t) * count);
	igt_assert_f(handle && offset, "Allocation failed\n");
	start_val = handle + count;

	for (i = 0; i < count; i++) {
		handle[i] = create_bo(fd, start);

		offset[i] = intel_allocator_alloc(ahnd, handle[i],
						  sizeof(linear), ALIGNMENT);

		start_val[i] = start;
		start += 1024 * 1024 / 4;
	}

	for (i = 0; i < count; i++) {
		int src = random() % count;
		int dst = random() % count;

		if (src == dst)
			continue;
		copy(fd, ahnd, handle[dst], handle[src],
		     offset[dst], offset[src], do_relocs);

		start_val[dst] = start_val[src];
	}

	for (i = 0; i < count; i++) {
		check_bo(fd, handle[i], start_val[i]);
		intel_allocator_free(ahnd, handle[i]);
		gem_close(fd, handle[i]);
	}

	free(handle);
	free(offset);

	intel_allocator_close(ahnd);
}

#define MAX_32b ((1ull << 32) - 4096)

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint64_t count = 0;
	bool do_relocs;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		do_relocs = !gem_uses_ppgtt(fd);

		count = gem_aperture_size(fd);
		if (count >> 32)
			count = MAX_32b;
		else
			do_relocs = true;

		count = 3 + count / (1024*1024);
		igt_require(count > 1);
		intel_require_memory(count, sizeof(linear), CHECK_RAM);

		igt_debug("Using %'"PRIu64" 1MiB buffers\n", count);
		count = (count + ncpus - 1) / ncpus;
	}

	igt_subtest("basic")
		run_test(fd, 2, do_relocs);

	igt_subtest("normal") {
		intel_allocator_multiprocess_start();
		igt_fork(child, ncpus)
			run_test(fd, count, do_relocs);
		igt_waitchildren();
		intel_allocator_multiprocess_stop();
	}

	igt_subtest("interruptible") {
		intel_allocator_multiprocess_start();
		igt_fork_signal_helper();
		igt_fork(child, ncpus)
			run_test(fd, count, do_relocs);
		igt_waitchildren();
		igt_stop_signal_helper();
		intel_allocator_multiprocess_stop();
	}
}
