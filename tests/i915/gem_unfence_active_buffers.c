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

/** @file gem_unfence_active_buffers.c
 *
 * Testcase: Check for use-after free in the fence stealing code
 *
 * If we're stealing the fence of a active object where the active list is the
 * only thing holding a reference, we need to be careful not to access the old
 * object we're stealing the fence from after that reference has been dropped by
 * retire_requests.
 *
 * Note that this needs slab poisoning enabled in the kernel to reliably hit the
 * problem - the race window is too small.
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

IGT_TEST_DESCRIPTION("Check for use-after-free in the fence stealing code.");

static uint32_t create_tiled(int i915)
{
	uint32_t handle;

	handle = gem_create(i915, 1 << 20);
	gem_set_tiling(i915, handle, I915_TILING_X, 1024);

	return handle;
}

igt_simple_main
{
	int i915, num_fences;
	igt_spin_t *spin;

	i915 = drm_open_driver(DRIVER_INTEL);
	igt_require_gem(i915);

	spin = igt_spin_new(i915);

	num_fences = gem_available_fences(i915);
	igt_info("creating havoc on %i fences\n", num_fences);

	for (int i = 0; i < num_fences + 3; i++) {
		struct drm_i915_gem_exec_object2 obj[2] = {
			{
				.handle = create_tiled(i915),
				.flags = EXEC_OBJECT_NEEDS_FENCE,
			},
			spin->obj[IGT_SPIN_BATCH],
		};
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(obj),
			.buffer_count = ARRAY_SIZE(obj),
		};
		gem_execbuf(i915, &execbuf);
		gem_close(i915, obj[0].handle);
	}

	igt_spin_free(i915, spin);
}
