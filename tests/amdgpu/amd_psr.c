/*
 * Copyright 2021 Advanced Micro Devices, Inc.
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

#include "drm_mode.h"
#include "igt.h"
#include "igt_core.h"
#include "igt_kms.h"
#include "igt_amd.h"
#include <stdint.h>
#include <fcntl.h>
#include <xf86drmMode.h>

/* hardware requirements:
 * 1. eDP panel that supports PSR (multiple panel can be connected at the same time)
 * 2. Optional DP display for testing a regression condition (setting crtc to null)
 * 3. eDP panel that supports PSR-SU
 */
IGT_TEST_DESCRIPTION("Basic test for enabling Panel Self Refresh for eDP displays");

/* After a full update, a few fast updates are necessary for PSR to be enabled */
#define N_FLIPS 6
/* DMCUB takes some time to actually enable PSR. Worst case delay is 4 seconds */
#define PSR_SETTLE_DELAY 4

typedef struct {
	int x, y;
	int w, h;
} pos_t;

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_plane_t *cursor;
	igt_plane_t *overlay;
	igt_output_t *output;
	igt_pipe_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	igt_fb_t ov_fb[2];
	igt_fb_t pm_fb[2];
	igt_fb_t cs_fb;		/* cursor framebuffer */
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int fd;
	int debugfs_fd;
	int w, h;
	int pfb_w, pfb_h;
	int ofb_w, ofb_h;
} data_t;

enum cursor_move {
	HORIZONTAL,
	VERTICAL,
	DIAGONAL,
	INVALID
};

struct {
	bool visual_confirm;
} opt = {
	.visual_confirm = false,	/* visual confirm debug option */
};

static void draw_color_alpha(igt_fb_t *fb, int x, int y, int w, int h,
		             double r, double g, double b, double a)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	igt_paint_color_alpha(cr, x, y, w, h, r, g, b, a);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	igt_put_cairo_ctx(cr);
}

/* draw a cursor pattern assuming the FB given is square w/ FORMAT ARGB */
static void draw_color_cursor(igt_fb_t *fb, int size, double r, double g, double b)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	int x, y, line_w;

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	/*
	 * draw cursor
	 * recall that alpha blending value:
	 * - 0, output pixel is the background
	 * - 1, output pixel is simply the foreground
	 * - (0, 1), mix of background + foreground
	 */

	/* set cursor FB to background first */
	igt_paint_color_alpha(cr, 0, 0, size, size, 1.0, 1.0, 1.0, .0);

	/*
	 * draw cursur pattern w/ alpha set to 1
	 * - 1. draw triangle part
	 * - 2. draw rectangle part
	 */
	for (x = y = 0, line_w = size / 2; line_w > 0; ++y, --line_w)
		igt_paint_color_alpha(cr, x, y, line_w, 1, r, g, b, 1.0);

	/*
	 * draw rectangle part, split into three geometry parts
	 * - triangle
	 * - rhombus
	 * - reversed triangle
	 */
	for (x = size * 3 / 8, y = size / 8, line_w = 1; y < size * 3 / 8; --x, ++y, line_w += 2)
		igt_paint_color_alpha(cr, x, y, line_w, 1, r, g, b, 1.0);

	for (x = size / 8, y = size * 3 / 8; y < size * 3 / 4; ++x, ++y)
		igt_paint_color_alpha(cr, x, y, line_w, 1, r, g, b, 1.0);

	for (; line_w > 0; ++x, ++y, line_w -= 2)
		igt_paint_color_alpha(cr, x, y, line_w, 1, r, g, b, 1.0);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	igt_put_cairo_ctx(cr);
}

