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

/* Maximum pipes on any AMD ASIC. */
#define MAX_PIPES 6

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

static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	int i, n, max_pipes = display->n_pipes;

	for (i = 0; i < max_pipes; ++i) {
		data->pipe_id[i] = PIPE_A + i;
		data->pipe[i] = &data->display.pipes[data->pipe_id[i]];
		data->primary[i] = igt_pipe_get_plane_type(
			data->pipe[i], DRM_PLANE_TYPE_PRIMARY);
		data->overlay[i] = igt_pipe_get_plane_type_index(
			data->pipe[i], DRM_PLANE_TYPE_OVERLAY, 0);
		data->cursor[i] = igt_pipe_get_plane_type(
			data->pipe[i], DRM_PLANE_TYPE_CURSOR);
		data->pipe_crc[i] =
			igt_pipe_crc_new(data->fd, data->pipe_id[i], "auto");
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
	int i, max_pipes = display->n_pipes;

	for (i = 0; i < max_pipes; ++i) {
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
	int w, h;

	test_init(data);

	/* Skip if not 4K resolution. */
	igt_skip_on(!(data->mode[0].hdisplay == 3840
		      && data->mode[0].vdisplay == 2160));

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

static void test_mpo_swizzle_toggle(data_t *data)
{
	struct amdgpu_bo_metadata meta = {};
	igt_display_t *display = &data->display;
	igt_fb_t fb_1280_xr24_tiled, fb_1280_ar24_tiled, fb_1920_xb24_tiled,
		fb_1920_xb24_linear, fb_1920_xr24_tiled;
	int w, h;

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
	igt_subtest("mpo-swizzle-toggle") test_mpo_swizzle_toggle(&data);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
