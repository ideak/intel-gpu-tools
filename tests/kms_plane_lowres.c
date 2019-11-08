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

#define SIZE 64

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb_primary;
	struct igt_fb fb_plane[2];
	struct {
		struct igt_fb fb;
		igt_crc_t crc;
	} ref_lowres;
	struct {
		struct igt_fb fb;
		igt_crc_t crc;
	} ref_hires;
	int x, y;
} data_t;

static drmModeModeInfo
get_lowres_mode(int drmfd, igt_output_t *output,
		const drmModeModeInfo *mode_default)
{
	const drmModeModeInfo *mode;
	bool found = false;
	int limit = mode_default->vdisplay - SIZE;
	int j;

	for (j = 0; j < output->config.connector->count_modes; j++) {
		mode = &output->config.connector->modes[j];
		if (mode->vdisplay < limit) {
			found = true;
			break;
		}
	}

	if (!found) {
		igt_require_f(mode_default->vdisplay - SIZE > 768,
			      "Current mode not tall enough; plane would still be onscreen after switch to 10x7.\n");

		return *igt_std_1024_mode_get();
	}

	return *mode;
}

static bool setup_plane(data_t *data, igt_plane_t *plane)
{
	struct igt_fb *fb;

	if (plane->type == DRM_PLANE_TYPE_PRIMARY)
		return false;

	fb = &data->fb_plane[0];
	if (!igt_plane_has_format_mod(plane, fb->drm_format, fb->modifier))
		fb = &data->fb_plane[1];
	if (!igt_plane_has_format_mod(plane, fb->drm_format, fb->modifier))
		return false;

	igt_plane_set_position(plane, data->x, data->y);
	igt_plane_set_fb(plane, fb);

	return true;
}

static void blit(data_t *data, cairo_t *cr,
		 struct igt_fb *src, int x, int y)
{
	cairo_surface_t *surface;

	surface = igt_get_cairo_surface(data->drm_fd, src);

	cairo_set_source_surface(cr, surface, x, y);
	cairo_rectangle(cr, x, y, src->width, src->height);
	cairo_fill (cr);

	cairo_surface_destroy(surface);
}

static void create_ref_fb(data_t *data, uint64_t modifier,
			  const drmModeModeInfo *mode, struct igt_fb *fb)
{
	cairo_t *cr;

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, modifier, fb);

	cr = igt_get_cairo_ctx(data->drm_fd, fb);
	blit(data, cr, &data->fb_primary, 0, 0);
	blit(data, cr, &data->fb_plane[0], data->x, data->y);
	igt_put_cairo_ctx(data->drm_fd, fb, cr);
}

