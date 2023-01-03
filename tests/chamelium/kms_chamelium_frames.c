/*
 * Copyright © 2016 Red Hat Inc.
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
 *    Lyude Paul <lyude@redhat.com>
 */

#include "igt_eld.h"
#include "igt_infoframe.h"
#include "kms_chamelium_helper.h"

#define connector_dynamic_subtest(name__, type__)                   \
	igt_subtest_with_dynamic(name__)                            \
	for_each_port(p, port) if (chamelium_port_get_type(port) == \
				   DRM_MODE_CONNECTOR_##type__)

struct vic_mode {
	int hactive, vactive;
	int vrefresh; /* Hz */
	uint32_t picture_ar;
};

static int chamelium_vga_modes[][2] = {
	{ 1600, 1200 }, { 1920, 1200 }, { 1920, 1080 }, { 1680, 1050 },
	{ 1280, 1024 }, { 1280, 960 },	{ 1440, 900 },	{ 1280, 800 },
	{ 1024, 768 },	{ 1360, 768 },	{ 1280, 720 },	{ 800, 600 },
	{ 640, 480 },	{ -1, -1 },
};

/* Maps Video Identification Codes to a mode */
static const struct vic_mode vic_modes[] = {
	[16] = {
		.hactive = 1920,
		.vactive = 1080,
		.vrefresh = 60,
		.picture_ar = DRM_MODE_PICTURE_ASPECT_16_9,
	},
};

/* Maps aspect ratios to their mode flag */
static const uint32_t mode_ar_flags[] = {
	[DRM_MODE_PICTURE_ASPECT_16_9] = DRM_MODE_FLAG_PIC_AR_16_9,
};

static bool prune_vga_mode(chamelium_data_t *data, drmModeModeInfo *mode)
{
	int i = 0;

	while (chamelium_vga_modes[i][0] != -1) {
		if (mode->hdisplay == chamelium_vga_modes[i][0] &&
		    mode->vdisplay == chamelium_vga_modes[i][1])
			return false;

		i++;
	}

	return true;
}

static void do_test_display(chamelium_data_t *data, struct chamelium_port *port,
			    igt_output_t *output, drmModeModeInfo *mode,
			    uint32_t fourcc, enum chamelium_check check,
			    int count)
{
	struct chamelium_fb_crc_async_data *fb_crc;
	struct igt_fb frame_fb, fb;
	int i, fb_id, captured_frame_count;
	int frame_id;

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, &fb);
	igt_assert(fb_id > 0);

	frame_id =
		igt_fb_convert(&frame_fb, &fb, fourcc, DRM_FORMAT_MOD_LINEAR);
	igt_assert(frame_id > 0);

	if (check == CHAMELIUM_CHECK_CRC)
		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&fb);

	chamelium_enable_output(data, port, output, mode, &frame_fb);

	if (check == CHAMELIUM_CHECK_CRC) {
		igt_crc_t *expected_crc;
		igt_crc_t *crc;

		/* We want to keep the display running for a little bit, since
		 * there's always the potential the driver isn't able to keep
		 * the display running properly for very long
		 */
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, count);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_assert(captured_frame_count == count);

		igt_debug("Captured %d frames\n", captured_frame_count);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		for (i = 0; i < captured_frame_count; i++)
			chamelium_assert_crc_eq_or_dump(
				data->chamelium, expected_crc, &crc[i], &fb, i);

		free(expected_crc);
		free(crc);
	} else if (check == CHAMELIUM_CHECK_ANALOG ||
		   check == CHAMELIUM_CHECK_CHECKERBOARD) {
		struct chamelium_frame_dump *dump;

		igt_assert(count == 1);

		dump = chamelium_port_dump_pixels(data->chamelium, port, 0, 0,
						  0, 0);

		if (check == CHAMELIUM_CHECK_ANALOG)
			chamelium_crop_analog_frame(dump, mode->hdisplay,
						    mode->vdisplay);

		chamelium_assert_frame_match_or_dump(data->chamelium, port,
						     dump, &fb, check);
		chamelium_destroy_frame_dump(dump);
	}

	igt_remove_fb(data->drm_fd, &frame_fb);
	igt_remove_fb(data->drm_fd, &fb);
}

