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
 */

#include "igt.h"
#include "igt_sysfs.h"
#include "igt_psr.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

IGT_TEST_DESCRIPTION("Tests to varify PSR2 selective fetch by sending multiple"
		     " damaged areas");

#define SQUARE_SIZE 100

#define CUR_SIZE 64
#define MAX_DAMAGE_AREAS 5

#define MAX_SCREEN_CHANGES 5

enum operations {
	PLANE_UPDATE,
	PLANE_UPDATE_CONTINUOUS,
	PLANE_MOVE,
	PLANE_MOVE_CONTINUOUS,
	PLANE_MOVE_CONTINUOUS_EXCEED,
	PLANE_MOVE_CONTINUOUS_EXCEED_FULLY,
	OVERLAY_PRIM_UPDATE
};

enum plane_move_postion {
	POS_TOP_LEFT,
	POS_TOP_RIGHT,
	POS_BOTTOM_LEFT,
	POS_BOTTOM_RIGHT,
	POS_CENTER,
	POS_TOP,
	POS_BOTTOM,
	POS_LEFT,
	POS_RIGHT,
};

typedef struct {
	int drm_fd;
	int debugfs_fd;
	igt_display_t display;
	drmModeModeInfo *mode;
	igt_output_t *output;
	struct igt_fb fb_primary, fb_overlay, fb_cursor;
	struct igt_fb fb_test;
	struct igt_fb *fb_continuous;
	uint32_t primary_format;
	int damage_area_count;
	int big_fb_width, big_fb_height;
	struct drm_mode_rect plane_update_clip[MAX_DAMAGE_AREAS];
	struct drm_mode_rect plane_move_clip;
	struct drm_mode_rect cursor_clip;
	enum operations op;
	enum plane_move_postion pos;
	int test_plane_id;
	igt_plane_t *test_plane;
	bool big_fb_test;
	cairo_t *cr;
	uint32_t screen_changes;
	int cur_x, cur_y;
} data_t;

static const char *op_str(enum operations op)
{
	static const char * const name[] = {
		[PLANE_UPDATE] = "plane-update",
		[PLANE_UPDATE_CONTINUOUS] = "plane-update-continuous",
		[PLANE_MOVE_CONTINUOUS] = "plane-move-continuous",
		[PLANE_MOVE_CONTINUOUS_EXCEED] = "plane-move-continuous-exceed",
		[PLANE_MOVE_CONTINUOUS_EXCEED_FULLY] =
		"plane-move-continuous-exceed-fully",
		[PLANE_MOVE] = "plane-move",
		[OVERLAY_PRIM_UPDATE] = "overlay-primary-update",
	};

	return name[op];
}

static void setup_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		drmModeConnectorPtr c = output->config.connector;

		if (c->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_output_set_pipe(output, pipe);
		data->output = output;
		data->mode = igt_output_get_mode(output);

		return;
	}
}

static void display_init(data_t *data)
{
	igt_display_require(&data->display, data->drm_fd);
	setup_output(data);
}

static void display_fini(data_t *data)
{
	igt_display_fini(&data->display);
}

static void draw_rect(data_t *data, igt_fb_t *fb, int x, int y, int w, int h,
			double r, double g, double b, double a)
{
	cairo_t *cr;

	cr = igt_get_cairo_ctx(data->drm_fd, fb);
	igt_paint_color_alpha(cr, x, y, w, h, r, g, b, a);
	igt_put_cairo_ctx(cr);
}

static void set_clip(struct drm_mode_rect *clip, int x, int y, int width,
		     int height)
{
	clip->x1 = x;
	clip->y1 = y;
	clip->x2 = x + width;
	clip->y2 = y + height;
}

static void plane_update_setup_squares(data_t *data, igt_fb_t *fb, uint32_t h,
				       uint32_t v, int pos_x, int pos_y)
{
	int x, y;
	int width = SQUARE_SIZE;
	int height = SQUARE_SIZE;

	switch (data->damage_area_count) {
	case 5:
		/*Bottom right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y + v - SQUARE_SIZE;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[4], x, y, width, height);
	case 4:
		/*Bottom left corner*/
		x = pos_x;
		y = pos_y + v - SQUARE_SIZE;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[3], x, y, width, height);
	case 3:
		/*Top right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[2], x, y, width, height);
	case 2:
		/*Top left corner*/
		x = pos_x;
		y = pos_y;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[1], x, y, width, height);
	case 1:
		/*Center*/
		x = pos_x + h / 2 - SQUARE_SIZE / 2;
		y = pos_y + v / 2 - SQUARE_SIZE / 2;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[0], x, y, width, height);
		break;
	default:
		igt_assert(false);
	}
}

