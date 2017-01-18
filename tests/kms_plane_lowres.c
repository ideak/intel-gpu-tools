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
	igt_pipe_crc_t *pipe_crc;
	igt_plane_t *plane[IGT_MAX_PLANES];
	struct igt_fb fb[IGT_MAX_PLANES];
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
test_init(data_t *data, enum pipe pipe)
{
	data->pipe_crc = igt_pipe_crc_new(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
}

static void
test_fini(data_t *data, igt_output_t *output)
{
	/* restore original mode */
	igt_output_override_mode(output, NULL);

	for (int i = 0; i < 2; i++)
		igt_plane_set_fb(data->plane[i], NULL);

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_ANY);

	igt_pipe_crc_free(data->pipe_crc);
}

static int
display_commit_mode(data_t *data, enum pipe pipe, int flags, igt_crc_t *crc)
{
	char buf[256];
	struct drm_event *e = (void *)buf;
	unsigned int vblank_start, vblank_stop;
	int n, ret;

	vblank_start = kmstest_get_vblank(data->display.drm_fd, pipe,
					  DRM_VBLANK_NEXTONMISS);

	ret = igt_display_try_commit_atomic(&data->display,
					    flags,
					    NULL);
	igt_skip_on(ret != 0);

	igt_set_timeout(1, "Stuck on page flip");
	ret = read(data->display.drm_fd, buf, sizeof(buf));
	igt_assert(ret >= 0);

	vblank_stop = kmstest_get_vblank(data->display.drm_fd, pipe, 0);
	igt_assert_eq(e->type, DRM_EVENT_FLIP_COMPLETE);
	igt_reset_timeout();

	n = igt_pipe_crc_get_crcs(data->pipe_crc, vblank_stop - vblank_start,
				  &crc);
	igt_assert_eq(n, vblank_stop - vblank_start);

	return n;
}

static void
check_mode(drmModeModeInfo *mode1, drmModeModeInfo *mode2)
{
	igt_assert_eq(mode1->hdisplay, mode2->hdisplay);
	igt_assert_eq(mode1->vdisplay, mode2->vdisplay);
	igt_assert_eq(mode1->vrefresh, mode2->vrefresh);
}

static drmModeModeInfo *
test_setup(data_t *data, enum pipe pipe, uint64_t modifier, int flags,
	   igt_output_t *output)
{
	struct kmstest_crtc crtc;
	drmModeModeInfo *mode;
	int size;
	int i, x, y;

	igt_output_set_pipe(output, pipe);

	kmstest_get_crtc(pipe, &crtc);
	igt_skip_on(crtc.n_planes > data->display.pipes[pipe].n_planes);
	igt_skip_on(crtc.n_planes == 0);

	for (i = 0; i < crtc.n_planes; i++)
		data->plane[i] = igt_output_get_plane(output, crtc.planes[i].index);

	mode = igt_output_get_mode(output);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    modifier,
			    0.0, 0.0, 1.0,
			    &data->fb[0]);

	igt_plane_set_fb(data->plane[0], &data->fb[0]);

	/* yellow sprite plane in lower left corner */
	for (i = IGT_PLANE_2; i < crtc.n_planes; i++) {
		if (data->plane[i]->is_cursor)
			size = 64;
		else
			size = SIZE;

		x = 0;
		y = mode->vdisplay - size;

		igt_create_color_fb(data->drm_fd,
				    size, size,
				    data->plane[i]->is_cursor ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
				    data->plane[i]->is_cursor ? LOCAL_DRM_FORMAT_MOD_NONE : modifier,
				    1.0, 1.0, 0.0,
				    &data->fb[i]);

		igt_plane_set_position(data->plane[i], x, y);
		igt_plane_set_fb(data->plane[i], &data->fb[i]);
	}

	return mode;
}

static void
test_plane_position_with_output(data_t *data, enum pipe pipe,
				igt_output_t *output, uint64_t modifier)
{
	igt_crc_t *crc_hires1, *crc_hires2;
	igt_crc_t *crc_lowres;
	drmModeModeInfo mode_lowres;
	drmModeModeInfo *mode1, *mode2, *mode3;
	int ret, n;
	int flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;

	igt_info("Testing connector %s using pipe %s\n",
		 igt_output_name(output), kmstest_pipe_name(pipe));

	test_init(data, pipe);

	mode1 = test_setup(data, pipe, modifier, flags, output);

	mode_lowres = get_lowres_mode(data->drm_fd, mode1);

	igt_pipe_crc_start(data->pipe_crc);
	ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
	igt_skip_on(ret != 0);

	n = igt_pipe_crc_get_crcs(data->pipe_crc, 1, &crc_hires1);
	igt_assert_eq(1, n);

	igt_assert_plane_visible(pipe, true);

	/* switch to lower resolution */
	igt_output_override_mode(output, &mode_lowres);
	igt_output_set_pipe(output, pipe);

	mode2 = igt_output_get_mode(output);

	check_mode(&mode_lowres, mode2);

	display_commit_mode(data, pipe, flags, crc_lowres);

	igt_assert_plane_visible(pipe, false);

	/* switch back to higher resolution */
	igt_output_override_mode(output, NULL);
	igt_output_set_pipe(output, pipe);

	mode3 = igt_output_get_mode(output);

	check_mode(mode1, mode3);

	display_commit_mode(data, pipe, flags, crc_hires2);

	igt_assert_plane_visible(pipe, true);

	igt_pipe_crc_stop(data->pipe_crc);

	test_fini(data, output);
}

static void
test_plane_position(data_t *data, enum pipe pipe, uint64_t modifier)
{
	igt_output_t *output;
	int connected_outs;
	const int gen = intel_gen(intel_get_drm_devid(data->drm_fd));

	igt_require(data->display.is_atomic);
	igt_skip_on(pipe >= data->display.n_pipes);

	if (modifier == LOCAL_I915_FORMAT_MOD_Y_TILED ||
	    modifier == LOCAL_I915_FORMAT_MOD_Yf_TILED)
		igt_skip_on(gen < 9);

	connected_outs = 0;
	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		test_plane_position_with_output(data, pipe, output, modifier);
		connected_outs++;
	}

	igt_skip_on(connected_outs == 0);
}

static void
run_tests_for_pipe(data_t *data, enum pipe pipe)
{
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
	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc();
		igt_display_init(&data.display, data.drm_fd);
	}

	for (int pipe = 0; pipe < I915_MAX_PIPES; pipe++)
		run_tests_for_pipe(&data, pipe);

	igt_fixture {
		igt_display_fini(&data.display);
	}

	igt_exit();
}
