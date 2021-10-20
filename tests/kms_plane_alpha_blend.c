/*
 * Copyright © 2018 Intel Corporation
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
 * Authors:
 *   Maarten Lankhorst <maarten.lankhorst@linux.intel.com>
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Test plane alpha and blending mode properties");

typedef struct {
	int gfx_fd;
	igt_display_t display;
	struct igt_fb xrgb_fb, argb_fb_0, argb_fb_cov_0, argb_fb_7e, argb_fb_cov_7e, argb_fb_fc, argb_fb_cov_fc, argb_fb_100, black_fb, gray_fb;
	igt_crc_t ref_crc;
	igt_pipe_crc_t *pipe_crc;
} data_t;

static void __draw_gradient(struct igt_fb *fb, int w, int h, double a, cairo_t *cr)
{
	cairo_pattern_t *pat;

	pat = cairo_pattern_create_linear(0, 0, w, h);
	cairo_pattern_add_color_stop_rgba(pat, 0.00, 0.00, 0.00, 0.00, 1.);
	cairo_pattern_add_color_stop_rgba(pat, 0.25, 1.00, 1.00, 0.00, 1.);
	cairo_pattern_add_color_stop_rgba(pat, 0.50, 0.00, 1.00, 1.00, 1.);
	cairo_pattern_add_color_stop_rgba(pat, 0.75, 1.00, 0.00, 1.00, 1.);
	cairo_pattern_add_color_stop_rgba(pat, 1.00, 1.00, 1.00, 1.00, 1.);

	cairo_rectangle(cr, 0, 0, w, h);
	cairo_set_source(cr, pat);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint_with_alpha(cr, a);
	cairo_pattern_destroy(pat);
}

static void draw_gradient(struct igt_fb *fb, int w, int h, double a)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);

	__draw_gradient(fb, w, h, a, cr);

	igt_put_cairo_ctx(cr);
}

static void draw_gradient_coverage(struct igt_fb *fb, int w, int h, uint8_t a)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	uint8_t *data = cairo_image_surface_get_data(fb->cairo_surface);
	uint32_t stride = fb->strides[0];
	int i;

	__draw_gradient(fb, w, h, 1., cr);

	for (; h--; data += stride)
		for (i = 0; i < w; i++)
			data[i * 4 + 3] = a;

	igt_put_cairo_ctx(cr);
}

static void draw_squares(struct igt_fb *fb, int w, int h, double a)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	igt_paint_color_alpha(cr, 0, 0,         w / 2, h / 2, 1., 0., 0., a);
	igt_paint_color_alpha(cr, w / 2, 0,     w / 2, h / 2, 0., 1., 0., a);
	igt_paint_color_alpha(cr, 0, h / 2,     w / 2, h / 2, 0., 0., 1., a);
	igt_paint_color_alpha(cr, w / 2, h / 2, w / 4, h / 2, 1., 1., 1., a);
	igt_paint_color_alpha(cr, w * 3 / 4, h / 2, w - (w * 3 / 4), h / 2, 0., 0., 0., a);

	igt_put_cairo_ctx(cr);
}

static void draw_squares_coverage(struct igt_fb *fb, int w, int h, uint8_t as)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	int i, j;
	uint32_t *data = (void *)cairo_image_surface_get_data(fb->cairo_surface);
	uint32_t stride = fb->strides[0] / 4;
	uint32_t a = as << 24;

	for (j = 0; j < h / 2; j++) {
		for (i = 0; i < w / 2; i++)
			data[j * stride + i] = a | 0xff0000;

		for (; i < w; i++)
			data[j * stride + i] = a | 0xff00;
	}

	for (j = h / 2; j < h; j++) {
		for (i = 0; i < w / 2; i++)
			data[j * stride + i] = a | 0xff;

		for (; i < 3 * w / 4; i++)
			data[j * stride + i] = a | 0xffffff;

		for (; i < w; i++)
			data[j * stride + i] = a;
	}

	igt_put_cairo_ctx(cr);
}

static void reset_alpha(igt_display_t *display, enum pipe pipe)
{
	igt_plane_t *plane;

	for_each_plane_on_pipe(display, pipe, plane) {
		if (igt_plane_has_prop(plane, IGT_PLANE_ALPHA))
			igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0xffff);

		if (igt_plane_has_prop(plane, IGT_PLANE_PIXEL_BLEND_MODE))
			igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "Pre-multiplied");
	}
}

static bool has_multiplied_alpha(data_t *data, igt_plane_t *plane)
{
	int ret;

	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0x8080);
	igt_plane_set_fb(plane, &data->argb_fb_100);
	ret = igt_display_try_commit_atomic(&data->display,
		DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0xffff);
	igt_plane_set_fb(plane, NULL);

	return ret == 0;
}

static void remove_fbs(data_t *data)
{
	igt_remove_fb(data->gfx_fd, &data->xrgb_fb);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_0);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_cov_0);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_7e);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_fc);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_cov_7e);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_cov_fc);
	igt_remove_fb(data->gfx_fd, &data->argb_fb_100);
	igt_remove_fb(data->gfx_fd, &data->black_fb);
	igt_remove_fb(data->gfx_fd, &data->gray_fb);
}

static void prepare_crtc(data_t *data, igt_output_t *output, enum pipe pipe)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	int w, h;
	igt_plane_t *primary = igt_pipe_get_plane_type(&display->pipes[pipe], DRM_PLANE_TYPE_PRIMARY);

	igt_display_reset(display);
	igt_output_set_pipe(output, pipe);

	/* create the pipe_crc object for this pipe */
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = igt_pipe_crc_new(data->gfx_fd, pipe,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	mode = igt_output_get_mode(output);
	w = mode->hdisplay;
	h = mode->vdisplay;

	/* recreate all fbs if incompatible */
	if (data->xrgb_fb.width != w || data->xrgb_fb.height != h) {
		cairo_t *cr;

		remove_fbs(data);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->xrgb_fb);
		draw_gradient(&data->xrgb_fb, w, h, 1.);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_cov_0);
		draw_gradient_coverage(&data->argb_fb_cov_0, w, h, 0);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_0);

		cr = igt_get_cairo_ctx(data->gfx_fd, &data->argb_fb_0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		igt_paint_color_alpha(cr, 0, 0, w, h, 0., 0., 0., 0.0);
		igt_put_cairo_ctx(cr);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_7e);
		draw_squares(&data->argb_fb_7e, w, h, 126. / 255.);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_cov_7e);
		draw_squares_coverage(&data->argb_fb_cov_7e, w, h, 0x7e);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_fc);
		draw_squares(&data->argb_fb_fc, w, h, 252. / 255.);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_cov_fc);
		draw_squares_coverage(&data->argb_fb_cov_fc, w, h, 0xfc);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->argb_fb_100);
		draw_gradient(&data->argb_fb_100, w, h, 1.);

		igt_create_fb(data->gfx_fd, w, h,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->black_fb);

		igt_create_color_fb(data->gfx_fd, w, h,
				    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
				    .5, .5, .5, &data->gray_fb);
	}

	igt_plane_set_fb(primary, &data->black_fb);
}

