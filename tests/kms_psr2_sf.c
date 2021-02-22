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
#include "intel_bufmgr.h"

IGT_TEST_DESCRIPTION("Tests to varify PSR2 selective fetch by sending multiple"
		     " damaged areas");

#define SQUARE_SIZE 100

#define CUR_SIZE 64
#define MAX_DAMAGE_AREAS 5

enum operations {
	PLANE_UPDATE,
	PLANE_MOVE,
	OVERLAY_PRIM_UPDATE
};

enum plane_move_postion {
	POS_TOP_LEFT,
	POS_TOP_RIGHT,
	POS_BOTTOM_LEFT,
	POS_BOTTOM_RIGHT
};

typedef struct {
	int drm_fd;
	int debugfs_fd;
	igt_display_t display;
	drm_intel_bufmgr *bufmgr;
	drmModeModeInfo *mode;
	igt_output_t *output;
	struct igt_fb fb_primary, fb_overlay, fb_cursor;
	struct igt_fb fb_test;
	int damage_area_count;
	struct drm_mode_rect plane_update_clip[MAX_DAMAGE_AREAS];
	struct drm_mode_rect plane_move_clip;
	struct drm_mode_rect cursor_clip;
	enum operations op;
	enum plane_move_postion pos;
	int test_plane_id;
	igt_plane_t *test_plane;
	cairo_t *cr;
} data_t;

static const char *op_str(enum operations op)
{
	static const char * const name[] = {
		[PLANE_UPDATE] = "plane-update",
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
				       uint32_t v)
{
	int x, y;
	int width = SQUARE_SIZE;
	int height = SQUARE_SIZE;

	switch (data->damage_area_count) {
	case 5:
		/*Bottom right corner*/
		x = h - SQUARE_SIZE;
		y = v - SQUARE_SIZE;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[4], x, y, width, height);
	case 4:
		/*Bottom left corner*/
		x = 0;
		y = v - SQUARE_SIZE;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[3], x, y, width, height);
	case 3:
		/*Top right corner*/
		x = h - SQUARE_SIZE;
		y = 0;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[2], x, y, width, height);
	case 2:
		/*Top left corner*/
		x = 0;
		y = 0;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[1], x, y, width, height);
	case 1:
		/*Center*/
		x = h/2 - SQUARE_SIZE/2;
		y = v/2 - SQUARE_SIZE/2;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[0], x, y, width, height);
		break;
	default:
		igt_assert(false);
	}
}

static void plane_move_setup_square(data_t *data, igt_fb_t *fb, uint32_t h,
				       uint32_t v)
{
	int x = 0, y = 0;

	switch (data->pos) {
	case POS_TOP_LEFT:
		/*Bottom right corner*/
		x = h - SQUARE_SIZE;
		y = v - SQUARE_SIZE;
		break;
	case POS_TOP_RIGHT:
		/*Bottom left corner*/
		x = 0;
		y = v - SQUARE_SIZE;
		break;
	case POS_BOTTOM_LEFT:
		/*Top right corner*/
		x = h - SQUARE_SIZE;
		y = 0;
		break;
	case POS_BOTTOM_RIGHT:
		/*Top left corner*/
		x = 0;
		y = 0;
		break;
	default:
		igt_assert(false);
	}

	draw_rect(data, fb, x, y,
		  SQUARE_SIZE, SQUARE_SIZE, 1.0, 1.0, 1.0, 1.0);
	set_clip(&data->plane_move_clip, x, y, SQUARE_SIZE, SQUARE_SIZE);
}

