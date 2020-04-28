/*
 * Copyright © 2020 Intel Corporation
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
 *  Karthik B S <karthik.b.s@intel.com>
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Test simultaneous modeset on all the supported pipes");

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb;
} data_t;

static void run_test(data_t *data, int valid_outputs)
{
	igt_output_t *output;
	igt_pipe_crc_t *pipe_crcs[IGT_MAX_PIPES] = { 0 };
	igt_crc_t ref_crcs[IGT_MAX_PIPES], new_crcs[IGT_MAX_PIPES];
	igt_display_t *display = &data->display;
	int width = 0, height = 0, i = 0;
	igt_pipe_t *pipe;
	igt_plane_t *plane;
	drmModeModeInfo *mode;

	for_each_connected_output(display, output) {
		mode = igt_output_get_mode(output);
		igt_assert(mode);

		igt_output_set_pipe(output, PIPE_NONE);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_pattern_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE, &data->fb);

	/* Collect reference CRC by Committing individually on all outputs*/
	for_each_connected_output(display, output) {
		pipe = &display->pipes[i];
		plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

		mode = NULL;

		pipe_crcs[i] = igt_pipe_crc_new(display->drm_fd, i,
						INTEL_PIPE_CRC_SOURCE_AUTO);

		igt_output_set_pipe(output, i);
		mode = igt_output_get_mode(output);
		igt_assert(mode);

		igt_plane_set_fb(plane, &data->fb);
		igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

		igt_display_commit2(display, COMMIT_ATOMIC);
		igt_pipe_crc_collect_crc(pipe_crcs[i], &ref_crcs[i]);
		igt_output_set_pipe(output, PIPE_NONE);
		i++;
	}

	i = 0;
	/* Simultaneously commit on all outputs */
	for_each_connected_output(display, output) {
		pipe = &display->pipes[i];
		plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

		mode = NULL;

		igt_output_set_pipe(output, i);
		mode = igt_output_get_mode(output);
		igt_assert(mode);

		igt_plane_set_fb(plane, &data->fb);
		igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
		i++;
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

	/* CRC Verification */
	for (i = 0; i < valid_outputs; i++) {
		igt_pipe_crc_collect_crc(pipe_crcs[i], &new_crcs[i]);
		igt_assert_crc_equal(&ref_crcs[i], &new_crcs[i]);
	}

	igt_plane_set_fb(plane, NULL);
	igt_remove_fb(data->drm_fd, &data->fb);
}

static void test_multipipe(data_t *data)
{
	igt_output_t *output;
	int valid_outputs = 0, num_pipes;

	num_pipes = igt_display_get_n_pipes(&data->display);
	for_each_connected_output(&data->display, output)
		valid_outputs++;

	igt_require_f(valid_outputs == num_pipes,
		      "Number of connected outputs(%d) not equal to the "
		      "number of pipes supported(%d)\n", valid_outputs, num_pipes);

	run_test(data, valid_outputs);
}

igt_main
{
	data_t data;
	drmModeResPtr res;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);

		res = drmModeGetResources(data.drm_fd);
		igt_assert(res);

		kmstest_unset_all_crtcs(data.drm_fd, res);
	}

	igt_describe("Verify if simultaneous modesets on all the supported "
		     "pipes is successful. Validate using CRC verification");
	igt_subtest("basic-max-pipe-crc-check")
		test_multipipe(&data);

	igt_fixture
		igt_display_fini(&data.display);
}
