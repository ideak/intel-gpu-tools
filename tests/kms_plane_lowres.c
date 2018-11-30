/*
 * Copyright © 2016 Intel Corporation
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

#include "igt.h"
#include "drmtest.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

IGT_TEST_DESCRIPTION("Test atomic mode setting with a plane by switching between high and low resolutions");

#define MAX_CRCS          1
#define SIZE            256
#define LOOP_FOREVER     -1

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb *fb;
} data_t;

static drmModeModeInfo
get_lowres_mode(int drmfd, drmModeModeInfo *mode_default)
{
	drmModeRes *mode_resources = drmModeGetResources(drmfd);
	drmModeModeInfo mode;
	drmModeModeInfo std_1024_mode = {
		.clock = 65000,
		.hdisplay = 1024,
		.hsync_start = 1048,
		.hsync_end = 1184,
		.htotal = 1344,
		.hskew = 0,
		.vdisplay = 768,
		.vsync_start = 771,
		.vsync_end = 777,
		.vtotal = 806,
		.vscan = 0,
		.vrefresh = 60,
		.flags = 0xA,
		.type = 0x40,
		.name = "Custom 1024x768",
	};
	bool found;
	int limit = mode_default->vdisplay-SIZE;
	int i, j;

	if (!mode_resources) {
		igt_warn("drmModeGetResources failed: %s\n", strerror(errno));
		return std_1024_mode;
	}

	found = false;
	for (i = 0; i < mode_resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnectorCurrent(drmfd,
						       mode_resources->connectors[i]);
		if (!connector) {
			igt_warn("could not get connector %i: %s\n",
				 mode_resources->connectors[i], strerror(errno));
			continue;
		}

		if (!connector->count_modes)
			continue;

		for (j = 0; j < connector->count_modes; j++) {
			mode = connector->modes[j];
			if (mode.vdisplay < limit) {
				found = true;
				break;
			}
		}

		drmModeFreeConnector(connector);
	}

	drmModeFreeResources(mode_resources);

	if (!found)
		return std_1024_mode;

	return mode;
}

static void
test_fini(data_t *data, igt_output_t *output, enum pipe pipe)
{
	igt_plane_t *plane;

	/* restore original mode */
	igt_output_override_mode(output, NULL);

	for_each_plane_on_pipe(&data->display, pipe, plane)
		igt_plane_set_fb(plane, NULL);

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_ANY);

	free(data->fb);
	data->fb = NULL;
}

static void
check_mode(drmModeModeInfo *mode1, drmModeModeInfo *mode2)
{
	igt_assert_eq(mode1->hdisplay, mode2->hdisplay);
	igt_assert_eq(mode1->vdisplay, mode2->vdisplay);
	igt_assert_eq(mode1->vrefresh, mode2->vrefresh);
}

static drmModeModeInfo *
test_setup(data_t *data, enum pipe pipe, uint64_t modifier,
	   igt_output_t *output)
{
	drmModeModeInfo *mode;
	int size;
	int i = 1, x, y;
	igt_plane_t *plane;

	igt_skip_on(!igt_display_has_format_mod(&data->display,
						DRM_FORMAT_XRGB8888,
						modifier));

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);

	data->fb = calloc(data->display.pipes[pipe].n_planes, sizeof(struct igt_fb));
	igt_assert_f(data->fb, "Failed to allocate memory for %d FBs\n",
	             data->display.pipes[pipe].n_planes);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    modifier,
			    0.0, 0.0, 1.0,
			    &data->fb[0]);

	/* yellow sprite plane in lower left corner */
	for_each_plane_on_pipe(&data->display, pipe, plane) {
		uint64_t plane_modifier;
		uint32_t plane_format;

		if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
			igt_plane_set_fb(plane, &data->fb[0]);
			continue;
		}

		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			size = 64;
		else
			size = SIZE;

		x = 0;
		y = mode->vdisplay - size;

		plane_format = plane->type == DRM_PLANE_TYPE_CURSOR ?
			DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;

		plane_modifier = plane->type == DRM_PLANE_TYPE_CURSOR ?
			LOCAL_DRM_FORMAT_MOD_NONE : modifier;

		igt_skip_on(!igt_plane_has_format_mod(plane, plane_format,
						      plane_modifier));

		igt_create_color_fb(data->drm_fd,
				    size, size,
				    plane_format,
				    plane_modifier,
				    1.0, 1.0, 0.0,
				    &data->fb[i]);

		igt_plane_set_position(plane, x, y);
		igt_plane_set_fb(plane, &data->fb[i++]);
	}

	return mode;
}