static enum infoframe_avi_picture_aspect_ratio
get_infoframe_avi_picture_ar(uint32_t aspect_ratio)
{
	/* The AVI picture aspect ratio field only supports 4:3 and 16:9 */
	switch (aspect_ratio) {
	case DRM_MODE_PICTURE_ASPECT_4_3:
		return INFOFRAME_AVI_PIC_AR_4_3;
	case DRM_MODE_PICTURE_ASPECT_16_9:
		return INFOFRAME_AVI_PIC_AR_16_9;
	default:
		return INFOFRAME_AVI_PIC_AR_UNSPECIFIED;
	}
}

static bool vic_mode_matches_drm(const struct vic_mode *vic_mode,
				 drmModeModeInfo *drm_mode)
{
	uint32_t ar_flag = mode_ar_flags[vic_mode->picture_ar];

	return vic_mode->hactive == drm_mode->hdisplay &&
	       vic_mode->vactive == drm_mode->vdisplay &&
	       vic_mode->vrefresh == drm_mode->vrefresh &&
	       ar_flag == (drm_mode->flags & DRM_MODE_FLAG_PIC_AR_MASK);
}

static void randomize_plane_stride(chamelium_data_t *data, uint32_t width,
				   uint32_t height, uint32_t format,
				   uint64_t modifier, size_t *stride)
{
	size_t stride_min;
	uint32_t max_tile_w = 4, tile_w, tile_h;
	int i;
	struct igt_fb dummy;

	stride_min = width * igt_format_plane_bpp(format, 0) / 8;

	/* Randomize the stride to less than twice the minimum. */
	*stride = (rand() % stride_min) + stride_min;

	/*
	 * Create a dummy FB to determine bpp for each plane, and calculate
	 * the maximum tile width from that.
	 */
	igt_create_fb(data->drm_fd, 64, 64, format, modifier, &dummy);
	for (i = 0; i < dummy.num_planes; i++) {
		igt_get_fb_tile_size(data->drm_fd, modifier, dummy.plane_bpp[i],
				     &tile_w, &tile_h);

		if (tile_w > max_tile_w)
			max_tile_w = tile_w;
	}
	igt_remove_fb(data->drm_fd, &dummy);

	/*
	 * Pixman requires the stride to be aligned to 32-bits, which is
	 * reflected in the initial value of max_tile_w and the hw
	 * may require a multiple of tile width, choose biggest of the 2.
	 */
	*stride = ALIGN(*stride, max_tile_w);
}

static void update_tiled_modifier(igt_plane_t *plane, uint32_t width,
				  uint32_t height, uint32_t format,
				  uint64_t *modifier)
{
	if (*modifier == DRM_FORMAT_MOD_BROADCOM_SAND256) {
		/* Randomize the column height to less than twice the minimum.
		 */
		size_t column_height = (rand() % height) + height;

		igt_debug(
			"Selecting VC4 SAND256 tiling with column height %ld\n",
			column_height);

		*modifier = DRM_FORMAT_MOD_BROADCOM_SAND256_COL_HEIGHT(
			column_height);
	}
}

static void randomize_plane_setup(chamelium_data_t *data, igt_plane_t *plane,
				  drmModeModeInfo *mode, uint32_t *width,
				  uint32_t *height, uint32_t *format,
				  uint64_t *modifier, bool allow_yuv)
{
	int min_dim;
	uint32_t idx[plane->format_mod_count];
	unsigned int count = 0;
	unsigned int i;

	/* First pass to count the supported formats. */
	for (i = 0; i < plane->format_mod_count; i++)
		if (igt_fb_supported_format(plane->formats[i]) &&
		    (allow_yuv || !igt_format_is_yuv(plane->formats[i])))
			idx[count++] = i;

	igt_assert(count > 0);

	i = idx[rand() % count];
	*format = plane->formats[i];
	*modifier = plane->modifiers[i];

	update_tiled_modifier(plane, *width, *height, *format, modifier);

	/*
	 * Randomize width and height in the mode dimensions range.
	 *
	 * Restrict to a min of 2 * min_dim, this way src_w/h are always at
	 * least min_dim, because src_w = width - (rand % w / 2).
	 *
	 * Use a minimum dimension of 16 for YUV, because planar YUV
	 * subsamples the UV plane.
	 */
	min_dim = igt_format_is_yuv(*format) ? 16 : 8;

	*width = max((rand() % mode->hdisplay) + 1, 2 * min_dim);
	*height = max((rand() % mode->vdisplay) + 1, 2 * min_dim);
}

