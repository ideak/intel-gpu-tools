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
#include "igt_vec.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Throw away enough lsbs in pixel formats tests
 * to get a match despite some differences between
 * the software and hardware YCbCr<->RGB conversion
 * routines.
 */
#define LUT_MASK 0xf800

/* restricted pipe count */
#define CRTC_RESTRICT_CNT 2

typedef struct {
	float red;
	float green;
	float blue;
} color_t;

typedef struct {
	int x, y;
	color_t color;
} rectangle_t;

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_pipe_crc_t *pipe_crc;
	const color_t *colors;
	int num_colors;
	uint32_t crop;
	bool extended;
} data_t;

static bool all_pipes;

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

enum {
	TEST_POSITION_PARTIALLY_COVERED = 1 << 0,
	TEST_DPMS                       = 1 << 1,
	TEST_PANNING_TOP_LEFT           = 1 << 2,
	TEST_PANNING_BOTTOM_RIGHT       = 1 << 3,
	TEST_SUSPEND_RESUME             = 1 << 4,
};

/*
 * create a colored fb, possibly with a series of 64x64 colored rectangles (used
 * for position tests)
 */
static void
create_fb_for_mode(data_t *data, drmModeModeInfo *mode,
		   color_t *fb_color,
		   const rectangle_t *rects, int rect_cnt,
		   struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE,
			      fb);
	igt_assert_fd(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, fb);
	igt_paint_color(cr, 0, 0, mode->hdisplay, mode->vdisplay,
			fb_color->red, fb_color->green, fb_color->blue);
	for (int i = 0; i < rect_cnt; i++) {
		const rectangle_t *rect = &rects[i];
		igt_paint_color(cr,
				rect->x, rect->y, 64, 64,
				rect->color.red,
				rect->color.green,
				rect->color.blue);
	}

	igt_put_cairo_ctx(cr);
}

static void
test_grab_crc(data_t *data, igt_output_t *output, enum pipe pipe,
	      color_t *fb_color, unsigned int flags, igt_crc_t *crc /* out */)
{
	struct igt_fb fb;
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	char *crc_str;
	int ret;

	igt_output_set_pipe(output, pipe);

	primary = igt_output_get_plane(output, 0);

	mode = igt_output_get_mode(output);
	if (flags & TEST_POSITION_PARTIALLY_COVERED) {
		static const rectangle_t rects[] = {
			{ .x = 100, .y = 100, .color = { 0.0, 0.0, 0.0 }},
			{ .x = 132, .y = 132, .color = { 0.0, 1.0, 0.0 }},
		};

		create_fb_for_mode(data, mode, fb_color,
				   rects, ARRAY_SIZE(rects), &fb);
	} else {
		igt_assert_fd(igt_create_color_fb(data->drm_fd,
						  mode->hdisplay, mode->vdisplay,
						  DRM_FORMAT_XRGB8888,
						  LOCAL_DRM_FORMAT_MOD_NONE,
						  fb_color->red, fb_color->green, fb_color->blue,
						  &fb));
	}

	igt_plane_set_fb(primary, &fb);
	ret = igt_display_try_commit2(&data->display, COMMIT_LEGACY);
	igt_skip_on(ret != 0);

	igt_pipe_crc_collect_crc(data->pipe_crc, crc);

	igt_plane_set_fb(primary, NULL);
	igt_display_commit(&data->display);

	igt_remove_fb(data->drm_fd, &fb);

	crc_str = igt_crc_to_string(crc);
	igt_debug("CRC for a %s covered (%.02f,%.02f,%.02f) fb: %s\n",
		  flags & TEST_POSITION_PARTIALLY_COVERED ? "partially" : "fully",
		  fb_color->red, fb_color->green, fb_color->blue,
		  crc_str);
	free(crc_str);
}

/*
 * Plane position test.
 *   - For testing positions that fully cover our hole, we start by grabbing a
 *     reference CRC of a full green fb being scanned out on the primary plane.
 *     For testing positions that only partially cover our hole, we instead use
 *     a full green fb with a partially covered black rectangle.
 *   - Then we scannout 2 planes:
 *      - the primary plane uses a green fb with a black rectangle
 *      - a plane, on top of the primary plane, with a green fb that is set-up
 *        to fully or partially cover the black rectangle of the primary plane
 *        fb
 *     The resulting CRC should be identical to the reference CRC
 */

