/*
 * Copyright © 2014 Intel Corporation
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
 *   Damien Lespiau <damien.lespiau@intel.com>
 */

#include "igt.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


typedef struct {
	float red;
	float green;
	float blue;
} color_t;

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_pipe_crc_t *pipe_crc;
} data_t;

static color_t red   = { 1.0f, 0.0f, 0.0f };
static color_t green = { 0.0f, 1.0f, 0.0f };
static color_t blue  = { 0.0f, 0.0f, 1.0f };

/*
 * Common code across all tests, acting on data_t
 */
static void test_init(data_t *data, enum pipe pipe)
{
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
}

static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
}

static void
test_grab_crc(data_t *data, igt_output_t *output, enum pipe pipe,
	      color_t *fb_color, igt_crc_t *crc /* out */)
{
	struct igt_fb fb;
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	char *crc_str;
	int ret;

	igt_output_set_pipe(output, pipe);

	primary = igt_output_get_plane(output, 0);

	mode = igt_output_get_mode(output);
	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    fb_color->red, fb_color->green, fb_color->blue,
			    &fb);
	igt_plane_set_fb(primary, &fb);

	ret = igt_display_try_commit2(&data->display, COMMIT_LEGACY);
	igt_skip_on(ret != 0);

	igt_pipe_crc_collect_crc(data->pipe_crc, crc);

	igt_plane_set_fb(primary, NULL);
	igt_display_commit(&data->display);

	igt_remove_fb(data->drm_fd, &fb);

	crc_str = igt_crc_to_string(crc);
	igt_debug("CRC for a (%.02f,%.02f,%.02f) fb: %s\n", fb_color->red,
		  fb_color->green, fb_color->blue, crc_str);
	free(crc_str);
}

/*
 * Plane position test.
 *   - We start by grabbing a reference CRC of a full green fb being scanned
 *     out on the primary plane
 *   - Then we scannout 2 planes:
 *      - the primary plane uses a green fb with a black rectangle
 *      - a plane, on top of the primary plane, with a green fb that is set-up
 *        to cover the black rectangle of the primary plane fb
 *     The resulting CRC should be identical to the reference CRC
 */

typedef struct {
	data_t *data;
	igt_crc_t reference_crc;
} test_position_t;

/*
 * create a green fb with a black rectangle at (rect_x,rect_y) and of size
 * (rect_w,rect_h)
 */
static void
create_fb_for_mode__position(data_t *data, drmModeModeInfo *mode,
			     double rect_x, double rect_y,
			     double rect_w, double rect_h,
			     struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(data->drm_fd,
				  mode->hdisplay, mode->vdisplay,
				  DRM_FORMAT_XRGB8888,
				  LOCAL_DRM_FORMAT_MOD_NONE,
				  fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, fb);
	igt_paint_color(cr, 0, 0, mode->hdisplay, mode->vdisplay,
			    0.0, 1.0, 0.0);
	igt_paint_color(cr, rect_x, rect_y, rect_w, rect_h, 0.0, 0.0, 0.0);
	igt_put_cairo_ctx(data->drm_fd, fb, cr);
}

enum {
	TEST_POSITION_FULLY_COVERED = 1 << 0,
	TEST_DPMS = 1 << 1,
};

static void
test_plane_position_with_output(data_t *data,
				enum pipe pipe,
				int plane,
				igt_output_t *output,
				igt_crc_t *reference_crc,
				unsigned int flags)
{
	igt_plane_t *primary, *sprite;
	struct igt_fb primary_fb, sprite_fb;
	drmModeModeInfo *mode;
	igt_crc_t crc, crc2;

	igt_info("Testing connector %s using pipe %s plane %d\n",
		 igt_output_name(output), kmstest_pipe_name(pipe), plane);

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	sprite = igt_output_get_plane(output, plane);

	create_fb_for_mode__position(data, mode, 100, 100, 64, 64,
				     &primary_fb);
	igt_plane_set_fb(primary, &primary_fb);

	igt_create_color_fb(data->drm_fd,
				64, 64, /* width, height */
				DRM_FORMAT_XRGB8888,
				LOCAL_DRM_FORMAT_MOD_NONE,
				0.0, 1.0, 0.0,
				&sprite_fb);
	igt_plane_set_fb(sprite, &sprite_fb);

	if (flags & TEST_POSITION_FULLY_COVERED)
		igt_plane_set_position(sprite, 100, 100);
	else
		igt_plane_set_position(sprite, 132, 132);

	igt_display_commit(&data->display);

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);

	if (flags & TEST_DPMS) {
		kmstest_set_connector_dpms(data->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc2);

	if (flags & TEST_POSITION_FULLY_COVERED)
		igt_assert_crc_equal(reference_crc, &crc);
	else {
		;/* FIXME: missing reference CRCs */
	}

	igt_assert_crc_equal(&crc, &crc2);

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(sprite, NULL);

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_ANY);
}

