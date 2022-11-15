/*
 * Copyright © 2013 Intel Corporation
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
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


IGT_TEST_DESCRIPTION(
   "Use the display CRC support to validate cursor plane functionality. "
   "The test will position the cursor plane either fully onscreen, "
   "partially onscreen, or fully offscreen, using either a fully opaque "
   "or fully transparent surface. In each case, it enables the cursor plane "
   "and then reads the PF CRC (hardware test) and compares it with the CRC "
   "value obtained when the cursor plane was disabled and its drawing is "
   "directly inserted on the PF by software.");

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

enum cursor_buffers {
	HWCURSORBUFFER,
	SWCOMPARISONBUFFER1,
	SWCOMPARISONBUFFER2,
	MAXCURSORBUFFER
};

typedef struct {
	int x;
	int y;
	int width;
	int height;
} cursorarea;

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb primary_fb[MAXCURSORBUFFER];
	struct igt_fb fb;
	igt_output_t *output;
	enum pipe pipe;
	int left, right, top, bottom;
	int screenw, screenh;
	int refresh;
	int curw, curh; /* cursor size */
	int cursor_max_w, cursor_max_h;
	igt_pipe_crc_t *pipe_crc;
	unsigned flags;
	igt_plane_t *primary;
	igt_plane_t *cursor;
	cairo_surface_t *surface;
	uint32_t devid;
	double alpha;
	int vblank_wait_count; /* because of msm */
	cursorarea oldcursorarea[MAXCURSORBUFFER];
} data_t;

#define TEST_DPMS (1<<0)
#define TEST_SUSPEND (1<<1)

#define RED 1.0, 0.0, 0.0
#define GREEN 0.0, 1.0, 0.0
#define BLUE 0.0, 0.0, 1.0
#define WHITE 1.0, 1.0, 1.0

static void draw_cursor(cairo_t *cr, cursorarea *cursor, double alpha)
{
	int wl, wr, ht, hb;

	/* deal with odd cursor width/height */
	wl = cursor->width / 2;
	wr = (cursor->width + 1) / 2;
	ht = cursor->height / 2;
	hb = (cursor->height + 1) / 2;

	/* Cairo doesn't like to be fed numbers that are too wild */
	if ((cursor->x < SHRT_MIN) || (cursor->x > SHRT_MAX) ||
	    (cursor->y < SHRT_MIN) || (cursor->y > SHRT_MAX))
		return;

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	/* 4 color rectangles in the corners, RGBY */
	igt_paint_color_alpha(cr, cursor->x, cursor->y, wl, ht, RED, alpha);
	igt_paint_color_alpha(cr, cursor->x + wl, cursor->y, wr, ht, GREEN, alpha);
	igt_paint_color_alpha(cr, cursor->x, cursor->y + ht, wl, hb, BLUE, alpha);
	igt_paint_color_alpha(cr, cursor->x + wl, cursor->y + ht, wr, hb, WHITE, alpha);
}

static void cursor_enable(data_t *data)
{
	igt_plane_set_fb(data->cursor, &data->fb);
	igt_plane_set_size(data->cursor, data->curw, data->curh);
	igt_fb_set_size(&data->fb, data->cursor, data->curw, data->curh);
}

static void cursor_disable(data_t *data)
{
	igt_plane_set_fb(data->cursor, NULL);
	igt_plane_set_position(data->cursor, 0, 0);
	igt_display_commit(&data->display);

	/* do this wait here so it will not need to be added everywhere */
	igt_wait_for_vblank_count(data->drm_fd,
				  data->display.pipes[data->pipe].crtc_offset,
				  data->vblank_wait_count);
}

static bool chv_cursor_broken(data_t *data, int x)
{
	uint32_t devid;

	if (!is_i915_device(data->drm_fd))
		return false;

	devid = intel_get_drm_devid(data->drm_fd);

	/*
	 * CHV gets a FIFO underrun on pipe C when cursor x coordinate
	 * is negative and the cursor visible.
	 *
	 * i915 is fixed to return -EINVAL on cursor updates with those
	 * negative coordinates, so require cursor update to fail with
	 * -EINVAL in that case.
	 *
	 * See also kms_chv_cursor_fail.c
	 */
	if (x >= 0)
		return false;

	return IS_CHERRYVIEW(devid) && data->pipe == PIPE_C;
}