static void basic_alpha(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, crc;
	int i;

	/* Testcase 1: alpha = 0.0, plane should be transparent. */
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_single(data->pipe_crc, &ref_crc);

	igt_plane_set_fb(plane, &data->argb_fb_0);

	/* transparent fb should be transparent, no matter what.. */
	for (i = 7; i < 256; i += 8) {
		igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, i | (i << 8));
		igt_display_commit2(display, COMMIT_ATOMIC);

		igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc);
		igt_assert_crc_equal(&ref_crc, &crc);
	}

	/* And test alpha = 0, should give same CRC, but doesn't on some i915 platforms. */
	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_pipe_crc_stop(data->pipe_crc);
}

static void argb_opaque(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, crc;

	/* alpha = 1.0, plane should be fully opaque, test with an opaque fb */
	igt_plane_set_fb(plane, &data->xrgb_fb);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	igt_plane_set_fb(plane, &data->argb_fb_100);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);

	igt_assert_crc_equal(&ref_crc, &crc);
}

static void argb_transparent(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, crc;

	/* alpha = 1.0, plane should be fully opaque, test with a transparent fb */
	igt_plane_set_fb(plane, NULL);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	igt_plane_set_fb(plane, &data->argb_fb_0);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);

	igt_assert_crc_equal(&ref_crc, &crc);
}

static void constant_alpha_min(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, crc;

	igt_plane_set_fb(plane, NULL);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "None");
	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0);
	igt_plane_set_fb(plane, &data->argb_fb_100);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_plane_set_fb(plane, &data->argb_fb_0);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);
}

