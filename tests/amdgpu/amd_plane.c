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
#include "libdrm/amdgpu.h"
#include "libdrm/amdgpu_drm.h"

IGT_TEST_DESCRIPTION("Tests for Multi Plane Overlay for single and dual displays");

/* Maximum pipes on any AMD ASIC. */
#define MAX_PIPES 6
#define DISPLAYS_TO_TEST 2

/* (De)gamma LUT. */
typedef struct lut {
	struct drm_color_lut *data;
	uint32_t size;
} lut_t;

/* Common test data. */
typedef struct data {
        igt_display_t display;
        igt_plane_t *primary[MAX_PIPES];
        igt_plane_t *cursor[MAX_PIPES];
	igt_plane_t *overlay[MAX_PIPES];
        igt_output_t *output[MAX_PIPES];
        igt_pipe_t *pipe[MAX_PIPES];
        igt_pipe_crc_t *pipe_crc[MAX_PIPES];
        drmModeModeInfo mode[MAX_PIPES];
        enum pipe pipe_id[MAX_PIPES];
        int w[MAX_PIPES];
        int h[MAX_PIPES];
        int fd;
} data_t;

static const drmModeModeInfo test_mode_1 = {
	.name = "1920x1080 Test",
	.vrefresh = 60,
	.clock = 148500,
	.hdisplay = 1920,
	.hsync_start = 2008,
	.hsync_end = 2052,
	.htotal = 2200,
	.vdisplay = 1080,
	.vsync_start = 1084,
	.vsync_end = 1089,
	.vtotal = 1125,
	.type = 0x40,
	.flags = DRM_MODE_FLAG_NHSYNC,
	.hskew = 0,
	.vscan = 0,
};

static const drmModeModeInfo test_mode_2 = {
	.name = "1280x1024 Test",
	.vrefresh = 60,
	.clock = 148500,
	.hdisplay = 1280,
	.hsync_start = 2008,
	.hsync_end = 2052,
	.htotal = 2200,
	.vdisplay = 1024,
	.vsync_start = 1084,
	.vsync_end = 1089,
	.vtotal = 1125,
	.type = DRM_MODE_TYPE_DRIVER,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.hskew = 0,
	.vscan = 0,
};

static const drmModeModeInfo test_mode_3 = {
	.name = "3840x2160 Test",
	.vrefresh = 60,
	.clock = 594000,
	.hdisplay = 3840,
	.hsync_start = 4016,
	.hsync_end = 4104,
	.htotal = 4400,
	.vdisplay = 2160,
	.vsync_start = 2168,
	.vsync_end = 2178,
	.vtotal = 2250,
	.type = DRM_MODE_TYPE_DRIVER,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.hskew = 0,
	.vscan = 0,
};

static void lut_init(lut_t *lut, uint32_t size)
{
	igt_assert(size > 0);
	lut->size = size;
	lut->data = malloc(size * sizeof(struct drm_color_lut));
	igt_assert(lut);
}
static void lut_gen(lut_t *lut)
{
	uint32_t i;
	/* 10% threshold */
	uint32_t threshold = (256 * 10) / 100;

	for (i = 0; i < threshold; ++i) {
		uint32_t v = 0;
		lut->data[i].red = v;
		lut->data[i].blue = v;
		lut->data[i].green = v;
	}
	for (i = threshold; i < lut->size; ++i) {
		uint32_t v = 0xffff;
		lut->data[i].red = v;
		lut->data[i].blue = v;
		lut->data[i].green = v;
	}
}
static void lut_free(lut_t *lut)
{
	if (lut->data) {
		free(lut->data);
		lut->data = NULL;
	}
	lut->size = 0;
}

enum test {
	MPO_SINGLE_PAN,
	MPO_MULTI_PAN,
	MPO_SCALE
};

