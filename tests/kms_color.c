/*
 * Copyright © 2015 Intel Corporation
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

#include "kms_color_helper.h"

IGT_TEST_DESCRIPTION("Test Color Features at Pipe level");

static void test_pipe_degamma(data_t *data,
			      igt_plane_t *primary)
{
	igt_output_t *output;
	igt_display_t *display = &data->display;
	gamma_lut_t *degamma_linear, *degamma_full;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT));
	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	degamma_full = generate_table_max(data->degamma_lut_size);

	for_each_valid_output_on_pipe(&data->display, primary->pipe->pipe, output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb;
		igt_crc_t crc_fullgamma, crc_fullcolors;
		int fb_id, fb_modeset_id;

		igt_output_set_pipe(output, primary->pipe->pipe);
		mode = igt_output_get_mode(output);

		/* Create a framebuffer at the size of the output. */
		fb_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fb);
		igt_assert(fb_id);

		fb_modeset_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      LOCAL_DRM_FORMAT_MOD_NONE,
					      &fb_modeset);
		igt_assert(fb_modeset_id);

		igt_plane_set_fb(primary, &fb_modeset);
		disable_ctm(primary->pipe);
		disable_gamma(primary->pipe);
		set_degamma(data, primary->pipe, degamma_linear);
		igt_display_commit(&data->display);

		/* Draw solid colors with linear degamma transformation. */
		paint_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullcolors);

		/* Draw a gradient with degamma LUT to remap all
		 * values to max red/green/blue.
		 */
		paint_gradient_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		set_degamma(data, primary->pipe, degamma_full);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullgamma);

		/* Verify that the CRC of the software computed output is
		 * equal to the CRC of the degamma LUT transformation output.
		 */
		igt_assert_crc_equal(&crc_fullgamma, &crc_fullcolors);

		disable_degamma(primary->pipe);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit(&data->display);
		igt_remove_fb(data->drm_fd, &fb);
		igt_remove_fb(data->drm_fd, &fb_modeset);
	}

	free_lut(degamma_linear);
	free_lut(degamma_full);
}

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out gamma
 * LUT and verify we have the same CRC as drawing solid color rectangles.
 */
static void test_pipe_gamma(data_t *data,
			    igt_plane_t *primary)
{
	igt_output_t *output;
	igt_display_t *display = &data->display;
	gamma_lut_t *gamma_full;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	gamma_full = generate_table_max(data->gamma_lut_size);

	for_each_valid_output_on_pipe(&data->display, primary->pipe->pipe, output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb;
		igt_crc_t crc_fullgamma, crc_fullcolors;
		int fb_id, fb_modeset_id;

		igt_output_set_pipe(output, primary->pipe->pipe);
		mode = igt_output_get_mode(output);

		/* Create a framebuffer at the size of the output. */
		fb_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fb);
		igt_assert(fb_id);

		fb_modeset_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      LOCAL_DRM_FORMAT_MOD_NONE,
					      &fb_modeset);
		igt_assert(fb_modeset_id);

		igt_plane_set_fb(primary, &fb_modeset);
		disable_ctm(primary->pipe);
		disable_degamma(primary->pipe);
		set_gamma(data, primary->pipe, gamma_full);
		igt_display_commit(&data->display);

		/* Draw solid colors with no gamma transformation. */
		paint_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullcolors);

		/* Draw a gradient with gamma LUT to remap all values
		 * to max red/green/blue.
		 */
		paint_gradient_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullgamma);

		/* Verify that the CRC of the software computed output is
		 * equal to the CRC of the gamma LUT transformation output.
		 */
		igt_assert_crc_equal(&crc_fullgamma, &crc_fullcolors);

		disable_gamma(primary->pipe);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit(&data->display);
		igt_remove_fb(data->drm_fd, &fb);
		igt_remove_fb(data->drm_fd, &fb_modeset);
	}

	free_lut(gamma_full);
}

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out legacy
 * gamma LUT and verify we have the same CRC as drawing solid color rectangles
 * with linear legacy gamma LUT.
 */