static void
test_plane_position(data_t *data, enum pipe pipe, unsigned int flags)
{
	igt_output_t *output;
	int connected_outs = 0;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		int n_planes = data->display.pipes[pipe].n_planes;
		igt_crc_t reference_crc;

		test_init(data, pipe);

		test_grab_crc(data, output, pipe, &green, &reference_crc);

		for (int plane = 1; plane < n_planes; plane++)
			test_plane_position_with_output(data, pipe, plane,
							output, &reference_crc,
							flags);

		test_fini(data);

		connected_outs++;
	}

	igt_skip_on(connected_outs == 0);
}

/*
 * Plane panning test.
 *   - We start by grabbing reference CRCs of a full red and a full blue fb
 *     being scanned out on the primary plane
 *   - Then we create a big fb, sized (2 * hdisplay, 2 * vdisplay) and:
 *      - fill the top left quarter with red
 *      - fill the bottom right quarter with blue
 *   - The TEST_PANNING_TOP_LEFT test makes sure that with panning at (0, 0)
 *     we do get the same CRC than the full red fb.
 *   - The TEST_PANNING_BOTTOM_RIGHT test makes sure that with panning at
 *     (vdisplay, hdisplay) we do get the same CRC than the full blue fb.
 */
static void
create_fb_for_mode__panning(data_t *data, drmModeModeInfo *mode,
			    struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay * 2, mode->vdisplay * 2,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE,
			      fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, fb);

	igt_paint_color(cr, 0, 0, mode->hdisplay, mode->vdisplay,
			1.0, 0.0, 0.0);

	igt_paint_color(cr,
			mode->hdisplay, mode->vdisplay,
			mode->hdisplay, mode->vdisplay,
			0.0, 0.0, 1.0);

	igt_put_cairo_ctx(data->drm_fd, fb, cr);
}

enum {
	TEST_PANNING_TOP_LEFT	  = 1 << 0,
	TEST_PANNING_BOTTOM_RIGHT = 1 << 1,
	TEST_SUSPEND_RESUME	  = 1 << 2,
};

static void
test_plane_panning_with_output(data_t *data,
			       enum pipe pipe,
			       int plane,
			       igt_output_t *output,
			       igt_crc_t *red_crc, igt_crc_t *blue_crc,
			       unsigned int flags)
{
	igt_plane_t *primary;
	struct igt_fb primary_fb;
	drmModeModeInfo *mode;
	igt_crc_t crc;

	igt_info("Testing connector %s using pipe %s plane %d\n",
		 igt_output_name(output), kmstest_pipe_name(pipe), plane);

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);
	primary = igt_output_get_plane(output, 0);

	create_fb_for_mode__panning(data, mode, &primary_fb);
	igt_plane_set_fb(primary, &primary_fb);

	if (flags & TEST_PANNING_TOP_LEFT)
		igt_fb_set_position(&primary_fb, primary, 0, 0);
	else
		igt_fb_set_position(&primary_fb, primary, mode->hdisplay, mode->vdisplay);

	igt_display_commit(&data->display);

	if (flags & TEST_SUSPEND_RESUME)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);

	if (flags & TEST_PANNING_TOP_LEFT)
		igt_assert_crc_equal(red_crc, &crc);
	else
		igt_assert_crc_equal(blue_crc, &crc);

	igt_plane_set_fb(primary, NULL);

	/* reset states to neutral values, assumed by other tests */
	igt_output_set_pipe(output, PIPE_ANY);
	igt_fb_set_position(&primary_fb, primary, 0, 0);
}

