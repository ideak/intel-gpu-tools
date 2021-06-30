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
 *  Swati Sharma <swati2.sharma@intel.com>
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Test cdclk features : squasher and crawling");

#define HDISPLAY_4K     3840
#define VDISPLAY_4K     2160
#define VREFRESH	60

/* Test flags */
enum {
	TEST_BASIC = 1 << 0,
	TEST_PLANESCALING = 1 << 1,
	TEST_MODETRANSITION = 1 << 2,
};

typedef struct {
	int drm_fd;
	int debugfs_fd;
	uint32_t devid;
	igt_display_t display;
} data_t;

static bool hardware_supported(data_t *data)
{
        if (intel_display_ver(data->devid) >= 13)
		return true;

	return false;
}

static int get_current_cdclk_freq(int debugfs_fd)
{
	int cdclk_freq_current;
	char buf[1024];
	char *start_loc;
	int res;

	res = igt_debugfs_simple_read(debugfs_fd, "i915_frequency_info",
				      buf, sizeof(buf));
	igt_require(res > 0);

	igt_assert(start_loc = strstr(buf, "Current CD clock frequency: "));
	igt_assert_eq(sscanf(start_loc, "Current CD clock frequency: %d", &cdclk_freq_current), 1);

	return cdclk_freq_current;
}

static __u64 get_mode_data_rate(drmModeModeInfo *mode)
{
	__u64 data_rate = (__u64)mode->hdisplay * (__u64)mode->vdisplay * (__u64)mode->vrefresh;
	return data_rate;
}

static drmModeModeInfo *get_highres_mode(igt_output_t *output)
{
	drmModeModeInfo *highest_mode = NULL;
	drmModeConnector *connector = output->config.connector;
	int j;

	for (j = 0; j < connector->count_modes; j++) {
		if (connector->modes[j].vdisplay == VDISPLAY_4K &&
		    connector->modes[j].hdisplay == HDISPLAY_4K &&
		    connector->modes[j].vrefresh == VREFRESH) {
			highest_mode = &connector->modes[j];
			break;
		}
	}

	return highest_mode;
}

static drmModeModeInfo *get_lowres_mode(igt_output_t *output)
{
	drmModeModeInfo *lowest_mode = NULL;
	drmModeConnector *connector = output->config.connector;
	int j;

	for (j = 0; j < connector->count_modes; j++) {
		if (!lowest_mode) {
			lowest_mode = &connector->modes[j];
		} else if (connector->modes[j].vdisplay && connector->modes[j].hdisplay) {
			__u64 lowest_data_rate = get_mode_data_rate(lowest_mode);
			__u64 data_rate = get_mode_data_rate(&output->config.connector->modes[j]);

			if (lowest_data_rate > data_rate)
				lowest_mode = &connector->modes[j];
		}
	}

	return lowest_mode;
}

static void do_cleanup_display(igt_display_t *dpy)
{
	enum pipe pipe;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_pipe(dpy, pipe)
		for_each_plane_on_pipe(dpy, pipe, plane)
			igt_plane_set_fb(plane, NULL);

	for_each_connected_output(dpy, output)
		igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void test_basic(data_t *data, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	int debugfs_fd = data->debugfs_fd;
	int cdclk_ref, cdclk_new;
	struct igt_fb fb;
	igt_plane_t *primary;
	drmModeModeInfo *mode;

	do_cleanup_display(display);
	igt_display_reset(display);

	igt_output_set_pipe(output, pipe);
	mode = get_highres_mode(output);
	igt_require(mode != NULL);
	igt_output_override_mode(output, mode);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_color_pattern_fb(display->drm_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    I915_TILING_NONE,
				    0.0, 0.0, 0.0, &fb);

	igt_plane_set_fb(primary, &fb);
	cdclk_ref = get_current_cdclk_freq(debugfs_fd);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	cdclk_new = get_current_cdclk_freq(debugfs_fd);
	igt_info("CD clock frequency %d -> %d\n", cdclk_ref, cdclk_new);

	/* cdclk should bump */
	igt_assert_lt(cdclk_ref, cdclk_new);

	/* cleanup */
	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb);
}