typedef struct {
	data_t *data;
	igt_crc_t reference_crc;
} test_position_t;

static void
test_plane_position_with_output(data_t *data,
				enum pipe pipe,
				int plane,
				igt_output_t *output,
				igt_crc_t *reference_crc,
				unsigned int flags)
{
	rectangle_t rect = { .x = 100, .y = 100, .color = { 0.0, 0.0, 0.0 }};
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

	create_fb_for_mode(data, mode, &green, &rect, 1, &primary_fb);
	igt_plane_set_fb(primary, &primary_fb);

	igt_create_color_fb(data->drm_fd,
			    64, 64, /* width, height */
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 1.0, 0.0,
			    &sprite_fb);
	igt_plane_set_fb(sprite, &sprite_fb);

	if (flags & TEST_POSITION_PARTIALLY_COVERED)
		igt_plane_set_position(sprite, 132, 132);
	else
		igt_plane_set_position(sprite, 100, 100);

	igt_display_commit(&data->display);

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(reference_crc, &crc);

	if (flags & TEST_DPMS) {
		kmstest_set_connector_dpms(data->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc2);

	igt_assert_crc_equal(&crc, &crc2);

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(sprite, NULL);

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(data->drm_fd, &primary_fb);
	igt_remove_fb(data->drm_fd, &sprite_fb);
}

static void
test_plane_position(data_t *data, enum pipe pipe, unsigned int flags)
{
	int n_planes = data->display.pipes[pipe].n_planes;
	igt_output_t *output;
	igt_crc_t reference_crc;

	output = igt_get_single_output_for_pipe(&data->display, pipe);
	igt_require(output);

	test_init(data, pipe);
	test_grab_crc(data, output, pipe, &green, flags, &reference_crc);

	for (int plane = 1; plane < n_planes; plane++)
		test_plane_position_with_output(data, pipe, plane,
						output, &reference_crc,
						flags);

	test_fini(data);
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
create_fb_for_mode_panning(data_t *data, drmModeModeInfo *mode,
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

	igt_put_cairo_ctx(cr);
}

static void
test_plane_panning_with_output(data_t *data,
			       enum pipe pipe,
			       igt_output_t *output,
			       igt_crc_t *ref_crc,
			       unsigned int flags)
{
	igt_plane_t *primary;
	struct igt_fb primary_fb;
	drmModeModeInfo *mode;
	igt_crc_t crc;

	igt_info("Testing connector %s using pipe %s\n",
		 igt_output_name(output), kmstest_pipe_name(pipe));

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);
	primary = igt_output_get_plane(output, 0);

	create_fb_for_mode_panning(data, mode, &primary_fb);
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

	igt_assert_crc_equal(ref_crc, &crc);

	igt_plane_set_fb(primary, NULL);

	/* reset states to neutral values, assumed by other tests */
	igt_output_set_pipe(output, PIPE_NONE);
	igt_fb_set_position(&primary_fb, primary, 0, 0);
	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(data->drm_fd, &primary_fb);
}

static void
test_plane_panning(data_t *data, enum pipe pipe, unsigned int flags)
{
	igt_output_t *output;
	igt_crc_t ref_crc;

	output = igt_get_single_output_for_pipe(&data->display, pipe);
	igt_require(output);

	test_init(data, pipe);

	if (flags & TEST_PANNING_TOP_LEFT)
		test_grab_crc(data, output, pipe, &red, flags, &ref_crc);
	else
		test_grab_crc(data, output, pipe, &blue, flags, &ref_crc);

	test_plane_panning_with_output(data, pipe, output, &ref_crc, flags);

	test_fini(data);
}

static const color_t colors_extended[] = {
	{ 1.0f, 0.0f, 0.0f, },
	{ 0.0f, 1.0f, 0.0f, },
	{ 0.0f, 0.0f, 1.0f, },
	{ 1.0f, 1.0f, 1.0f, },
	{ 0.0f, 0.0f, 0.0f, },
	{ 0.0f, 1.0f, 1.0f, },
	{ 1.0f, 0.0f, 1.0f, },
	{ 1.0f, 1.0f, 0.0f, },
};

static const color_t colors_reduced[] = {
	{ 1.0f, 0.0f, 0.0f, },
	{ 1.0f, 1.0f, 1.0f, },
	{ 0.0f, 0.0f, 0.0f, },
	{ 0.0f, 1.0f, 1.0f, },
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

static bool set_c8_legacy_lut(data_t *data, enum pipe pipe,
			      uint16_t mask)
{
	igt_pipe_t *pipe_obj = &data->display.pipes[pipe];
	drmModeCrtc *crtc;
	uint16_t *r, *g, *b;
	int i, lut_size;

	crtc = drmModeGetCrtc(data->drm_fd, pipe_obj->crtc_id);
	lut_size = crtc->gamma_size;
	drmModeFreeCrtc(crtc);

	if (lut_size != 256)
		return false;

	r = malloc(sizeof(uint16_t) * 3 * lut_size);
	g = r + lut_size;
	b = g + lut_size;

	/* igt_fb uses RGB332 for C8 */
	for (i = 0; i < lut_size; i++) {
		r[i] = (((i & 0xe0) >> 5) * 0xffff / 0x7) & mask;
		g[i] = (((i & 0x1c) >> 2) * 0xffff / 0x7) & mask;
		b[i] = (((i & 0x03) >> 0) * 0xffff / 0x3) & mask;
	}

	igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd, pipe_obj->crtc_id,
					  lut_size, r, g, b), 0);

	free(r);

	return true;
}