static bool cursor_visible(data_t *data, int x, int y)
{
	if (x + data->curw <= 0 || y + data->curh <= 0)
		return false;

	if (x >= data->screenw || y >= data->screenh)
		return false;

	return true;
}

static void restore_image(data_t *data, uint32_t buffer, cursorarea *cursor)
{
	cairo_t *cr;

	cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb[buffer]);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, data->surface, 0, 0);
	cairo_rectangle(cr, data->oldcursorarea[buffer].x,
			data->oldcursorarea[buffer].y,
			data->oldcursorarea[buffer].width,
			data->oldcursorarea[buffer].height);
	cairo_fill(cr);

	if (cursor) {
		draw_cursor(cr, cursor, data->alpha);
		data->oldcursorarea[buffer] = *cursor;
	}

	igt_put_cairo_ctx(cr);
}

static void do_single_test(data_t *data, int x, int y, bool hw_test,
			   igt_crc_t *hwcrc)
{
	igt_display_t *display = &data->display;
	igt_pipe_crc_t *pipe_crc = data->pipe_crc;
	igt_crc_t crc;
	int ret = 0, swbufidx;

	igt_print_activity();

	if (hw_test) {
		/* Hardware test */
		igt_plane_set_position(data->cursor, x, y);

		if (chv_cursor_broken(data, x) && cursor_visible(data, x, y)) {
			ret = igt_display_try_commit2(display, COMMIT_LEGACY);
			igt_assert_eq(ret, -EINVAL);
			igt_plane_set_position(data->cursor, 0, y);
			return;
		}

		igt_display_commit(display);

		/* Extra vblank wait is because nonblocking cursor ioctl */
		igt_wait_for_vblank_count(data->drm_fd,
					  display->pipes[data->pipe].crtc_offset,
					  data->vblank_wait_count);

		igt_pipe_crc_get_current(data->drm_fd, pipe_crc, hwcrc);

		if (data->flags & (TEST_DPMS | TEST_SUSPEND)) {
			igt_crc_t crc_after;
			/*
			* stop/start crc to avoid dmesg notifications about userspace
			* reading too slow.
			*/
			igt_pipe_crc_stop(pipe_crc);

			if (data->flags & TEST_DPMS) {
				igt_debug("dpms off/on cycle\n");
				kmstest_set_connector_dpms(data->drm_fd,
							data->output->config.connector,
							DRM_MODE_DPMS_OFF);
				kmstest_set_connector_dpms(data->drm_fd,
							data->output->config.connector,
							DRM_MODE_DPMS_ON);
			}

			if (data->flags & TEST_SUSPEND)
				igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
							      SUSPEND_TEST_NONE);

			igt_pipe_crc_start(pipe_crc);
			igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &crc_after);
			igt_assert_crc_equal(hwcrc, &crc_after);
		}
	} else {
		/* Now render the same in software and collect crc */
		swbufidx = (data->primary->drm_plane->fb_id ==
			    data->primary_fb[SWCOMPARISONBUFFER1].fb_id) ?
			    SWCOMPARISONBUFFER2 : SWCOMPARISONBUFFER1;

		restore_image(data, swbufidx, &((cursorarea){x, y, data->curw, data->curh}));
		igt_plane_set_fb(data->primary, &data->primary_fb[swbufidx]);
		igt_display_commit(display);
		igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &crc);
		igt_assert_crc_equal(&crc, hwcrc);
	}
}

static void do_fail_test(data_t *data, int x, int y, int expect)
{
	igt_display_t *display = &data->display;
	int ret;

	igt_print_activity();

	/* Hardware test */
	cursor_enable(data);
	igt_plane_set_position(data->cursor, x, y);
	ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	cursor_disable(data);

	igt_assert_eq(ret, expect);
}

static void do_test(data_t *data, const cursorarea *coords, igt_crc_t crc[4],
		    bool hwtest)
{
	do_single_test(data, coords->x, coords->width, hwtest, &crc[0]);
	do_single_test(data, coords->y, coords->width, hwtest, &crc[1]);
	do_single_test(data, coords->y, coords->height, hwtest, &crc[2]);
	do_single_test(data, coords->x, coords->height, hwtest, &crc[3]);
}