static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	int i, n, max_pipes = display->n_pipes;

	for_each_pipe(display, i) {
		data->pipe_id[i] = PIPE_A + i;
		data->pipe[i] = &display->pipes[data->pipe_id[i]];
		data->primary[i] = igt_pipe_get_plane_type(
			data->pipe[i], DRM_PLANE_TYPE_PRIMARY);
		data->overlay[i] = igt_pipe_get_plane_type_index(
			data->pipe[i], DRM_PLANE_TYPE_OVERLAY, 0);
		data->cursor[i] = igt_pipe_get_plane_type(
			data->pipe[i], DRM_PLANE_TYPE_CURSOR);
		data->pipe_crc[i] =
			igt_pipe_crc_new(data->fd, data->pipe_id[i],
					 IGT_PIPE_CRC_SOURCE_AUTO);
	}

	for (i = 0, n = 0; i < display->n_outputs && n < max_pipes; ++i) {
		igt_output_t *output = &display->outputs[i];

		data->output[n] = output;

		/* Only allow physically connected displays for the tests. */
		if (!igt_output_is_connected(output))
			continue;

		igt_assert(kmstest_get_connector_default_mode(
			data->fd, output->config.connector, &data->mode[n]));

		data->w[n] = data->mode[n].hdisplay;
		data->h[n] = data->mode[n].vdisplay;

		n += 1;
	}

	igt_require(data->output[0]);
	igt_display_reset(display);
}

static void test_fini(data_t *data)
{
	igt_display_t *display = &data->display;
	int i;

	for_each_pipe(display, i) {
		igt_pipe_crc_free(data->pipe_crc[i]);
	}

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

/* Forces a mode for a connector. */
static void force_output_mode(data_t *data, igt_output_t *output,
			      drmModeModeInfo const *mode)
{
	/* This allows us to create a virtual sink. */
	if (!igt_output_is_connected(output)) {
		kmstest_force_edid(data->fd, output->config.connector,
				   igt_kms_get_4k_edid());

		kmstest_force_connector(data->fd, output->config.connector,
					FORCE_CONNECTOR_DIGITAL);
	}

	igt_output_override_mode(output, mode);
}


static int set_metadata(data_t *data, igt_fb_t *fb, struct amdgpu_bo_metadata *info)
{
	struct drm_amdgpu_gem_metadata args = {};

	args.handle = fb->gem_handle;
	args.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
	args.data.flags = info->flags;
	args.data.tiling_info = info->tiling_info;

	if (info->size_metadata > sizeof(args.data.data))
		return -EINVAL;

	if (info->size_metadata) {
		args.data.data_size_bytes = info->size_metadata;
		memcpy(args.data.data, info->umd_metadata, info->size_metadata);
	}

	return drmCommandWriteRead(data->fd, DRM_AMDGPU_GEM_METADATA, &args,
				   sizeof(args));
}

static void draw_color_alpha(igt_fb_t *fb, int x, int y, int w, int h,
		             double r, double g, double b, double a)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	igt_paint_color_alpha(cr, x, y, w, h, r, g, b, a);
	igt_put_cairo_ctx(cr);
}

struct fbc {
	igt_fb_t ref_primary;
	igt_fb_t test_primary;
	igt_fb_t test_overlay;
	igt_crc_t ref_crc;
};

/* Sets the regamma LUT. */
static void set_regamma_lut(data_t *data, lut_t const *lut, int n)
{
	size_t size = lut ? sizeof(lut->data) * lut->size : 0;
	const void *ptr = lut ? lut->data : NULL;
	igt_pipe_obj_replace_prop_blob(data->pipe[n], IGT_CRTC_GAMMA_LUT, ptr,
				       size);
}

/*
 * Compares the result of white backgroud with white window with and without MPO
 *
 * Reference crc:
 * Draws a White background of size (pw,ph).
 *
 * Test crc:
 * Draws a White Overlay of size (pw,ph) then creates a cutout of size (p,w) at location (x,y)
 * Draws a White Primary plane of size (p,w) at location (x,y) (under the overlay)
 *
 * NOTE: The reason for using White+White is to speed up the crc (reuse the ref crc for all cases vs taking
 * a ref crc per flip)
 */
