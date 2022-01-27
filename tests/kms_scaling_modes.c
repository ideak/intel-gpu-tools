/*
 * Copyright © 2022 Intel Corporation
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
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author:
 *     Swati Sharma <swati2.sharma@intel.com>
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Test display scaling modes");

/* Common test data */
typedef struct data {
	igt_display_t display;
	int drm_fd;
} data_t;

static void test_scaling_mode_on_output(igt_display_t *display, const enum pipe pipe,
					igt_output_t *output, uint32_t flags)
{
	igt_plane_t *primary, *sprite;
	drmModeModeInfo mode;
	struct igt_fb red, blue;
	int ret;

	igt_output_set_pipe(output, pipe);
	mode = *igt_output_get_mode(output);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	sprite = igt_output_get_plane_type(output, DRM_PLANE_TYPE_OVERLAY);

	igt_create_color_fb(display->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_NONE,
			    0.f, 0.f, 1.f, &blue);

	igt_create_color_fb(display->drm_fd, 640, 480,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_NONE,
			    1.f, 0.f, 0.f, &red);

	igt_plane_set_fb(primary, &blue);
	igt_plane_set_fb(sprite, &red);

	igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	mode.hdisplay = 640;
	mode.vdisplay = 480;
	igt_output_override_mode(output, &mode);

	igt_plane_set_fb(sprite, NULL);
	igt_plane_set_fb(primary, &red);

	igt_output_set_prop_value(output, IGT_CONNECTOR_SCALING_MODE, flags);

	/* Don't pass ALLOW_MODESET with overridden mode, force fastset */
	ret = igt_display_try_commit_atomic(display, 0, NULL);

	igt_remove_fb(display->drm_fd, &red);
	igt_remove_fb(display->drm_fd, &blue);

	igt_skip_on_f(ret == -EINVAL, "Scaling mode not supported\n");
}

/* Returns true if an output supports scaling mode property */
static bool has_scaling_mode(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_SCALING_MODE) &&
	       igt_output_get_prop(output, IGT_CONNECTOR_SCALING_MODE);
}

static void test_scaling_mode(data_t *data, uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		if (!has_scaling_mode(output))
			continue;

		igt_dynamic_f("%s-pipe-%s", output->name, kmstest_pipe_name(pipe))
			test_scaling_mode_on_output(display, pipe, output, flags);

		igt_display_reset(display);
	}
}

igt_main
{
	data_t data = {};

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_require(data.drm_fd >= 0);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		igt_display_require_output(&data.display);
	}

	igt_describe("Tests full display scaling mode");
	igt_subtest_with_dynamic("scaling-mode-full")
		test_scaling_mode(&data, DRM_MODE_SCALE_FULLSCREEN);
	igt_describe("Tests center display scaling mode");
	igt_subtest_with_dynamic("scaling-mode-center")
		test_scaling_mode(&data, DRM_MODE_SCALE_CENTER);
	igt_describe("Tests full aspect display scaling mode");
	igt_subtest_with_dynamic("scaling-mode-full-aspect")
		test_scaling_mode(&data, DRM_MODE_SCALE_ASPECT);
	igt_describe("Tests none display scaling mode (no scaling)");
	igt_subtest_with_dynamic("scaling-mode-none")
		test_scaling_mode(&data, DRM_MODE_SCALE_NONE);

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
