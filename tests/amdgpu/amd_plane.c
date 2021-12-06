/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "igt.h"

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_plane_t *overlay;
	igt_output_t *output;
	igt_pipe_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int fd;
	int w;
	int h;
} data_t;

static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = &data->display.pipes[data->pipe_id];

	igt_display_reset(display);

	data->output = igt_get_single_output_for_pipe(display, data->pipe_id);
	igt_require(data->output);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);

	data->primary =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);
	data->overlay =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_OVERLAY);

	data->pipe_crc = igt_pipe_crc_new(data->fd, data->pipe_id,
					  INTEL_PIPE_CRC_SOURCE_AUTO);

	igt_output_set_pipe(data->output, data->pipe_id);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
}

static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	igt_display_reset(&data->display);
}

static void draw_color_alpha(igt_fb_t *fb, int x, int y, int w, int h,
		             double r, double g, double b, double a)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	igt_paint_color_alpha(cr, x, y, w, h, r, g, b, a);
	igt_put_cairo_ctx(cr);
}

/*
 * Compares a white 4K reference FB against a white 4K primary FB and a
 * white 4K overlay with an RGBA (0, 0, 0, 0) cutout in the center.
 */
static void test_mpo_4k(data_t *data)
{
	igt_fb_t r_fb, p_fb, o_fb;
	igt_crc_t ref_crc, new_crc;
	igt_display_t *display = &data->display;
	int cutout_x, cutout_y, cutout_w, cutout_h;

	test_init(data);

	/* Skip if not 4K resolution. */
	igt_skip_on(!(data->mode->hdisplay == 3840 &&
		    data->mode->vdisplay == 2160));

	cutout_x = cutout_w = 1280;
	cutout_y = cutout_h = 720;

	igt_create_color_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888,
			    0, 1.00, 1.00, 1.00, &r_fb);
	igt_create_color_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888,
			    0, 1.00, 1.00, 1.00, &p_fb);
	igt_create_fb(data->fd, data->w, data->h, DRM_FORMAT_ARGB8888,
		      0, &o_fb);
	draw_color_alpha(&o_fb, 0, 0, o_fb.width, o_fb.height, 1.00, 1.00, 1.00, 1.00);
	draw_color_alpha(&o_fb, cutout_x, cutout_y, cutout_w, cutout_h,
			 0.00, 0.00, 0.00, 0.00);

	igt_plane_set_fb(data->primary, &r_fb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	igt_plane_set_fb(data->primary, &p_fb);
	igt_plane_set_fb(data->overlay, &o_fb);
	igt_display_commit_atomic(display, 0, NULL);

	igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

	igt_assert_crc_equal(&ref_crc, &new_crc);

	test_fini(data);
	igt_remove_fb(data->fd, &o_fb);
	igt_remove_fb(data->fd, &p_fb);
	igt_remove_fb(data->fd, &r_fb);
}

igt_main
{
	data_t data;

	igt_skip_on_simulation();

	memset(&data, 0, sizeof(data));

	igt_fixture
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_subtest("test-mpo-4k") test_mpo_4k(&data);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