static void test_pipe_legacy_gamma(data_t *data,
				   igt_plane_t *primary)
{
	igt_output_t *output;
	igt_display_t *display = &data->display;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	drmModeCrtc *kms_crtc;
	uint32_t i, legacy_lut_size;
	uint16_t *red_lut, *green_lut, *blue_lut;

	kms_crtc = drmModeGetCrtc(data->drm_fd, primary->pipe->crtc_id);
	legacy_lut_size = kms_crtc->gamma_size;
	drmModeFreeCrtc(kms_crtc);

	red_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	green_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	blue_lut = malloc(sizeof(uint16_t) * legacy_lut_size);

	for_each_valid_output_on_pipe(&data->display, primary->pipe->pipe, output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb;
		igt_crc_t crc_fullgamma, crc_fullcolors;
		int fb_id, fb_modeset_id;

		igt_output_set_pipe(output, primary->pipe->pipe);
		mode = igt_output_get_mode(output);

		/* Create a framebuffer at the size of the output. */
		fb_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fb);
		igt_assert(fb_id);

		fb_modeset_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      LOCAL_DRM_FORMAT_MOD_NONE,
					      &fb_modeset);
		igt_assert(fb_modeset_id);

		igt_plane_set_fb(primary, &fb_modeset);
		disable_degamma(primary->pipe);
		disable_gamma(primary->pipe);
		disable_ctm(primary->pipe);
		igt_display_commit(&data->display);

		/* Draw solid colors with no gamma transformation. */
		paint_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullcolors);

		/* Draw a gradient with gamma LUT to remap all values
		 * to max red/green/blue.
		 */
		paint_gradient_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);

		red_lut[0] = green_lut[0] = blue_lut[0] = 0;
		for (i = 1; i < legacy_lut_size; i++)
			red_lut[i] = green_lut[i] = blue_lut[i] = 0xffff;
		igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd, primary->pipe->crtc_id,
						  legacy_lut_size, red_lut, green_lut, blue_lut), 0);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullgamma);

		/* Verify that the CRC of the software computed output is
		 * equal to the CRC of the gamma LUT transformation output.
		 */
		igt_assert_crc_equal(&crc_fullgamma, &crc_fullcolors);

		/* Reset output. */
		for (i = 1; i < legacy_lut_size; i++)
			red_lut[i] = green_lut[i] = blue_lut[i] = i << 8;

		igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd, primary->pipe->crtc_id,
						  legacy_lut_size, red_lut, green_lut, blue_lut), 0);
		igt_display_commit(&data->display);

		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
		igt_remove_fb(data->drm_fd, &fb);
		igt_remove_fb(data->drm_fd, &fb_modeset);
	}

	free(red_lut);
	free(green_lut);
	free(blue_lut);
}

/*
 * Verify that setting the legacy gamma LUT resets the gamma LUT set
 * through the GAMMA_LUT property.
 */