static void test_plane(data_t *data, int n, int x, int y, double w, double h, double dw, double dh, int pw, int ph, struct fbc *fbc){

	igt_crc_t test_crc;
	igt_display_t *display = &data->display;

	/* Reference: */

	igt_plane_set_fb(data->primary[n], &fbc[n].ref_primary);

	igt_plane_set_position(data->primary[n], 0, 0);
	igt_plane_set_size(data->primary[n], pw, ph);

	igt_display_commit_atomic(display, 0, 0);

	/* Test: */
	/* Draw a white overlay with a cutout */
	draw_color_alpha(&fbc[n].test_overlay, 0, 0, pw, ph, 1.0, 1.0, 1.0, 1.00);
	draw_color_alpha(&fbc[n].test_overlay, x, y, dw, dh, 0.0, 0.0, 0.0, 0.0);

	igt_plane_set_fb(data->primary[n], &fbc[n].test_primary);
	igt_plane_set_fb(data->overlay[n], &fbc[n].test_overlay);

	/* Move the overlay to cover the cutout */
	igt_plane_set_position(data->primary[n], x, y);
	igt_plane_set_size(data->primary[n], dw, dh);

	igt_display_commit_atomic(display, 0, 0);
	igt_pipe_crc_collect_crc(data->pipe_crc[n], &test_crc);
	igt_plane_set_fb(data->overlay[n], NULL);

	igt_assert_crc_equal(&fbc[n].ref_crc, &test_crc);

	/* Set window to white, this is to avoid flashing between black/white after each flip */
	draw_color_alpha(&fbc[n].ref_primary, 0, 0, pw, ph, 1.0, 1.0, 1.0, 1.00);
	igt_plane_set_fb(data->primary[n], &fbc[n].ref_primary);
	igt_plane_set_position(data->primary[n], 0, 0);
	igt_plane_set_size(data->primary[n], pw, ph);
	igt_display_commit_atomic(display, 0, 0);


}
/*
 * MPO_SINGLE_PAN: This test moves the window (w,h) horizontally, vertically and diagonally
 * Horizontal: from top-left (0,0) to top-right (pw-w,0)
 * Vertical: from top-left (0,0) to bottom-left (0,ph-h)
 * Diagonal: from top-left (0,0) to bottom-right (pw-w, ph-h)
 */
static void test_panning_1_display(data_t *data, int display_count, int w, int h, struct fbc *fb)
{
	/* x and y movements */
	int dir[3][2]= {
		{0,1}, /* Only Y */
		{1,0}, /* Only X */
		{1,1}, /* Both X and Y */

	};

	/* # of iterations to use to move from one side to the other */
	int it = 3;

	for (int n = 0; n < display_count; n++) {

		int pw = data->w[n];
		int ph = data->h[n];
		int dx = (pw-w)/it;
		int dy = (ph-h)/it;

		for (int i = 0; i < ARRAY_SIZE(dir); i++){
			for (int j = 0; j <= it; j++){

				int x = dx*j*dir[i][0];
				int y = dy*j*dir[i][1];

				/* No need to pan a overley that is bigger than the display */
				if (pw <= w && ph <= h)
					break;

				test_plane(data, n, x, y, w, h, w, h, pw, ph, fb);

			}
		}
	}

	return;


}

 /* MPO_SCALE: This test scales a window of size (w,h) from x1/4->x16.
  */
static void test_scaling_planes(data_t *data, int display_count, int w, int h, struct fbc *fb)
{

	/* Scale limit is x1/4 -> x16
	 * some combinations of mode/window sizes fail for x0.25 so start from 0.30 -> 16
	 */
	double scale[]= {
		0.30,
		0.50,
		0.75,
		1.50,
		3.00,
		6.00,
		12.00,
		16.00

	};

	for (int n = 0; n < display_count; n++) {
		int pw = data->w[n];
		int ph = data->h[n];

		for (int i=0;i<ARRAY_SIZE(scale);i++) {
			/* No need to scale a overley that is bigger than the display */
			if (pw <= w*scale[i] && ph <= h*scale[i])
				break;
			test_plane(data, n, 0, 0, w, h, w*scale[i], h*scale[i], pw, ph, fb);
		}

		/* Test Fullscreen scale*/
		test_plane(data, n, 0, 0, w, h, pw, ph, pw, ph, fb);
	}

	return;
}
/*
 * MPO_MULTI_PAN: Requires 2 displays. This test swaps a window (w,h) between 2 displays at 3 different
 * vertical locations (top, middle, bottom)
 *
 * MPO will usually be the 'largest' part of the video window. Which means when a window is
 * being dragged between 2 displays there is a instance where the MPO will jump between the displays.
 * This test should be called with w/2 to emulate the behaviour of MPO switching between displays
 */