static void test_crc_onscreen(data_t *data)
{
	const int left = data->left;
	const int right = data->right;
	const int top = data->top;
	const int bottom = data->bottom;
	const int cursor_w = data->curw;
	const int cursor_h = data->curh;

	struct {
		const cursorarea coords;
		igt_crc_t crc[4];
	} tests[] = {
		/* fully inside  */
		{{left, right, top, bottom}},
		/* 2 pixels inside */
		{{left - (cursor_w - 2), right + (cursor_w - 2), top, bottom}},
		{{left, right, top - (cursor_h - 2), bottom + (cursor_h - 2)}},
		{{left - (cursor_w - 2), right + (cursor_w - 2),
		  top - (cursor_h - 2), bottom + (cursor_h - 2)}},
		/* 1 pixel inside */
		{{left - (cursor_w - 1), right + (cursor_w - 1), top, bottom}},
		{{left, right, top - (cursor_h - 1), bottom + (cursor_h - 1)}},
		{{left - (cursor_w - 1), right + (cursor_w - 1),
		  top - (cursor_h - 1), bottom + (cursor_h - 1)}},
	};

	/* HW test */
	cursor_enable(data);
	igt_plane_set_fb(data->primary, &data->primary_fb[HWCURSORBUFFER]);
	for (int i = 0; i < ARRAY_SIZE(tests); i++)
		do_test(data, &tests[i].coords, tests[i].crc, true);

	/* SW test */
	cursor_disable(data);
	for (int i = 0; i < ARRAY_SIZE(tests); i++)
		do_test(data, &tests[i].coords, tests[i].crc, false);
}

static void test_crc_offscreen(data_t *data)
{
	const int left = data->left;
	const int right = data->right;
	const int top = data->top;
	const int bottom = data->bottom;
	const int cursor_w = data->curw;
	const int cursor_h = data->curh;

	struct {
		const cursorarea coords;
		igt_crc_t crc[4];
	} tests[] = {
		/* fully outside */
		{{left - (cursor_w), right + (cursor_w), top, bottom}},
		{{left, right, top - (cursor_h), bottom + (cursor_h)}},
		{{left - (cursor_w), right + (cursor_w), top - (cursor_h),
		  bottom + (cursor_h)}},
		/* fully outside by 1 extra pixels */
		{{left - (cursor_w + 1), right + (cursor_w + 1), top, bottom}},
		{{left, right, top - (cursor_h + 1), bottom + (cursor_h + 1)}},
		{{left - (cursor_w + 1), right + (cursor_w + 1),
		  top - (cursor_h + 1), bottom + (cursor_h + 1)}},
		/* fully outside by 2 extra pixels */
		{{left - (cursor_w + 2), right + (cursor_w + 2), top, bottom}},
		{{left, right, top - (cursor_h + 2), bottom + (cursor_h + 2)}},
		{{left - (cursor_w + 2), right + (cursor_w + 2),
		  top - (cursor_h + 2), bottom + (cursor_h + 2)}},
		/* fully outside by a lot of extra pixels */
		{{left - (cursor_w + 512), right + (cursor_w + 512), top, bottom}},
		{{left, right, top - (cursor_h + 512), bottom + (cursor_h + 512)}},
		{{left - (cursor_w + 512), right + (cursor_w + 512),
		  top - (cursor_h + 512), bottom + (cursor_h + 512)}},
		/* go nuts */
		{{INT_MIN, INT_MAX - cursor_w, INT_MIN, INT_MAX - cursor_h}},
		{{SHRT_MIN, SHRT_MAX, SHRT_MIN, SHRT_MAX}},
	};

	/* HW test */
	cursor_enable(data);
	igt_plane_set_fb(data->primary, &data->primary_fb[HWCURSORBUFFER]);
	for (int i = 0; i < ARRAY_SIZE(tests); i++)
		do_test(data, &tests[i].coords, tests[i].crc, true);

	/* SW test */
	cursor_disable(data);
	/*
	 * all these crc's should be the same, actually render only first image
	 * to check crc and then compare rest of crc are matching
	 */
	do_test(data, &tests[0].coords, tests[0].crc, false);

	for (int i = 1; i < ARRAY_SIZE(tests); i++) {
		igt_assert_crc_equal(&tests[0].crc[0], &tests[i].crc[0]);
		igt_assert_crc_equal(&tests[0].crc[0], &tests[i].crc[1]);
		igt_assert_crc_equal(&tests[0].crc[0], &tests[i].crc[2]);
		igt_assert_crc_equal(&tests[0].crc[0], &tests[i].crc[3]);
	}

	/* Make sure we get -ERANGE on integer overflow */
	do_fail_test(data, INT_MAX - cursor_w + 1, INT_MAX - cursor_h + 1, -ERANGE);
}