static void configure_plane(igt_plane_t *plane, uint32_t src_w, uint32_t src_h,
			    uint32_t src_x, uint32_t src_y, uint32_t crtc_w,
			    uint32_t crtc_h, int32_t crtc_x, int32_t crtc_y,
			    struct igt_fb *fb)
{
	igt_plane_set_fb(plane, fb);

	igt_plane_set_position(plane, crtc_x, crtc_y);
	igt_plane_set_size(plane, crtc_w, crtc_h);

	igt_fb_set_position(fb, plane, src_x, src_y);
	igt_fb_set_size(fb, plane, src_w, src_h);
}

static void randomize_plane_coordinates(
	chamelium_data_t *data, igt_plane_t *plane, drmModeModeInfo *mode,
	struct igt_fb *fb, uint32_t *src_w, uint32_t *src_h, uint32_t *src_x,
	uint32_t *src_y, uint32_t *crtc_w, uint32_t *crtc_h, int32_t *crtc_x,
	int32_t *crtc_y, bool allow_scaling)
{
	bool is_yuv = igt_format_is_yuv(fb->drm_format);
	uint32_t width = fb->width, height = fb->height;
	double ratio;
	int ret;

	/* Randomize source offset in the first half of the original size. */
	*src_x = rand() % (width / 2);
	*src_y = rand() % (height / 2);

	/* The source size only includes the active source area. */
	*src_w = width - *src_x;
	*src_h = height - *src_y;

	if (allow_scaling) {
		*crtc_w = (rand() % mode->hdisplay) + 1;
		*crtc_h = (rand() % mode->vdisplay) + 1;

		/*
		 * Don't bother with scaling if dimensions are quite close in
		 * order to get non-scaling cases more frequently. Also limit
		 * scaling to 3x to avoid aggressive filtering that makes
		 * comparison less reliable, and don't go above 2x downsampling
		 * to avoid possible hw limitations.
		 */

		ratio = ((double)*crtc_w / *src_w);
		if (ratio < 0.5)
			*src_w = *crtc_w * 2;
		else if (ratio > 0.8 && ratio < 1.2)
			*crtc_w = *src_w;
		else if (ratio > 3.0)
			*crtc_w = *src_w * 3;

		ratio = ((double)*crtc_h / *src_h);
		if (ratio < 0.5)
			*src_h = *crtc_h * 2;
		else if (ratio > 0.8 && ratio < 1.2)
			*crtc_h = *src_h;
		else if (ratio > 3.0)
			*crtc_h = *src_h * 3;
	} else {
		*crtc_w = *src_w;
		*crtc_h = *src_h;
	}

	if (*crtc_w != *src_w || *crtc_h != *src_h) {
		/*
		 * When scaling is involved, make sure to not go off-bounds or
		 * scaled clipping may result in decimal dimensions, that most
		 * drivers don't support.
		 */
		if (*crtc_w < mode->hdisplay)
			*crtc_x = rand() % (mode->hdisplay - *crtc_w);
		else
			*crtc_x = 0;

		if (*crtc_h < mode->vdisplay)
			*crtc_y = rand() % (mode->vdisplay - *crtc_h);
		else
			*crtc_y = 0;
	} else {
		/*
		 * Randomize the on-crtc position and allow the plane to go
		 * off-display by less than half of its on-crtc dimensions.
		 */
		*crtc_x = (rand() % mode->hdisplay) - *crtc_w / 2;
		*crtc_y = (rand() % mode->vdisplay) - *crtc_h / 2;
	}

	configure_plane(plane, *src_w, *src_h, *src_x, *src_y, *crtc_w, *crtc_h,
			*crtc_x, *crtc_y, fb);
	ret = igt_display_try_commit_atomic(
		&data->display,
		DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
		NULL);
	if (!ret)
		return;

	/* Coordinates are logged in the dumped debug log, so only report w/h on
	 * failure here. */
	igt_assert_f(ret != -ENOSPC,
		     "Failure in testcase, invalid coordinates on a %ux%u fb\n",
		     width, height);

	/* Make YUV coordinates a multiple of 2 and retry the math. */
	if (is_yuv) {
		*src_x &= ~1;
		*src_y &= ~1;
		*src_w &= ~1;
		*src_h &= ~1;
		/* To handle 1:1 scaling, clear crtc_w/h too. */
		*crtc_w &= ~1;
		*crtc_h &= ~1;

		if (*crtc_x < 0 && (*crtc_x & 1))
			(*crtc_x)++;
		else
			*crtc_x &= ~1;

		/* If negative, round up to 0 instead of down */
		if (*crtc_y < 0 && (*crtc_y & 1))
			(*crtc_y)++;
		else
			*crtc_y &= ~1;

		configure_plane(plane, *src_w, *src_h, *src_x, *src_y, *crtc_w,
				*crtc_h, *crtc_x, *crtc_y, fb);
		ret = igt_display_try_commit_atomic(
			&data->display,
			DRM_MODE_ATOMIC_TEST_ONLY |
				DRM_MODE_ATOMIC_ALLOW_MODESET,
			NULL);
		if (!ret)
			return;
	}

	igt_assert(!ret || allow_scaling);
	igt_info("Scaling ratio %g / %g failed, trying without scaling.\n",
		 ((double)*crtc_w / *src_w), ((double)*crtc_h / *src_h));

	*crtc_w = *src_w;
	*crtc_h = *src_h;

	configure_plane(plane, *src_w, *src_h, *src_x, *src_y, *crtc_w, *crtc_h,
			*crtc_x, *crtc_y, fb);
	igt_display_commit_atomic(&data->display,
				  DRM_MODE_ATOMIC_TEST_ONLY |
					  DRM_MODE_ATOMIC_ALLOW_MODESET,
				  NULL);
}