/* update the given framebuffer and draw colorful strip in new position */
static void update_color_strip(igt_fb_t *fb, pos_t *old, pos_t *new, double r, double g, double b)
{
	cairo_t *cr;

	if (!fb || !new || !old)
		return;

	cr = igt_get_cairo_ctx(fb->fd, fb);
	igt_assert_f(cr, "Failed to get cairo context\n");

	/**
	 * we'd draw the strip in the new position w/ given color
	 * and make the background of the framebuffer as black
	 * - render black BG in the old position of strip instead of rendering the whole FB
	 * - render colorful strip in the new position
	 */
	igt_paint_color(cr, old->x, old->y, old->w, old->h, .0, .0, .0);
	igt_paint_color(cr, new->x, new->y, new->w, new->h, r, g, b);

	igt_put_cairo_ctx(cr);
}

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = &data->display.pipes[data->pipe_id];

	igt_display_reset(display);

	data->output = igt_get_single_output_for_pipe(display, data->pipe_id);
	igt_require(data->output);
	igt_info("output %s\n", data->output->name);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);
	kmstest_dump_mode(data->mode);

	data->primary =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	data->cursor =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_CURSOR);

	data->overlay =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_OVERLAY);

	data->pipe_crc = igt_pipe_crc_new(data->fd, data->pipe_id,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_output_set_pipe(data->output, data->pipe_id);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
	data->ofb_w = data->w;
	data->ofb_h = data->h;
	data->pfb_w = data->w / 2;
	data->pfb_h = data->h / 2;

	if (opt.visual_confirm) {
		/**
		 * if visual confirm option is enabled, we'd trigger a full modeset before test run
		 * to make PSR visual confirm enable take effect. DPMS off -> ON transition is one of
		 * the approaches.
		 */
		kmstest_set_connector_dpms(data->fd, data->output->config.connector, DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->fd, data->output->config.connector, DRM_MODE_DPMS_ON);
	}
}
/* Common test cleanup. */
static void test_fini(data_t *data)
{
        igt_display_t *display = &data->display;

        igt_pipe_crc_free(data->pipe_crc);
        igt_display_reset(display);
        igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

static int check_conn_type(data_t *data, uint32_t type) {
	int i;

	for (i = 0; i < data->display.n_outputs; i++) {
		uint32_t conn_type = data->display.outputs[i].config.connector->connector_type;
		if (conn_type == type)
			return i;
	}

	return -1;
}

static bool psr_su_supported(data_t *data)
{
	/* run PSR-SU test i.i.f. eDP panel and kernel driver both support PSR-SU */
	if (!igt_amd_output_has_psr_cap(data->fd, data->output->name)) {
		igt_warn(" driver does not have %s debugfs interface\n", DEBUGFS_EDP_PSR_CAP);
		return false;
	}

	if (!igt_amd_output_has_psr_state(data->fd, data->output->name)) {
		igt_warn(" driver does not have %s debugfs interface\n", DEBUGFS_EDP_PSR_STATE);
		return false;
	}

	if (!igt_amd_psr_support_sink(data->fd, data->output->name, PSR_MODE_2)) {
		igt_warn(" output %s not support PSR-SU\n", data->output->name);
		return false;
	}

	if (!igt_amd_psr_support_drv(data->fd, data->output->name, PSR_MODE_2)) {
		igt_warn(" kernel driver not support PSR-SU\n");
		return false;
	}

	return true;
}

static void run_check_psr(data_t *data, bool test_null_crtc) {
	int fd, edp_idx, dp_idx, ret, i, psr_state;
	igt_fb_t ref_fb, ref_fb2;
	igt_fb_t *flip_fb;
	enum pipe pipe;
	igt_output_t *output;

	test_init(data);

	edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	dp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_DisplayPort);
	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	for_each_pipe_with_single_output(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_create_color_fb(data->fd, data->mode->hdisplay,
				    data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 1.0,
				    0.0, 0.0, &ref_fb);
		igt_create_color_fb(data->fd, data->mode->hdisplay,
				    data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.0,
				    1.0, 0.0, &ref_fb2);

		igt_plane_set_fb(data->primary, &ref_fb);
		igt_output_set_pipe(output, pipe);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

		for (i = 0; i < N_FLIPS; i++) {
			if (i % 2 == 0)
				flip_fb = &ref_fb2;
			else
				flip_fb = &ref_fb;

			ret = drmModePageFlip(data->fd, output->config.crtc->crtc_id,
					      flip_fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
			igt_require(ret == 0);
			kmstest_wait_for_pageflip(data->fd);
		}
	}

	/* PSR state takes some time to settle its value on static screen */
	sleep(PSR_SETTLE_DELAY);

	for_each_pipe_with_single_output(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		psr_state =  igt_amd_read_psr_state(data->fd, output->name);
		igt_fail_on_f(psr_state < PSR_STATE0, "Open PSR state debugfs failed\n");
		igt_fail_on_f(psr_state < PSR_STATE1, "PSR was not enabled for connector %s\n", output->name);
		igt_fail_on_f(psr_state == PSR_STATE_INVALID, "PSR is invalid for connector %s\n", output->name);
		igt_fail_on_f(psr_state != PSR_STATE3, "PSR state is expected to be at PSR_STATE3 (Active) on a "

			      "static screen for connector %s\n", output->name);
	}

	if (test_null_crtc) {
		/* check whether settings crtc to null generated any warning (eDP+DP) */
		igt_skip_on_f(dp_idx == -1, "no DP connector found\n");

		for_each_pipe_with_single_output(&data->display, pipe, output) {
			if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
				continue;

			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}
	}

	igt_remove_fb(data->fd, &ref_fb);
	igt_remove_fb(data->fd, &ref_fb2);
	close(fd);
	test_fini(data);
}

static void run_check_psr_su_mpo(data_t *data, bool scaling, float scaling_ratio)
{
	int edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	igt_fb_t ref_fb;	/* reference fb */
	igt_fb_t *flip_fb;
	int ret;
	const int run_sec = 5;
	int frame_rate = 0;
	pos_t old[2], new;
	int pm_w_scale, pm_h_scale;	/* primary plane width/heigh after scaling */

	/* skip the test run if no eDP sink detected */
	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	/* init */
	test_init(data);
	frame_rate = data->mode->vrefresh;
	memset(&old, 0, sizeof(pos_t) * 2);
	memset(&new, 0, sizeof(pos_t));
	old[0].w = old[1].w = new.w = 30;
	old[0].h = old[1].h = new.h = data->pfb_h;
	if (scaling) {
		pm_w_scale = (int) (data->pfb_w * scaling_ratio);
		pm_h_scale = (int) (data->pfb_h * scaling_ratio);
	}

	/* run the test i.i.f. eDP panel supports and kernel driver both support PSR-SU  */
	igt_skip_on(!psr_su_supported(data));

	/* reference background pattern in grey */
	igt_create_color_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    .5, .5, .5, &ref_fb);
	igt_plane_set_fb(data->primary, &ref_fb);
	igt_output_set_pipe(data->output, data->pipe_id);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	/*
	 * overlay and primary fbs creation
	 * for MPO vpb use case, the vpb is always in the primary plane as an underlay,
	 * while the control panel/tool bar such icons/items are all in the overlay plane,
	 * and alpha for vpb region is adjusted to control the transparency.
	 * thus the overlay fb be initialized w/ ARGB pixel format to support blending
	 */
	igt_create_color_fb(data->fd, data->w, data->h, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			    1.0, 1.0, 1.0, &data->ov_fb[0]);
	igt_create_color_fb(data->fd, data->w, data->h, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			    1.0, 1.0, 1.0, &data->ov_fb[1]);
	igt_create_color_fb(data->fd, data->pfb_w, data->pfb_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    .0, .0, .0, &data->pm_fb[0]);
	igt_create_color_fb(data->fd, data->pfb_w, data->pfb_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    .0, .0, .0, &data->pm_fb[1]);

	/* tie fbs to planes and set position/size/blending */
	igt_plane_set_fb(data->overlay, &data->ov_fb[0]);
	igt_plane_set_fb(data->primary, &data->pm_fb[1]);
	igt_plane_set_position(data->primary, 0, 0);
	igt_plane_set_size(data->primary, data->pfb_w, data->pfb_h);

	/**
	 * adjust alpha for vpb (primary plane) region in overlay.
	 * given alpha, we have below formula:
	 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	 *    blended = alpha * overlay + (1 - alpha) * underlay
	 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	 * since primary plane is underlay while overlay plane is overlay,
	 * to display the content of primary plane w/ blending, we'd set
	 * the alpha of each pixel in overlay corresponding to primary plane
	 * position/size to be zero.
	 */
	draw_color_alpha(&data->ov_fb[0], 0, 0, data->pfb_w, data->pfb_h, .5, .5, .5, .0);
	draw_color_alpha(&data->ov_fb[1], 0, 0, pm_w_scale, pm_h_scale, .5, .5, .5, .0);

	igt_output_set_pipe(data->output, data->pipe_id);
	igt_display_commit_atomic(&data->display, 0, NULL);

	/* multiplane overlay to emulate video playback use case */
	igt_info("\n start flipping ...\n");

	for (int i = 0; i < run_sec * frame_rate; ++i) {
		flip_fb = &data->pm_fb[i & 1];
		/* draw the color strip onto primary plane FB */
		update_color_strip(flip_fb, &old[i & 1], &new, 1.0, .0, 1.0);

		igt_plane_set_fb(data->primary, flip_fb);
		igt_plane_set_position(data->primary, 0, 0);
		/* do scaling at 1/3 iteration, update both primary/overlay */
		if (scaling && (i >= run_sec * frame_rate / 3)) {
			igt_plane_set_fb(data->overlay, &data->ov_fb[1]);
			igt_plane_set_size(data->primary, pm_w_scale, pm_h_scale);
		}
		igt_output_set_pipe(data->output, data->pipe_id);

		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		igt_require(ret == 0);
		kmstest_wait_for_pageflip(data->fd);

		/**
		 * allow some time to observe visual confirm of PSR-SU disabled
		 * once the plane scaling occurs, i.e. green bar on the right side
		 * screen disappears. From driver's view, the PSR-SU would be
		 * disabled when the specific plane height/width detected changed.
		 * and w/ the test run continues, each MPO FB is scaled to the same
		 * size as the first scaled frame, then the PSR-SU is expected to
		 * be re-enabled and green bar should be appear again if visual
		 * confirm debug option is ON.
		 */
		if (scaling && (i == run_sec * frame_rate / 3)) {
			sleep(2);
		}

		/* update strip position */
		old[i & 1].x = new.x;
		new.x += 3;
		new.x = (new.x + new.w > data->pfb_w) ? 0 : new.x;
	}

	/* fini */
	igt_remove_fb(data->fd, &ref_fb);
	igt_remove_fb(data->fd, &data->ov_fb[0]);
	igt_remove_fb(data->fd, &data->ov_fb[1]);
	igt_remove_fb(data->fd, &data->pm_fb[0]);
	igt_remove_fb(data->fd, &data->pm_fb[1]);
	test_fini(data);
}