static void draw_entire_color_array(data_t *data, cairo_t *cr, uint32_t format,
				    struct igt_fb *fb)
{
	const int color_amount = ARRAY_SIZE(colors_extended);
	const int x = format == DRM_FORMAT_XRGB8888 ? 0 : data->crop;

	for (int n = 0; n < color_amount; n++) {
		int y = (fb->height - x * 2) * n / color_amount + x;

		igt_paint_color(cr, x, y,
				fb->width - x * 2,
				(fb->height - x * 2) / color_amount,
				colors_extended[n].red,
				colors_extended[n].green,
				colors_extended[n].blue);
	}
}

static void prepare_format_color(data_t *data, enum pipe pipe,
				 igt_plane_t *plane,
				 uint32_t format, uint64_t modifier,
				 int width, int height,
				 enum igt_color_encoding color_encoding,
				 enum igt_color_range color_range,
				 const color_t *c, struct igt_fb *fb,
				 bool packed)
{
	cairo_t *cr;
	const int localcrop = format == DRM_FORMAT_XRGB8888 ? 0 : data->crop;

	igt_create_fb_with_bo_size(data->drm_fd,
				   width + localcrop * 2,
				   height + localcrop * 2,
				   format, modifier, color_encoding,
				   color_range, fb, 0, 0);

	cr = igt_get_cairo_ctx(data->drm_fd, fb);

	/*
	 * paint border in inverted color, then visible area in middle
	 * with correct color for clamping test
	 */
	if (localcrop)
		igt_paint_color(cr, 0, 0,
				width + localcrop * 2,
				height + localcrop * 2,
				1.0f - c->red,
				1.0f - c->green,
				1.0f - c->blue);


	if (packed)
		draw_entire_color_array(data, cr, format, fb);
	else
		igt_paint_color(cr, localcrop, localcrop,
				width, height,
				c->red, c->green, c->blue);

	igt_put_cairo_ctx(cr);
	igt_plane_set_fb(plane, fb);

	/*
	 * if clamping test.
	 */
	if (localcrop) {
		igt_fb_set_position(fb, plane, localcrop, localcrop);
		igt_fb_set_size(fb, plane, width, height);
		igt_plane_set_size(plane, width, height);
	}
}