static void blit_plane_cairo(chamelium_data_t *data, cairo_surface_t *result,
			     uint32_t src_w, uint32_t src_h, uint32_t src_x,
			     uint32_t src_y, uint32_t crtc_w, uint32_t crtc_h,
			     int32_t crtc_x, int32_t crtc_y, struct igt_fb *fb)
{
	cairo_surface_t *surface;
	cairo_surface_t *clipped_surface;
	cairo_t *cr;

	surface = igt_get_cairo_surface(data->drm_fd, fb);

	if (src_x || src_y) {
		clipped_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
							     src_w, src_h);

		cr = cairo_create(clipped_surface);

		cairo_translate(cr, -1. * src_x, -1. * src_y);

		cairo_set_source_surface(cr, surface, 0, 0);

		cairo_paint(cr);
		cairo_surface_flush(clipped_surface);

		cairo_destroy(cr);
	} else {
		clipped_surface = surface;
	}

	cr = cairo_create(result);

	cairo_translate(cr, crtc_x, crtc_y);

	if (src_w != crtc_w || src_h != crtc_h) {
		cairo_scale(cr, (double)crtc_w / src_w, (double)crtc_h / src_h);
	}

	cairo_set_source_surface(cr, clipped_surface, 0, 0);
	cairo_surface_destroy(clipped_surface);

	if (src_w != crtc_w || src_h != crtc_h) {
		cairo_pattern_set_filter(cairo_get_source(cr),
					 CAIRO_FILTER_BILINEAR);
		cairo_pattern_set_extend(cairo_get_source(cr),
					 CAIRO_EXTEND_NONE);
	}

	cairo_paint(cr);
	cairo_surface_flush(result);

	cairo_destroy(cr);
}