static void panning_rect_fb(data_t *data, igt_fb_t *rect_fb, int rect_w, int rect_h, int curr_x, int curr_y)
{
	int ret;

	/* set new position for primary plane */
	igt_plane_set_position(data->primary, curr_x, curr_y);
	igt_plane_set_size(data->primary, rect_w, rect_h);

	/* fill in entire overlay planes w/ different colors and set opaque */
	draw_color_alpha(&data->ov_fb[0], 0, 0, data->w, data->h, 1.0, 1.0, 1.0, 1.0); /* white overlay */
	draw_color_alpha(&data->ov_fb[1], 0, 0, data->w, data->h, .0, 1.0, .0, 1.0);   /* greeen overlay */

	/* update alpha region in overlay w/ size of primary plane and set transparent */
	draw_color_alpha(&data->ov_fb[0], curr_x, curr_y, rect_w, rect_h, 1.0, 1.0, 1.0, .0);
	draw_color_alpha(&data->ov_fb[1], curr_x, curr_y, rect_w, rect_h, .0, 1.0, .0, .0);

	/* flip overlay for couple of frames */
	igt_info("\n  primary at (%d, %d) of size (%d, %d), flipping overlay ...\n", curr_x, curr_y, rect_w, rect_h);
	for (int i = 0; i < N_FLIPS; ++i) {
		/* do flip overlay */
		igt_plane_set_fb(data->overlay, &data->ov_fb[i % 2]);
		igt_plane_set_fb(data->primary, rect_fb);
		igt_plane_set_size(data->primary, rect_w, rect_h);
		igt_output_set_pipe(data->output, data->pipe_id);

		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		igt_require(ret == 0);
		kmstest_wait_for_pageflip(data->fd);
	}
}