static int num_unique_crcs(const igt_crc_t crc[], int num_crc)
{
	int num_unique_crc = 0;

	for (int i = 0; i < num_crc; i++) {
		int j;

		for (j = i + 1; j < num_crc; j++) {
			if (igt_check_crc_equal(&crc[i], &crc[j]))
				break;
		}

		if (j == num_crc)
			num_unique_crc++;
	}

	return num_unique_crc;
}

static void capture_crc(data_t *data, unsigned int vblank, igt_crc_t *crc)
{
	igt_pipe_crc_get_for_frame(data->drm_fd, data->pipe_crc, vblank, crc);

	igt_fail_on_f(!igt_skip_crc_compare &&
		      crc->has_valid_frame && crc->frame != vblank,
		      "Got CRC for the wrong frame (got %u, expected %u). CRC buffer overflow?\n",
		      crc->frame, vblank);
}

static void capture_format_crcs_packed(data_t *data, enum pipe pipe,
				       igt_plane_t *plane,
				       uint32_t format, uint64_t modifier,
				       int width, int height,
				       enum igt_color_encoding encoding,
				       enum igt_color_range range,
				       igt_crc_t crc[], struct igt_fb *fb)
{
	struct igt_fb old_fb = *fb;
	const color_t black = { 0.0f, 0.0f, 0.0f };

	prepare_format_color(data, pipe, plane, format, modifier,
			     width, height, encoding, range, &black, fb, true);

	igt_display_commit2(&data->display, data->display.is_atomic ?
			    COMMIT_ATOMIC : COMMIT_UNIVERSAL);

	igt_remove_fb(data->drm_fd, &old_fb);

	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &crc[0]);
}