static void prepare_randomized_plane(chamelium_data_t *data,
				     drmModeModeInfo *mode, igt_plane_t *plane,
				     struct igt_fb *overlay_fb,
				     unsigned int index,
				     cairo_surface_t *result_surface,
				     bool allow_scaling, bool allow_yuv)
{
	struct igt_fb pattern_fb;
	uint32_t overlay_fb_w, overlay_fb_h;
	uint32_t overlay_src_w, overlay_src_h;
	uint32_t overlay_src_x, overlay_src_y;
	int32_t overlay_crtc_x, overlay_crtc_y;
	uint32_t overlay_crtc_w, overlay_crtc_h;
	uint32_t format;
	uint64_t modifier;
	size_t stride;
	bool tiled;
	int fb_id;

	randomize_plane_setup(data, plane, mode, &overlay_fb_w, &overlay_fb_h,
			      &format, &modifier, allow_yuv);

	tiled = (modifier != DRM_FORMAT_MOD_LINEAR);
	igt_debug("Plane %d: framebuffer size %dx%d %s format (%s)\n", index,
		  overlay_fb_w, overlay_fb_h, igt_format_str(format),
		  tiled ? "tiled" : "linear");

	/* Get a pattern framebuffer for the overlay plane. */
	fb_id = chamelium_get_pattern_fb(data, overlay_fb_w, overlay_fb_h,
					 DRM_FORMAT_XRGB8888, 32, &pattern_fb);
	igt_assert(fb_id > 0);

	randomize_plane_stride(data, overlay_fb_w, overlay_fb_h, format,
			       modifier, &stride);

	igt_debug("Plane %d: stride %ld\n", index, stride);

	fb_id = igt_fb_convert_with_stride(overlay_fb, &pattern_fb, format,
					   modifier, stride);
	igt_assert(fb_id > 0);

	randomize_plane_coordinates(data, plane, mode, overlay_fb,
				    &overlay_src_w, &overlay_src_h,
				    &overlay_src_x, &overlay_src_y,
				    &overlay_crtc_w, &overlay_crtc_h,
				    &overlay_crtc_x, &overlay_crtc_y,
				    allow_scaling);

	igt_debug("Plane %d: in-framebuffer size %dx%d\n", index, overlay_src_w,
		  overlay_src_h);
	igt_debug("Plane %d: in-framebuffer position %dx%d\n", index,
		  overlay_src_x, overlay_src_y);
	igt_debug("Plane %d: on-crtc size %dx%d\n", index, overlay_crtc_w,
		  overlay_crtc_h);
	igt_debug("Plane %d: on-crtc position %dx%d\n", index, overlay_crtc_x,
		  overlay_crtc_y);

	blit_plane_cairo(data, result_surface, overlay_src_w, overlay_src_h,
			 overlay_src_x, overlay_src_y, overlay_crtc_w,
			 overlay_crtc_h, overlay_crtc_x, overlay_crtc_y,
			 &pattern_fb);

	/* Remove the original pattern framebuffer. */
	igt_remove_fb(data->drm_fd, &pattern_fb);
}

static const char test_display_one_mode_desc[] =
	"Pick the first mode of the IGT base EDID, display and capture a few "
	"frames, then check captured frames are correct";
static void test_display_one_mode(chamelium_data_t *data,
				  struct chamelium_port *port, uint32_t fourcc,
				  enum chamelium_check check, int count)
{
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	igt_output_t *output;
	igt_plane_t *primary;

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	output = chamelium_prepare_output(data, port, IGT_CUSTOM_EDID_BASE);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_require(igt_plane_has_format_mod(primary, fourcc,
					     DRM_FORMAT_MOD_LINEAR));

	mode = &connector->modes[0];
	if (check == CHAMELIUM_CHECK_ANALOG) {
		bool bridge = chamelium_check_analog_bridge(data, port);

		igt_assert(!(bridge && prune_vga_mode(data, mode)));
	}

	do_test_display(data, port, output, mode, fourcc, check, count);

	drmModeFreeConnector(connector);
}

static const char test_display_all_modes_desc[] =
	"For each mode of the IGT base EDID, display and capture a few "
	"frames, then check captured frames are correct";
static void test_display_all_modes(chamelium_data_t *data,
				   struct chamelium_port *port, uint32_t fourcc,
				   enum chamelium_check check, int count)
{
	bool bridge;
	int i, count_modes;

	if (check == CHAMELIUM_CHECK_ANALOG)
		bridge = chamelium_check_analog_bridge(data, port);

	i = 0;
	do {
		igt_output_t *output;
		igt_plane_t *primary;
		drmModeConnector *connector;
		drmModeModeInfo *mode;

		/*
		 * let's reset state each mode so we will get the
		 * HPD pulses realibably
		 */
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_reset_state(&data->display, data->chamelium, port,
				      data->ports, data->port_count);

		/*
		 * modes may change due to mode pruining and link issues, so we
		 * need to refresh the connector
		 */
		output = chamelium_prepare_output(data, port,
						  IGT_CUSTOM_EDID_BASE);
		connector = chamelium_port_get_connector(data->chamelium, port,
							 false);
		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_assert(primary);
		igt_require(igt_plane_has_format_mod(primary, fourcc,
						     DRM_FORMAT_MOD_LINEAR));

		/* we may skip some modes due to above but that's ok */
		count_modes = connector->count_modes;
		if (i >= count_modes)
			break;

		mode = &connector->modes[i];

		if (check == CHAMELIUM_CHECK_ANALOG && bridge &&
		    prune_vga_mode(data, mode))
			continue;

		do_test_display(data, port, output, mode, fourcc, check, count);
		drmModeFreeConnector(connector);
	} while (++i < count_modes);
}