static void test_crc_sliding(data_t *data)
{
	int i;
	struct {
		igt_crc_t crc[3];
	} rounds[16] = {};

	/* Make sure cursor moves smoothly and pixel-by-pixel, and that there are
	 * no alignment issues. Horizontal, vertical and diagonal test.
	 */

	/* HW test */
	cursor_enable(data);
	igt_plane_set_fb(data->primary, &data->primary_fb[HWCURSORBUFFER]);

	for (i = 0; i < ARRAY_SIZE(rounds); i++) {
		do_single_test(data, i, 0, true, &rounds[i].crc[0]);
		do_single_test(data, 0, i, true, &rounds[i].crc[1]);
		do_single_test(data, i, i, true, &rounds[i].crc[2]);
	}

	/* SW test */
	cursor_disable(data);
	for (i = 0; i < ARRAY_SIZE(rounds); i++) {
		do_single_test(data, i, 0, false, &rounds[i].crc[0]);
		do_single_test(data, 0, i, false, &rounds[i].crc[1]);
		do_single_test(data, i, i, false, &rounds[i].crc[2]);
	}
}

static void test_crc_random(data_t *data)
{
	igt_crc_t crc[50];
	int i, max, x[ARRAY_SIZE(crc)], y[ARRAY_SIZE(crc)];

	max = data->flags & (TEST_DPMS | TEST_SUSPEND) ? 2 : ARRAY_SIZE(crc);

	/* Random cursor placement */

	/* HW test */
	cursor_enable(data);
	igt_plane_set_fb(data->primary, &data->primary_fb[HWCURSORBUFFER]);

	for (i = 0; i < max; i++) {
		x[i] = rand() % (data->screenw + data->curw * 2) - data->curw;
		y[i] = rand() % (data->screenh + data->curh * 2) - data->curh;
		do_single_test(data, x[i], y[i], true, &crc[i]);
	}

	/* SW test */
	cursor_disable(data);
	for (i = 0; i < max; i++)
		do_single_test(data, x[i], y[i], false, &crc[i]);

}

static void cleanup_crtc(data_t *data)
{
	igt_display_t *display = &data->display;

	igt_pipe_crc_stop(data->pipe_crc);
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	cairo_surface_destroy(data->surface);
	data->surface = NULL;

	igt_output_set_pipe(data->output, PIPE_NONE);
	igt_plane_set_fb(data->primary, NULL);
	igt_display_commit(display);

	igt_remove_fb(data->drm_fd, &data->primary_fb[HWCURSORBUFFER]);
	igt_remove_fb(data->drm_fd, &data->primary_fb[SWCOMPARISONBUFFER1]);
	igt_remove_fb(data->drm_fd, &data->primary_fb[SWCOMPARISONBUFFER2]);
}

static void prepare_crtc(data_t *data, int cursor_w, int cursor_h)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	cairo_t *cr;
	igt_output_t *output = data->output;

	igt_display_reset(display);

	/* select the pipe we want to use */
	igt_output_set_pipe(output, data->pipe);

	/* create and set the primary plane fbs */
	mode = igt_output_get_mode(output);
	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
		      &data->primary_fb[HWCURSORBUFFER]);

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
		      &data->primary_fb[SWCOMPARISONBUFFER1]);

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
		      &data->primary_fb[SWCOMPARISONBUFFER2]);

	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	data->cursor = igt_output_get_plane_type(output, DRM_PLANE_TYPE_CURSOR);

	igt_plane_set_fb(data->primary, &data->primary_fb[SWCOMPARISONBUFFER1]);

	igt_display_commit(display);

	/* create the pipe_crc object for this pipe */
	if (data->pipe_crc)
		igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	/* x/y position where the cursor is still fully visible */
	data->left = 0;
	data->right = mode->hdisplay - cursor_w;
	data->top = 0;
	data->bottom = mode->vdisplay - cursor_h;
	data->screenw = mode->hdisplay;
	data->screenh = mode->vdisplay;
	data->curw = cursor_w;
	data->curh = cursor_h;
	data->refresh = mode->vrefresh;

	/* initialize old cursor area to full screen so first run will copy image in place */
	data->oldcursorarea[HWCURSORBUFFER]      = (cursorarea){0, 0, data->screenw, data->screenh};
	data->oldcursorarea[SWCOMPARISONBUFFER1] = (cursorarea){0, 0, data->screenw, data->screenh};
	data->oldcursorarea[SWCOMPARISONBUFFER2] = (cursorarea){0, 0, data->screenw, data->screenh};

	data->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
						   data->screenw,
						   data->screenh);

	/* store test image as cairo surface */
	cr = cairo_create(data->surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	igt_paint_test_pattern(cr, data->screenw, data->screenh);
	cairo_destroy(cr);

	/* Set HW cursor buffer in place */
	restore_image(data, HWCURSORBUFFER, NULL);

	igt_pipe_crc_start(data->pipe_crc);
}