static void
test_plane_panning(data_t *data, enum pipe pipe, unsigned int flags)
{
	igt_output_t *output;
	int connected_outs = 0;

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		int n_planes = data->display.pipes[pipe].n_planes;
		igt_crc_t red_crc;
		igt_crc_t blue_crc;

		test_init(data, pipe);

		test_grab_crc(data, output, pipe, &red, &red_crc);
		test_grab_crc(data, output, pipe, &blue, &blue_crc);

		for (int plane = 1; plane < n_planes; plane++)
			test_plane_panning_with_output(data, pipe, plane,
						       output,
						       &red_crc, &blue_crc,
						       flags);

		test_fini(data);

		connected_outs++;
	}

	igt_skip_on(connected_outs == 0);
}

static const color_t colors[] = {
	{ 1.0f, 0.0f, 0.0f, },
	{ 0.0f, 1.0f, 0.0f, },
	{ 0.0f, 0.0f, 1.0f, },
	{ 1.0f, 1.0f, 1.0f, },
	{ 0.0f, 0.0f, 0.0f, },
	{ 0.0f, 1.0f, 1.0f, },
	{ 1.0f, 0.0f, 1.0f, },
	{ 1.0f, 1.0f, 0.0f, },
};

static void set_legacy_lut(data_t *data, enum pipe pipe,
			   uint16_t mask)
{
	igt_pipe_t *pipe_obj = &data->display.pipes[pipe];
	drmModeCrtc *crtc;
	uint16_t *lut;
	int i, lut_size;

	crtc = drmModeGetCrtc(data->drm_fd, pipe_obj->crtc_id);
	lut_size = crtc->gamma_size;
	drmModeFreeCrtc(crtc);

	lut = malloc(sizeof(uint16_t) * lut_size);

	for (i = 0; i < lut_size; i++)
		lut[i] = (i * 0xffff / (lut_size - 1)) & mask;

	igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd, pipe_obj->crtc_id,
					  lut_size, lut, lut, lut), 0);

	free(lut);
}

#define IGT_FORMAT_FMT "%c%c%c%c (0x%08x)"
#define IGT_FORMAT_ARGS(f) ((f) >> 0) & 0xff, ((f) >> 8) & 0xff, \
		((f) >> 16) & 0xff, ((f) >> 24) & 0xff, (f)

static void test_format_plane_color(data_t *data, enum pipe pipe,
				    igt_plane_t *plane,
				    uint32_t format, int width, int height,
				    int color, igt_crc_t *crc, struct igt_fb *fb)
{
	const color_t *c = &colors[color];
	struct igt_fb old_fb = *fb;

	igt_create_color_fb(data->drm_fd, width, height,
			    format, LOCAL_DRM_FORMAT_MOD_NONE,
			    c->red, c->green, c->blue, fb);

	igt_plane_set_fb(plane, fb);

	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_UNIVERSAL);
	igt_pipe_crc_get_current(data->display.drm_fd, data->pipe_crc, crc);

	igt_remove_fb(data->drm_fd, &old_fb);
}