static const char test_display_frame_dump_desc[] =
	"For each mode of the IGT base EDID, display and capture a few "
	"frames, then download the captured frames and compare them "
	"bit-by-bit to the sent ones";
static void test_display_frame_dump(chamelium_data_t *data,
				    struct chamelium_port *port)
{
	int i, count_modes;

	i = 0;
	do {
		igt_output_t *output;
		igt_plane_t *primary;
		struct igt_fb fb;
		struct chamelium_frame_dump *frame;
		drmModeModeInfo *mode;
		drmModeConnector *connector;
		int fb_id, j;

		/*
		 * let's reset state each mode so we will get the
		 * HPD pulses realibably
		 */
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_reset_state(&data->display, data->chamelium, port,
				      data->ports, data->port_count);

		/*
		 * modes may change due to mode pruining and link issues, so we
		 * need to refresh the connector
		 */
		output = chamelium_prepare_output(data, port,
						  IGT_CUSTOM_EDID_BASE);
		connector = chamelium_port_get_connector(data->chamelium, port,
							 false);
		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_assert(primary);

		/* we may skip some modes due to above but that's ok */
		count_modes = connector->count_modes;
		if (i >= count_modes)
			break;

		mode = &connector->modes[i];

		fb_id = igt_create_color_pattern_fb(
			data->drm_fd, mode->hdisplay, mode->vdisplay,
			DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 0,
			&fb);
		igt_assert(fb_id > 0);

		chamelium_enable_output(data, port, output, mode, &fb);

		igt_debug("Reading frame dumps from Chamelium...\n");
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 5);
		for (j = 0; j < 5; j++) {
			frame = chamelium_read_captured_frame(data->chamelium,
							      j);
			chamelium_assert_frame_eq(data->chamelium, frame, &fb);
			chamelium_destroy_frame_dump(frame);
		}

		igt_remove_fb(data->drm_fd, &fb);
		drmModeFreeConnector(connector);
	} while (++i < count_modes);
}

static const char test_display_aspect_ratio_desc[] =
	"Pick a mode with a picture aspect-ratio, capture AVI InfoFrames and "
	"check they include the relevant fields";
static void test_display_aspect_ratio(chamelium_data_t *data,
				      struct chamelium_port *port)
{
	igt_output_t *output;
	igt_plane_t *primary;
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	int fb_id, i;
	struct igt_fb fb;
	bool found, ok;
	struct chamelium_infoframe *infoframe;
	struct infoframe_avi infoframe_avi;
	uint8_t vic = 16; /* TODO: test more VICs */
	const struct vic_mode *vic_mode;
	uint32_t aspect_ratio;
	enum infoframe_avi_picture_aspect_ratio frame_ar;

	igt_require(chamelium_supports_get_last_infoframe(data->chamelium));

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	output = chamelium_prepare_output(data, port,
					  IGT_CUSTOM_EDID_ASPECT_RATIO);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	vic_mode = &vic_modes[vic];
	aspect_ratio = vic_mode->picture_ar;

	found = false;
	igt_assert(connector->count_modes > 0);
	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];

		if (vic_mode_matches_drm(vic_mode, mode)) {
			found = true;
			break;
		}
	}
	igt_assert_f(found,
		     "Failed to find mode with the correct aspect ratio\n");

	fb_id = igt_create_color_pattern_fb(data->drm_fd, mode->hdisplay,
					    mode->vdisplay, DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR, 0, 0, 0,
					    &fb);
	igt_assert(fb_id > 0);

	chamelium_enable_output(data, port, output, mode, &fb);

	infoframe = chamelium_get_last_infoframe(data->chamelium, port,
						 CHAMELIUM_INFOFRAME_AVI);
	igt_assert_f(infoframe, "AVI InfoFrame not received\n");

	ok = infoframe_avi_parse(&infoframe_avi, infoframe->version,
				 infoframe->payload, infoframe->payload_size);
	igt_assert_f(ok, "Failed to parse AVI InfoFrame\n");

	frame_ar = get_infoframe_avi_picture_ar(aspect_ratio);

	igt_debug("Checking AVI InfoFrame\n");
	igt_debug("Picture aspect ratio: got %d, expected %d\n",
		  infoframe_avi.picture_aspect_ratio, frame_ar);
	igt_debug("Video Identification Code (VIC): got %d, expected %d\n",
		  infoframe_avi.vic, vic);

	igt_assert(infoframe_avi.picture_aspect_ratio == frame_ar);
	igt_assert(infoframe_avi.vic == vic);

	chamelium_infoframe_destroy(infoframe);
	igt_remove_fb(data->drm_fd, &fb);
	drmModeFreeConnector(connector);
}