static void test_panning_2_display(data_t *data, int w, int h, struct fbc *fbc)
{
	bool toggle = true;
	int pw =  data->w[0];
	int ph =  data->h[0];
	int pw2 =  data->w[1];
	int ph2 =  data->h[1];
	int smallest_h = min(ph, ph2);
	int y[] = {0, smallest_h/2-h/2, smallest_h-h};
	int it = 3; /* # of times to swap */

	/* Set y to 0 if window is bigger than one of the displays
	 * beacause y will be negative in that case
	 */
	if (h >= smallest_h)
		y[0] = y[1] = y[2] = 0;


	for (int j = 0; j < ARRAY_SIZE(y); j++){
		for (int i = 0; i < it; i++){
			if (toggle)
				test_plane(data, 0, pw-w, y[j], w, h, w, h, pw, ph, fbc);
			else
				test_plane(data, 1, 0, y[j], w, h, w, h, pw2, ph2, fbc);

			toggle = !toggle;
		}
	}

	return;

}

/*
 * Setup and runner for panning test. Creates common video sizes and pans them across the display
 */
static void test_display_mpo(data_t *data, enum test test, uint32_t format, int display_count)
{

	igt_display_t *display = &data->display;
	igt_output_t *output;
	uint32_t regamma_lut_size;
	lut_t lut;
	struct fbc fb[4];
	int valid_outputs = 0;
	int videos[][2]= {
		{426, 240},
		{640, 360},
		{854, 480},
		{1280, 720},
		{1920, 1080},
		{2560, 1440},
		{3840, 2160},
	};

	test_init(data);

	/* Skip if there is less valid outputs than the required. */
	for_each_connected_output(display, output)
		valid_outputs++;

	igt_skip_on_f(valid_outputs < display_count,
			"Valid outputs (%d) should be equal or greater than %d\n", valid_outputs, display_count);

	regamma_lut_size = igt_pipe_obj_get_prop(data->pipe[0], IGT_CRTC_GAMMA_LUT_SIZE);
	igt_assert_lt(0, regamma_lut_size);
	lut_init(&lut, regamma_lut_size);
	lut_gen(&lut);

	for (int n = 0; n < display_count;  n++) {
		int w = data->w[n];
		int h = data->h[n];

		if (w == 0) {
			force_output_mode(data, data->output[n], &test_mode_3);
			w = data->w[n] = test_mode_3.hdisplay;
			h = data->h[n] = test_mode_3.vdisplay;
		}

		igt_output_set_pipe(data->output[n], data->pipe_id[n]);

		igt_create_fb(data->fd, w, h, DRM_FORMAT_XRGB8888, 0, &fb[n].ref_primary);
		igt_create_color_fb(data->fd, w, h, DRM_FORMAT_XRGB8888, 0, 1.0, 1.0, 1.0, &fb[n].ref_primary);
		igt_create_fb(data->fd, w, h, DRM_FORMAT_ARGB8888, 0, &fb[n].test_overlay);

		igt_plane_set_fb(data->primary[n], &fb[n].ref_primary);

		if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_P010)
			set_regamma_lut(data, &lut,  n);
	}

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	for (int n = 0; n < display_count; n++)
		igt_pipe_crc_collect_crc(data->pipe_crc[n], &fb[n].ref_crc);

	for (int i = 0; i < ARRAY_SIZE(videos); ++i) {

		/* Video(mpo) should be in the middle when it transitions between displays. This
		 * means MPO plane will be w/2
		 */
		if (test == MPO_MULTI_PAN)
			videos[i][0] = videos[i][0]/2;

		for (int n = 0; n < display_count; n++)
			igt_create_color_fb(data->fd, videos[i][0], videos[i][1],
					    format, 0, 1.0, 1.0, 1.0, &fb[n].test_primary);

		if (test == MPO_SINGLE_PAN)
			test_panning_1_display(data, display_count, videos[i][0], videos[i][1], fb);
		if (test == MPO_MULTI_PAN)
			test_panning_2_display(data, videos[i][0], videos[i][1], fb);
		if(test == MPO_SCALE)
			test_scaling_planes(data, display_count, videos[i][0], videos[i][1], fb);

		for (int n = 0; n < display_count; n++)
			igt_remove_fb(data->fd, &fb[n].test_primary);
	}

	test_fini(data);

	lut_free(&lut);

	for (int n = 0; n < display_count; n++) {
		igt_remove_fb(data->fd, &fb[n].ref_primary);
		igt_remove_fb(data->fd, &fb[n].test_overlay);
	}
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
	int w, h;

	test_init(data);

	/* Skip if not 4K resolution. */
	igt_skip_on(!((data->mode[0].hdisplay == 4096
		      && data->mode[0].vdisplay == 2160)||
		      (data->mode[0].hdisplay == 3840
		      && data->mode[0].vdisplay == 2160)));

	w = data->w[0];
	h = data->h[0];
	cutout_x = cutout_w = 1280;
	cutout_y = cutout_h = 720;

	igt_create_color_fb(data->fd, w, h, DRM_FORMAT_XRGB8888, 0, 1.00, 1.00,
			    1.00, &r_fb);
	igt_create_color_fb(data->fd, w, h, DRM_FORMAT_XRGB8888, 0, 1.00, 1.00,
			    1.00, &p_fb);
	igt_create_fb(data->fd, w, h, DRM_FORMAT_ARGB8888, 0, &o_fb);
	draw_color_alpha(&o_fb, 0, 0, o_fb.width, o_fb.height, 1.00, 1.00, 1.00, 1.00);
	draw_color_alpha(&o_fb, cutout_x, cutout_y, cutout_w, cutout_h,
			 0.00, 0.00, 0.00, 0.00);

	igt_output_set_pipe(data->output[0], data->pipe_id[0]);
	igt_plane_set_fb(data->primary[0], &r_fb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_pipe_crc_collect_crc(data->pipe_crc[0], &ref_crc);

	igt_plane_set_fb(data->primary[0], &p_fb);
	igt_plane_set_fb(data->overlay[0], &o_fb);
	igt_display_commit_atomic(display, 0, NULL);

	igt_pipe_crc_collect_crc(data->pipe_crc[0], &new_crc);

	igt_assert_crc_equal(&ref_crc, &new_crc);

	test_fini(data);
	igt_remove_fb(data->fd, &o_fb);
	igt_remove_fb(data->fd, &p_fb);
	igt_remove_fb(data->fd, &r_fb);
}