static void
test_plane_position_with_output(data_t *data, enum pipe pipe,
				igt_output_t *output, uint64_t modifier)
{
	igt_crc_t crc_hires1, crc_hires2;
	igt_crc_t crc_lowres;
	drmModeModeInfo mode_lowres;
	drmModeModeInfo *mode1, *mode2, *mode3;
	int ret;
	igt_pipe_crc_t *pipe_crc;

	igt_info("Testing connector %s using pipe %s\n",
		 igt_output_name(output), kmstest_pipe_name(pipe));

	mode1 = test_setup(data, pipe, modifier, output);

	mode_lowres = get_lowres_mode(data->drm_fd, mode1);

	ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
	igt_skip_on(ret != 0);

	pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
	igt_pipe_crc_start(pipe_crc);
	igt_pipe_crc_get_single(pipe_crc, &crc_hires1);

	igt_assert_plane_visible(data->drm_fd, pipe, true);

	/* switch to lower resolution */
	igt_output_override_mode(output, &mode_lowres);
	igt_output_set_pipe(output, pipe);

	mode2 = igt_output_get_mode(output);

	check_mode(&mode_lowres, mode2);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	igt_pipe_crc_get_current(data->display.drm_fd, pipe_crc, &crc_lowres);

	igt_assert_plane_visible(data->drm_fd, pipe, false);

	/* switch back to higher resolution */
	igt_output_override_mode(output, NULL);
	igt_output_set_pipe(output, pipe);

	mode3 = igt_output_get_mode(output);

	check_mode(mode1, mode3);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(data->display.drm_fd, pipe_crc, &crc_hires2);

	igt_assert_plane_visible(data->drm_fd, pipe, true);

	igt_assert_crc_equal(&crc_hires1, &crc_hires2);

	igt_pipe_crc_stop(pipe_crc);
	igt_pipe_crc_free(pipe_crc);

	test_fini(data, output, pipe);
}

static void
test_plane_position(data_t *data, enum pipe pipe, uint64_t modifier)
{
	igt_output_t *output;

	for_each_valid_output_on_pipe(&data->display, pipe, output)
		test_plane_position_with_output(data, pipe, output, modifier);
}

static void
run_tests_for_pipe(data_t *data, enum pipe pipe)
{
	igt_fixture {
		igt_skip_on(pipe >= data->display.n_pipes);

		igt_display_require_output_on_pipe(&data->display, pipe);
	}

	igt_subtest_f("pipe-%s-tiling-none",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, LOCAL_DRM_FORMAT_MOD_NONE);

	igt_subtest_f("pipe-%s-tiling-x",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, LOCAL_I915_FORMAT_MOD_X_TILED);

	igt_subtest_f("pipe-%s-tiling-y",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, LOCAL_I915_FORMAT_MOD_Y_TILED);

	igt_subtest_f("pipe-%s-tiling-yf",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, LOCAL_I915_FORMAT_MOD_Yf_TILED);
}

static data_t data;

igt_main
{
	enum pipe pipe;

	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
	}

	for_each_pipe_static(pipe)
		igt_subtest_group
			run_tests_for_pipe(&data, pipe);

	igt_fixture {
		igt_display_fini(&data.display);
	}

	igt_exit();
}