static void test_pipe_legacy_gamma_reset(data_t *data,
					 igt_plane_t *primary)
{
	const double ctm_identity[] = {
		1.0, 0.0, 0.0,
		0.0, 1.0, 0.0,
		0.0, 0.0, 1.0
	};
	drmModeCrtc *kms_crtc;
	gamma_lut_t *degamma_linear = NULL, *gamma_zero;
	uint32_t i, legacy_lut_size;
	uint16_t *red_lut, *green_lut, *blue_lut;
	struct drm_color_lut *lut;
	drmModePropertyBlobPtr blob;
	igt_output_t *output;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
		degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_zero = generate_table_zero(data->gamma_lut_size);

	for_each_valid_output_on_pipe(&data->display, primary->pipe->pipe, output) {
		igt_output_set_pipe(output, primary->pipe->pipe);

		/* Ensure we have a clean state to start with. */
		disable_degamma(primary->pipe);
		disable_ctm(primary->pipe);
		disable_gamma(primary->pipe);
		igt_display_commit(&data->display);

		/* Set a degama & gamma LUT and a CTM using the
		 * properties and verify the content of the
		 * properties. */
		if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
			set_degamma(data, primary->pipe, degamma_linear);
		if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM))
			set_ctm(primary->pipe, ctm_identity);
		set_gamma(data, primary->pipe, gamma_zero);
		igt_display_commit(&data->display);

		if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT)) {
			blob = get_blob(data, primary->pipe, IGT_CRTC_DEGAMMA_LUT);
			igt_assert(blob &&
				   blob->length == (sizeof(struct drm_color_lut) *
						    data->degamma_lut_size));
			drmModeFreePropertyBlob(blob);
		}

		if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM)) {
			blob = get_blob(data, primary->pipe, IGT_CRTC_CTM);
			igt_assert(blob &&
				   blob->length == sizeof(struct drm_color_ctm));
			drmModeFreePropertyBlob(blob);
		}

		blob = get_blob(data, primary->pipe, IGT_CRTC_GAMMA_LUT);
		igt_assert(blob &&
			   blob->length == (sizeof(struct drm_color_lut) *
					    data->gamma_lut_size));
		lut = (struct drm_color_lut *) blob->data;
		for (i = 0; i < data->gamma_lut_size; i++)
			igt_assert(lut[i].red == 0 &&
				   lut[i].green == 0 &&
				   lut[i].blue == 0);
		drmModeFreePropertyBlob(blob);

		/* Set a gamma LUT using the legacy ioctl and verify
		 * the content of the GAMMA_LUT property is changed
		 * and that CTM and DEGAMMA_LUT are empty. */
		kms_crtc = drmModeGetCrtc(data->drm_fd, primary->pipe->crtc_id);
		legacy_lut_size = kms_crtc->gamma_size;
		drmModeFreeCrtc(kms_crtc);

		red_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
		green_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
		blue_lut = malloc(sizeof(uint16_t) * legacy_lut_size);

		for (i = 0; i < legacy_lut_size; i++)
			red_lut[i] = green_lut[i] = blue_lut[i] = 0xffff;

		igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd,
						  primary->pipe->crtc_id,
						  legacy_lut_size,
						  red_lut, green_lut, blue_lut),
			      0);
		igt_display_commit(&data->display);

		if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
			igt_assert(get_blob(data, primary->pipe,
					    IGT_CRTC_DEGAMMA_LUT) == NULL);

		if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM))
			igt_assert(get_blob(data, primary->pipe, IGT_CRTC_CTM) == NULL);

		blob = get_blob(data, primary->pipe, IGT_CRTC_GAMMA_LUT);
		igt_assert(blob &&
			   blob->length == (sizeof(struct drm_color_lut) *
					    legacy_lut_size));
		lut = (struct drm_color_lut *) blob->data;
		for (i = 0; i < legacy_lut_size; i++)
			igt_assert(lut[i].red == 0xffff &&
				   lut[i].green == 0xffff &&
				   lut[i].blue == 0xffff);
		drmModeFreePropertyBlob(blob);

		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
	}

	free_lut(degamma_linear);
	free_lut(gamma_zero);
}

/*
 * Draw 3 rectangles using before colors with the ctm matrix apply and verify
 * the CRC is equal to using after colors with an identify ctm matrix.
 */
static bool test_pipe_ctm(data_t *data,
			  igt_plane_t *primary,
			  color_t *before,
			  color_t *after,
			  double *ctm_matrix)
{
	const double ctm_identity[] = {
		1.0, 0.0, 0.0,
		0.0, 1.0, 0.0,
		0.0, 0.0, 1.0
	};
	gamma_lut_t *degamma_linear, *gamma_linear;
	igt_output_t *output;
	bool ret = true;
	igt_display_t *display = &data->display;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM));

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	for_each_valid_output_on_pipe(&data->display, primary->pipe->pipe, output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb;
		igt_crc_t crc_software, crc_hardware;
		int fb_id, fb_modeset_id;

		igt_output_set_pipe(output, primary->pipe->pipe);
		mode = igt_output_get_mode(output);

		/* Create a framebuffer at the size of the output. */
		fb_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fb);
		igt_assert(fb_id);

		fb_modeset_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      LOCAL_DRM_FORMAT_MOD_NONE,
					      &fb_modeset);
		igt_assert(fb_modeset_id);
		igt_plane_set_fb(primary, &fb_modeset);

		/*
		 * Don't program LUT's for max CTM cases, as limitation of
		 * representing intermediate values between 0 and 1.0 causes
		 * rounding issues and inaccuracies leading to crc mismatch.
		 */
		if (memcmp(before, after, sizeof(color_t))) {
			set_degamma(data, primary->pipe, degamma_linear);
			set_gamma(data, primary->pipe, gamma_linear);
		} else {
			/* Disable Degamma and Gamma for ctm max test */
			disable_degamma(primary->pipe);
			disable_gamma(primary->pipe);
		}

		disable_ctm(primary->pipe);
		igt_display_commit(&data->display);

		paint_rectangles(data, mode, after, &fb);
		igt_plane_set_fb(primary, &fb);
		set_ctm(primary->pipe, ctm_identity);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_software);

		/* With CTM transformation. */
		paint_rectangles(data, mode, before, &fb);
		igt_plane_set_fb(primary, &fb);
		set_ctm(primary->pipe, ctm_matrix);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_hardware);

		/* Verify that the CRC of the software computed output is
		 * equal to the CRC of the CTM matrix transformation output.
		 */
		ret &= !igt_skip_crc_compare || igt_check_crc_equal(&crc_software, &crc_hardware);

		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
		igt_remove_fb(data->drm_fd, &fb);
		igt_remove_fb(data->drm_fd, &fb_modeset);
	}

	free_lut(degamma_linear);
	free_lut(gamma_linear);

	return ret;
}