static void run_check_psr_su_ffu(data_t *data)
{
	int edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	igt_fb_t ref_fb;	/* reference fb */

	/* skip the test run if no eDP sink detected */
	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	/* init */
	test_init(data);

	/* run the test i.i.f. eDP panel supports and kernel driver both support PSR-SU  */
	igt_skip_on(!psr_su_supported(data));

	/* reference background pattern in grey */
	igt_create_color_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    .5, .5, .5, &ref_fb);
	igt_plane_set_fb(data->primary, &ref_fb);
	igt_output_set_pipe(data->output, data->pipe_id);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	/*
	 * overlay and primary fbs creation
	 * for full frame update (FFU) test case, we don't change primary FB content but to change
	 * the position of primary FB (panning) and update the overlay plane alpha region.
	 * Any overlay change is expected to be regarded as FFU from KMD's perspective.
	 *
	 * 1. create two overlay FBs of full screen size and different colors and
	 *    one primary FB of quarter screen size
	 * 2. panning the primary plane to top-left and flip for couple of frames
	 * 3. wait for couple of seconds to allow visual confirm
	 * 4. panning the primary plane from top-left to middle of screen
	 * 5. repeat step 3
	 * 6. panning the primary plane from middle to bottom-right of screen
	 * 7. repeat step 3
	 *
	 * Note:
	 * Ideally we only want 0.0 alpha over the primary plane region, with the
	 * rest as solid (1.0) alpha:
	 * +----------------------------+
	 * | +-------------+            |
	 * | |             |            |
	 * | |  Primary    |		|
	 * | |  (alpha=0.0)|            |
	 * | +-------------+            |
	 * |                Overlay     |
	 * |              (alpha=1.0)   |
	 * +----------------------------+
	 */

	/* step 1 */
	igt_create_fb(data->fd, data->w, data->h, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, &data->ov_fb[0]);
	igt_create_fb(data->fd, data->w, data->h, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, &data->ov_fb[1]);
	igt_create_color_fb(data->fd, data->pfb_w, data->pfb_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    1.0, .0, 1.0, &data->pm_fb[0]); /* magenta primary */

	/* step 2 & 3 */
	panning_rect_fb(data, &data->pm_fb[0], data->pfb_w, data->pfb_h, 0, 0);
	sleep(5);

	/* step 4 & 5 */
	panning_rect_fb(data, &data->pm_fb[0], data->pfb_w, data->pfb_h, data->pfb_w / 2, data->pfb_h / 2);
	sleep(5);

	/* step 6 & 7 */
	panning_rect_fb(data, &data->pm_fb[0], data->pfb_w, data->pfb_h, data->pfb_w, data->pfb_h);
	sleep(5);

	/* fini */
	igt_remove_fb(data->fd, &ref_fb);
	igt_remove_fb(data->fd, &data->ov_fb[0]);
	igt_remove_fb(data->fd, &data->ov_fb[1]);
	igt_remove_fb(data->fd, &data->pm_fb[0]);
	test_fini(data);
}