static void create_cursor_fb(data_t *data, int cur_w, int cur_h)
{
	cairo_t *cr;
	uint32_t fb_id;

	/*
	 * Make the FB slightly taller and leave the extra
	 * line opaque white, so that we can see that the
	 * hardware won't scan beyond what it should (esp.
	 * with non-square cursors).
	 */
	fb_id = igt_create_color_fb(data->drm_fd, cur_w, cur_h + 1,
				    DRM_FORMAT_ARGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    1.0, 1.0, 1.0,
				    &data->fb);

	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	igt_paint_color_alpha(cr, 0, 0, cur_w, cur_h, 0.0, 0.0, 0.0, 0.0);
	draw_cursor(cr, &((cursorarea){0, 0, cur_w, cur_h}), data->alpha);
	igt_put_cairo_ctx(cr);
}

static void test_cursor_alpha(data_t *data)
{
	igt_crc_t crc;

	igt_plane_set_fb(data->primary, &data->primary_fb[HWCURSORBUFFER]);
	create_cursor_fb(data, data->curw, data->curh);
	cursor_enable(data);
	do_single_test(data, 0, 0, true, &crc);

	cursor_disable(data);
	igt_remove_fb(data->drm_fd, &data->fb);
	do_single_test(data, 0, 0, false, &crc);
}

static void test_cursor_transparent(data_t *data)
{
	data->alpha = 0.0;
	test_cursor_alpha(data);
	data->alpha = 1.0;
}

static void test_cursor_opaque(data_t *data)
{
	data->alpha = 1.0;
	test_cursor_alpha(data);
}

static bool require_cursor_size(data_t *data, int w, int h)
{
	igt_fb_t primary_fb;
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	igt_output_t *output = data->output;
	igt_plane_t *primary, *cursor;
	int ret;

	igt_require(w <= data->cursor_max_w &&
		    h <= data->cursor_max_h);

	igt_display_reset(display);
	igt_output_set_pipe(output, data->pipe);

	mode = igt_output_get_mode(output);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	cursor = igt_output_get_plane_type(output, DRM_PLANE_TYPE_CURSOR);

	/* Create temporary primary fb for testing */
	igt_assert(igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
				 DRM_FORMAT_MOD_LINEAR, &primary_fb));

	igt_plane_set_fb(primary, &primary_fb);
	igt_plane_set_fb(cursor, &data->fb);
	igt_plane_set_size(cursor, w, h);
	igt_fb_set_size(&data->fb, cursor, w, h);

	/* Test if the kernel supports the given cursor size or not */
	if (display->is_atomic)
		ret = igt_display_try_commit_atomic(display,
						    DRM_MODE_ATOMIC_TEST_ONLY |
						    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	else
		ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(cursor, NULL);

	igt_remove_fb(data->drm_fd, &primary_fb);
	igt_output_set_pipe(output, PIPE_NONE);

	return !!ret;
}

static void run_test(data_t *data, void (*testfunc)(data_t *), int cursor_w, int cursor_h)
{
	prepare_crtc(data, cursor_w, cursor_h);
	testfunc(data);
	cleanup_crtc(data);
}