static void constant_alpha_mid(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, crc;

	if (plane->type != DRM_PLANE_TYPE_PRIMARY)
		igt_plane_set_fb(igt_pipe_get_plane_type(&display->pipes[pipe], DRM_PLANE_TYPE_PRIMARY), &data->gray_fb);

	igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "None");
	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0x7fff);
	igt_plane_set_fb(plane, &data->xrgb_fb);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	igt_plane_set_fb(plane, &data->argb_fb_cov_0);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_plane_set_fb(plane, &data->argb_fb_100);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);
}

static void constant_alpha_max(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, crc;

	if (plane->type != DRM_PLANE_TYPE_PRIMARY)
		igt_plane_set_fb(igt_pipe_get_plane_type(&display->pipes[pipe], DRM_PLANE_TYPE_PRIMARY), &data->gray_fb);

	igt_plane_set_fb(plane, &data->argb_fb_100);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "None");
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_plane_set_fb(plane, &data->argb_fb_cov_0);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_plane_set_fb(plane, &data->xrgb_fb);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_plane_set_fb(plane, NULL);
}

static void alpha_7efc(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc = {}, crc = {};

	if (plane->type != DRM_PLANE_TYPE_PRIMARY)
		igt_plane_set_fb(igt_pipe_get_plane_type(&display->pipes[pipe], DRM_PLANE_TYPE_PRIMARY), &data->gray_fb);

	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_start(data->pipe_crc);

	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0x7e7e);
	igt_plane_set_fb(plane, &data->argb_fb_fc);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &ref_crc);

	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0xfcfc);
	igt_plane_set_fb(plane, &data->argb_fb_7e);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_pipe_crc_stop(data->pipe_crc);
}

static void coverage_7efc(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc = {}, crc = {};

	igt_require(igt_plane_try_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "Coverage"));
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_start(data->pipe_crc);

	/* for coverage, plane alpha and fb alpha should be swappable, so swap fb and alpha */
	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0x7e7e);
	igt_plane_set_fb(plane, &data->argb_fb_cov_fc);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &ref_crc);

	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0xfcfc);
	igt_plane_set_fb(plane, &data->argb_fb_cov_7e);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_pipe_crc_stop(data->pipe_crc);
}

static void coverage_premult_constant(data_t *data, enum pipe pipe, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc = {}, crc = {};

	/* Set a background color on the primary fb for testing */
	if (plane->type != DRM_PLANE_TYPE_PRIMARY)
		igt_plane_set_fb(igt_pipe_get_plane_type(&display->pipes[pipe], DRM_PLANE_TYPE_PRIMARY), &data->gray_fb);

	igt_require(igt_plane_try_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "Coverage"));
	igt_plane_set_fb(plane, &data->argb_fb_cov_7e);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_single(data->pipe_crc, &ref_crc);

	igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "Pre-multiplied");
	igt_plane_set_fb(plane, &data->argb_fb_7e);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_plane_set_prop_enum(plane, IGT_PLANE_PIXEL_BLEND_MODE, "None");
	igt_plane_set_prop_value(plane, IGT_PLANE_ALPHA, 0x7e7e);
	igt_plane_set_fb(plane, &data->argb_fb_cov_7e);
	igt_display_commit2(display, COMMIT_ATOMIC);
	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc);
	igt_assert_crc_equal(&ref_crc, &crc);

	igt_pipe_crc_stop(data->pipe_crc);
}

static void run_test_on_pipe_planes(data_t *data, enum pipe pipe, igt_output_t *output,
				    bool blend, bool must_multiply,
				    void(*test)(data_t *, enum pipe, igt_plane_t *))
{
	igt_display_t *display = &data->display;
	igt_plane_t *plane;

	for_each_plane_on_pipe(display, pipe, plane) {
		if (!igt_plane_has_prop(plane, IGT_PLANE_ALPHA))
			continue;

		if (blend && !igt_plane_has_prop(plane, IGT_PLANE_PIXEL_BLEND_MODE))
			continue;

		/* reset plane alpha properties between each plane */
		reset_alpha(display, pipe);

		if (must_multiply && !has_multiplied_alpha(data, plane))
			continue;

		igt_info("Testing plane %u\n", plane->index);
		test(data, pipe, plane);
		igt_plane_set_fb(plane, NULL);
	}

	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, COMMIT_ATOMIC);
}