/*
 * Hardware computes CRC based on the number of bits it is working with (8,
 * 10, 12, 16 bits), meaning with a framebuffer of 8bits per color will
 * usually leave the remaining lower bits at 0.
 *
 * We're programming the gamma LUT in order to get rid of those lower bits so
 * we can compare the CRC of a framebuffer without any transformation to a CRC
 * with transformation applied and verify the CRCs match.
 *
 * This test is currently disabled as the CRC computed on Intel hardware seems
 * to include data on the lower bits, this is preventing us to CRC checks.
 */
#if 0
static void test_pipe_limited_range_ctm(data_t *data,
					igt_plane_t *primary)
{
	double limited_result = 235.0 / 255.0;
	color_t red_green_blue_limited[] = {
		{ limited_result, 0.0, 0.0 },
		{ 0.0, limited_result, 0.0 },
		{ 0.0, 0.0, limited_result }
	};
	color_t red_green_blue_full[] = {
		{ 0.5, 0.0, 0.0 },
		{ 0.0, 0.5, 0.0 },
		{ 0.0, 0.0, 0.5 }
	};
	double ctm[] = { 1.0, 0.0, 0.0,
			0.0, 1.0, 0.0,
			0.0, 0.0, 1.0 };
	gamma_lut_t *degamma_linear, *gamma_linear;
	igt_output_t *output;
	bool has_broadcast_rgb_output = false;
	igt_display_t *display = &data->display;

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	for_each_valid_output_on_pipe(&data->display, primary->pipe->pipe, output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb;
		igt_crc_t crc_full, crc_limited;
		int fb_id, fb_modeset_id;

		if (!igt_output_has_prop(output, IGT_CONNECTOR_BROADCAST_RGB))
			continue;

		has_broadcast_rgb_output = true;

		igt_output_set_pipe(output, primary->pipe->pipe);
		mode = igt_output_get_mode(output);

		/* Create a framebuffer at the size of the output. */
		fb_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fb);
		igt_assert(fb_id);

		fb_modeset_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      LOCAL_DRM_FORMAT_MOD_NONE,
					      &fb_modeset);
		igt_assert(fb_modeset_id);
		igt_plane_set_fb(primary, &fb_modeset);

		set_degamma(data, primary->pipe, degamma_linear);
		set_gamma(data, primary->pipe, gamma_linear);
		set_ctm(primary->pipe, ctm);

		igt_output_set_prop_value(output, IGT_CONNECTOR_BROADCAST_RGB, BROADCAST_RGB_FULL);
		paint_rectangles(data, mode, red_green_blue_limited, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_full);

		/* Set the output into limited range. */
		igt_output_set_prop_value(output, IGT_CONNECTOR_BROADCAST_RGB, BROADCAST_RGB_16_235);
		paint_rectangles(data, mode, red_green_blue_full, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		igt_wait_for_vblank(data->drm_fd,
				display->pipes[primary->pipe->pipe].crtc_offset);
		igt_pipe_crc_collect_crc(data->pipe_crc, &crc_limited);

		/* And reset.. */
		igt_output_set_prop_value(output, IGT_CONNECTOR_BROADCAST_RGB, BROADCAST_RGB_FULL);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);

		/* Verify that the CRC of the software computed output is
		 * equal to the CRC of the CTM matrix transformation output.
		 */
		igt_assert_crc_equal(&crc_full, &crc_limited);

		igt_remove_fb(data->drm_fd, &fb);
		igt_remove_fb(data->drm_fd, &fb_modeset);
	}

	free_lut(gamma_linear);
	free_lut(degamma_linear);

	igt_require(has_broadcast_rgb_output);
}
#endif