static void plane_move_setup_square(data_t *data, igt_fb_t *fb, uint32_t h,
				    uint32_t v, int pos_x, int pos_y)
{
	int x = 0, y = 0;

	switch (data->pos) {
	case POS_TOP_LEFT:
		/*Bottom right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y + v - SQUARE_SIZE;
		break;
	case POS_TOP_RIGHT:
		/*Bottom left corner*/
		x = pos_x;
		y = pos_y + v - SQUARE_SIZE;
		break;
	case POS_BOTTOM_LEFT:
		/*Top right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y + 0;
		break;
	case POS_BOTTOM_RIGHT:
		/*Top left corner*/
		x = pos_x;
		y = pos_y;
		break;
	default:
		igt_assert(false);
	}

	draw_rect(data, fb, x, y,
		  SQUARE_SIZE, SQUARE_SIZE, 1.0, 1.0, 1.0, 1.0);
	set_clip(&data->plane_move_clip, x, y, SQUARE_SIZE, SQUARE_SIZE);
}

static void prepare(data_t *data, igt_output_t *output)
{
	igt_plane_t *primary, *sprite = NULL, *cursor = NULL;
	int fb_w, fb_h, x, y, view_w, view_h;

	if (data->big_fb_test) {
		fb_w = data->big_fb_width;
		fb_h = data->big_fb_height;
		x = fb_w / 2;
		y = fb_h / 2;
		view_w = data->mode->hdisplay;
		view_h = data->mode->vdisplay;
	} else {
		fb_w = view_w = data->mode->hdisplay;
		fb_h = view_h = data->mode->vdisplay;
		x = y = 0;
	}

	/* all green frame */
	igt_create_color_fb(data->drm_fd, fb_w, fb_h,
			    data->primary_format,
			    DRM_FORMAT_MOD_LINEAR,
			    0.0, 1.0, 0.0,
			    &data->fb_primary);

	primary = igt_output_get_plane_type(output,
			DRM_PLANE_TYPE_PRIMARY);

	switch (data->test_plane_id) {
	case DRM_PLANE_TYPE_OVERLAY:
		sprite = igt_output_get_plane_type(output,
						   DRM_PLANE_TYPE_OVERLAY);
		/*All blue plane*/
		igt_create_color_fb(data->drm_fd,
				    fb_w / 2, fb_h / 2,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 0.0, 1.0,
				    &data->fb_overlay);

		igt_create_color_fb(data->drm_fd,
				    fb_w / 2, fb_h / 2,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 0.0, 1.0,
				    &data->fb_test);

		data->fb_continuous = &data->fb_overlay;

		if (data->op == PLANE_MOVE) {
			plane_move_setup_square(data, &data->fb_test,
						view_w / 2, view_h / 2,
						x, y);

		} else {
			plane_update_setup_squares(data, &data->fb_test,
						   view_w / 2, view_h / 2,
						   x, y);
		}

		igt_plane_set_fb(sprite, &data->fb_overlay);
		igt_fb_set_position(&data->fb_overlay, sprite, x, y);
		igt_fb_set_size(&data->fb_overlay, primary, view_w / 2,
				view_h / 2);
		igt_plane_set_size(sprite, view_w / 2, view_h / 2);
		data->test_plane = sprite;
		break;

	case DRM_PLANE_TYPE_PRIMARY:
		igt_create_color_fb(data->drm_fd, fb_w, fb_h,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 1.0, 0.0,
				    &data->fb_test);

		plane_update_setup_squares(data, &data->fb_test,
					   view_w, view_h, x, y);
		data->fb_continuous = &data->fb_primary;
		data->test_plane = primary;

		if (data->op == OVERLAY_PRIM_UPDATE) {
			sprite = igt_output_get_plane_type(output,
						   DRM_PLANE_TYPE_OVERLAY);

			igt_create_color_fb(data->drm_fd, fb_w, fb_h,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR,
					    0.0, 0.0, 1.0,
					    &data->fb_overlay);

			igt_plane_set_fb(sprite, &data->fb_overlay);
			igt_fb_set_position(&data->fb_overlay, sprite, x, y);
			igt_fb_set_size(&data->fb_overlay, primary, view_w,
					view_h);
			igt_plane_set_size(sprite, view_w, view_h);
			igt_plane_set_prop_value(sprite, IGT_PLANE_ALPHA,
						 0x6060);
		}
		break;

	case DRM_PLANE_TYPE_CURSOR:
		cursor = igt_output_get_plane_type(output,
						   DRM_PLANE_TYPE_CURSOR);
		igt_plane_set_position(cursor, 0, 0);

		igt_create_fb(data->drm_fd, CUR_SIZE, CUR_SIZE,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->fb_cursor);

		draw_rect(data, &data->fb_cursor, 0, 0, CUR_SIZE, CUR_SIZE,
			    0.0, 0.0, 1.0, 1.0);

		igt_create_fb(data->drm_fd, CUR_SIZE, CUR_SIZE,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->fb_test);
		data->fb_continuous = &data->fb_cursor;

		draw_rect(data, &data->fb_test, 0, 0, CUR_SIZE, CUR_SIZE,
			    1.0, 1.0, 1.0, 1.0);

		set_clip(&data->cursor_clip, 0, 0, CUR_SIZE, CUR_SIZE);
		igt_plane_set_fb(cursor, &data->fb_cursor);
		data->test_plane = cursor;
		break;
	default:
		igt_assert(false);
	}

	igt_plane_set_fb(primary, &data->fb_primary);
	igt_fb_set_position(&data->fb_primary, primary, x, y);
	igt_fb_set_size(&data->fb_primary, primary, view_w,
			view_h);
	igt_plane_set_size(primary, view_w, view_h);
	igt_plane_set_position(primary, 0, 0);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static inline void manual(const char *expected)
{
	igt_debug_interactive_mode_check("all", expected);
}

static void plane_update_expected_output(int plane_type, int box_count,
					 int screen_changes)
{
	char expected[64] = {};

	switch (plane_type) {
	case DRM_PLANE_TYPE_PRIMARY:
		sprintf(expected, "screen Green with %d White box(es)",
			box_count);
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		/*
		 * Continuous updates only for DRM_PLANE_TYPE_OVERLAY
		 * for now.
		 */
		if (screen_changes & 1)
			sprintf(expected, "screen Green with Blue box");
		else
			sprintf(expected,
				"screen Green with Blue box and %d White box(es)",
				box_count);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		sprintf(expected, "screen Green with %d White box(es)",
			box_count);
		break;
	default:
		igt_assert(false);
	}

	manual(expected);
}

static void plane_move_expected_output(enum plane_move_postion pos)
{
	char expected[64] = {};

	switch (pos) {
	case POS_TOP_LEFT:
		sprintf(expected,
			"screen Green with Blue box on top left corner and White box");
		break;
	case POS_TOP_RIGHT:
		sprintf(expected,
			"screen Green with Blue box on top right corner and White box");
		break;
	case POS_BOTTOM_LEFT:
		sprintf(expected,
			"screen Green with Blue box on bottom left corner and White box");
		break;
	case POS_BOTTOM_RIGHT:
		sprintf(expected,
			"screen Green with Blue box on bottom right corner and White box");
		break;
	default:
		igt_assert(false);
	}

	manual(expected);
}

static void plane_move_continuous_expected_output(data_t *data)
{
	char expected[128] = {};
	int ret = 0;

	switch (data->pos) {
	case POS_TOP_LEFT:
		ret = sprintf(expected,
			      "screen Green with Blue box on top left corner");
		break;
	case POS_TOP_RIGHT:
		ret = sprintf(expected,
			      "screen Green with Blue box on top right corner");
		break;
	case POS_BOTTOM_LEFT:
		ret = sprintf(expected,
			      "screen Green with Blue box on bottom left corner");
		break;
	case POS_BOTTOM_RIGHT:
		ret = sprintf(expected,
			      "screen Green with Blue box on bottom right corner");
		break;
	case POS_CENTER:
		ret = sprintf(expected, "screen Green with Blue box on center");
		break;
	case POS_TOP:
		ret = sprintf(expected, "screen Green with Blue box on top");
		break;
	case POS_BOTTOM:
		ret = sprintf(expected, "screen Green with Blue box on bottom");
		break;
	case POS_LEFT:
		ret = sprintf(expected, "screen Green with Blue box on left");
		break;
	case POS_RIGHT:
		ret = sprintf(expected, "screen Green with Blue box on right");
		break;
	default:
		igt_assert(false);
	}

	if (ret) {
		if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED)
			sprintf(expected + ret, "(partly exceeding area)");
		else if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED_FULLY)
			sprintf(expected + ret, "(fully exceeding area)");
	}

	manual(expected);
}

