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

static bool test_pipe_degamma(data_t *data,
			      igt_plane_t *primary)
{
	igt_output_t *output = data->output;
	igt_display_t *display = &data->display;
	gamma_lut_t *degamma_linear, *degamma_full;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb;
	igt_crc_t crc_fullgamma, crc_fullcolors;
	int fb_id, fb_modeset_id;
	bool ret;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT));
	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	degamma_full = generate_table_max(data->degamma_lut_size);

	igt_output_set_pipe(output, primary->pipe->pipe);
	igt_output_override_mode(output, mode);

	/* Create a framebuffer at the size of the output. */
	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      data->drm_format,
			      DRM_FORMAT_MOD_LINEAR,
			      &fb);
	igt_assert(fb_id);

	fb_modeset_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      data->drm_format,
				      DRM_FORMAT_MOD_LINEAR,
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

	/*
	 * Draw a gradient with degamma LUT to remap all
	 * values to max red/green/blue.
	 */
	paint_gradient_rectangles(data, mode, red_green_blue, &fb);
	igt_plane_set_fb(primary, &fb);
	set_degamma(data, primary->pipe, degamma_full);
	igt_display_commit(&data->display);
	igt_wait_for_vblank(data->drm_fd,
			    display->pipes[primary->pipe->pipe].crtc_offset);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullgamma);

	/*
	 * Verify that the CRC of the software computed output is
	 * equal to the CRC of the degamma LUT transformation output.
	 */
	ret = igt_skip_crc_compare || igt_check_crc_equal(&crc_fullgamma, &crc_fullcolors);

	disable_degamma(primary->pipe);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &fb);
	igt_remove_fb(data->drm_fd, &fb_modeset);

	free_lut(degamma_linear);
	free_lut(degamma_full);

	return ret;
}

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out gamma
 * LUT and verify we have the same CRC as drawing solid color rectangles.
 */
static bool test_pipe_gamma(data_t *data,
			    igt_plane_t *primary)
{
	igt_output_t *output = data->output;
	igt_display_t *display = &data->display;
	gamma_lut_t *gamma_full;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb;
	igt_crc_t crc_fullgamma, crc_fullcolors;
	int fb_id, fb_modeset_id;
	bool ret;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	gamma_full = generate_table_max(data->gamma_lut_size);

	igt_output_set_pipe(output, primary->pipe->pipe);
	igt_output_override_mode(output, mode);

	/* Create a framebuffer at the size of the output. */
	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      data->drm_format,
			      DRM_FORMAT_MOD_LINEAR,
			      &fb);
	igt_assert(fb_id);

	fb_modeset_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      data->drm_format,
				      DRM_FORMAT_MOD_LINEAR,
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

	/*
	 * Draw a gradient with gamma LUT to remap all values
	 * to max red/green/blue.
	 */
	paint_gradient_rectangles(data, mode, red_green_blue, &fb);
	igt_plane_set_fb(primary, &fb);
	igt_display_commit(&data->display);
	igt_wait_for_vblank(data->drm_fd,
			    display->pipes[primary->pipe->pipe].crtc_offset);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc_fullgamma);

	/*
	 * Verify that the CRC of the software computed output is
	 * equal to the CRC of the gamma LUT transformation output.
	 */
	ret = igt_skip_crc_compare || igt_check_crc_equal(&crc_fullgamma, &crc_fullcolors);

	disable_gamma(primary->pipe);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &fb);
	igt_remove_fb(data->drm_fd, &fb_modeset);

	free_lut(gamma_full);

	return ret;
}

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out legacy
 * gamma LUT and verify we have the same CRC as drawing solid color rectangles
 * with linear legacy gamma LUT.
 */