static void
run_tests_for_pipe(data_t *data, enum pipe p)
{
	igt_pipe_t *pipe;
	igt_plane_t *primary;
	double delta;
	int i;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};

	igt_fixture {
		igt_require_pipe_crc(data->drm_fd);

		igt_require_pipe(&data->display, p);

		pipe = &data->display.pipes[p];
		igt_require(pipe->n_planes >= 0);

		primary = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

		data->pipe_crc = igt_pipe_crc_new(data->drm_fd,
						  primary->pipe->pipe,
						  INTEL_PIPE_CRC_SOURCE_AUTO);

		if (igt_pipe_obj_has_prop(&data->display.pipes[p], IGT_CRTC_DEGAMMA_LUT_SIZE)) {
			data->degamma_lut_size =
				igt_pipe_obj_get_prop(&data->display.pipes[p],
						      IGT_CRTC_DEGAMMA_LUT_SIZE);
			igt_assert_lt(0, data->degamma_lut_size);
		}

		if (igt_pipe_obj_has_prop(&data->display.pipes[p], IGT_CRTC_GAMMA_LUT_SIZE)) {
			data->gamma_lut_size =
				igt_pipe_obj_get_prop(&data->display.pipes[p],
						      IGT_CRTC_GAMMA_LUT_SIZE);
			igt_assert_lt(0, data->gamma_lut_size);
		}

		igt_display_require_output_on_pipe(&data->display, p);
	}

	/* We assume an 8bits depth per color for degamma/gamma LUTs
	 * for CRC checks with framebuffer references. */
	data->color_depth = 8;
	delta = 1.0 / (1 << data->color_depth);

	igt_describe("Check the color transformation from red to blue");
	igt_subtest_f("pipe-%s-ctm-red-to-blue", kmstest_pipe_name(p)) {
		color_t blue_green_blue[] = {
			{ 0.0, 0.0, 1.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, 1.0 }
		};
		double ctm[] = { 0.0, 0.0, 0.0,
				0.0, 1.0, 0.0,
				1.0, 0.0, 1.0 };
		igt_assert(test_pipe_ctm(data, primary, red_green_blue,
					 blue_green_blue, ctm));
	}

	igt_describe("Check the color transformation from green to red");
	igt_subtest_f("pipe-%s-ctm-green-to-red", kmstest_pipe_name(p)) {
		color_t red_red_blue[] = {
			{ 1.0, 0.0, 0.0 },
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 0.0, 1.0 }
		};
		double ctm[] = { 1.0, 1.0, 0.0,
				0.0, 0.0, 0.0,
				0.0, 0.0, 1.0 };
		igt_assert(test_pipe_ctm(data, primary, red_green_blue,
					 red_red_blue, ctm));
	}

	igt_describe("Check the color transformation from blue to red");
	igt_subtest_f("pipe-%s-ctm-blue-to-red", kmstest_pipe_name(p)) {
		color_t red_green_red[] = {
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 1.0, 0.0, 0.0 }
		};
		double ctm[] = { 1.0, 0.0, 1.0,
				0.0, 1.0, 0.0,
				0.0, 0.0, 0.0 };
		igt_assert(test_pipe_ctm(data, primary, red_green_blue,
					 red_green_red, ctm));
	}

	/* We tests a few values around the expected result because
	 * the it depends on the hardware we're dealing with, we can
	 * either get clamped or rounded values and we also need to
	 * account for odd number of items in the LUTs. */
	igt_describe("Check the color transformation for 0.25 transparency");
	igt_subtest_f("pipe-%s-ctm-0-25", kmstest_pipe_name(p)) {
		color_t expected_colors[] = {
			{ 0.0, }, { 0.0, }, { 0.0, }
		};
		double ctm[] = { 0.25, 0.0,  0.0,
				 0.0,  0.25, 0.0,
				 0.0,  0.0,  0.25 };
		bool success = false;

		for (i = 0; i < 5; i++) {
			expected_colors[0].r =
				expected_colors[1].g =
				expected_colors[2].b =
				0.25 + delta * (i - 2);
			success |= test_pipe_ctm(data, primary,
						 red_green_blue,
						 expected_colors, ctm);
		}
		igt_assert(success);
	}

	igt_describe("Check the color transformation for 0.5 transparency");
	igt_subtest_f("pipe-%s-ctm-0-5", kmstest_pipe_name(p)) {
		color_t expected_colors[] = {
			{ 0.0, }, { 0.0, }, { 0.0, }
		};
		double ctm[] = { 0.5, 0.0, 0.0,
				 0.0, 0.5, 0.0,
				 0.0, 0.0, 0.5 };
		bool success = false;

		for (i = 0; i < 5; i++) {
			expected_colors[0].r =
				expected_colors[1].g =
				expected_colors[2].b =
				0.5 + delta * (i - 2);
			success |= test_pipe_ctm(data, primary,
						 red_green_blue,
						 expected_colors, ctm);
		}
		igt_assert(success);
	}

	igt_describe("Check the color transformation for 0.75 transparency");
	igt_subtest_f("pipe-%s-ctm-0-75", kmstest_pipe_name(p)) {
		color_t expected_colors[] = {
			{ 0.0, }, { 0.0, }, { 0.0, }
		};
		double ctm[] = { 0.75, 0.0,  0.0,
				 0.0,  0.75, 0.0,
				 0.0,  0.0,  0.75 };
		bool success = false;

		for (i = 0; i < 7; i++) {
			expected_colors[0].r =
				expected_colors[1].g =
				expected_colors[2].b =
				0.75 + delta * (i - 3);
			success |= test_pipe_ctm(data, primary,
						 red_green_blue,
						 expected_colors, ctm);
		}
		igt_assert(success);
	}

	igt_describe("Check the color transformation for maximum transparency");
	igt_subtest_f("pipe-%s-ctm-max", kmstest_pipe_name(p)) {
		color_t full_rgb[] = {
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, 1.0 }
		};
		double ctm[] = { 100.0,   0.0,   0.0,
				 0.0,   100.0,   0.0,
				 0.0,     0.0, 100.0 };

		/* CherryView generates values on 10bits that we
		 * produce with an 8 bits per color framebuffer. */
		igt_require(!IS_CHERRYVIEW(data->devid));

		igt_assert(test_pipe_ctm(data, primary, red_green_blue,
					 full_rgb, ctm));
	}

	igt_describe("Check the color transformation for negative transparency");
	igt_subtest_f("pipe-%s-ctm-negative", kmstest_pipe_name(p)) {
		color_t all_black[] = {
			{ 0.0, 0.0, 0.0 },
			{ 0.0, 0.0, 0.0 },
			{ 0.0, 0.0, 0.0 }
		};
		double ctm[] = { -1.0,  0.0,  0.0,
				 0.0, -1.0,  0.0,
				 0.0,  0.0, -1.0 };
		igt_assert(test_pipe_ctm(data, primary, red_green_blue,
					 all_black, ctm));
	}