static void test_plane_scaling(data_t *data, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	int debugfs_fd = data->debugfs_fd;
	int cdclk_ref, cdclk_new;
	struct igt_fb fb;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	int scaling = 50;
	int ret;
	bool test_complete = false;

	while (!test_complete) {
		do_cleanup_display(display);
		igt_display_reset(display);

		igt_output_set_pipe(output, pipe);
		mode = get_highres_mode(output);
		igt_require(mode != NULL);
		igt_output_override_mode(output, mode);

		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

		igt_create_color_pattern_fb(display->drm_fd,
					    mode->hdisplay, mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    I915_TILING_NONE,
					    0.0, 0.0, 0.0, &fb);
		igt_plane_set_fb(primary, &fb);

		/* downscaling */
		igt_plane_set_size(primary, ((fb.width * scaling) / 100), ((fb.height * scaling) / 100));
		cdclk_ref = get_current_cdclk_freq(debugfs_fd);
		ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret != -EINVAL) {
			igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			cdclk_new = get_current_cdclk_freq(debugfs_fd);
			igt_info("CD clock frequency %d -> %d\n", cdclk_ref, cdclk_new);

			/* cdclk should bump */
			igt_assert_lt(cdclk_ref, cdclk_new);

			test_complete = true;
		}

		scaling += 5;

		/* cleanup */
		do_cleanup_display(display);
		igt_remove_fb(display->drm_fd, &fb);
	}
}

static void test_mode_transition(data_t *data, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	int debugfs_fd = data->debugfs_fd;
	int cdclk_ref, cdclk_new;
	struct igt_fb fb;
	igt_plane_t *primary;
	drmModeModeInfo *mode_hi, *mode_lo, *mode;

	do_cleanup_display(display);
	igt_display_reset(display);

	igt_output_set_pipe(output, pipe);
	mode = igt_output_get_mode(output);
	mode_lo = get_lowres_mode(output);
	mode_hi = get_highres_mode(output);
	igt_require(mode_hi != NULL);

	if (mode_hi->hdisplay == mode_lo->hdisplay &&
	    mode_hi->vdisplay == mode_lo->vdisplay)
		igt_skip("Highest and lowest mode resolutions are same; no transition\n");

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_color_pattern_fb(display->drm_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    I915_TILING_NONE,
				    0.0, 0.0, 0.0, &fb);

	/* switch to lower resolution */
	igt_output_override_mode(output, mode_lo);
	igt_plane_set_fb(primary, &fb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	cdclk_ref = get_current_cdclk_freq(debugfs_fd);

	/* switch to higher resolution */
	igt_output_override_mode(output, mode_hi);
	igt_plane_set_fb(primary, &fb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	cdclk_new = get_current_cdclk_freq(debugfs_fd);
	igt_info("CD clock frequency %d -> %d\n", cdclk_ref, cdclk_new);

	/* cdclk should bump */
	igt_assert_lt(cdclk_ref, cdclk_new);

	/* cleanup */
	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb);
}

static void run_cdclk_test(data_t *data, uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		igt_dynamic_f("%s-pipe-%s", output->name, kmstest_pipe_name(pipe))
			if (igt_pipe_connector_valid(pipe, output)) {
				if (flags & TEST_BASIC)
					test_basic(data, pipe, output);
				if (flags & TEST_PLANESCALING)
					test_plane_scaling(data, pipe, output);
				if (flags & TEST_MODETRANSITION)
					test_mode_transition(data, pipe, output);
			}
	}
}

igt_main
{
	data_t data = {};

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require(data.drm_fd >= 0);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		igt_require(data.debugfs_fd);
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_require_f(hardware_supported(&data),
			      "Hardware doesn't support either squashing "\
			      "or crawling.\n");
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
	}

	igt_describe("Basic test to validate cdclk frequency change w/o "\
		     "requiring full modeset.");
	igt_subtest_with_dynamic("basic")
		run_cdclk_test(&data, TEST_BASIC);
	igt_describe("Plane scaling test to validate cdclk frequency change w/o "\
		     "requiring full modeset.");
	igt_subtest_with_dynamic("plane-scaling")
		run_cdclk_test(&data, TEST_PLANESCALING);
	igt_describe("Mode transition (low to high) test to validate cdclk frequency "\
		     "change w/o requiring full modeset.");
	igt_subtest_with_dynamic("mode-transition")
		run_cdclk_test(&data, TEST_MODETRANSITION);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