static const char test_display_planes_random_desc[] =
	"Setup a few overlay planes with random parameters, capture the frame "
	"and check it matches the expected output";
static void test_display_planes_random(chamelium_data_t *data,
				       struct chamelium_port *port,
				       enum chamelium_check check)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	igt_plane_t *primary_plane;
	struct igt_fb primary_fb;
	struct igt_fb result_fb;
	struct igt_fb *overlay_fbs;
	igt_crc_t *crc;
	igt_crc_t *expected_crc;
	struct chamelium_fb_crc_async_data *fb_crc;
	unsigned int overlay_planes_max = 0;
	unsigned int overlay_planes_count;
	cairo_surface_t *result_surface;
	int captured_frame_count;
	bool allow_scaling;
	bool allow_yuv;
	unsigned int i;
	unsigned int fb_id;

	switch (check) {
	case CHAMELIUM_CHECK_CRC:
		allow_scaling = false;
		allow_yuv = false;
		break;
	case CHAMELIUM_CHECK_CHECKERBOARD:
		allow_scaling = true;
		allow_yuv = true;
		break;
	default:
		igt_assert(false);
	}

	srand(time(NULL));

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	/* Find the connector and pipe. */
	output = chamelium_prepare_output(data, port, IGT_CUSTOM_EDID_BASE);

	mode = igt_output_get_mode(output);

	/* Get a framebuffer for the primary plane. */
	primary_plane =
		igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary_plane);

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, &primary_fb);
	igt_assert(fb_id > 0);

	/* Get a framebuffer for the cairo composition result. */
	fb_id = igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &result_fb);
	igt_assert(fb_id > 0);

	result_surface = igt_get_cairo_surface(data->drm_fd, &result_fb);

	/* Paint the primary framebuffer on the result surface. */
	blit_plane_cairo(data, result_surface, 0, 0, 0, 0, 0, 0, 0, 0,
			 &primary_fb);

	/* Configure the primary plane. */
	igt_plane_set_fb(primary_plane, &primary_fb);

	overlay_planes_max =
		igt_output_count_plane_type(output, DRM_PLANE_TYPE_OVERLAY);

	/* Limit the number of planes to a reasonable scene. */
	overlay_planes_max = min(overlay_planes_max, 4u);

	overlay_planes_count = (rand() % overlay_planes_max) + 1;
	igt_debug("Using %d overlay planes\n", overlay_planes_count);

	overlay_fbs = calloc(sizeof(struct igt_fb), overlay_planes_count);

	for (i = 0; i < overlay_planes_count; i++) {
		struct igt_fb *overlay_fb = &overlay_fbs[i];
		igt_plane_t *plane = igt_output_get_plane_type_index(
			output, DRM_PLANE_TYPE_OVERLAY, i);
		igt_assert(plane);

		prepare_randomized_plane(data, mode, plane, overlay_fb, i,
					 result_surface, allow_scaling,
					 allow_yuv);
	}

	cairo_surface_destroy(result_surface);

	if (check == CHAMELIUM_CHECK_CRC)
		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&result_fb);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (check == CHAMELIUM_CHECK_CRC) {
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_assert(captured_frame_count == 1);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		chamelium_assert_crc_eq_or_dump(data->chamelium, expected_crc,
						crc, &result_fb, 0);

		free(expected_crc);
		free(crc);
	} else if (check == CHAMELIUM_CHECK_CHECKERBOARD) {
		struct chamelium_frame_dump *dump;

		dump = chamelium_port_dump_pixels(data->chamelium, port, 0, 0,
						  0, 0);
		chamelium_assert_frame_match_or_dump(data->chamelium, port,
						     dump, &result_fb, check);
		chamelium_destroy_frame_dump(dump);
	}

	for (i = 0; i < overlay_planes_count; i++)
		igt_remove_fb(data->drm_fd, &overlay_fbs[i]);

	free(overlay_fbs);

	igt_remove_fb(data->drm_fd, &primary_fb);
	igt_remove_fb(data->drm_fd, &result_fb);
}

