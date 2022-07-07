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
 */

#include "igt.h"
#include <errno.h>


IGT_TEST_DESCRIPTION("Check that the legacy set colorkey ioctl only works on sprite planes.");

static int drm_fd;
static igt_display_t display;
static int p;
static igt_plane_t *plane;
static uint32_t max_id;

static void test_plane(uint32_t plane_id, int expected_ret)
{
	struct drm_intel_sprite_colorkey ckey = {
		.plane_id = plane_id,
	};

	igt_assert(drmCommandWrite(drm_fd, DRM_I915_SET_SPRITE_COLORKEY, &ckey,
				   sizeof(ckey)) == expected_ret);
}

igt_main
{
	igt_fixture {
		drm_fd = drm_open_driver_master(DRIVER_INTEL);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, drm_fd);
		for_each_pipe(&display, p) {
			for_each_plane_on_pipe(&display, p, plane) {
				max_id = max(max_id, plane->drm_plane->plane_id);
			}
		}

	}

	igt_describe("Test to check the legacy set colorkey ioctl "
		     "only works for sprite planes.\n");
	igt_subtest_with_dynamic("basic") {
		for_each_pipe(&display, p) {
			igt_dynamic_f("pipe-%s", kmstest_pipe_name(p)) {
				for_each_plane_on_pipe(&display, p, plane) {
					bool is_valid = (plane->type == DRM_PLANE_TYPE_PRIMARY ||
							 plane->type == DRM_PLANE_TYPE_CURSOR);

					test_plane(plane->drm_plane->plane_id,
						   is_valid ? -ENOENT : 0);
					max_id = max(max_id, plane->drm_plane->plane_id);
				}
			}
		}
	}

	/* try some invalid IDs too */
	igt_describe("Check invalid plane id's, zero and outrange\n");
	igt_subtest_with_dynamic("invalid-plane") {
		igt_dynamic("zero-id")
			test_plane(0, -ENOENT);
		igt_dynamic("outrange-id")
			test_plane(max_id + 1, -ENOENT);
	}

	igt_fixture {
		igt_display_fini(&display);
		close(drm_fd);
	}
}