static void capture_format_crcs_planar(data_t *data, enum pipe pipe,
				       igt_plane_t *plane,
				       uint32_t format, uint64_t modifier,
				       int width, int height,
				       enum igt_color_encoding encoding,
				       enum igt_color_range range,
				       igt_crc_t crc[], struct igt_fb *fb)
{
	unsigned int vblank[ARRAY_SIZE(colors_extended)];
	struct drm_event_vblank ev;
	int i;

restart_round:
	for (i = 0; i < data->num_colors; i++) {
		const color_t *c = &data->colors[i];
		struct igt_fb old_fb = *fb;
		int ret;

		prepare_format_color(data, pipe, plane, format, modifier,
				     width, height, encoding, range, c, fb,
				     false);

		if (data->display.is_atomic && i >= 1) {
			igt_assert(read(data->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
			/*
			 * The last time we saw the crc for
			 * flip N-2 is when the flip N-1 latched.
			 */
			if (i >= 2)
				vblank[i - 2] = ev.sequence;
		}

		/*
		 * The flip issued during frame N will latch
		 * at the start of frame N+1, and its CRC will
		 * be ready at the start of frame N+2. So the
		 * CRC captured here before the flip is issued
		 * is for frame N-2.
		 */
		if (i >= 2)
			capture_crc(data, vblank[i - 2], &crc[i - 2]);

		if (data->display.is_atomic) {
			/*
			 * Use non-blocking commits to allow the next fb
			 * to be prepared in parallel while the current fb
			 * awaits to be latched.
			 */
			ret = igt_display_try_commit_atomic(&data->display,
							    DRM_MODE_ATOMIC_NONBLOCK |
							    DRM_MODE_PAGE_FLIP_EVENT, NULL);
			if (ret) {
				/*
				 * there was needed modeset for pixel format.
				 * modeset here happen only on first color of
				 * given set so restart round as modeset will
				 * mess up crc frame sequence.
				 */
				igt_display_commit_atomic(&data->display,
							  DRM_MODE_ATOMIC_ALLOW_MODESET,
							  NULL);
				igt_remove_fb(data->drm_fd, &old_fb);
				goto restart_round;
			}
		} else {
			/*
			 * Last moment to grab the previous crc
			 * is when the next flip latches.
			 */
			if (i >= 1)
				vblank[i - 1] = kmstest_get_vblank(data->drm_fd, pipe, 0) + 1;

			/*
			 * Can't use drmModePageFlip() since we need to
			 * change pixel format and potentially update some
			 * properties as well.
			 */
			igt_display_commit2(&data->display, COMMIT_UNIVERSAL);

			/* setplane for the cursor does not block */
			if (plane->type == DRM_PLANE_TYPE_CURSOR) {
				igt_display_t *display = &data->display;

				igt_wait_for_vblank(data->drm_fd,
						display->pipes[pipe].crtc_offset);
			}
		}

		igt_remove_fb(data->drm_fd, &old_fb);
	}

	if (data->display.is_atomic) {
		igt_assert(read(data->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
		/*
		 * The last time we saw the crc for
		 * flip N-2 is when the flip N-1 latched.
		 */
		if (i >= 2)
			vblank[i - 2] = ev.sequence;
		/*
		 * The last crc is available earliest one
		 * frame after the last flip latched.
		 */
		vblank[i - 1] = ev.sequence + 1;
	} else {
		/*
		 * The last crc is available earliest one
		 * frame after the last flip latched.
		 */
		vblank[i - 1] = kmstest_get_vblank(data->drm_fd, pipe, 0) + 1;
	}

	/*
	 * Get the remaining two crcs
	 *
	 * TODO: avoid the extra wait by maintaining the pipeline
	 * between different pixel formats as well? Could get messy.
	 */
	if (i >= 2)
		capture_crc(data, vblank[i - 2], &crc[i - 2]);
	capture_crc(data, vblank[i - 1], &crc[i - 1]);
}

static bool test_format_plane_colors(data_t *data, enum pipe pipe,
				     igt_plane_t *plane,
				     uint32_t format, uint64_t modifier,
				     int width, int height,
				     enum igt_color_encoding encoding,
				     enum igt_color_range range,
				     igt_crc_t ref_crc[],
				     struct igt_fb *fb)
{
	igt_crc_t crc[ARRAY_SIZE(colors_extended)];
	unsigned int crc_mismatch_mask = 0;
	int crc_mismatch_count = 0;
	bool result = true;
	int i, total_crcs = 1;
	bool planar = igt_format_is_yuv_semiplanar(format);

	if (planar) {
		capture_format_crcs_planar(data, pipe, plane, format, modifier,
					   width, height, encoding, range, crc,
					   fb);
		total_crcs = data->num_colors;
	} else
		capture_format_crcs_packed(data, pipe, plane, format, modifier,
					   width, height, encoding, range, crc,
					   fb);

	for (i = 0; i < total_crcs; i++) {
		if (!igt_check_crc_equal(&crc[i], &ref_crc[i])) {
			crc_mismatch_count++;
			crc_mismatch_mask |= (1 << i);
			result = false;
		}
	}

	if (crc_mismatch_count)
		igt_warn("CRC mismatches with format " IGT_FORMAT_FMT " on %s.%u with %d/%d solid colors tested (0x%X)\n",
			 IGT_FORMAT_ARGS(format), kmstest_pipe_name(pipe),
			 plane->index, crc_mismatch_count, data->num_colors, crc_mismatch_mask);

	return result;
}

static bool test_format_plane_rgb(data_t *data, enum pipe pipe,
				  igt_plane_t *plane,
				  uint32_t format, uint64_t modifier,
				  int width, int height,
				  igt_crc_t ref_crc[],
				  struct igt_fb *fb)
{
	igt_info("Testing format " IGT_FORMAT_FMT " / modifier 0x%" PRIx64 " on %s.%u\n",
		 IGT_FORMAT_ARGS(format), modifier,
		 kmstest_pipe_name(pipe), plane->index);

	return test_format_plane_colors(data, pipe, plane,
					format, modifier,
					width, height,
					IGT_COLOR_YCBCR_BT601,
					IGT_COLOR_YCBCR_LIMITED_RANGE,
					ref_crc, fb);
}

static bool test_format_plane_yuv(data_t *data, enum pipe pipe,
				  igt_plane_t *plane,
				  uint32_t format, uint64_t modifier,
				  int width, int height,
				  igt_crc_t ref_crc[],
				  struct igt_fb *fb)
{
	bool result = true;

	if (!igt_plane_has_prop(plane, IGT_PLANE_COLOR_ENCODING))
		return true;
	if (!igt_plane_has_prop(plane, IGT_PLANE_COLOR_RANGE))
		return true;

	for (enum igt_color_encoding e = 0; e < IGT_NUM_COLOR_ENCODINGS; e++) {
		if (!igt_plane_try_prop_enum(plane,
					     IGT_PLANE_COLOR_ENCODING,
					     igt_color_encoding_to_str(e)))
			continue;

		for (enum igt_color_range r = 0; r < IGT_NUM_COLOR_RANGES; r++) {
			if (!igt_plane_try_prop_enum(plane,
						     IGT_PLANE_COLOR_RANGE,
						     igt_color_range_to_str(r)))
				continue;

			igt_info("Testing format " IGT_FORMAT_FMT " / modifier 0x%" PRIx64 " (%s, %s) on %s.%u\n",
				 IGT_FORMAT_ARGS(format), modifier,
				 igt_color_encoding_to_str(e),
				 igt_color_range_to_str(r),
				 kmstest_pipe_name(pipe), plane->index);

			result &= test_format_plane_colors(data, pipe, plane,
							   format, modifier,
							   width, height,
							   e, r, ref_crc, fb);

			/*
			 * Only test all combinations for linear or
			 * if the user asked for extended tests.
			 */
			if (result && !data->extended &&
			    modifier != DRM_FORMAT_MOD_LINEAR)
				break;
		}
		if (result && !data->extended &&
		    modifier != DRM_FORMAT_MOD_LINEAR)
			break;
	}

	return result;
}

enum crc_set { PACKED_CRC_SET,
	       PLANAR_CRC_SET,
	       MAX_CRC_SET };

struct format_mod {
	uint64_t modifier;
	uint32_t format;
};

static bool test_format_plane(data_t *data, enum pipe pipe,
			      igt_output_t *output, igt_plane_t *plane, igt_fb_t *primary_fb)
{
	struct igt_fb fb = {};
	struct igt_fb *clear_fb = plane->type == DRM_PLANE_TYPE_PRIMARY ? primary_fb : NULL;
	drmModeModeInfo *mode;
	uint64_t width, height;
	igt_crc_t ref_crc[MAX_CRC_SET][ARRAY_SIZE(colors_extended)];
	struct igt_vec tested_formats;
	struct format_mod ref = {};
	igt_crc_t* crcset;
	bool result = true;

	/*
	 * No clamping test for cursor plane
	 */
	if (data->crop != 0 && plane->type == DRM_PLANE_TYPE_CURSOR)
		return true;

	igt_vec_init(&tested_formats, sizeof(struct format_mod));

	mode = igt_output_get_mode(output);
	if (plane->type != DRM_PLANE_TYPE_CURSOR) {
		width = mode->hdisplay;
		height = mode->vdisplay;
		ref.format = DRM_FORMAT_XRGB8888;
		ref.modifier = DRM_FORMAT_MOD_NONE;
	} else {
		if (!plane->drm_plane) {
			igt_debug("Only legacy cursor ioctl supported, skipping cursor plane\n");
			return true;
		}
		do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &width));
		do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &height));
		ref.format = DRM_FORMAT_ARGB8888;
		ref.modifier = DRM_FORMAT_MOD_NONE;
	}

	igt_debug("Testing connector %s on %s plane %s.%u\n",
		  igt_output_name(output), kmstest_plane_type_name(plane->type),
		  kmstest_pipe_name(pipe), plane->index);

	igt_pipe_crc_start(data->pipe_crc);

	igt_info("Testing format " IGT_FORMAT_FMT " / modifier 0x%" PRIx64 " on %s.%u\n",
		 IGT_FORMAT_ARGS(ref.format), ref.modifier,
		 kmstest_pipe_name(pipe), plane->index);

	if (data->display.is_atomic) {
		struct igt_fb test_fb;
		int ret;

		igt_create_fb(data->drm_fd, 64, 64, ref.format,
			      DRM_FORMAT_MOD_LINEAR, &test_fb);

		igt_plane_set_fb(plane, &test_fb);

		ret = igt_display_try_commit_atomic(&data->display,
						    DRM_MODE_ATOMIC_TEST_ONLY |
						    DRM_MODE_ATOMIC_ALLOW_MODESET,
						    NULL);
		if (!ret) {
			width = test_fb.width;
			height = test_fb.height;
		}

		igt_plane_set_fb(plane, clear_fb);

		igt_remove_fb(data->drm_fd, &test_fb);
	}

	capture_format_crcs_packed(data, pipe, plane, ref.format, ref.modifier,
				   width, height, IGT_COLOR_YCBCR_BT709,
				   IGT_COLOR_YCBCR_LIMITED_RANGE,
				   ref_crc[PACKED_CRC_SET], &fb);

	capture_format_crcs_planar(data, pipe, plane, ref.format, ref.modifier,
				   width, height, IGT_COLOR_YCBCR_BT709,
				   IGT_COLOR_YCBCR_LIMITED_RANGE,
				   ref_crc[PLANAR_CRC_SET], &fb);

	/*
	 * Make sure we have some difference between the colors. This
	 * at least avoids claiming success when everything is just
	 * black all the time (eg. if the plane is never even on).
	 */
	igt_require(num_unique_crcs(ref_crc[PLANAR_CRC_SET], data->num_colors) > 1);

	for (int i = 0; i < plane->format_mod_count; i++) {
		struct format_mod f = {
			.format = plane->formats[i],
			.modifier = plane->modifiers[i],
		};

		if (f.format == ref.format &&
		    f.modifier == ref.modifier)
			continue;

		/* test each format "class" only once in non-extended tests */
		if (!data->extended && f.modifier != DRM_FORMAT_MOD_LINEAR) {
			struct format_mod rf = {
				.format = igt_reduce_format(f.format),
				.modifier = f.modifier,
			};

			if (igt_vec_index(&tested_formats, &rf) >= 0) {
				igt_info("Skipping format " IGT_FORMAT_FMT " / modifier 0x%" PRIx64 " on %s.%u\n",
					 IGT_FORMAT_ARGS(f.format), f.modifier,
					 kmstest_pipe_name(pipe), plane->index);
				continue;
			}

			igt_vec_push(&tested_formats, &rf);
		}

		if (f.format == DRM_FORMAT_C8) {
			if (!set_c8_legacy_lut(data, pipe, LUT_MASK))
				continue;
		} else {
			if (!igt_fb_supported_format(f.format))
				continue;
		}

		crcset = ref_crc[(igt_format_is_yuv_semiplanar(f.format)
				 ? PLANAR_CRC_SET : PACKED_CRC_SET)];

		if (igt_format_is_yuv(f.format))
			result &= test_format_plane_yuv(data, pipe, plane,
							f.format, f.modifier,
							width, height,
							crcset, &fb);
		else
			result &= test_format_plane_rgb(data, pipe, plane,
							f.format, f.modifier,
							width, height,
							crcset, &fb);

		if (f.format == DRM_FORMAT_C8)
			set_legacy_lut(data, pipe, LUT_MASK);
	}

	igt_pipe_crc_stop(data->pipe_crc);

	igt_plane_set_fb(plane, clear_fb);
	igt_remove_fb(data->drm_fd, &fb);

	igt_vec_fini(&tested_formats);

	return result;
}