static void test_format_plane(data_t *data, enum pipe pipe,
			      igt_output_t *output, igt_plane_t *plane)
{
	igt_plane_t *primary;
	struct igt_fb primary_fb;
	struct igt_fb fb = {};
	drmModeModeInfo *mode;
	uint32_t format, ref_format;
	uint64_t width, height;
	igt_crc_t ref_crc[ARRAY_SIZE(colors)];

	mode = igt_output_get_mode(output);
	if (plane->type != DRM_PLANE_TYPE_CURSOR) {
		width = mode->hdisplay;
		height = mode->vdisplay;
		ref_format = format = DRM_FORMAT_XRGB8888;
	} else {
		if (!plane->drm_plane) {
			igt_debug("Only legacy cursor ioctl supported, skipping cursor plane\n");
			return;
		}
		do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &width));
		do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &height));
		ref_format = format = DRM_FORMAT_ARGB8888;
	}

	igt_debug("Testing connector %s on %s plane %s.%u\n",
		  igt_output_name(output), kmstest_plane_type_name(plane->type),
		  kmstest_pipe_name(pipe), plane->index);

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE, &primary_fb);

	igt_output_set_pipe(output, pipe);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &primary_fb);

	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	set_legacy_lut(data, pipe, 0xfc00);

	test_init(data, pipe);
	igt_pipe_crc_start(data->pipe_crc);

	igt_info("Testing format " IGT_FORMAT_FMT " on %s.%u\n",
		 IGT_FORMAT_ARGS(format),
		 kmstest_pipe_name(pipe), plane->index);

	for (int i = 0; i < ARRAY_SIZE(colors); i++) {
		test_format_plane_color(data, pipe, plane,
					format, width, height,
					i, &ref_crc[i], &fb);
	}

	for (int i = 0; i < plane->drm_plane->count_formats; i++) {
		igt_crc_t crc;

		format = plane->drm_plane->formats[i];

		if (format == ref_format)
			continue;

		if (!igt_fb_supported_format(format))
			continue;

		/*
		 * There seems to be some issue there with the CRC not
		 * matching. Both CRCs are stable, but don't match,
		 * which seems to indicate some issue with the CRC
		 * computation logic, but I haven't been able to find
		 * what.
		 */
		if (format == DRM_FORMAT_XBGR8888)
			continue;

		igt_info("Testing format " IGT_FORMAT_FMT " on %s.%u\n",
			 IGT_FORMAT_ARGS(format),
			 kmstest_pipe_name(pipe), plane->index);

		for (int j = 0; j < ARRAY_SIZE(colors); j++) {
			test_format_plane_color(data, pipe, plane,
						format, width, height,
						j, &crc, &fb);

			igt_assert_crc_equal(&crc, &ref_crc[j]);
		}
	}

	igt_pipe_crc_stop(data->pipe_crc);
	test_fini(data);

	set_legacy_lut(data, pipe, 0xffff);

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(plane, NULL);
	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(data->drm_fd, &fb);
	igt_remove_fb(data->drm_fd, &primary_fb);
}

static void
test_pixel_formats(data_t *data, enum pipe pipe)
{
	igt_output_t *output;

	igt_display_require_output_on_pipe(&data->display, pipe);

	for_each_valid_output_on_pipe(&data->display, pipe, output) {
		igt_plane_t *plane;

		for_each_plane_on_pipe(&data->display, pipe, plane)
			test_format_plane(data, pipe, output, plane);

		igt_output_set_pipe(output, PIPE_ANY);
	}
}

static void
run_tests_for_pipe_plane(data_t *data, enum pipe pipe)
{
	igt_fixture {
		igt_skip_on(pipe >= data->display.n_pipes);
		igt_require(data->display.pipes[pipe].n_planes > 0);
	}

	igt_subtest_f("pixel-format-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_pixel_formats(data, pipe);

	igt_subtest_f("plane-position-covered-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, TEST_POSITION_FULLY_COVERED);

	igt_subtest_f("plane-position-hole-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, 0);

	igt_subtest_f("plane-position-hole-dpms-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_plane_position(data, pipe, TEST_DPMS);

	igt_subtest_f("plane-panning-top-left-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_plane_panning(data, pipe, TEST_PANNING_TOP_LEFT);

	igt_subtest_f("plane-panning-bottom-right-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_plane_panning(data, pipe, TEST_PANNING_BOTTOM_RIGHT);

	igt_subtest_f("plane-panning-bottom-right-suspend-pipe-%s-planes",
		      kmstest_pipe_name(pipe))
		test_plane_panning(data, pipe, TEST_PANNING_BOTTOM_RIGHT |
					       TEST_SUSPEND_RESUME);
}


static data_t data;

igt_main
{
	enum pipe pipe;

	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
	}

	for_each_pipe_static(pipe)
		run_tests_for_pipe_plane(&data, pipe);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