static bool test_pipe_legacy_gamma(data_t *data,
				   igt_plane_t *primary)
{
	igt_output_t *output = data->output;
	igt_display_t *display = &data->display;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	drmModeCrtc *kms_crtc;
	uint32_t i, legacy_lut_size;
	uint16_t *red_lut, *green_lut, *blue_lut;
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb;
	igt_crc_t crc_fullgamma, crc_fullcolors;
	int fb_id, fb_modeset_id;
	bool ret;

	kms_crtc = drmModeGetCrtc(data->drm_fd, primary->pipe->crtc_id);
	legacy_lut_size = kms_crtc->gamma_size;
	drmModeFreeCrtc(kms_crtc);

	red_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	green_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	blue_lut = malloc(sizeof(uint16_t) * legacy_lut_size);

	igt_output_set_pipe(output, primary->pipe->pipe);
	igt_output_override_mode(output, mode);

	/* Create a framebuffer at the size of the output. */
	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR,
			      &fb);
	igt_assert(fb_id);

	fb_modeset_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR,
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

	/*
	 * Draw a gradient with gamma LUT to remap all values
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

	/*
	 * Verify that the CRC of the software computed output is
	 * equal to the CRC of the gamma LUT transformation output.
	 */
	ret = igt_skip_crc_compare || igt_check_crc_equal(&crc_fullgamma, &crc_fullcolors);

	/* Reset output. */
	for (i = 1; i < legacy_lut_size; i++)
		red_lut[i] = green_lut[i] = blue_lut[i] = i << 8;

	igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd, primary->pipe->crtc_id,
					  legacy_lut_size, red_lut, green_lut, blue_lut), 0);
	igt_display_commit(&data->display);

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &fb);
	igt_remove_fb(data->drm_fd, &fb_modeset);

	free(red_lut);
	free(green_lut);
	free(blue_lut);

	return ret;
}

/*
 * Verify that setting the legacy gamma LUT resets the gamma LUT set
 * through the GAMMA_LUT property.
 */
static bool test_pipe_legacy_gamma_reset(data_t *data,
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
	igt_output_t *output = data->output;
	bool ret = true;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
		degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_zero = generate_table_zero(data->gamma_lut_size);

	igt_output_set_pipe(output, primary->pipe->pipe);

	/* Ensure we have a clean state to start with. */
	disable_degamma(primary->pipe);
	disable_ctm(primary->pipe);
	disable_gamma(primary->pipe);
	igt_display_commit(&data->display);

	/*
	 * Set a degama & gamma LUT and a CTM using the
	 * properties and verify the content of the
	 * properties.
	 */
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
		ret &=(lut[i].red == 0 &&
			   lut[i].green == 0 &&
			   lut[i].blue == 0);
	drmModeFreePropertyBlob(blob);
	if(!ret)
		goto end;

	/*
	 * Set a gamma LUT using the legacy ioctl and verify
	 * the content of the GAMMA_LUT property is changed
	 * and that CTM and DEGAMMA_LUT are empty.
	 */
	kms_crtc = drmModeGetCrtc(data->drm_fd, primary->pipe->crtc_id);
	legacy_lut_size = kms_crtc->gamma_size;
	drmModeFreeCrtc(kms_crtc);

	red_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	igt_assert(red_lut);

	green_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	igt_assert(green_lut);

	blue_lut = malloc(sizeof(uint16_t) * legacy_lut_size);
	igt_assert(blue_lut);

	for (i = 0; i < legacy_lut_size; i++)
		red_lut[i] = green_lut[i] = blue_lut[i] = 0xffff;

	igt_assert_eq(drmModeCrtcSetGamma(data->drm_fd,
					  primary->pipe->crtc_id,
					  legacy_lut_size,
					  red_lut, green_lut, blue_lut), 0);
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
		ret &= (lut[i].red == 0xffff &&
			   lut[i].green == 0xffff &&
			   lut[i].blue == 0xffff);
	drmModeFreePropertyBlob(blob);

	free(red_lut);
	free(green_lut);
	free(blue_lut);

end:
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);

	free_lut(degamma_linear);
	free_lut(gamma_zero);
	return ret;
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
	gamma_lut_t *degamma_linear = NULL, *gamma_linear = NULL;
	igt_output_t *output = data->output;
	bool ret = true;
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb;
	igt_crc_t crc_software, crc_hardware;
	int fb_id, fb_modeset_id;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM));

	igt_output_set_pipe(output, primary->pipe->pipe);
	igt_output_override_mode(output, mode);

	/* Create a framebuffer at the size of the output. */
	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      data->drm_format,
			      DRM_FORMAT_MOD_LINEAR,
			      &fb);
	igt_assert(fb_id);

	fb_modeset_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      data->drm_format,
				      DRM_FORMAT_MOD_LINEAR,
				      &fb_modeset);
	igt_assert(fb_modeset_id);
	igt_plane_set_fb(primary, &fb_modeset);

	/*
	 * Don't program LUT's for max CTM cases, as limitation of
	 * representing intermediate values between 0 and 1.0 causes
	 * rounding issues and inaccuracies leading to crc mismatch.
	 */
	if (memcmp(before, after, sizeof(color_t))) {
		igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT));
		igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

		degamma_linear = generate_table(data->degamma_lut_size, 1.0);
		gamma_linear = generate_table(data->gamma_lut_size, 1.0);

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

	/*
	 * Verify that the CRC of the software computed output is
	 * equal to the CRC of the CTM matrix transformation output.
	 */
	ret &= igt_skip_crc_compare || igt_check_crc_equal(&crc_software, &crc_hardware);

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &fb);
	igt_remove_fb(data->drm_fd, &fb_modeset);

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
				      DRM_FORMAT_MOD_LINEAR,
				      &fb);
		igt_assert(fb_id);

		fb_modeset_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      DRM_FORMAT_MOD_LINEAR,
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
prep_pipe(data_t *data, enum pipe p)
{
	igt_require_pipe(&data->display, p);

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
}