static bool skip_plane(data_t *data, igt_plane_t *plane)
{
	int index = plane->index;

	if (data->extended)
		return false;

	if (!is_i915_device(data->drm_fd))
		return false;

	if (plane->type == DRM_PLANE_TYPE_CURSOR)
		return false;

	if (intel_display_ver(intel_get_drm_devid(data->drm_fd)) < 11)
		return false;

	/*
	 * Test 1 HDR plane, 1 SDR UV plane, 1 SDR Y plane.
	 *
	 * Kernel registers planes in the hardware Z order:
	 * 0,1,2 HDR planes
	 * 3,4 SDR UV planes
	 * 5,6 SDR Y planes
	 */
	return index != 0 && index != 3 && index != 5;
}

static void
test_pixel_formats(data_t *data, enum pipe pipe)
{
	struct igt_fb primary_fb;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	bool result;
	igt_output_t *output;
	igt_plane_t *plane;

	if (data->extended) {
		data->colors = colors_extended;
		data->num_colors = ARRAY_SIZE(colors_extended);
	} else {
		data->colors = colors_reduced;
		data->num_colors = ARRAY_SIZE(colors_reduced);
	}

	output = igt_get_single_output_for_pipe(&data->display, pipe);
	igt_require(output);

	mode = igt_output_get_mode(output);

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE, &primary_fb);

	igt_output_set_pipe(output, pipe);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &primary_fb);

	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	set_legacy_lut(data, pipe, LUT_MASK);

	test_init(data, pipe);

	result = true;
	for_each_plane_on_pipe(&data->display, pipe, plane) {
		if (skip_plane(data, plane))
			continue;
		result &= test_format_plane(data, pipe, output, plane, &primary_fb);
	}

	test_fini(data);

	set_legacy_lut(data, pipe, 0xffff);

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(data->drm_fd, &primary_fb);

	igt_assert_f(result, "At least one CRC mismatch happened\n");
}

