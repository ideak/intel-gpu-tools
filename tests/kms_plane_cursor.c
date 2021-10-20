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

/*
 * This file tests cursor interactions with primary and overlay planes.
 *
 * Some assumptions made:
 * - Cursor planes can be placed on top of overlay planes
 * - DRM index indicates z-ordering, higher index = higher z-order
 */

enum {
	TEST_PRIMARY = 0,
	TEST_OVERLAY = 1 << 0,
	TEST_VIEWPORT = 1 << 1,
};

typedef struct {
	int x;
	int y;
} pos_t;

typedef struct {
	int x;
	int y;
	int w;
	int h;
} rect_t;

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_plane_t *overlay;
	igt_plane_t *cursor;
	igt_output_t *output;
	igt_pipe_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfo *mode;
	igt_fb_t pfb;
	igt_fb_t ofb;
	igt_fb_t cfb;
	enum pipe pipe_id;
	int drm_fd;
	rect_t or;
	uint64_t max_curw;
	uint64_t max_curh;
} data_t;

/* Common test setup. */
static void test_init(data_t *data, enum pipe pipe_id, igt_output_t *output)
{
	igt_display_t *display = &data->display;

	data->pipe_id = pipe_id;
	data->pipe = &data->display.pipes[data->pipe_id];
	data->output = output;

	igt_display_reset(display);

	data->mode = igt_output_get_mode(data->output);

	data->primary = igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);
	data->overlay = igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_OVERLAY);
	data->cursor = igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_CURSOR);

	igt_require_pipe_crc(data->drm_fd);
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe_id,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	/* Overlay rectangle for a rect in the center of the screen */
	data->or.x = data->mode->hdisplay / 4;
	data->or.y = data->mode->vdisplay / 4;
	data->or.w = data->mode->hdisplay / 2;
	data->or.h = data->mode->vdisplay / 2;
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	igt_display_reset(&data->display);
	igt_plane_set_fb(data->primary, NULL);
	igt_plane_set_fb(data->overlay, NULL);
	igt_plane_set_fb(data->cursor, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

/* Fills a FB with the solid color given. */
static void draw_color(igt_fb_t *fb, double r, double g, double b)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	igt_paint_color(cr, 0, 0, fb->width, fb->height, r, g, b);
	igt_put_cairo_ctx(cr);
}

/*
 * Draws white and gray (if overlay FB is given) on the primary FB.
 * Draws a magenta square where the cursor should be over top both planes.
 * Takes this as the reference CRC.
 *
 * Draws white on the primary FB and gray on the overlay FB if given.
 * Places the cursor where the magenta square should be with a magenta FB.
 * Takes this as the test CRC and compares it to the reference.
 */
static void test_cursor_pos(data_t *data, int x, int y, unsigned int flags)
{
	igt_crc_t ref_crc, test_crc;
	cairo_t *cr;
	igt_fb_t *pfb = &data->pfb;
	igt_fb_t *ofb = &data->ofb;
	igt_fb_t *cfb = &data->cfb;
	int cw = cfb->width;
	int ch = cfb->height;
	const rect_t *or = &data->or;

	cr = igt_get_cairo_ctx(pfb->fd, pfb);
	igt_paint_color(cr, 0, 0, pfb->width, pfb->height, 1.0, 1.0, 1.0);

	if (flags & TEST_OVERLAY)
		igt_paint_color(cr, or->x, or->y, or->w, or->h, 0.5, 0.5, 0.5);

	igt_paint_color(cr, x, y, cw, ch, 1.0, 0.0, 1.0);
	igt_put_cairo_ctx(cr);

	igt_plane_set_fb(data->overlay, NULL);
	igt_plane_set_fb(data->cursor, NULL);
	igt_display_commit_atomic(&data->display, 0, NULL);

	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &ref_crc);

	draw_color(pfb, 1.0, 1.0, 1.0);

	if (flags & TEST_OVERLAY) {
		igt_plane_set_fb(data->overlay, ofb);
		igt_plane_set_position(data->overlay, or->x, or->y);
		igt_plane_set_size(data->overlay, or->w, or->h);
		igt_fb_set_size(ofb, data->overlay, or->w, or->h);
		igt_fb_set_position(ofb, data->overlay,
				    (ofb->width - or->w) / 2,
				    (ofb->height - or->h) / 2);
	}

	igt_plane_set_fb(data->cursor, cfb);
	igt_plane_set_position(data->cursor, x, y);
	igt_display_commit_atomic(&data->display, 0, NULL);

	/* Wait for one more vblank since cursor updates are not
	 * synchronized to the same frame on AMD hw */
	if(is_amdgpu_device(data->drm_fd))
		igt_wait_for_vblank_count(data->drm_fd, data->display.pipes[data->pipe_id].crtc_offset, 1);

	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &test_crc);
	igt_pipe_crc_stop(data->pipe_crc);

	igt_assert_crc_equal(&ref_crc, &test_crc);
}