static void test_setup(data_t *data, enum pipe p)
{
	igt_pipe_t *pipe;

	prep_pipe(data, p);
	igt_require_pipe_crc(data->drm_fd);

	pipe = &data->display.pipes[p];
	igt_require(pipe->n_planes >= 0);

	data->primary = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd,
					  data->primary->pipe->pipe,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_display_require_output_on_pipe(&data->display, p);
	data->output = igt_get_single_output_for_pipe(&data->display, p);
	igt_require(data->output);

	igt_display_reset(&data->display);
}

static void test_cleanup(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;
}

static void
run_gamma_degamma_tests_for_pipe(data_t *data, enum pipe p,
				 bool (*test_t)(data_t*, igt_plane_t*))
{
	test_setup(data, p);

	/*
	 * We assume an 8bits depth per color for degamma/gamma LUTs
	 * for CRC checks with framebuffer references.
	 */
	data->color_depth = 8;
	data->drm_format = DRM_FORMAT_XRGB8888;
	data->mode = igt_output_get_mode(data->output);

	igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(p), data->output->name)
		igt_assert(test_t(data, data->primary));

	test_cleanup(data);
}

static void
run_ctm_tests_for_pipe(data_t *data, enum pipe p,
		       color_t *expected_colors,
		       double *ctm,
		       int iter)
{
	double delta;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};

	test_setup(data, p);

	/*
	 * We assume an 8bits depth per color for degamma/gamma LUTs
	 * for CRC checks with framebuffer references.
	 */
	data->color_depth = 8;
	delta = 1.0 / (1 << data->color_depth);
	data->drm_format = DRM_FORMAT_XRGB8888;
	data->mode = igt_output_get_mode(data->output);

	igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(p), data->output->name) {
		bool success = false;
		int i;

		if (!iter)
			success = test_pipe_ctm(data, data->primary, red_green_blue,
						expected_colors, ctm);

		/*
		 * We tests a few values around the expected result because
		 * it depends on the hardware we're dealing with, we can either
		 * get clamped or rounded values and we also need to account
		 * for odd number of items in the LUTs.
		 */
		for (i = 0; i < iter; i++) {
			expected_colors[0].r =
				expected_colors[1].g =
				expected_colors[2].b =
				ctm[0] + delta * (i - (iter / 2));
			if (test_pipe_ctm(data, data->primary, red_green_blue,
					  expected_colors, ctm)) {
				success = true;
				break;
			}
		}
		igt_assert(success);
	}

	test_cleanup(data);
}

