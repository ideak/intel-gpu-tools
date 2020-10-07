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

#define MAX_HDISPLAY_PER_PIPE 5120

IGT_TEST_DESCRIPTION("Test big joiner");

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb;
	int mode_number;
	int n_pipes;
	uint32_t big_joiner_output_id;
} data_t;

static void test_invalid_modeset(data_t *data)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	igt_output_t *output, *big_joiner_output = NULL, *second_output = NULL;
	int width = 0, height = 0, i, ret;
	igt_pipe_t *pipe;
	igt_plane_t *plane;

	for_each_connected_output(display, output) {
		mode = &output->config.connector->modes[0];

		if (data->big_joiner_output_id == output->id) {
			mode = &output->config.connector->modes[data->mode_number];
			big_joiner_output = output;
		} else if (second_output == NULL) {
			second_output = output;
		}

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_pattern_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE, &data->fb);

	for_each_pipe(display, i) {
		if (i < (data->n_pipes - 1)) {
			igt_output_set_pipe(big_joiner_output, i);

			mode = &big_joiner_output->config.connector->modes[data->mode_number];
			igt_output_override_mode(big_joiner_output, mode);

			pipe = &display->pipes[i];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

			igt_plane_set_fb(plane, &data->fb);
			igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			igt_display_commit2(display, COMMIT_ATOMIC);

			igt_output_set_pipe(second_output, i + 1);

			mode = igt_output_get_mode(second_output);

			pipe = &display->pipes[i + 1];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

			igt_plane_set_fb(plane, &data->fb);
			igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			/* This commit is expectd to fail as this pipe is being used for big joiner */
			ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
							    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			igt_assert_lt(ret, 0);

			igt_output_set_pipe(big_joiner_output, PIPE_NONE);
			igt_output_set_pipe(second_output, PIPE_NONE);

			pipe = &display->pipes[i];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

			/*
			 * Do not explicitly set the plane of the second output to NULL,
			 * as it is the adjacent pipe to the big joiner output and
			 * setting the big joiner plane to NULL will take care of this.
			 */
			igt_plane_set_fb(plane, NULL);
			igt_display_commit2(display, COMMIT_ATOMIC);
			igt_output_override_mode(big_joiner_output, NULL);
		}
	}

	for_each_pipe(display, i) {
		if (i < (data->n_pipes - 1)) {
			igt_output_set_pipe(second_output, i + 1);

			mode = igt_output_get_mode(second_output);

			pipe = &display->pipes[i + 1];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

			igt_plane_set_fb(plane, &data->fb);
			igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			igt_display_commit2(display, COMMIT_ATOMIC);

			igt_output_set_pipe(big_joiner_output, i);

			mode = &big_joiner_output->config.connector->modes[data->mode_number];
			igt_output_override_mode(big_joiner_output, mode);

			pipe = &display->pipes[i];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

			igt_plane_set_fb(plane, &data->fb);
			igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			/* This commit is expected to fail as the adjacent pipe is already in use*/
			ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
							    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			igt_assert_lt(ret, 0);

			igt_output_set_pipe(big_joiner_output, PIPE_NONE);
			igt_output_set_pipe(second_output, PIPE_NONE);
			igt_plane_set_fb(plane, NULL);

			pipe = &display->pipes[i + 1];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
			igt_plane_set_fb(plane, NULL);

			igt_display_commit2(display, COMMIT_ATOMIC);

			igt_output_override_mode(big_joiner_output, NULL);
		}
	}

	igt_remove_fb(data->drm_fd, &data->fb);
}

static void test_basic_modeset(data_t *data)
{
	drmModeModeInfo *mode;
	igt_output_t *output, *big_joiner_output = NULL;
	igt_display_t *display = &data->display;
	int width = 0, height = 0, i;
	igt_pipe_t *pipe;
	igt_plane_t *plane;

	for_each_connected_output(display, output) {
		if (data->big_joiner_output_id == output->id) {
			mode = &output->config.connector->modes[data->mode_number];
			big_joiner_output = output;
			width = mode->hdisplay;
			height = mode->vdisplay;
			break;
		}
	}

	igt_create_pattern_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE, &data->fb);

	for_each_pipe(display, i) {
		if (i < (data->n_pipes - 1)) {
			igt_output_set_pipe(big_joiner_output, i);

			mode = &big_joiner_output->config.connector->modes[data->mode_number];
			igt_output_override_mode(big_joiner_output, mode);

			pipe = &display->pipes[i];
			plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

			igt_plane_set_fb(plane, &data->fb);
			igt_fb_set_size(&data->fb, plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			igt_display_commit2(display, COMMIT_ATOMIC);

			igt_output_set_pipe(big_joiner_output, PIPE_NONE);
			igt_plane_set_fb(plane, NULL);
			igt_display_commit2(display, COMMIT_ATOMIC);
		}
	}

	igt_remove_fb(data->drm_fd, &data->fb);
}

igt_main
{
	data_t data;
	bool big_joiner_mode_found = false;
	igt_output_t *output;
	drmModeModeInfo *mode;
	int valid_output = 0, i;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);

		for_each_connected_output(&data.display, output) {
			if (!big_joiner_mode_found) {
				for (i = 0; i < output->config.connector->count_modes; i++) {
					mode = &output->config.connector->modes[i];
					if (mode->hdisplay > MAX_HDISPLAY_PER_PIPE) {
						big_joiner_mode_found = true;
						data.mode_number = i;
						data.big_joiner_output_id = output->id;
						break;
					}
				}
			}
			valid_output++;
		}

		data.n_pipes = 0;
		for_each_pipe(&data.display, i)
			data.n_pipes++;

		igt_require_f(big_joiner_mode_found, "No output with 5k+ mode found\n");
	}

	igt_describe("Verify the basic modeset on big joiner mode on all pipes");
	igt_subtest("basic")
		test_basic_modeset(&data);

	igt_describe("Verify if the modeset on the adjoining pipe is rejected "
		     "when the pipe is active with a big joiner modeset");
	igt_subtest("invalid-modeset") {
		igt_require_f(valid_output > 1, "No valid Second output found\n");
		test_invalid_modeset(&data);
	}

	igt_fixture
		igt_display_fini(&data.display);
}