static const struct {
	const char *name;
	void (*test)(data_t *, enum pipe, igt_plane_t *);
	bool blend;
	bool must_multiply;
	const char *desc;
} subtests[] = {
	{ .name = "alpha-basic",
	  .test = basic_alpha,
	  .blend = false,
	  .must_multiply = true,
	  .desc = "Tests basic plane alpha properties.",
	},
	{ .name = "alpha-7efc",
	  .test = alpha_7efc,
	  .blend = false,
	  .must_multiply = true,
	  .desc = "Uses alpha values 0x7e and 0xfc to validate fg.alpha and "
		  "plane_alpha are swappable on pre-multiplied blend mode.",
	},
	{ .name = "coverage-7efc",
	  .test = coverage_7efc,
	  .blend = true,
	  .must_multiply = true,
	  .desc = "Uses alpha values 0x7e and 0xfc to validate fg.alpha and "
		  "plane_alpha are swappable on coverage blend mode.",
	},
	{ .name = "coverage-vs-premult-vs-constant",
	  .test = coverage_premult_constant,
	  .blend = true,
	  .must_multiply = false,
	  .desc = "Tests pipe coverage blending properties.",
	},
	{ .name = "alpha-transparent-fb",
	  .test = argb_transparent,
	  .blend = false,
	  .must_multiply = false,
	  .desc = "Tests the alpha property with transparent fb.",
	},
	{ .name = "alpha-opaque-fb",
	  .test = argb_opaque,
	  .blend = false,
	  .must_multiply = false,
	  .desc = "Tests alpha properties with opaque fb.",
	},
	{ .name = "constant-alpha-min",
	  .test = constant_alpha_min,
	  .blend = true,
	  .must_multiply = false,
	  .desc = "Tests plane alpha and blending properties with minimum alpha value.",
	},
	{ .name = "constant-alpha-mid",
	  .test = constant_alpha_mid,
	  .blend = true,
	  .must_multiply = false,
	  .desc = "Tests plane alpha and blending properties with medium alpha value.",
	},
	{ .name = "constant-alpha-max",
	  .test = constant_alpha_max,
	  .blend = true,
	  .must_multiply = false,
	  .desc = "Tests plane alpha and blending properties with maximum alpha value.",
	},
};

static bool pipe_check(data_t *data, enum pipe pipe,
		       bool blend, bool must_multiply)
{
	igt_display_t *display = &data->display;
	igt_plane_t *plane;
	bool plane_alpha = false, plane_blend = false, multiply = false;

	igt_display_require_output_on_pipe(display, pipe);
	for_each_plane_on_pipe(display, pipe, plane) {
		if (!igt_plane_has_prop(plane, IGT_PLANE_ALPHA))
			continue;
		plane_alpha = true;

		if (blend && !igt_plane_has_prop(plane, IGT_PLANE_PIXEL_BLEND_MODE))
			continue;
		plane_blend = true;

		/* reset plane alpha properties between each plane */
		reset_alpha(display, pipe);

		if (must_multiply && !has_multiplied_alpha(data, plane))
			continue;
		multiply = true;

		break;
	}

	if (!plane_alpha || !plane_blend || !multiply) {
		if (!plane_alpha)
			igt_debug("No planes with alpha property found\n");
		if (!plane_blend)
			igt_debug("No planes with pixel blending mode property found\n");
		if (!multiply)
			igt_debug("Multiplied (plane x pixel) alpha not available\n");

		return false;
	} else {
		return true;
	}
}

static void run_subtests(data_t *data)
{
	igt_output_t *output;
	enum pipe pipe;

	for (int i = 0; i < ARRAY_SIZE(subtests); i++) {
		igt_describe_f("%s\n", subtests[i].desc);

		igt_subtest_with_dynamic(subtests[i].name) {
			for_each_pipe_with_single_output(&data->display, pipe, output) {
				prepare_crtc(data, output, pipe);
				if (!pipe_check(data, pipe, subtests[i].blend, subtests[i].must_multiply))
					continue;

				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), output->name)
					run_test_on_pipe_planes(data, pipe, output, subtests[i].blend,
								subtests[i].must_multiply, subtests[i].test);
			}
		}
	}
}

igt_main
{
	data_t data = {};

	igt_fixture {
		data.gfx_fd = drm_open_driver_master(DRIVER_ANY);
		igt_require_pipe_crc(data.gfx_fd);
		igt_display_require(&data.display, data.gfx_fd);
		igt_require(data.display.is_atomic);
	}

	run_subtests(&data);

	igt_fixture {
		remove_fbs(&data);
		igt_display_reset(&data.display);
		igt_display_commit2(&data.display, data.display.is_atomic ?
				    COMMIT_ATOMIC : COMMIT_LEGACY);

		igt_display_fini(&data.display);
		close(data.gfx_fd);
	}
}