/*
 * Tests the cursor on a variety of positions on the screen.
 * Specific edge cases that should be captured here are the negative edges
 * of each plane and the centers.
 */
static void test_cursor_spots(data_t *data, int size, unsigned int flags)
{
	int sw = data->mode->hdisplay;
	int sh = data->mode->vdisplay;
	const rect_t *or = &data->or;
	int i;
	const pos_t pos[] = {
		/* Test diagonally from top left to bottom right. */
		{ -size / 3, -size / 3 },
		{ 0, 0 },
		{ or->x - size, or->y - size },
		{ or->x - size / 3, or->y - size / 3 },
		{ or->x, or->y },
		{ or->x + size, or->y + size },
		{ sw / 2, sh / 2 },
		{ or->x + or->w - size, or->y + or->h - size },
		{ or->x + or->w - size / 3, or->y + or->h - size / 3 },
		{ or->x + or->w + size, or->y + or->h + size },
		{ sw - size, sh - size },
		{ sw - size / 3, sh - size / 3 },
		/* Test remaining corners. */
		{ sw - size, 0 },
		{ 0, sh - size },
		{ or->x + or->w - size, or->y },
		{ or->x, or->y + or->h - size }
	};

	for (i = 0; i < ARRAY_SIZE(pos); ++i) {
		test_cursor_pos(data, pos[i].x, pos[i].y, flags);
	}
}

static void test_cleanup(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->cfb);
	igt_remove_fb(data->drm_fd, &data->ofb);
	igt_remove_fb(data->drm_fd, &data->pfb);
}

static void test_cursor(data_t *data, int size, unsigned int flags)
{
	int sw, sh;
	int pad = 128;

	igt_skip_on(size > data->max_curw || size > data->max_curh);

	sw = data->mode->hdisplay;
	sh = data->mode->vdisplay;

	test_cleanup(data);

	igt_create_color_fb(data->drm_fd, sw, sh, DRM_FORMAT_XRGB8888, 0,
			    1.0, 1.0, 1.0, &data->pfb);

	if (flags & TEST_OVERLAY) {
		int width = (flags & TEST_VIEWPORT) ? data->or.w + pad : data->or.w;
		int height = (flags & TEST_VIEWPORT) ? data->or.h + pad : data->or.h;

		igt_create_color_fb(data->drm_fd, width, height,
				    DRM_FORMAT_XRGB8888, 0, 0.5, 0.5, 0.5, &data->ofb);
	}

	igt_create_color_fb(data->drm_fd, size, size, DRM_FORMAT_ARGB8888, 0,
			    1.0, 0.0, 1.0, &data->cfb);

	igt_plane_set_fb(data->primary, &data->pfb);
	igt_output_set_pipe(data->output, data->pipe_id);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	test_cursor_spots(data, size, flags);
}

igt_main
{
	static const int cursor_sizes[] = { 64, 128, 256 };
	data_t data = { .max_curw = 64, .max_curh = 64 };
	enum pipe pipe;
	igt_output_t *output;
	int i, j;
	struct {
		const char *name;
		unsigned int flags;
		const char *desc;
	} tests[] = {
		{ "primary", TEST_PRIMARY,
		  "Tests atomic cursor positioning on primary plane" },
		{ "overlay", TEST_PRIMARY | TEST_OVERLAY,
		  "Tests atomic cursor positioning on primary plane and overlay plane" },
		{ "viewport", TEST_PRIMARY | TEST_OVERLAY | TEST_VIEWPORT,
		  "Tests atomic cursor positioning on primary plane and overlay plane "
		  "with buffer larger than viewport used for display" },
	};

	igt_fixture {
		int ret;

		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_WIDTH, &data.max_curw);
		igt_assert(ret == 0 || errno == EINVAL);
		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_HEIGHT, &data.max_curh);
		igt_assert(ret == 0 || errno == EINVAL);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe_f("%s", tests[i].desc);
		igt_subtest_with_dynamic_f("%s", tests[i].name) {
			for_each_pipe_with_single_output(&data.display, pipe, output) {
				if ((tests[i].flags & TEST_OVERLAY) &&
				    !igt_pipe_get_plane_type(&data.display.pipes[pipe],
							     DRM_PLANE_TYPE_OVERLAY))
					continue;

				test_init(&data, pipe, output);

				for (j = 0; j < ARRAY_SIZE(cursor_sizes); j++) {
					int size = cursor_sizes[j];

					igt_dynamic_f("pipe-%s-%s-size-%d",
						      kmstest_pipe_name(pipe),
						      igt_output_name(output),
						      size)
						test_cursor(&data, size, tests[i].flags);

					test_cleanup(&data);
				}

				test_fini(&data);
			}
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