static void
run_deep_color_tests_for_pipe(data_t *data, enum pipe p)
{
	igt_output_t *output;
	color_t blue_green_blue[] = {
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	double ctm[] = { 0.0, 0.0, 0.0,
			 0.0, 1.0, 0.0,
			 1.0, 0.0, 1.0 };

	if (is_i915_device(data->drm_fd))
		igt_require_f((intel_display_ver(data->devid) >= 11),
				"At least GEN 11 is required to validate Deep-color.\n");

	test_setup(data, p);

	for_each_valid_output_on_pipe(&data->display, p, output) {
		uint64_t max_bpc = get_max_bpc(output);
		bool ret;

		if (!max_bpc)
			continue;

		if (!panel_supports_deep_color(data->drm_fd, output->name))
			continue;

		igt_display_reset(&data->display);
		igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, 10);
		igt_output_set_pipe(output, p);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		if (is_i915_device(data->drm_fd) &&
		    !igt_max_bpc_constraint(&data->display, p, output, 10))
			continue;

		data->color_depth = 10;
		data->drm_format = DRM_FORMAT_XRGB2101010;
		data->output = output;

		data->mode = malloc(sizeof(drmModeModeInfo));
		igt_assert(data->mode);
		memcpy(data->mode, igt_output_get_mode(data->output), sizeof(drmModeModeInfo));

		igt_dynamic_f("pipe-%s-%s-gamma", kmstest_pipe_name(p), output->name) {
			igt_display_reset(&data->display);
			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, 10);

			ret = test_pipe_gamma(data, data->primary);

			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, max_bpc);
			igt_assert(ret);
		}

		igt_dynamic_f("pipe-%s-%s-degamma", kmstest_pipe_name(p), output->name) {
			igt_display_reset(&data->display);
			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, 10);

			ret = test_pipe_degamma(data, data->primary);

			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, max_bpc);
			igt_assert(ret);
		}

		igt_dynamic_f("pipe-%s-%s-ctm", kmstest_pipe_name(p), output->name) {
			igt_display_reset(&data->display);
			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, 10);

			ret = test_pipe_ctm(data, data->primary,
					    red_green_blue,
					    blue_green_blue, ctm);

			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, max_bpc);
			igt_assert(ret);
		}

		free(data->mode);

		break;
	}

	test_cleanup(data);
}