static void test_cursor_size(data_t *data)
{
	igt_crc_t crc;

	for (data->curw = data->curh = data->cursor_max_w; data->curw >= 64;
	     data->curw /= 2, data->curh /= 2) {
		igt_plane_set_fb(data->primary,
				 &data->primary_fb[HWCURSORBUFFER]);
		create_cursor_fb(data, data->curw, data->curh);
		cursor_enable(data);
		do_single_test(data, 0, 0, true, &crc);

		cursor_disable(data);
		igt_remove_fb(data->drm_fd, &data->fb);
		do_single_test(data, 0, 0, false, &crc);
	}
}

static void test_rapid_movement(data_t *data)
{
	struct timeval start, end, delta;
	int x = 0, y = 0;
	long usec;
	igt_display_t *display = &data->display;

	cursor_enable(data);

	gettimeofday(&start, NULL);
	for ( ; x < 100; x++) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	for ( ; y < 100; y++) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	for ( ; x > 0; x--) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	for ( ; y > 0; y--) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	gettimeofday(&end, NULL);

	cursor_disable(data);

	/*
	 * We've done 400 cursor updates now.  If we're being throttled to
	 * vblank, then that would take roughly 400/refresh seconds.  If the
	 * elapsed time is greater than 90% of that value, we'll consider it
	 * a failure (since cursor updates shouldn't be throttled).
	 */
	timersub(&end, &start, &delta);
	usec = delta.tv_usec + 1000000 * delta.tv_sec;
	igt_assert_lt(usec, 0.9 * 400 * 1000000 / data->refresh);
}

static void run_size_tests(data_t *data, int w, int h)
{
	enum pipe pipe;
	struct {
		const char *name;
		void (*testfunc)(data_t *);
		const char *desc;
	} size_tests[] = {
		{ "cursor-onscreen", test_crc_onscreen,
			"Check if a given-size cursor is well-positioned inside the screen." },
		{ "cursor-offscreen", test_crc_offscreen,
			"Check if a given-size cursor is well-positioned outside the screen." },
		{ "cursor-sliding", test_crc_sliding,
			"Check the smooth and pixel-by-pixel given-size cursor movements on horizontal, vertical and diagonal." },
		{ "cursor-random", test_crc_random,
			"Check random placement of a cursor with given size." },
		{ "cursor-rapid-movement", test_rapid_movement,
			"Check the rapid update of given-size cursor movements." },
	};
	int i;
	char name[32];

	if (w == 0 && h == 0) {
		w = data->cursor_max_w;
		h = data->cursor_max_h;

		strcpy(name, "max-size");
	} else {
		snprintf(name, sizeof(name), "%dx%d", w, h);
	}

	igt_fixture
		create_cursor_fb(data, w, h);

	for (i = 0; i < ARRAY_SIZE(size_tests); i++) {
		igt_describe(size_tests[i].desc);
		igt_subtest_with_dynamic_f("%s-%s", size_tests[i].name, name) {
			if (!strcmp(name, "max-size")) {
				/*
				 * No point in doing the "max-size" test if
				 * it was already covered by the other tests.
				 */
				if ((w == h) && (w <= 512) && (h <= 512) &&
				    is_power_of_two(w) && is_power_of_two(h)) {
					igt_info("Cursor max size %dx%d already covered by other tests\n", w, h);
					continue;
				}
			}

			for_each_pipe(&data->display, pipe) {
				data->pipe = pipe;

				if (require_cursor_size(data, w, h)) {
					igt_info("Cursor size %dx%d not supported by driver\n", w, h);
					continue;
				}

				igt_dynamic_f("pipe-%s-%s",
					      kmstest_pipe_name(pipe), igt_output_name(data->output))
					run_test(data, size_tests[i].testfunc, w, h);
			}
		}
	}

	igt_fixture
		igt_remove_fb(data->drm_fd, &data->fb);
}

