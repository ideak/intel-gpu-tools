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

#include "kms_color_helper.h"

IGT_TEST_DESCRIPTION("Test Color Features at Pipe level using Chamelium to verify instead of CRC");

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out
 * degamma LUT and verify we have the same frame dump as drawing solid color
 * rectangles with linear degamma LUT.
 */
static void test_pipe_degamma(data_t *data,
			      igt_plane_t *primary)
{
	igt_output_t *output;
	gamma_lut_t *degamma_linear, *degamma_full;
	gamma_lut_t *gamma_linear;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};

	int i;
	struct chamelium_port *port;
	char *connected_ports[4];

	for (i = 0; i < data->port_count; i++)
		connected_ports[i] =
			(char *) chamelium_port_get_name(data->ports[i]);

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT));
	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	degamma_full = generate_table_max(data->degamma_lut_size);

	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	for_each_valid_output_on_pipe(&data->display,
				      primary->pipe->pipe,
				      output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb, fbref;
		struct chamelium_frame_dump *frame_fullcolors;
		int fb_id, fb_modeset_id, fbref_id;
		bool valid_output = false;

		for (i = 0; i < data->port_count; i++)
			valid_output |=
				(strcmp(output->name, connected_ports[i]) == 0);
		if (!valid_output)
			continue;
		else
			for (i = 0; i < data->port_count; i++)
				if (strcmp(output->name,
					   connected_ports[i]) == 0)
					port = data->ports[i];

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

		fbref_id = igt_create_fb(data->drm_fd,
					      mode->hdisplay,
					      mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      LOCAL_DRM_FORMAT_MOD_NONE,
					      &fbref);
		igt_assert(fbref_id);

		igt_plane_set_fb(primary, &fb_modeset);
		disable_ctm(primary->pipe);
		disable_degamma(primary->pipe);
		set_gamma(data, primary->pipe, gamma_linear);
		igt_display_commit(&data->display);

		/* Draw solid colors with no degamma transformation. */
		paint_rectangles(data, mode, red_green_blue, &fbref);

		/* Draw a gradient with degamma LUT to remap all
		 * values to max red/green/blue.
		 */
		paint_gradient_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		set_degamma(data, primary->pipe, degamma_full);
		igt_display_commit(&data->display);
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		frame_fullcolors =
			chamelium_read_captured_frame(data->chamelium, 0);

		/* Verify that the framebuffer reference of the software
		 * computed output is equal to the frame dump of the degamma
		 * LUT transformation output.
		 */
		chamelium_frame_match_or_dump(data->chamelium, port,
					      frame_fullcolors, &fbref,
					      CHAMELIUM_CHECK_ANALOG);

		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
	}

	free_lut(degamma_linear);
	free_lut(degamma_full);
	free_lut(gamma_linear);
}

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out
 * gamma LUT and verify we have the same frame dump as drawing solid
 * color rectangles.
 */
static void test_pipe_gamma(data_t *data,
			    igt_plane_t *primary)
{
	igt_output_t *output;
	gamma_lut_t *gamma_full;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};

	int i;
	struct chamelium_port *port;
	char *connected_ports[4];

	for (i = 0; i < data->port_count; i++)
		connected_ports[i] =
			(char *) chamelium_port_get_name(data->ports[i]);

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	gamma_full = generate_table_max(data->gamma_lut_size);

	for_each_valid_output_on_pipe(&data->display,
				      primary->pipe->pipe,
				      output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb, fbref;
		struct chamelium_frame_dump *frame_fullcolors;
		int fb_id, fb_modeset_id, fbref_id;
		bool valid_output = false;

		for (i = 0; i < data->port_count; i++)
			valid_output |=
				(strcmp(output->name, connected_ports[i]) == 0);
		if (!valid_output)
			continue;
		else
			for (i = 0; i < data->port_count; i++)
				if (strcmp(output->name,
					   connected_ports[i]) == 0)
					port = data->ports[i];

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

		fbref_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fbref);
		igt_assert(fbref_id);

		igt_plane_set_fb(primary, &fb_modeset);
		disable_ctm(primary->pipe);
		disable_degamma(primary->pipe);
		set_gamma(data, primary->pipe, gamma_full);
		igt_display_commit(&data->display);

		/* Draw solid colors with no gamma transformation. */
		paint_rectangles(data, mode, red_green_blue, &fbref);

		/* Draw a gradient with gamma LUT to remap all values
		 * to max red/green/blue.
		 */
		paint_gradient_rectangles(data, mode, red_green_blue, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		frame_fullcolors =
			chamelium_read_captured_frame(data->chamelium, 0);

		/* Verify that the framebuffer reference of the software computed
		 * output is equal to the frame dump of the degamma LUT
		 * transformation output.
		 */
		chamelium_frame_match_or_dump(data->chamelium, port,
					      frame_fullcolors, &fbref,
					      CHAMELIUM_CHECK_ANALOG);

		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
	}

	free_lut(gamma_full);
}