static void
run_invalid_tests_for_pipe(data_t *data)
{
	enum pipe pipe;
	struct {
		const char *name;
		void (*test_t) (data_t *data, enum pipe pipe);
		const char *desc;
	} tests[] = {
		{ "invalid-gamma-lut-sizes", invalid_gamma_lut_sizes,
			"Negative check for invalid gamma lut sizes" },

		{ "invalid-degamma-lut-sizes", invalid_degamma_lut_sizes,
			"Negative check for invalid degamma lut sizes" },

		{ "invalid-ctm-matrix-sizes", invalid_ctm_matrix_sizes,
			"Negative check for color tranformation matrix sizes" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe_f("%s", tests[i].desc);
		igt_subtest_with_dynamic_f("%s", tests[i].name) {
			for_each_pipe(&data->display, pipe) {
				prep_pipe(data, pipe);

				igt_dynamic_f("pipe-%s", kmstest_pipe_name(pipe))
					tests[i].test_t(data, pipe);
			}
		}
	}
}

static void
run_tests_for_pipe(data_t *data)
{
	enum pipe pipe;
	struct {
		const char *name;
		bool (*test_t)(data_t*, igt_plane_t*);
		const char *desc;
	} gamma_degamma_tests[] = {
		{ "degamma", test_pipe_degamma,
		  "Verify that degamma LUT transformation works correctly" },

		{ "gamma", test_pipe_gamma,
		  "Verify that gamma LUT transformation works correctly" },

		{ "legacy-gamma", test_pipe_legacy_gamma,
		  "Verify that legacy gamma LUT transformation works correctly" },

		{ "legacy-gamma-reset", test_pipe_legacy_gamma_reset,
		  "Verify that setting the legacy gamma LUT resets the gamma LUT set through GAMMA_LUT property" },
	};
	struct {
		const char *name;
		int iter;
		color_t colors[3];
		double ctm[9];
		const char *desc;
	} ctm_tests[] = {
		{ "ctm-red-to-blue", 0,
			{{ 0.0, 0.0, 1.0 },
			 { 0.0, 1.0, 0.0 },
			 { 0.0, 0.0, 1.0 }},
		  { 0.0, 0.0, 0.0,
		    0.0, 1.0, 0.0,
		    1.0, 0.0, 1.0 },
		  "Check the color transformation from red to blue"
		},
		{ "ctm-green-to-red", 0,
			{{ 1.0, 0.0, 0.0 },
			 { 1.0, 0.0, 0.0 },
			 { 0.0, 0.0, 1.0 }},
		  { 1.0, 1.0, 0.0,
		    0.0, 0.0, 0.0,
		    0.0, 0.0, 1.0 },
		  "Check the color transformation from green to red"
		},
		{ "ctm-blue-to-red", 0,
			{{ 1.0, 0.0, 0.0 },
			 { 0.0, 1.0, 0.0 },
			 { 1.0, 0.0, 0.0 }},
		  { 1.0, 0.0, 1.0,
		    0.0, 1.0, 0.0,
		    0.0, 0.0, 0.0 },
		  "Check the color transformation from blue to red"
		},
		{ "ctm-max", 0,
			{{ 1.0, 0.0, 0.0 },
			 { 0.0, 1.0, 0.0 },
			 { 0.0, 0.0, 1.0 }},
		  { 100.0, 0.0, 0.0,
		    0.0, 100.0, 0.0,
		    0.0, 0.0, 100.0 },
		  "Check the color transformation for maximum transparency"
		},
		{ "ctm-negative", 0,
			{{ 0.0, 0.0, 0.0 },
			 { 0.0, 0.0, 0.0 },
			 { 0.0, 0.0, 0.0 }},
		  { -1.0, 0.0, 0.0,
		    0.0, -1.0, 0.0,
		    0.0, 0.0, -1.0 },
		  "Check the color transformation for negative transparency"
		},
		{ "ctm-0-25", 5,
			{{ 0.0, }, { 0.0, }, { 0.0, }},
		  { 0.25, 0.0,  0.0,
		    0.0,  0.25, 0.0,
		    0.0,  0.0,  0.25 },
		  "Check the color transformation for 0.25 transparency"
		},
		{ "ctm-0-50", 5,
			{{ 0.0, }, { 0.0, }, { 0.0, }},
		  { 0.5,  0.0,  0.0,
		    0.0,  0.5,  0.0,
		    0.0,  0.0,  0.5 },
		  "Check the color transformation for 0.5 transparency"
		},
		{ "ctm-0-75", 7,
			{{ 0.0, }, { 0.0, }, { 0.0, }},
		  { 0.75, 0.0,  0.0,
		    0.0,  0.75, 0.0,
		    0.0,  0.0,  0.75 },
		  "Check the color transformation for 0.75 transparency"
		},
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(gamma_degamma_tests); i++) {
		igt_describe_f("%s", gamma_degamma_tests[i].desc);
		igt_subtest_with_dynamic_f("%s", gamma_degamma_tests[i].name) {
			for_each_pipe(&data->display, pipe) {
				run_gamma_degamma_tests_for_pipe(data, pipe,
								 gamma_degamma_tests[i].test_t);
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(ctm_tests); i++) {
		igt_describe_f("%s", ctm_tests[i].desc);
		igt_subtest_with_dynamic_f("%s", ctm_tests[i].name) {
			for_each_pipe(&data->display, pipe) {
				run_ctm_tests_for_pipe(data, pipe,
						       ctm_tests[i].colors,
						       ctm_tests[i].ctm,
						       ctm_tests[i].iter);
			}
		}
	}

	igt_fixture
		igt_require(data->display.is_atomic);

	igt_describe("Verify that deep color works correctly");
	igt_subtest_with_dynamic("deep-color") {
		for_each_pipe(&data->display, pipe) {
			run_deep_color_tests_for_pipe(data, pipe);
		}
	}
}

igt_main
{
	data_t data = {};

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		if (is_i915_device(data.drm_fd))
			data.devid = intel_get_drm_devid(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
	}

	igt_subtest_group
		run_tests_for_pipe(&data);

	igt_subtest_group
		run_invalid_tests_for_pipe(&data);

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