static unsigned
test_planes_on_pipe_with_output(data_t *data, enum pipe pipe,
				igt_output_t *output, uint64_t modifier)
{
	igt_pipe_t *pipe_obj = &data->display.pipes[pipe];
	const drmModeModeInfo *mode;
	drmModeModeInfo mode_lowres;
	igt_pipe_crc_t *pipe_crc;
	unsigned tested = 0;
	igt_plane_t *plane;
	igt_plane_t *primary;

	primary = igt_pipe_get_plane_type(pipe_obj, DRM_PLANE_TYPE_PRIMARY);

	igt_info("Testing connector %s using pipe %s\n",
		 igt_output_name(output), kmstest_pipe_name(pipe));

	igt_output_set_pipe(output, pipe);
	mode = igt_output_get_mode(output);
	mode_lowres = get_lowres_mode(data->drm_fd, output, mode);

	igt_create_color_pattern_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888, modifier, 0.0, 0.0, 1.0,
				    &data->fb_primary);

	data->x = 0;
	data->y = mode->vdisplay - SIZE;

	/* for other planes */
	igt_create_color_pattern_fb(data->drm_fd, SIZE, SIZE,
				    DRM_FORMAT_XRGB8888, modifier,
				    1.0, 1.0, 0.0, &data->fb_plane[0]);
	/* for cursor */
	igt_create_color_pattern_fb(data->drm_fd, SIZE, SIZE,
				    DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
				    1.0, 1.0, 0.0, &data->fb_plane[1]);

	create_ref_fb(data, modifier, mode, &data->ref_hires.fb);
	create_ref_fb(data, modifier, &mode_lowres, &data->ref_lowres.fb);

	pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe,
				    INTEL_PIPE_CRC_SOURCE_AUTO);

	igt_output_override_mode(output, &mode_lowres);
	igt_plane_set_fb(primary, &data->ref_lowres.fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(pipe_crc, &data->ref_lowres.crc);

	igt_output_override_mode(output, NULL);
	igt_plane_set_fb(primary, &data->ref_hires.fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(pipe_crc, &data->ref_hires.crc);

	igt_plane_set_fb(primary, &data->fb_primary);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	/* yellow sprite plane in lower left corner */
	for_each_plane_on_pipe(&data->display, pipe, plane) {
		igt_crc_t crc_lowres, crc_hires1, crc_hires2;

		if (!setup_plane(data, plane))
			continue;

		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		igt_pipe_crc_collect_crc(pipe_crc, &crc_hires1);

		/* switch to lower resolution */
		igt_output_override_mode(output, &mode_lowres);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		igt_pipe_crc_collect_crc(pipe_crc, &crc_lowres);

		/* switch back to higher resolution */
		igt_output_override_mode(output, NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		igt_pipe_crc_collect_crc(pipe_crc, &crc_hires2);

		igt_assert_crc_equal(&data->ref_hires.crc, &crc_hires1);
		igt_assert_crc_equal(&data->ref_hires.crc, &crc_hires2);
		igt_assert_crc_equal(&data->ref_lowres.crc, &crc_lowres);

		igt_plane_set_fb(plane, NULL);
		tested++;
	}

	igt_pipe_crc_free(pipe_crc);

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);

	igt_remove_fb(data->drm_fd, &data->fb_plane[1]);
	igt_remove_fb(data->drm_fd, &data->fb_plane[0]);
	igt_remove_fb(data->drm_fd, &data->fb_primary);
	igt_remove_fb(data->drm_fd, &data->ref_hires.fb);
	igt_remove_fb(data->drm_fd, &data->ref_lowres.fb);

	return tested;
}

static void
test_planes_on_pipe(data_t *data, enum pipe pipe, uint64_t modifier)
{
	igt_output_t *output;
	unsigned tested = 0;

	igt_skip_on(pipe >= data->display.n_pipes);
	igt_display_require_output_on_pipe(&data->display, pipe);
	igt_skip_on(!igt_display_has_format_mod(&data->display,
						DRM_FORMAT_XRGB8888, modifier));

	for_each_valid_output_on_pipe(&data->display, pipe, output)
		tested += test_planes_on_pipe_with_output(data, pipe, output,
							  modifier);

	igt_assert(tested > 0);
}

igt_main
{
	data_t data = {};
	enum pipe pipe;

	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
	}

	for_each_pipe_static(pipe) {
		igt_subtest_f("pipe-%s-tiling-none", kmstest_pipe_name(pipe))
			test_planes_on_pipe(&data, pipe,
					    LOCAL_DRM_FORMAT_MOD_NONE);

		igt_subtest_f("pipe-%s-tiling-x", kmstest_pipe_name(pipe))
			test_planes_on_pipe(&data, pipe,
					    LOCAL_I915_FORMAT_MOD_X_TILED);

		igt_subtest_f("pipe-%s-tiling-y", kmstest_pipe_name(pipe))
			test_planes_on_pipe(&data, pipe,
					    LOCAL_I915_FORMAT_MOD_Y_TILED);

		igt_subtest_f("pipe-%s-tiling-yf", kmstest_pipe_name(pipe))
			test_planes_on_pipe(&data, pipe,
					    LOCAL_I915_FORMAT_MOD_Yf_TILED);
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