/*
 * Draw 3 rectangles using before colors with the ctm matrix apply and verify
 * the frame dump is equal to using after colors with an identify ctm matrix.
 */
static bool test_pipe_ctm(data_t *data,
			  igt_plane_t *primary,
			  color_t *before,
			  color_t *after,
			  double *ctm_matrix)
{
	gamma_lut_t *degamma_linear, *gamma_linear;
	igt_output_t *output;

	int i;
	bool ret = true;
	struct chamelium_port *port;
	char *connected_ports[4];

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM));

	for (i = 0; i < data->port_count; i++)
		connected_ports[i] =
			(char *) chamelium_port_get_name(data->ports[i]);

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	for_each_valid_output_on_pipe(&data->display,
				      primary->pipe->pipe,
				      output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb, fbref;
		struct chamelium_frame_dump *frame_hardware;
		int fb_id, fb_modeset_id, fbref_id;
		bool valid_output = false;

		for (i = 0; i < data->port_count; i++)
			valid_output |=
				(strcmp(output->name, connected_ports[i]) == 0);
		if (!valid_output)
			continue;
		else
			for (i = 0; i < data->port_count; i++)
				if (strcmp(output->name,
					   connected_ports[i]) == 0)
					port = data->ports[i];

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

		fbref_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fbref);
		igt_assert(fbref_id);

		igt_plane_set_fb(primary, &fb_modeset);

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

		paint_rectangles(data, mode, after, &fbref);

		/* With CTM transformation. */
		paint_rectangles(data, mode, before, &fb);
		igt_plane_set_fb(primary, &fb);
		set_ctm(primary->pipe, ctm_matrix);
		igt_display_commit(&data->display);
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		frame_hardware =
			chamelium_read_captured_frame(data->chamelium, 0);

		/* Verify that the framebuffer reference of the software
		 * computed output is equal to the frame dump of the CTM
		 * matrix transformation output.
		 */
		ret &= chamelium_frame_match_or_dump(data->chamelium, port,
						     frame_hardware,
						     &fbref,
						     CHAMELIUM_CHECK_ANALOG);

		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
	}

	free_lut(degamma_linear);
	free_lut(gamma_linear);

	return ret;
}

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

	int i;
	struct chamelium_port *port;
	char *connected_ports[4];

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	for (i = 0; i < data->port_count; i++)
		connected_ports[i] =
			(char *) chamelium_port_get_name(data->ports[i]);

	for_each_valid_output_on_pipe(&data->display,
				      primary->pipe->pipe,
				      output) {
		drmModeModeInfo *mode;
		struct igt_fb fb_modeset, fb, fbref;
		struct chamelium_frame_dump *frame_limited;
		int fb_id, fb_modeset_id, fbref_id;
		bool valid_output = false;

		for (i = 0; i < data->port_count; i++)
			valid_output |=
				(strcmp(output->name, connected_ports[i]) == 0);
		if (!valid_output)
			continue;
		else
			for (i = 0; i < data->port_count; i++)
				if (strcmp(output->name,
				    connected_ports[i]) == 0)
					port = data->ports[i];

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

		fbref_id = igt_create_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &fbref);
		igt_assert(fbref_id);

		igt_plane_set_fb(primary, &fb_modeset);

		set_degamma(data, primary->pipe, degamma_linear);
		set_gamma(data, primary->pipe, gamma_linear);
		set_ctm(primary->pipe, ctm);

		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_BROADCAST_RGB,
					  BROADCAST_RGB_FULL);
		paint_rectangles(data, mode, red_green_blue_limited, &fb);
		igt_plane_set_fb(primary, &fb);
		igt_display_commit(&data->display);

		/* Set the output into limited range. */
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_BROADCAST_RGB,
					  BROADCAST_RGB_16_235);
		paint_rectangles(data, mode, red_green_blue_full, &fb);

		/* And reset.. */
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_BROADCAST_RGB,
					  BROADCAST_RGB_FULL);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		frame_limited =
			chamelium_read_captured_frame(data->chamelium, 0);


		/* Verify that the framebuffer reference of the software
		 * computed output is equal to the frame dump of the CTM
		 * matrix transformation output.
		 */
		chamelium_frame_match_or_dump(data->chamelium, port,
					      frame_limited, &fbref,
					      CHAMELIUM_CHECK_ANALOG);

	}

	free_lut(gamma_linear);
	free_lut(degamma_linear);

	igt_require(has_broadcast_rgb_output);
}

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

		igt_require(p < data->display.n_pipes);

		pipe = &data->display.pipes[p];
		igt_require(pipe->n_planes >= 0);

		primary = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

		if (igt_pipe_obj_has_prop(&data->display.pipes[p],
					  IGT_CRTC_DEGAMMA_LUT_SIZE)) {
			data->degamma_lut_size =
				igt_pipe_obj_get_prop(&data->display.pipes[p],
						IGT_CRTC_DEGAMMA_LUT_SIZE);
			igt_assert_lt(0, data->degamma_lut_size);
		}

		if (igt_pipe_obj_has_prop(&data->display.pipes[p],
					  IGT_CRTC_GAMMA_LUT_SIZE)) {
			data->gamma_lut_size =
				igt_pipe_obj_get_prop(&data->display.pipes[p],
						      IGT_CRTC_GAMMA_LUT_SIZE);
			igt_assert_lt(0, data->gamma_lut_size);
		}

		igt_display_require_output_on_pipe(&data->display, p);
	}

	data->color_depth = 8;
	delta = 1.0 / (1 << data->color_depth);

	igt_describe("Compare after applying ctm matrix & identity matrix");
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
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
	 * account for odd number of items in the LUTs.
	 */
	igt_describe("Compare after applying ctm matrix & identity matrix");
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
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
		 * produce with an 8 bits per color framebuffer.
		 */
		igt_require(!IS_CHERRYVIEW(data->devid));

		igt_assert(test_pipe_ctm(data, primary, red_green_blue,
					 full_rgb, ctm));
	}

	igt_describe("Compare after applying ctm matrix & identity matrix");
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
	igt_subtest_f("pipe-%s-ctm-limited-range", kmstest_pipe_name(p))
		test_pipe_limited_range_ctm(data, primary);

	igt_describe("Compare maxed out gamma LUT and solid color linear LUT");
	igt_subtest_f("pipe-%s-degamma", kmstest_pipe_name(p))
		test_pipe_degamma(data, primary);

	igt_describe("Compare maxed out gamma LUT and solid color linear LUT");
	igt_subtest_f("pipe-%s-gamma", kmstest_pipe_name(p))
		test_pipe_gamma(data, primary);

	igt_fixture {
		disable_degamma(primary->pipe);
		disable_gamma(primary->pipe);
		disable_ctm(primary->pipe);
		igt_display_commit(&data->display);
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

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		/* we need to initalize chamelium after igt_display_require */
		data.chamelium = chamelium_init(data.drm_fd);
		igt_require(data.chamelium);

		data.ports = chamelium_get_ports(data.chamelium,
						 &data.port_count);

		if (!data.port_count)
			igt_skip("No ports connected\n");

		kmstest_set_vt_graphics_mode();
	}

	for_each_pipe_static(pipe)
		igt_subtest_group
			run_tests_for_pipe(&data, pipe);
	igt_describe("Negative test case gamma lut size");
	igt_subtest_f("pipe-invalid-gamma-lut-sizes")
		invalid_gamma_lut_sizes(&data);

	igt_describe("Negative test case degamma lut size");
	igt_subtest_f("pipe-invalid-degamma-lut-sizes")
		invalid_degamma_lut_sizes(&data);

	igt_describe("Negative test case ctm matrix size");
	igt_subtest_f("pipe-invalid-ctm-matrix-sizes")
		invalid_ctm_matrix_sizes(&data);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