static void overlay_prim_update_expected_output(int box_count)
{
	char expected[64] = {};

	sprintf(expected,
		"screen Green with Blue overlay, %d light Blue box(es)",
		box_count);

	manual(expected);

}

static void expected_output(data_t *data)
{
	switch (data->op) {
	case PLANE_MOVE:
		plane_move_expected_output(data->pos);
		break;
	case PLANE_MOVE_CONTINUOUS:
	case PLANE_MOVE_CONTINUOUS_EXCEED:
	case PLANE_MOVE_CONTINUOUS_EXCEED_FULLY:
		plane_move_continuous_expected_output(data);
		break;
	case PLANE_UPDATE:
		plane_update_expected_output(data->test_plane_id,
					     data->damage_area_count,
					     data->screen_changes);
		break;
	case PLANE_UPDATE_CONTINUOUS:
		plane_update_expected_output(data->test_plane_id,
					     data->damage_area_count,
					     data->screen_changes);
		break;
	case OVERLAY_PRIM_UPDATE:
		overlay_prim_update_expected_output(data->damage_area_count);
		break;
	default:
		igt_assert(false);
	}
}

static void damaged_plane_move(data_t *data)
{
	igt_plane_t *test_plane = data->test_plane;
	uint32_t h = data->mode->hdisplay;
	uint32_t v = data->mode->vdisplay;
	int x, y;

	if (data->big_fb_test) {
		x = data->big_fb_width / 2;
		y = data->big_fb_height / 2;
	} else {
		x = y = 0;
	}

	if (data->test_plane_id == DRM_PLANE_TYPE_OVERLAY) {
		h = h/2;
		v = v/2;
	}

	igt_plane_set_fb(test_plane, &data->fb_test);

	igt_fb_set_position(&data->fb_test, test_plane, x,
			    y);
	igt_fb_set_size(&data->fb_test, test_plane, h, v);
	igt_plane_set_size(test_plane, h, v);

	igt_plane_replace_prop_blob(test_plane, IGT_PLANE_FB_DAMAGE_CLIPS,
				    &data->plane_move_clip,
				    sizeof(struct drm_mode_rect));

	switch (data->pos) {
	case POS_TOP_LEFT:
		igt_plane_set_position(data->test_plane, 0, 0);
		break;
	case POS_TOP_RIGHT:
		igt_plane_set_position(data->test_plane,
				       data->mode->hdisplay/2, 0);
		break;
	case POS_BOTTOM_LEFT:
		igt_plane_set_position(data->test_plane, 0,
				       data->mode->vdisplay/2);
		break;
	case POS_BOTTOM_RIGHT:
		igt_plane_set_position(data->test_plane,
				       data->mode->hdisplay/2,
				       data->mode->vdisplay/2);
		break;
	default:
		igt_assert(false);
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_assert(psr_wait_entry(data->debugfs_fd, PSR_MODE_2));

	expected_output(data);
}
static void get_target_coords(data_t *data, int *x, int *y)
{
	int target_x, target_y, exceed_x, exceed_y;

	switch (data->pos) {
	case POS_TOP_LEFT:
		target_x = 0;
		target_y = 0;
		break;
	case POS_TOP_RIGHT:
		target_x = data->mode->hdisplay - data->fb_test.width;
		target_y = 0;
		break;
	case POS_BOTTOM_LEFT:
		target_x = 0;
		target_y = data->mode->vdisplay - data->fb_test.height;
		break;
	case POS_BOTTOM_RIGHT:
		target_x = data->mode->hdisplay - data->fb_test.width;
		target_y = data->mode->vdisplay - data->fb_test.height;
		break;
	case POS_CENTER:
		target_x = data->mode->hdisplay / 2;
		target_y = data->mode->vdisplay / 2;
		break;
	case POS_BOTTOM:
		target_x = data->mode->hdisplay / 2;
		target_y = data->mode->vdisplay - data->fb_test.height;
		break;
	case POS_TOP:
		target_x = data->mode->hdisplay / 2;
		target_y = 0;
		break;
	case POS_RIGHT:
		target_x = data->mode->hdisplay - data->fb_test.width;
		target_y = data->mode->vdisplay / 2;
		break;
	case POS_LEFT:
		target_x = 0;
		target_y = data->mode->vdisplay / 2;
		break;
	default:
		igt_assert(false);
	}

	if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED) {
		exceed_x  = data->fb_test.width / 2;
		exceed_y  = data->fb_test.height / 2;
	} else if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED_FULLY) {
		exceed_x  = data->fb_test.width;
		exceed_y  = data->fb_test.height;
	}

	if (data->op != PLANE_MOVE_CONTINUOUS) {
		switch (data->pos) {
		case POS_TOP_LEFT:
			target_x -= exceed_x;
			target_y -= exceed_y;
			break;
		case POS_TOP_RIGHT:
			target_x += exceed_x;
			target_y -= exceed_y;
			break;
		case POS_BOTTOM_LEFT:
			target_x -= exceed_x;
			target_y += exceed_y;
			break;
		case POS_BOTTOM_RIGHT:
			target_x += exceed_x;
			target_y += exceed_y;
			break;
		case POS_BOTTOM:
			target_y += exceed_y;
			break;
		case POS_TOP:
			target_y -= exceed_y;
			break;
		case POS_RIGHT:
			target_x += exceed_x;
			break;
		case POS_LEFT:
			target_x -= exceed_x;
			break;
		case POS_CENTER:
			break;
		}
	}

	*x = target_x;
	*y = target_y;
}