static void test_cursor_movement(data_t *data, int iters, int cs_size, enum cursor_move move_type, bool test_mpo)
{
	int i, pos_x, pos_y;
	int ret;
	igt_fb_t *pfb;

	/* incremental step == cursor size / 16 */
	for (i = 0, pos_y = 0, pos_x = 0; i < iters; ++i) {
		if (move_type == HORIZONTAL && (pos_x + cs_size > data->w))
			pos_x = 0;
		else if (move_type == VERTICAL && (pos_y + cs_size > data->h))
			pos_y = 0;
		else if (move_type == DIAGONAL && ((pos_y + cs_size > data->h) || (pos_x + cs_size > data->w)))
			pos_x = pos_y = 0;

		/* move cursor */
		igt_plane_set_position(data->cursor, pos_x, pos_y);

		/* flip primary FB if MPO flag set */
		pfb = test_mpo ? &data->pm_fb[i % 2] : &data->pm_fb[0];
		igt_plane_set_fb(data->primary, pfb);

		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		igt_require(ret == 0);
		kmstest_wait_for_pageflip(data->fd);

		/* update position */
		if (move_type == HORIZONTAL)
			pos_x += cs_size / 16;
		else if (move_type == VERTICAL)
			pos_y += cs_size / 16;
		else if (move_type == DIAGONAL) {
			pos_x += cs_size / 16;
			pos_y += cs_size / 16;
		}
	}
}