IGT_TEST_DESCRIPTION("Tests requiring a Chamelium board");
igt_main
{
	chamelium_data_t data;
	struct chamelium_port *port;
	int p;

	igt_fixture {
		chamelium_init_test(&data);
	}

	igt_describe("DisplayPort tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_DisplayPort,
				data.port_count, 1);
		}

		igt_describe(test_display_all_modes_desc);
		connector_subtest("dp-crc-single", DisplayPort)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_one_mode_desc);
		connector_subtest("dp-crc-fast", DisplayPort)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("dp-crc-multiple", DisplayPort)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 3);

		igt_describe(test_display_frame_dump_desc);
		connector_subtest("dp-frame-dump", DisplayPort)
			test_display_frame_dump(&data, port);
	}

	igt_describe("HDMI tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_HDMIA,
				data.port_count, 1);
		}

		igt_describe(test_display_all_modes_desc);
		connector_subtest("hdmi-crc-single", HDMIA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_one_mode_desc);
		connector_subtest("hdmi-crc-fast", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		igt_describe(test_display_all_modes_desc);
		connector_subtest("hdmi-crc-multiple", HDMIA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 3);

		igt_describe(test_display_one_mode_desc);
		connector_dynamic_subtest("hdmi-crc-nonplanar-formats", HDMIA)
		{
			int k;
			igt_output_t *output;
			igt_plane_t *primary;

			output = chamelium_prepare_output(&data, port,
							  IGT_CUSTOM_EDID_BASE);
			primary = igt_output_get_plane_type(
				output, DRM_PLANE_TYPE_PRIMARY);
			igt_assert(primary);

			for (k = 0; k < primary->format_mod_count; k++) {
				if (!igt_fb_supported_format(
					    primary->formats[k]))
					continue;

				if (igt_format_is_yuv(primary->formats[k]))
					continue;

				if (primary->modifiers[k] !=
				    DRM_FORMAT_MOD_LINEAR)
					continue;

				igt_dynamic_f(
					"%s",
					igt_format_str(primary->formats[k]))
					test_display_one_mode(
						&data, port,
						primary->formats[k],
						CHAMELIUM_CHECK_CRC, 1);
			}
		}

		igt_describe(test_display_planes_random_desc);
		connector_subtest("hdmi-crc-planes-random", HDMIA)
			test_display_planes_random(&data, port,
						   CHAMELIUM_CHECK_CRC);

		igt_describe(test_display_one_mode_desc);
		connector_dynamic_subtest("hdmi-cmp-planar-formats", HDMIA)
		{
			int k;
			igt_output_t *output;
			igt_plane_t *primary;

			output = chamelium_prepare_output(&data, port,
							  IGT_CUSTOM_EDID_BASE);
			primary = igt_output_get_plane_type(
				output, DRM_PLANE_TYPE_PRIMARY);
			igt_assert(primary);

			for (k = 0; k < primary->format_mod_count; k++) {
				if (!igt_fb_supported_format(
					    primary->formats[k]))
					continue;

				if (!igt_format_is_yuv(primary->formats[k]))
					continue;

				if (primary->modifiers[k] !=
				    DRM_FORMAT_MOD_LINEAR)
					continue;

				igt_dynamic_f(
					"%s",
					igt_format_str(primary->formats[k]))
					test_display_one_mode(
						&data, port,
						primary->formats[k],
						CHAMELIUM_CHECK_CHECKERBOARD,
						1);
			}
		}

		igt_describe(test_display_planes_random_desc);
		connector_subtest("hdmi-cmp-planes-random", HDMIA)
			test_display_planes_random(
				&data, port, CHAMELIUM_CHECK_CHECKERBOARD);

		igt_describe(test_display_frame_dump_desc);
		connector_subtest("hdmi-frame-dump", HDMIA)
			test_display_frame_dump(&data, port);

		igt_describe(test_display_aspect_ratio_desc);
		connector_subtest("hdmi-aspect-ratio", HDMIA)
			test_display_aspect_ratio(&data, port);
	}

	igt_describe("VGA tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_VGA,
				data.port_count, 1);
		}

		igt_describe(test_display_all_modes_desc);
		connector_subtest("vga-frame-dump", VGA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_ANALOG, 1);
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