static void plane_move_continuous(data_t *data)
{
	int target_x, target_y;

	igt_assert(psr_wait_entry(data->debugfs_fd, PSR_MODE_2));

	get_target_coords(data, &target_x, &target_y);

	while (data->cur_x != target_x || data->cur_y != target_y) {
		if (data->cur_x < target_x)
			data->cur_x += min(target_x - data->cur_x, 20);
		else if (data->cur_x > target_x)
			data->cur_x -= min(data->cur_x - target_x, 20);

		if (data->cur_y < target_y)
			data->cur_y += min(target_y - data->cur_y, 20);
		else if (data->cur_y > target_y)
			data->cur_y -= min(data->cur_y - target_y, 20);

		igt_plane_set_position(data->test_plane, data->cur_x, data->cur_y);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);
	}

	expected_output(data);
}

static void damaged_plane_update(data_t *data)
{
	igt_plane_t *test_plane = data->test_plane;
	struct igt_fb *fb_test;
	uint32_t h, v;
	int x, y;

	if (data->big_fb_test) {
		x = data->big_fb_width / 2;
		y = data->big_fb_height / 2;
	} else {
		x = y = 0;
	}

	switch (data->test_plane_id) {
	case DRM_PLANE_TYPE_OVERLAY:
		h = data->mode->hdisplay / 2;
		v = data->mode->vdisplay / 2;
		break;
	case DRM_PLANE_TYPE_PRIMARY:
		h = data->mode->hdisplay;
		v = data->mode->vdisplay;
		break;
	case DRM_PLANE_TYPE_CURSOR:
		h = v = CUR_SIZE;
		break;
	default:
		igt_assert(false);
	}

	if (data->screen_changes & 1)
		fb_test = data->fb_continuous;
	else
		fb_test = &data->fb_test;

	igt_plane_set_fb(test_plane, fb_test);

	if (data->test_plane_id == DRM_PLANE_TYPE_CURSOR)
		igt_plane_replace_prop_blob(test_plane,
					    IGT_PLANE_FB_DAMAGE_CLIPS,
					    &data->cursor_clip,
					    sizeof(struct drm_mode_rect));
	else
		igt_plane_replace_prop_blob(test_plane,
					    IGT_PLANE_FB_DAMAGE_CLIPS,
					    &data->plane_update_clip,
					    sizeof(struct drm_mode_rect)*
					    data->damage_area_count);

	igt_fb_set_position(fb_test, test_plane, x, y);
	igt_fb_set_size(fb_test, test_plane, h, v);
	igt_plane_set_size(test_plane, h, v);
	igt_plane_set_position(data->test_plane, 0, 0);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_assert(psr_wait_entry(data->debugfs_fd, PSR_MODE_2));

	expected_output(data);
}

