/*
 * Copyright © 2018 Intel Corporation
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
 */
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("A very simple workload for the VME media block.");

#define WIDTH	64
#define STRIDE	(WIDTH)
#define HEIGHT	64

#define INPUT_SIZE	(WIDTH * HEIGHT * sizeof(char) * 1.5)
#define OUTPUT_SIZE	(56*sizeof(int))

static uint64_t switch_off_n_bits(uint64_t mask, unsigned int n)
{
	unsigned int i;

	igt_assert(n > 0 && n <= (sizeof(mask) * 8));
	igt_assert(n <= __builtin_popcount(mask));

	for (i = 0; n && i < (sizeof(mask) * 8); i++) {
		uint64_t bit = 1ULL << i;

		if (bit & mask) {
			mask &= ~bit;
			n--;
		}
	}

	return mask;
}

static void shut_non_vme_subslices(int drm_fd, uint32_t ctx)
{
	struct drm_i915_gem_context_param_sseu sseu = { };
	struct drm_i915_gem_context_param arg = {
		.param = I915_CONTEXT_PARAM_SSEU,
		.ctx_id = ctx,
		.size = sizeof(sseu),
		.value = to_user_pointer(&sseu),
	};
	int ret;

	if (__gem_context_get_param(drm_fd, &arg))
		return; /* no sseu support */

	ret = __gem_context_set_param(drm_fd, &arg);
	igt_assert(ret == 0 || ret == -ENODEV || ret == -EINVAL);
	if (ret)
		return; /* no sseu support */

	/* shutdown half subslices */
	sseu.subslice_mask =
		switch_off_n_bits(sseu.subslice_mask,
				  __builtin_popcount(sseu.subslice_mask) / 2);

	gem_context_set_param(drm_fd, &arg);
}

igt_simple_main
{
	struct buf_ops *bops;
	struct intel_buf src, dst;
	int drm_fd;
	uint32_t devid, ctx;
	igt_vme_func_t media_vme;

	drm_fd = drm_open_driver(DRIVER_INTEL);
	igt_require_gem(drm_fd);

	devid = intel_get_drm_devid(drm_fd);

	media_vme = igt_get_media_vme_func(devid);
	igt_require_f(media_vme, "no media-vme function\n");

	bops = buf_ops_create(drm_fd);

	/* Use WIDTH/HEIGHT/STRIDE according to INPUT_SIZE */
	intel_buf_init(bops, &src, WIDTH, HEIGHT * 1.5, 8, STRIDE,
		       I915_TILING_NONE, 0);

	/* This comes from OUTPUT_SIZE requirements */
	intel_buf_init(bops, &dst, 56, sizeof(int), 8, 56,
		       I915_TILING_NONE, 0);
	dst.stride = 1;

	ctx = gem_context_create(drm_fd);
	igt_assert(ctx);

	/* ICL hangs if non-VME enabled slices are enabled with a VME kernel. */
	if (intel_gen(devid) == 11)
		shut_non_vme_subslices(drm_fd, ctx);

	igt_fork_hang_detector(drm_fd);

	media_vme(drm_fd, ctx, &src, WIDTH, HEIGHT, &dst);

	gem_sync(drm_fd, dst.handle);

	igt_stop_hang_detector();
}