static bool is_pipe_limit_reached(int count) {
	return count >= CRTC_RESTRICT_CNT && !all_pipes;
}

static void
run_tests_for_pipe_plane(data_t *data)
{
	enum pipe pipe;
	int count;
	igt_fixture {
		igt_require_pipe(&data->display, pipe);
		igt_require(data->display.pipes[pipe].n_planes > 0);
	}

	igt_describe("verify the pixel formats for given plane and pipe");
	igt_subtest_with_dynamic_f("pixel-format") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_pixel_formats(data, pipe);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}
	igt_describe("verify the pixel formats for given plane and pipe with source clamping");
	igt_subtest_with_dynamic_f("pixel-format-source-clamping") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe)) {
				data->crop = 4;
				test_pixel_formats(data, pipe);
			}
			if (is_pipe_limit_reached(++count))
				break;
		}
	}

	data->crop = 0;
	igt_describe("verify plane position using two planes to create a fully covered screen");
	igt_subtest_with_dynamic_f("plane-position-covered") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_plane_position(data, pipe, 0);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}

	igt_describe("verify plane position using two planes to create a partially covered screen");
	igt_subtest_with_dynamic_f("plane-position-hole") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_plane_position(data, pipe,
				TEST_POSITION_PARTIALLY_COVERED);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}

	igt_describe("verify plane position using two planes to create a partially covered screen and"
		       "check for DPMS");
	igt_subtest_with_dynamic_f("plane-position-hole-dpms") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_plane_position(data, pipe,
				TEST_POSITION_PARTIALLY_COVERED | TEST_DPMS);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}

	igt_describe("verify plane panning at top-left position using primary plane");
	igt_subtest_with_dynamic_f("plane-panning-top-left") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_plane_panning(data, pipe, TEST_PANNING_TOP_LEFT);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}

	igt_describe("verify plane panning at bottom-right position using primary plane");
	igt_subtest_with_dynamic_f("plane-panning-bottom-right") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_plane_panning(data, pipe, TEST_PANNING_BOTTOM_RIGHT);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}

	igt_describe("verify plane panning at bottom-right position using primary plane and executes system"
		       "suspend cycles");
	igt_subtest_with_dynamic_f("plane-panning-bottom-right-suspend") {
		count = 0;
		for_each_pipe(&data->display, pipe) {
			igt_dynamic_f("pipe-%s-planes", kmstest_pipe_name(pipe))
				test_plane_panning(data, pipe,
				TEST_PANNING_BOTTOM_RIGHT |
				TEST_SUSPEND_RESUME);
			if (is_pipe_limit_reached(++count))
				break;
		}
	}
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'e':
		data->extended = true;
		break;
	case 'p':
		all_pipes = true;
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{ .name = "all-pipes", .has_arg = false, .val = 'p', },
	{}
};

static const char help_str[] =
	"  --extended\t\tRun the extended tests\n"
	"  --all-pipes\t\tRun on all pipes.(Default it will Run only two pipes)\n";

static data_t data;

igt_main_args("", long_opts, help_str, opt_handler, &data)
{
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
	}

	run_tests_for_pipe_plane(&data);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