static void run(data_t *data)
{
	int i;

	igt_assert(psr_wait_entry(data->debugfs_fd, PSR_MODE_2));

	data->screen_changes = 0;

	switch (data->op) {
	case PLANE_UPDATE:
	case OVERLAY_PRIM_UPDATE:
		damaged_plane_update(data);
		break;
	case PLANE_UPDATE_CONTINUOUS:
		for (data->screen_changes = 0;
		     data->screen_changes < MAX_SCREEN_CHANGES;
		     data->screen_changes++) {
			damaged_plane_update(data);
		}
		break;
	case PLANE_MOVE:
		damaged_plane_move(data);
		break;
	case PLANE_MOVE_CONTINUOUS:
	case PLANE_MOVE_CONTINUOUS_EXCEED:
	case PLANE_MOVE_CONTINUOUS_EXCEED_FULLY:
		/*
		 * Start from top left corner and keep plane position
		 * over iterations.
		 */
		data->cur_x = data->cur_y = 0;
		for (i = POS_TOP_LEFT; i <= POS_RIGHT; i++) {
			data->pos = i;
			plane_move_continuous(data);
		}
		break;
	default:
		igt_assert(false);
	}
}

static void cleanup(data_t *data, igt_output_t *output)
{
	igt_plane_t *primary;
	igt_plane_t *sprite;

	primary = igt_output_get_plane_type(output,
					    DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(primary, NULL);

	if (data->test_plane_id != DRM_PLANE_TYPE_PRIMARY) {
		igt_plane_set_position(data->test_plane, 0, 0);
		igt_plane_set_fb(data->test_plane, NULL);
	}

	if (data->op == OVERLAY_PRIM_UPDATE) {
		sprite = igt_output_get_plane_type(output,
				DRM_PLANE_TYPE_OVERLAY);
		igt_plane_set_position(sprite, 0, 0);
		igt_plane_set_fb(sprite, NULL);
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_remove_fb(data->drm_fd, &data->fb_primary);
	igt_remove_fb(data->drm_fd, &data->fb_overlay);
	igt_remove_fb(data->drm_fd, &data->fb_cursor);
	igt_remove_fb(data->drm_fd, &data->fb_test);
}

static int check_psr2_support(data_t *data, enum pipe pipe)
{
	int status;

	igt_output_t *output;
	igt_display_t *display = &data->display;

	igt_display_reset(display);
	output = data->output;
	igt_output_set_pipe(output, pipe);

	prepare(data, output);
	status = psr_wait_entry(data->debugfs_fd, PSR_MODE_2);
	cleanup(data, output);

	return status;
}

igt_main
{
	data_t data = {};
	igt_output_t *outputs[IGT_MAX_PIPES * IGT_MAX_PIPES];
	int i, j;
	enum pipe pipe;
	int pipes[IGT_MAX_PIPES * IGT_MAX_PIPES];
	int n_pipes = 0;

	igt_fixture {
		drmModeResPtr res;

		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		igt_require_f(psr_sink_support(data.drm_fd,
					       data.debugfs_fd, PSR_MODE_2),
			      "Sink does not support PSR2\n");

		display_init(&data);

		/* Test if PSR2 can be enabled */
		igt_require_f(psr_enable(data.drm_fd,
					 data.debugfs_fd, PSR_MODE_2_SEL_FETCH),
			      "Error enabling PSR2\n");

		data.damage_area_count = MAX_DAMAGE_AREAS;
		data.op = PLANE_UPDATE;
		data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
		data.primary_format = DRM_FORMAT_XRGB8888;
		data.big_fb_test = 0;

		res = drmModeGetResources(data.drm_fd);
		data.big_fb_width = res->max_width;
		data.big_fb_height = res->max_height;
		igt_info("Big framebuffer size %dx%d\n",
			 data.big_fb_width, data.big_fb_height);

		igt_require_f(psr2_selective_fetch_check(data.debugfs_fd),
			      "PSR2 selective fetch not enabled\n");

		for_each_pipe_with_valid_output(&data.display, pipe, data.output) {
			if (check_psr2_support(&data, pipe)) {
				pipes[n_pipes] = pipe;
				outputs[n_pipes] = data.output;
				n_pipes++;
			}
		}
	}

	/* Verify primary plane selective fetch */
	igt_describe("Test that selective fetch works on primary plane");
	igt_subtest_with_dynamic_f("primary-%s-sf-dmg-area", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				for (j = 1; j <= MAX_DAMAGE_AREAS; j++) {
					data.damage_area_count = j;
					data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
					prepare(&data, outputs[i]);
					run(&data);
					cleanup(&data, outputs[i]);
				}
			}
		}
	}

	/* Verify primary plane selective fetch with big fb */
	data.big_fb_test = 1;
	igt_describe("Test that selective fetch works on primary plane with big fb");
	igt_subtest_with_dynamic_f("primary-%s-sf-dmg-area-big-fb", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				for (j = 1; j <= MAX_DAMAGE_AREAS; j++) {
					data.damage_area_count = j;
					data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
					prepare(&data, outputs[i]);
					run(&data);
					cleanup(&data, outputs[i]);
				}
			}
		}
	}

	data.big_fb_test = 0;
	/* Verify overlay plane selective fetch */
	igt_describe("Test that selective fetch works on overlay plane");
	igt_subtest_with_dynamic_f("overlay-%s-sf-dmg-area", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				for (j = 1; j <= MAX_DAMAGE_AREAS; j++) {
					data.damage_area_count = j;
					data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
					prepare(&data, outputs[i]);
					run(&data);
					cleanup(&data, outputs[i]);
				}
			}
		}
	}

	data.damage_area_count = 1;
	/* Verify cursor plane selective fetch */
	igt_describe("Test that selective fetch works on cursor plane");
	igt_subtest_with_dynamic_f("cursor-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	data.op = PLANE_MOVE_CONTINUOUS;
	igt_describe("Test that selective fetch works on moving cursor plane (no update)");
	igt_subtest_with_dynamic_f("cursor-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	data.op = PLANE_MOVE_CONTINUOUS_EXCEED;
	igt_describe("Test that selective fetch works on moving cursor plane exceeding partially visible area (no update)");
	igt_subtest_with_dynamic_f("cursor-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	data.op = PLANE_MOVE_CONTINUOUS_EXCEED_FULLY;
	igt_describe("Test that selective fetch works on moving cursor plane exceeding fully visible area (no update)");
	igt_subtest_with_dynamic_f("cursor-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	/* Only for overlay plane */
	data.op = PLANE_MOVE;
	/* Verify overlay plane move selective fetch */
	igt_describe("Test that selective fetch works on moving overlay plane");
	igt_subtest_with_dynamic_f("%s-sf-dmg-area", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				for (j = POS_TOP_LEFT; j <= POS_BOTTOM_RIGHT ; j++) {
					data.pos = j;
					data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
					prepare(&data, outputs[i]);
					run(&data);
					cleanup(&data, outputs[i]);
				}
			}
		}
	}

	data.op = PLANE_MOVE_CONTINUOUS;
	igt_describe("Test that selective fetch works on moving overlay plane (no update)");
	igt_subtest_with_dynamic_f("overlay-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	data.op = PLANE_MOVE_CONTINUOUS_EXCEED;
	igt_describe("Test that selective fetch works on moving overlay plane partially exceeding visible area (no update)");
	igt_subtest_with_dynamic_f("overlay-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	data.op = PLANE_MOVE_CONTINUOUS_EXCEED_FULLY;
	igt_describe("Test that selective fetch works on moving overlay plane fully exceeding visible area (no update)");
	igt_subtest_with_dynamic_f("overlay-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	/* Verify primary plane selective fetch with overplay plane blended */
	data.op = OVERLAY_PRIM_UPDATE;
	igt_describe("Test that selective fetch works on primary plane "
		     "with blended overlay plane");
	igt_subtest_with_dynamic_f("%s-sf-dmg-area", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				for (j = 1; j <= MAX_DAMAGE_AREAS; j++) {
					data.damage_area_count = j;
					data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
					prepare(&data, outputs[i]);
					run(&data);
					cleanup(&data, outputs[i]);
				}
			}
		}
	}

	/*
	 * Verify overlay plane selective fetch using NV12 primary
	 * plane and continuous updates.
	 */
	data.op = PLANE_UPDATE_CONTINUOUS;
	data.primary_format = DRM_FORMAT_NV12;
	igt_describe("Test that selective fetch works on overlay plane");
	igt_subtest_with_dynamic_f("overlay-%s-sf", op_str(data.op)) {
		for (i = 0; i < n_pipes; i++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipes[i]),
					igt_output_name(outputs[i])) {
				igt_output_set_pipe(outputs[i], pipes[i]);
				data.damage_area_count = 1;
				data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
				prepare(&data, outputs[i]);
				run(&data);
				cleanup(&data, outputs[i]);
			}
		}
	}

	igt_fixture {
		close(data.debugfs_fd);
		display_fini(&data);
		close(data.drm_fd);
	}
}
