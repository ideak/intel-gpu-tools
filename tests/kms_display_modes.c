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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Author:
 *  Jeevan B <jeevan.b@intel.com>
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Test Display Modes");

typedef struct {
	int drm_fd;
	igt_display_t display;
	int n_pipes;
} data_t;

static void run_extendedmode_basic(data_t *data,
				   enum pipe pipe1, igt_output_t *output1,
				   enum pipe pipe2, igt_output_t *output2)
{
	struct igt_fb fb, fbs[2];
	drmModeModeInfo *mode[2];
	igt_display_t *display = &data->display;
	igt_plane_t *plane[2];
	igt_pipe_crc_t *pipe_crc[2] = { 0 };
	igt_crc_t ref_crc[2], crc[2];
	int width, height;
	cairo_t *cr;

	igt_display_reset(display);

	igt_output_set_pipe(output1, pipe1);
	igt_output_set_pipe(output2, pipe2);

	mode[0] = igt_output_get_mode(output1);
	mode[1] = igt_output_get_mode(output2);

	pipe_crc[0] = igt_pipe_crc_new(data->drm_fd, pipe1, IGT_PIPE_CRC_SOURCE_AUTO);
	pipe_crc[1] = igt_pipe_crc_new(data->drm_fd, pipe2, IGT_PIPE_CRC_SOURCE_AUTO);

	igt_create_color_fb(data->drm_fd, mode[0]->hdisplay, mode[0]->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 1, 0, 0, &fbs[0]);
	igt_create_color_fb(data->drm_fd, mode[1]->hdisplay, mode[1]->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 1, &fbs[1]);

	plane[0] = igt_pipe_get_plane_type(&display->pipes[pipe1], DRM_PLANE_TYPE_PRIMARY);
	plane[1] = igt_pipe_get_plane_type(&display->pipes[pipe2], DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(plane[0], &fbs[0]);
	igt_fb_set_size(&fbs[0], plane[0], mode[0]->hdisplay, mode[0]->vdisplay);
	igt_plane_set_size(plane[0], mode[0]->hdisplay, mode[0]->vdisplay);

	igt_plane_set_fb(plane[1], &fbs[1]);
	igt_fb_set_size(&fbs[1], plane[1], mode[1]->hdisplay, mode[1]->vdisplay);
	igt_plane_set_size(plane[1], mode[1]->hdisplay, mode[1]->vdisplay);

	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_collect_crc(pipe_crc[0], &ref_crc[0]);
	igt_pipe_crc_collect_crc(pipe_crc[1], &ref_crc[1]);

	/*Create a big framebuffer and display it on 2 monitors*/
	width = mode[0]->hdisplay + mode[1]->hdisplay;
	height = max(mode[0]->vdisplay, mode[1]->vdisplay);

	igt_create_fb(data->drm_fd, width, height,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &fb);
	cr = igt_get_cairo_ctx(data->drm_fd, &fb);
	igt_paint_color(cr, 0, 0, mode[0]->hdisplay, mode[0]->vdisplay, 1, 0, 0);
	igt_paint_color(cr, mode[0]->hdisplay, 0, mode[1]->hdisplay, mode[1]->vdisplay, 0, 0, 1);
	igt_put_cairo_ctx(cr);

	igt_plane_set_fb(plane[0], &fb);
	igt_fb_set_position(&fb, plane[0], 0, 0);
	igt_fb_set_size(&fb, plane[0], mode[0]->hdisplay, mode[0]->vdisplay);

	igt_plane_set_fb(plane[1], &fb);
	igt_fb_set_position(&fb, plane[1], mode[0]->hdisplay, 0);
	igt_fb_set_size(&fb, plane[1], mode[1]->hdisplay, mode[1]->vdisplay);

	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_collect_crc(pipe_crc[0], &crc[0]);
	igt_pipe_crc_collect_crc(pipe_crc[1], &crc[1]);

	/*Clean up*/
	igt_remove_fb(data->drm_fd, &fbs[0]);
	igt_remove_fb(data->drm_fd, &fbs[1]);
	igt_remove_fb(data->drm_fd, &fb);

	igt_pipe_crc_free(pipe_crc[0]);
	igt_pipe_crc_free(pipe_crc[1]);

	igt_output_set_pipe(output1, PIPE_NONE);
	igt_output_set_pipe(output2, PIPE_NONE);

	igt_plane_set_fb(igt_pipe_get_plane_type(&display->pipes[pipe1],
			  DRM_PLANE_TYPE_PRIMARY), NULL);
	igt_plane_set_fb(igt_pipe_get_plane_type(&display->pipes[pipe2],
			  DRM_PLANE_TYPE_PRIMARY), NULL);
	igt_display_commit2(display, COMMIT_ATOMIC);

	/*Compare CRC*/
	igt_assert_crc_equal(&crc[0], &ref_crc[0]);
	igt_assert_crc_equal(&crc[1], &ref_crc[1]);
}

#define for_each_connected_output_local(display, output)		\
	for (int j__ = 0;  assert(igt_can_fail()), j__ < (display)->n_outputs; j__++)	\
		for_each_if ((((output) = &(display)->outputs[j__]), \
			      igt_output_is_connected((output))))

#define for_each_valid_output_on_pipe_local(display, pipe, output) \
	for_each_connected_output_local((display), (output)) \
		for_each_if (igt_pipe_connector_valid((pipe), (output)))

static void run_extendedmode_test(data_t *data) {
	enum pipe pipe1, pipe2;
	igt_output_t *output1, *output2;
	igt_display_t *display = &data->display;

	igt_display_reset(display);

	for_each_pipe(display, pipe1) {
		for_each_valid_output_on_pipe(display, pipe1, output1) {
			for_each_pipe(display, pipe2) {
				if (pipe1 == pipe2)
					continue;

				for_each_valid_output_on_pipe_local(display, pipe2, output2) {
					if (output1 == output2)
						continue;

					igt_display_reset(display);

					igt_output_set_pipe(output1, pipe1);
					igt_output_set_pipe(output2, pipe2);

					if (!i915_pipe_output_combo_valid(display))
						continue;

					igt_dynamic_f("pipe-%s-%s-pipe-%s-%s",
						      kmstest_pipe_name(pipe1),
						      igt_output_name(output1),
						      kmstest_pipe_name(pipe2),
						      igt_output_name(output2))
						run_extendedmode_basic(data,
								pipe1, output1,
								pipe2, output2);
				}
			}
			/*
			 * For simulation env, no need to run
			 * test with each valid output on pipe.
			 */
			if (igt_run_in_simulation())
				break;
		}
	}
}

igt_main
{
	data_t data;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
	}

	igt_describe("Test for validating display extended mode with a pair of connected displays");
	igt_subtest_with_dynamic("extended-mode-basic")
		run_extendedmode_test(&data);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