static void test_mpo_swizzle_toggle_multihead(data_t *data)
{
	struct amdgpu_bo_metadata meta = {};
	igt_display_t *display = &data->display;
	igt_output_t *output;
	igt_fb_t fb_1280_xr24_tiled, fb_1280_ar24_tiled, fb_1920_xb24_tiled,
		fb_1920_xb24_linear, fb_1920_xr24_tiled;
	int w, h;
	int valid_outputs = 0;

	/* Skip if only one display is connected. */
	for_each_connected_output(display, output)
		valid_outputs++;

	igt_skip_on_f(valid_outputs == 1, "Must have more than one output connected\n");

	w = 2400;
	h = 1350;

	igt_create_pattern_fb(data->fd, 1280, 1024, DRM_FORMAT_XRGB8888, 0,
			      &fb_1280_xr24_tiled);
	igt_create_pattern_fb(data->fd, 1280, 1024, DRM_FORMAT_ARGB8888, 0,
			      &fb_1280_ar24_tiled);
	igt_create_pattern_fb(data->fd, 1920, 1080, DRM_FORMAT_XBGR8888, 0,
			      &fb_1920_xb24_tiled);
	igt_create_pattern_fb(data->fd, 1920, 1080, DRM_FORMAT_XBGR8888, 0,
			      &fb_1920_xb24_linear);
	igt_create_pattern_fb(data->fd, 1920, 1080, DRM_FORMAT_XRGB8888, 0,
			      &fb_1920_xr24_tiled);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1280_xr24_tiled, &meta);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1280_ar24_tiled, &meta);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1920_xb24_tiled, &meta);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1920_xr24_tiled, &meta);

	test_init(data);

	/* Initial modeset */
	igt_output_set_pipe(data->output[0], data->pipe_id[0]);
	igt_output_set_pipe(data->output[1], data->pipe_id[1]);
	force_output_mode(data, data->output[0], &test_mode_1);
	force_output_mode(data, data->output[1], &test_mode_2);

	igt_plane_set_fb(data->primary[0], &fb_1920_xr24_tiled);
	igt_plane_set_fb(data->primary[1], &fb_1920_xb24_linear);
	igt_plane_set_size(data->primary[1], w, h);

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	/* Enable overlay plane. */
	igt_plane_set_fb(data->overlay[1], &fb_1280_ar24_tiled);
	igt_plane_set_fb(data->primary[1], &fb_1920_xb24_linear);
	igt_plane_set_size(data->primary[1], w, h);
	igt_display_commit_atomic(display, 0, 0);

	/* Switch to tiled. */
	igt_plane_set_fb(data->overlay[1], &fb_1280_ar24_tiled);
	igt_plane_set_fb(data->primary[1], &fb_1920_xb24_tiled);
	igt_plane_set_size(data->primary[1], w, h);
	igt_display_commit_atomic(display, 0, 0);

	/* Switch to linear. */
	igt_plane_set_fb(data->overlay[1], &fb_1280_ar24_tiled);
	igt_plane_set_fb(data->primary[1], &fb_1920_xb24_linear);
	igt_plane_set_size(data->primary[1], w, h);
	igt_display_commit_atomic(display, 0, 0);

	test_fini(data);
	igt_remove_fb(data->fd, &fb_1280_xr24_tiled);
	igt_remove_fb(data->fd, &fb_1280_ar24_tiled);
	igt_remove_fb(data->fd, &fb_1920_xb24_tiled);
	igt_remove_fb(data->fd, &fb_1920_xb24_linear);
	igt_remove_fb(data->fd, &fb_1920_xr24_tiled);
}