static void prepare(data_t *data)
{
	igt_plane_t *primary, *sprite = NULL, *cursor = NULL;

	/* all green frame */
	igt_create_color_fb(data->drm_fd,
			    data->mode->hdisplay, data->mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 1.0, 0.0,
			    &data->fb_primary);

	primary = igt_output_get_plane_type(data->output,
			DRM_PLANE_TYPE_PRIMARY);

	switch (data->test_plane_id) {
	case DRM_PLANE_TYPE_OVERLAY:
		sprite = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_OVERLAY);
		/*All blue plane*/
		igt_create_color_fb(data->drm_fd,
				    data->mode->hdisplay/2,
				    data->mode->vdisplay/2,
				    DRM_FORMAT_XRGB8888,
				    LOCAL_DRM_FORMAT_MOD_NONE,
				    0.0, 0.0, 1.0,
				    &data->fb_overlay);

		igt_create_color_fb(data->drm_fd,
				    data->mode->hdisplay/2,
				    data->mode->vdisplay/2,
				    DRM_FORMAT_XRGB8888,
				    LOCAL_DRM_FORMAT_MOD_NONE,
				    0.0, 0.0, 1.0,
				    &data->fb_test);

		if (data->op == PLANE_MOVE) {
			plane_move_setup_square(data, &data->fb_test,
					   data->mode->hdisplay/2,
					   data->mode->vdisplay/2);

		} else {
			plane_update_setup_squares(data, &data->fb_test,
					   data->mode->hdisplay/2,
					   data->mode->vdisplay/2);
		}

		igt_plane_set_fb(sprite, &data->fb_overlay);
		data->test_plane = sprite;
		break;

	case DRM_PLANE_TYPE_PRIMARY:
		igt_create_color_fb(data->drm_fd,
			    data->mode->hdisplay, data->mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 1.0, 0.0,
			    &data->fb_test);

		plane_update_setup_squares(data, &data->fb_test,
					   data->mode->hdisplay,
					   data->mode->vdisplay);
		data->test_plane = primary;

		if (data->op == OVERLAY_PRIM_UPDATE) {
			sprite = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_OVERLAY);

			igt_create_color_fb(data->drm_fd,
					    data->mode->hdisplay,
					    data->mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    LOCAL_DRM_FORMAT_MOD_NONE,
					    0.0, 0.0, 1.0,
					    &data->fb_overlay);

			igt_plane_set_fb(sprite, &data->fb_overlay);
			igt_plane_set_prop_value(sprite, IGT_PLANE_ALPHA,
						 0x6060);
		}
		break;

	case DRM_PLANE_TYPE_CURSOR:
		cursor = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_CURSOR);
		igt_plane_set_position(cursor, 0, 0);

		igt_create_fb(data->drm_fd, CUR_SIZE, CUR_SIZE,
			      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			      &data->fb_cursor);

		draw_rect(data, &data->fb_cursor, 0, 0, CUR_SIZE, CUR_SIZE,
			    0.0, 0.0, 1.0, 1.0);

		igt_create_fb(data->drm_fd, CUR_SIZE, CUR_SIZE,
			      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			      &data->fb_test);

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

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static inline void manual(const char *expected)
{
	igt_debug_manual_check("all", expected);
}