static void run_check_psr_su_cursor(data_t *data, bool test_mpo)
{
	int edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	const int cs_size = 128;
	const int delay_sec = 5; /* seconds */
	int frame_rate = 0;

	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	test_init(data);
	igt_skip_on(!psr_su_supported(data));

	frame_rate = data->mode->vrefresh;

	/*
	 * primary & overlay FBs creation
	 * - create primary FBs of quarter screen size of different colors (blue and green)
	 * - create overlay FB of screen size of white color (default alpha 1.0)
	 */
	igt_create_color_fb(data->fd, data->pfb_w, data->pfb_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    .0, .0, 1.0, &data->pm_fb[0]);
	igt_create_color_fb(data->fd, data->pfb_w, data->pfb_h, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    .0, 1.0, .0, &data->pm_fb[1]);
	igt_create_color_fb(data->fd, data->ofb_w, data->ofb_h, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			    1.0, 1.0, 1.0, &data->ov_fb[0]);

	/* cursor FB creation, draw cursor pattern/set alpha regions */
	igt_create_fb(data->fd, cs_size, cs_size, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, &data->cs_fb);
	draw_color_cursor(&data->cs_fb, cs_size, 1.0, .0, 1.0);

	/*
	 * panning the primary plane at the top-left of screen
	 * set alpha region in overlay plane and set alpha to 0.0 to show primary plane
	 * set cursor plane and starting from position of (0, 0)
	 */ 
	draw_color_alpha(&data->ov_fb[0], 0, 0, data->pfb_w, data->pfb_h, 1.0, 1.0, 1.0, .0);
	igt_plane_set_fb(data->primary, &data->pm_fb[0]);
	igt_plane_set_fb(data->overlay, &data->ov_fb[0]);
	igt_plane_set_fb(data->cursor, &data->cs_fb);
	igt_plane_set_position(data->cursor, 0, 0);

	igt_output_set_pipe(data->output, data->pipe_id);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	/*
	 * test by setting different cursor position in screen
	 * - horizontal movement
	 * - vertial movement
	 * - diagonal movement
	 */

	/* horizontal */
	igt_info("  moving cursor in horizontal ...\n");
	test_cursor_movement(data, frame_rate * delay_sec, cs_size, HORIZONTAL, test_mpo);

	/* vertical */
	igt_info("  moving cursor in vertical ...\n");
	test_cursor_movement(data, frame_rate * delay_sec, cs_size, VERTICAL, test_mpo);

	/* diagonal */
	igt_info("  moving cursor in diagonal ...\n");
	test_cursor_movement(data, frame_rate * delay_sec, cs_size, DIAGONAL, test_mpo);

	igt_remove_fb(data->fd, &data->pm_fb[0]);
	igt_remove_fb(data->fd, &data->pm_fb[1]);
	igt_remove_fb(data->fd, &data->cs_fb);
	igt_remove_fb(data->fd, &data->ov_fb[0]);
	test_fini(data);
}