static void run_tests_on_pipe(data_t *data)
{
	enum pipe pipe;
	int cursor_size;

	igt_fixture {
		data->output = igt_get_single_output_for_pipe(&data->display, pipe);
		igt_require(data->output);
		data->alpha = 1.0;
		data->flags = 0;
	}

	igt_describe("Create a maximum size cursor, then change the size in "
		     "flight to smaller ones to see that the size is applied "
		     "correctly.");
	igt_subtest_with_dynamic("cursor-size-change") {
		for_each_pipe(&data->display, pipe) {
			data->pipe = pipe;

			igt_dynamic_f("pipe-%s-%s",
				      kmstest_pipe_name(pipe),
				      data->output->name)
				run_test(data, test_cursor_size,
					 data->cursor_max_w, data->cursor_max_h);
		}
	}

	igt_describe("Validates the composition of a fully opaque cursor "
		     "plane, i.e., alpha channel equal to 1.0.");
	igt_subtest_with_dynamic("cursor-alpha-opaque") {
		for_each_pipe(&data->display, pipe) {
			data->pipe = pipe;

			igt_dynamic_f("pipe-%s-%s",
				      kmstest_pipe_name(pipe),
				      data->output->name)
				run_test(data, test_cursor_opaque,
					 data->cursor_max_w, data->cursor_max_h);
		}
	}

	igt_describe("Validates the composition of a fully transparent cursor "
		     "plane, i.e., alpha channel equal to 0.0.");
	igt_subtest_with_dynamic("cursor-alpha-transparent") {
		for_each_pipe(&data->display, pipe) {
			data->pipe = pipe;

			igt_dynamic_f("pipe-%s-%s",
				      kmstest_pipe_name(pipe),
				      data->output->name)
				run_test(data, test_cursor_transparent,
					 data->cursor_max_w, data->cursor_max_h);
		}
	}

	igt_fixture {
		create_cursor_fb(data, data->cursor_max_w, data->cursor_max_h);
	}

	igt_describe("Check random placement of a cursor with DPMS.");
	igt_subtest_with_dynamic("cursor-dpms") {
		for_each_pipe(&data->display, pipe) {
			data->pipe = pipe;
			data->flags = TEST_DPMS;

			if (require_cursor_size(data, data->cursor_max_w, data->cursor_max_h)) {
				igt_debug("Cursor size %dx%d not supported by driver\n",
					  data->cursor_max_w, data->cursor_max_h);
				continue;
			}

			igt_dynamic_f("pipe-%s-%s",
				      kmstest_pipe_name(pipe),
				      data->output->name)
				run_test(data, test_crc_random,
					 data->cursor_max_w, data->cursor_max_h);
		}
		data->flags = 0;
	}

	igt_describe("Check random placement of a cursor with suspend.");
	igt_subtest_with_dynamic("cursor-suspend") {
		for_each_pipe(&data->display, pipe) {
			data->pipe = pipe;
			data->flags = TEST_SUSPEND;

			if (require_cursor_size(data, data->cursor_max_w, data->cursor_max_h)) {
				igt_debug("Cursor size %dx%d not supported by driver\n",
					  data->cursor_max_w, data->cursor_max_h);
				continue;
			}

			igt_dynamic_f("pipe-%s-%s",
				      kmstest_pipe_name(pipe),
				      data->output->name)
				run_test(data, test_crc_random,
					 data->cursor_max_w, data->cursor_max_h);
		}
		data->flags = 0;
	}

	igt_fixture
		igt_remove_fb(data->drm_fd, &data->fb);

	for (cursor_size = 32; cursor_size <= 512; cursor_size *= 2) {
		int w = cursor_size;
		int h = cursor_size;

		igt_subtest_group
			run_size_tests(data, w, h);

		/*
		 * Test non-square cursors a bit on the platforms
		 * that support such things. And make it a bit more
		 * interesting by using a non-pot height.
		 */
		h /= 3;

		igt_subtest_group
			run_size_tests(data, w, h);
	}

	run_size_tests(data, 0, 0);
}

static data_t data;

igt_main
{
	uint64_t cursor_width = 64, cursor_height = 64;
	int ret;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);

		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width);
		igt_assert(ret == 0 || errno == EINVAL);
		/* Not making use of cursor_height since it is same as width, still reading */
		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height);
		igt_assert(ret == 0 || errno == EINVAL);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);

		data.vblank_wait_count = is_msm_device(data.drm_fd) ? 2 : 1;
	}

	data.cursor_max_w = cursor_width;
	data.cursor_max_h = cursor_height;

	igt_subtest_group
		run_tests_on_pipe(&data);

	igt_fixture {
		if (data.pipe_crc != NULL) {
			igt_pipe_crc_stop(data.pipe_crc);
			igt_pipe_crc_free(data.pipe_crc);
		}

		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