static void plane_update_expected_output(int plane_type, int box_count)
{
	char expected[64] = {};

	switch (plane_type) {
	case DRM_PLANE_TYPE_PRIMARY:
		sprintf(expected, "screen Green with %d White box(es)",
			box_count);
		break;
	case DRM_PLANE_TYPE_OVERLAY:
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
	case PLANE_UPDATE:
		plane_update_expected_output(data->test_plane_id,
					     data->damage_area_count);
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

	igt_plane_set_fb(test_plane, &data->fb_test);

	if (data->test_plane_id == DRM_PLANE_TYPE_OVERLAY) {
		h = h/2;
		v = v/2;
	}

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

static void damaged_plane_update(data_t *data)
{
	igt_plane_t *test_plane = data->test_plane;
	uint32_t h = data->mode->hdisplay;
	uint32_t v = data->mode->vdisplay;

	igt_plane_set_fb(test_plane, &data->fb_test);

	if (data->test_plane_id == DRM_PLANE_TYPE_OVERLAY) {
		h = h/2;
		v = v/2;
	}

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

	igt_plane_set_position(data->test_plane, 0, 0);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_assert(psr_wait_entry(data->debugfs_fd, PSR_MODE_2));

	expected_output(data);
}

static void run(data_t *data)
{
	igt_assert(psr_wait_entry(data->debugfs_fd, PSR_MODE_2));

	switch (data->op) {
	case PLANE_UPDATE:
	case OVERLAY_PRIM_UPDATE:
		damaged_plane_update(data);
		break;
	case PLANE_MOVE:
		damaged_plane_move(data);
		break;
	default:
		igt_assert(false);
	}
}

static void cleanup(data_t *data)
{
	igt_plane_t *primary;
	igt_plane_t *sprite;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(primary, NULL);

	if (data->test_plane_id != DRM_PLANE_TYPE_PRIMARY) {
		igt_plane_set_position(data->test_plane, 0, 0);
		igt_plane_set_fb(data->test_plane, NULL);
	}

	if (data->op == OVERLAY_PRIM_UPDATE) {
		sprite = igt_output_get_plane_type(data->output,
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

igt_main
{
	data_t data = {};
	int i;

	igt_fixture {
		int r;

		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		igt_require_f(psr_sink_support(data.drm_fd,
					       data.debugfs_fd, PSR_MODE_2),
			      "Sink does not support PSR2\n");

		data.bufmgr = drm_intel_bufmgr_gem_init(data.drm_fd, 4096);
		igt_assert(data.bufmgr);
		drm_intel_bufmgr_gem_enable_reuse(data.bufmgr);

		display_init(&data);

		/* Test if PSR2 can be enabled */
		igt_require_f(psr_enable(data.drm_fd,
					 data.debugfs_fd, PSR_MODE_2_SEL_FETCH),
			      "Error enabling PSR2\n");

		data.damage_area_count = MAX_DAMAGE_AREAS;
		data.op = PLANE_UPDATE;
		data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
		prepare(&data);
		r = psr_wait_entry(data.debugfs_fd, PSR_MODE_2);
		if (!r)
			psr_print_debugfs(data.debugfs_fd);

		igt_require_f(psr2_selective_fetch_check(data.debugfs_fd),
			      "PSR2 selective fetch not enabled\n");
		cleanup(&data);
		if (!r)
			psr_print_debugfs(data.debugfs_fd);
		igt_require_f(r, "PSR2 can not be enabled\n");
	}

	/* Verify primary plane selective fetch */
	for (i = 1; i <= MAX_DAMAGE_AREAS; i++) {
		igt_subtest_f("primary-%s-sf-dmg-area-%d", op_str(data.op), i) {
			data.damage_area_count = i;
			data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
			prepare(&data);
			run(&data);
			cleanup(&data);
		}
	}

	/* Verify overlay plane selective fetch */
	for (i = 1; i <= MAX_DAMAGE_AREAS; i++) {
		igt_subtest_f("overlay-%s-sf-dmg-area-%d", op_str(data.op), i) {
			data.damage_area_count = i;
			data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
			prepare(&data);
			run(&data);
			cleanup(&data);
		}
	}

	/* Verify overlay plane selective fetch */
	igt_subtest_f("cursor-%s-sf", op_str(data.op)) {
		data.damage_area_count = 1;
		data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
		prepare(&data);
		run(&data);
		cleanup(&data);
	}

	/* Only for overlay plane */
	data.op = PLANE_MOVE;
	/* Verify overlay plane move selective fetch */
	for (i = POS_TOP_LEFT; i <= POS_BOTTOM_RIGHT ; i++) {
		igt_subtest_f("%s-sf-dmg-area-%d", op_str(data.op), i) {
			data.pos = i;
			data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
			prepare(&data);
			run(&data);
			cleanup(&data);
		}
	}

	/* Verify primary plane selective fetch with overplay plane blended */
	data.op = OVERLAY_PRIM_UPDATE;
	for (i = 1; i <= MAX_DAMAGE_AREAS; i++) {
		igt_subtest_f("%s-sf-dmg-area-%d", op_str(data.op), i) {
			data.damage_area_count = i;
			data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
			prepare(&data);
			run(&data);
			cleanup(&data);
		}
	}

	igt_fixture {
		close(data.debugfs_fd);
		drm_intel_bufmgr_destroy(data.bufmgr);
		display_fini(&data);
	}
}