const char *help_str =
"  --visual-confirm           PSR visual confirm debug option enable\n";

struct option long_options[] = {
	{"visual-confirm",	required_argument, NULL, 'v'},
	{ 0, 0, 0, 0 }
};

static int opt_handler(int option, int option_index, void *data)
{
	switch (option) {
	case 'v':
		opt.visual_confirm = strtol(optarg, NULL, 0);
		igt_info(" PSR Visual Confirm %s\n", opt.visual_confirm ? "enabled" : "disabled");
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	data_t data;

	igt_skip_on_simulation();
	memset(&data, 0, sizeof(data));

	igt_fixture
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);
		if (data.fd == -1) igt_skip("Not an amdgpu driver.\n");
		data.debugfs_fd = igt_debugfs_dir(data.fd);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(&data.display.is_atomic);
		igt_display_require_output(&data.display);

		/* check if visual confirm option available */
		if (opt.visual_confirm) {
			igt_skip_on(!igt_amd_has_visual_confirm(data.fd));
			igt_skip_on_f(!igt_amd_set_visual_confirm(data.fd, VISUAL_CONFIRM_PSR),
				      "set PSR visual confirm failed\n");
		}
	}

	igt_describe("Test whether PSR can be enabled with static screen");
	igt_subtest("psr_enable") run_check_psr(&data, false);

	igt_describe("Test whether setting CRTC to null triggers any warning with PSR enabled");
	igt_subtest("psr_enable_null_crtc") run_check_psr(&data, true);

	igt_describe("Test to validate PSR SU enablement with Visual Confirm "
		     "and to imitate Multiplane Overlay video playback scenario");
	igt_subtest("psr_su_mpo") run_check_psr_su_mpo(&data, false, .0);

	igt_describe("Test to validate PSR SU enablement with Visual Confirm "
		     "and to validate Full Frame Update scenario");
	igt_subtest("psr_su_ffu") run_check_psr_su_ffu(&data);

	igt_describe("Test to validate PSR SU enablement with Visual Confirm "
		     "and to validate cursor movement + static background scenario");
	igt_subtest("psr_su_cursor") run_check_psr_su_cursor(&data, false);

	igt_describe("Test to validate PSR SU enablement with Visual Confirm "
		     "and to validate cursor movement + MPO scenario");
	igt_subtest("psr_su_cursor_mpo") run_check_psr_su_cursor(&data, true);

	igt_describe("Test to validate PSR SU enablement with Visual Confirm "
		     "and to validate PSR SU disable/re-enable w/ primary scaling ratio 1.5");
	igt_subtest("psr_su_mpo_scaling_1_5") run_check_psr_su_mpo(&data, true, 1.5);

	igt_describe("Test to validate PSR SU enablement with Visual Confirm "
		     "and to validate PSR SU disable/re-enable w/ primary scaling ratio 0.75");
	igt_subtest("psr_su_mpo_scaling_0_75") run_check_psr_su_mpo(&data, true, .75);

	igt_fixture
	{
		if (opt.visual_confirm) {
			igt_require_f(igt_amd_set_visual_confirm(data.fd, VISUAL_CONFIRM_DISABLE),
				      "reset PSR visual confirm option failed\n");
		}
		close(data.debugfs_fd);
		igt_display_fini(&data.display);
	}
}