#if 0
	igt_subtest_f("pipe-%s-ctm-limited-range", kmstest_pipe_name(p))
		test_pipe_limited_range_ctm(data, primary);
#endif

	igt_describe("Verify that degamma LUT transformation works correctly");
	igt_subtest_f("pipe-%s-degamma", kmstest_pipe_name(p))
		test_pipe_degamma(data, primary);

	igt_describe("Verify that gamma LUT transformation works correctly");
	igt_subtest_f("pipe-%s-gamma", kmstest_pipe_name(p))
		test_pipe_gamma(data, primary);

	igt_describe("Verify that legacy gamma LUT transformation works correctly");
	igt_subtest_f("pipe-%s-legacy-gamma", kmstest_pipe_name(p))
		test_pipe_legacy_gamma(data, primary);

	igt_describe("Verify that setting the legacy gamma LUT resets the gamma LUT set through "
			"GAMMA_LUT property");
	igt_subtest_f("pipe-%s-legacy-gamma-reset", kmstest_pipe_name(p))
		test_pipe_legacy_gamma_reset(data, primary);

	igt_fixture {
		disable_degamma(primary->pipe);
		disable_gamma(primary->pipe);
		disable_ctm(primary->pipe);
		igt_display_commit(&data->display);

		igt_pipe_crc_free(data->pipe_crc);
		data->pipe_crc = NULL;
	}
}

igt_main
{
	data_t data = {};
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		if (is_i915_device(data.drm_fd))
			data.devid = intel_get_drm_devid(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
	}

	for_each_pipe_static(pipe)
		igt_subtest_group
			run_tests_for_pipe(&data, pipe);

	igt_describe("Negative check for invalid gamma lut sizes");
	igt_subtest_f("pipe-invalid-gamma-lut-sizes")
		invalid_gamma_lut_sizes(&data);

	igt_describe("Negative check for invalid degamma lut sizes");
	igt_subtest_f("pipe-invalid-degamma-lut-sizes")
		invalid_degamma_lut_sizes(&data);

	igt_describe("Negative check for color tranformation matrix sizes");
	igt_subtest_f("pipe-invalid-ctm-matrix-sizes")
		invalid_ctm_matrix_sizes(&data);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