static void test_mpo_swizzle_toggle(data_t *data)
{
	struct amdgpu_bo_metadata meta = {};
	igt_display_t *display = &data->display;
	igt_fb_t fb_1280_ar24_tiled, fb_1920_xb24_tiled, fb_1920_xb24_linear,
		 fb_1920_xr24_tiled;
	int w, h;

	w = 2400;
	h = 1350;

	igt_create_pattern_fb(data->fd, 1280, 1024, DRM_FORMAT_ARGB8888, 0,
			      &fb_1280_ar24_tiled);
	igt_create_pattern_fb(data->fd, 1920, 1080, DRM_FORMAT_XBGR8888, 0,
			      &fb_1920_xb24_tiled);
	igt_create_pattern_fb(data->fd, 1920, 1080, DRM_FORMAT_XBGR8888, 0,
			      &fb_1920_xb24_linear);
	igt_create_pattern_fb(data->fd, 1920, 1080, DRM_FORMAT_XRGB8888, 0,
			      &fb_1920_xr24_tiled);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1280_ar24_tiled, &meta);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1920_xb24_tiled, &meta);

	meta.tiling_info = AMDGPU_TILING_SET(SWIZZLE_MODE, 0x19);
	set_metadata(data, &fb_1920_xr24_tiled, &meta);

	test_init(data);

	/* Initial modeset */
	igt_output_set_pipe(data->output[0], data->pipe_id[0]);
	force_output_mode(data, data->output[0], &test_mode_1);

	igt_plane_set_fb(data->primary[0], &fb_1920_xb24_linear);
	igt_plane_set_size(data->primary[0], w, h);

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	/* Enable overlay plane. */
	igt_plane_set_fb(data->overlay[0], &fb_1280_ar24_tiled);
	igt_plane_set_fb(data->primary[0], &fb_1920_xb24_linear);
	igt_plane_set_size(data->primary[0], w, h);
	igt_display_commit_atomic(display, 0, 0);

	/* Switch to tiled. */
	igt_plane_set_fb(data->overlay[0], &fb_1280_ar24_tiled);
	igt_plane_set_fb(data->primary[0], &fb_1920_xb24_tiled);
	igt_plane_set_size(data->primary[0], w, h);
	igt_display_commit_atomic(display, 0, 0);

	/* Switch to linear. */
	igt_plane_set_fb(data->overlay[0], &fb_1280_ar24_tiled);
	igt_plane_set_fb(data->primary[0], &fb_1920_xb24_linear);
	igt_plane_set_size(data->primary[0], w, h);
	igt_display_commit_atomic(display, 0, 0);

	test_fini(data);
	igt_remove_fb(data->fd, &fb_1280_ar24_tiled);
	igt_remove_fb(data->fd, &fb_1920_xb24_tiled);
	igt_remove_fb(data->fd, &fb_1920_xb24_linear);
	igt_remove_fb(data->fd, &fb_1920_xr24_tiled);
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

	igt_describe("MPO with 4K planes");
	igt_subtest("test-mpo-4k") test_mpo_4k(&data);
	igt_describe("MPO with tiled and linear buffers");
	igt_subtest("mpo-swizzle-toggle") test_mpo_swizzle_toggle(&data);
	igt_describe("MPO with tiled and linear buffers on dual displays");
	igt_subtest("mpo-swizzle-toggle-multihead")
		test_mpo_swizzle_toggle_multihead(&data);

	igt_describe("MPO and moving RGB primary plane around");
	igt_subtest("mpo-pan-rgb")
		test_display_mpo(&data, MPO_SINGLE_PAN, DRM_FORMAT_XRGB8888, 1);
	igt_describe("MPO and moving RGB primary plane around with dual displays");
	igt_subtest("mpo-pan-rgb-multihead")
		test_display_mpo(&data, MPO_SINGLE_PAN, DRM_FORMAT_XRGB8888, DISPLAYS_TO_TEST);

	igt_describe("MPO and moving NV12 primary plane around");
	igt_subtest("mpo-pan-nv12")
		test_display_mpo(&data, MPO_SINGLE_PAN, DRM_FORMAT_NV12, 1);
	igt_describe("MPO and moving NV12 primary plane around with dual displays");
	igt_subtest("mpo-pan-nv12-multihead")
		test_display_mpo(&data, MPO_SINGLE_PAN, DRM_FORMAT_NV12, DISPLAYS_TO_TEST);

	igt_describe("MPO and moving P010 primary plane around");
	igt_subtest("mpo-pan-p010")
		test_display_mpo(&data, MPO_SINGLE_PAN, DRM_FORMAT_P010, 1);
	igt_describe("MPO and moving P010 primary plane around with dual displays");
	igt_subtest("mpo-pan-p010-multihead")
		test_display_mpo(&data, MPO_SINGLE_PAN, DRM_FORMAT_P010, DISPLAYS_TO_TEST);

	igt_describe("MPO and moving RGB primary plane between 2 displays");
	igt_subtest("mpo-pan-multi-rgb")
		test_display_mpo(&data, MPO_MULTI_PAN, DRM_FORMAT_XRGB8888, DISPLAYS_TO_TEST);
	igt_describe("MPO and moving NV12 primary plane between 2 displays");
	igt_subtest("mpo-pan-multi-nv12")
		test_display_mpo(&data, MPO_MULTI_PAN, DRM_FORMAT_NV12, DISPLAYS_TO_TEST);
	igt_describe("MPO and moving P010 primary plane between 2 displays");
	igt_subtest("mpo-pan-multi-p010")
		test_display_mpo(&data, MPO_MULTI_PAN, DRM_FORMAT_P010, DISPLAYS_TO_TEST);

	igt_describe("MPO and scaling RGB primary plane");
	igt_subtest("mpo-scale-rgb")
		test_display_mpo(&data, MPO_SCALE, DRM_FORMAT_XRGB8888, 1);
	igt_describe("MPO and scaling RGB primary plane with 2 displays");
	igt_subtest("mpo-scale-rgb-multihead")
		test_display_mpo(&data, MPO_SCALE, DRM_FORMAT_XRGB8888, DISPLAYS_TO_TEST);
	igt_describe("MPO and scaling NV12 primary plane");
	igt_subtest("mpo-scale-nv12")
		test_display_mpo(&data, MPO_SCALE, DRM_FORMAT_NV12, 1);
	igt_describe("MPO and scaling NV12 primary plane with 2 displays");
	igt_subtest("mpo-scale-nv12-multihead") test_display_mpo(&data, MPO_SCALE, DRM_FORMAT_NV12, DISPLAYS_TO_TEST);
	igt_describe("MPO and scaling P010 primary plane");
	igt_subtest("mpo-scale-p010")
		test_display_mpo(&data, MPO_SCALE, DRM_FORMAT_P010, 1);
	igt_describe("MPO and scaling P010 primary plane with 2 displays");
	igt_subtest("mpo-scale-p010-multihead")
		test_display_mpo(&data, MPO_SCALE, DRM_FORMAT_P010, DISPLAYS_TO_TEST);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
