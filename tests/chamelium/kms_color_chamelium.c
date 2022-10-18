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
static bool test_pipe_degamma(data_t *data,
			      igt_plane_t *primary,
			      struct chamelium_port *port)
{
	igt_output_t *output = data->output;
	gamma_lut_t *degamma_full;
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb, fbref;
	struct chamelium_frame_dump *frame_fullcolors;
	int fb_id, fb_modeset_id, fbref_id;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	bool ret;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT));

	degamma_full = generate_table_max(data->degamma_lut_size);

	igt_output_set_pipe(output, primary->pipe->pipe);

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

	fbref_id = igt_create_fb(data->drm_fd,
				 mode->hdisplay,
				 mode->vdisplay,
				 DRM_FORMAT_XRGB8888,
				 DRM_FORMAT_MOD_LINEAR,
				 &fbref);
	igt_assert(fbref_id);

	igt_plane_set_fb(primary, &fb_modeset);
	disable_ctm(primary->pipe);
	disable_gamma(primary->pipe);
	igt_display_commit(&data->display);

	/* Draw solid colors with linear degamma transformation. */
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
	ret = chamelium_frame_match_or_dump(data->chamelium, port,
					    frame_fullcolors, &fbref,
					    CHAMELIUM_CHECK_ANALOG);

	disable_degamma(primary->pipe);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	free_lut(degamma_full);

	return ret;
}

/*
 * Draw 3 gradient rectangles in red, green and blue, with a maxed out
 * gamma LUT and verify we have the same frame dump as drawing solid
 * color rectangles.
 */
static bool test_pipe_gamma(data_t *data,
			    igt_plane_t *primary,
			    struct chamelium_port *port)
{
	igt_output_t *output = data->output;
	gamma_lut_t *gamma_full;
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb, fbref;
	struct chamelium_frame_dump *frame_fullcolors;
	int fb_id, fb_modeset_id, fbref_id;
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	bool ret;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT));

	gamma_full = generate_table_max(data->gamma_lut_size);

	igt_output_set_pipe(output, primary->pipe->pipe);

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

	fbref_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR,
			      &fbref);
	igt_assert(fbref_id);

	igt_plane_set_fb(primary, &fbref);
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
	ret = chamelium_frame_match_or_dump(data->chamelium, port,
					    frame_fullcolors, &fbref,
					    CHAMELIUM_CHECK_ANALOG);

	disable_gamma(primary->pipe);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	free_lut(gamma_full);

	return ret;
}

/*
 * Draw 3 rectangles using before colors with the ctm matrix apply and verify
 * the frame dump is equal to using after colors with an identify ctm matrix.
 */
static bool test_pipe_ctm(data_t *data,
			  igt_plane_t *primary,
			  color_t *before,
			  color_t *after,
			  double *ctm_matrix,
			  struct chamelium_port *port)
{
	gamma_lut_t *degamma_linear, *gamma_linear;
	igt_output_t *output = data->output;
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb, fbref;
	struct chamelium_frame_dump *frame_hardware;
	int fb_id, fb_modeset_id, fbref_id;
	bool ret = true;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM));

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	igt_output_set_pipe(output, primary->pipe->pipe);

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

	fbref_id = igt_create_fb(data->drm_fd,
				 mode->hdisplay,
				 mode->vdisplay,
				 DRM_FORMAT_XRGB8888,
				 DRM_FORMAT_MOD_LINEAR,
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
	disable_degamma(primary->pipe);
	disable_gamma(primary->pipe);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(&data->display);
	free_lut(degamma_linear);
	free_lut(gamma_linear);

	return ret;
}

static bool test_pipe_limited_range_ctm(data_t *data,
					igt_plane_t *primary,
					struct chamelium_port *port)
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
	igt_output_t *output = data->output;
	drmModeModeInfo *mode = data->mode;
	struct igt_fb fb_modeset, fb, fbref;
	struct chamelium_frame_dump *frame_limited;
	int fb_id, fb_modeset_id, fbref_id;
	bool ret = false;

	igt_require(igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM));

	degamma_linear = generate_table(data->degamma_lut_size, 1.0);
	gamma_linear = generate_table(data->gamma_lut_size, 1.0);

	igt_output_set_pipe(output, primary->pipe->pipe);

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

	fbref_id = igt_create_fb(data->drm_fd,
				 mode->hdisplay,
				 mode->vdisplay,
				 DRM_FORMAT_XRGB8888,
				 DRM_FORMAT_MOD_LINEAR,
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
	ret = chamelium_frame_match_or_dump(data->chamelium, port,
					    frame_limited, &fbref,
					    CHAMELIUM_CHECK_ANALOG);

	free_lut(gamma_linear);
	free_lut(degamma_linear);

	return ret;
}

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

static int test_setup(data_t *data, enum pipe p)
{
	igt_pipe_t *pipe;
	int i = 0;

	igt_display_reset(&data->display);
	prep_pipe(data, p);

	pipe = &data->display.pipes[p];
	igt_require(pipe->n_planes >= 0);

	data->primary = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

	for_each_valid_output_on_pipe(&data->display, p, data->output) {
		for (i = 0; i < data->port_count; i++) {
			if (strcmp(data->output->name,
				   chamelium_port_get_name(data->ports[i])) == 0)
				return i;
		}
	}

	return -1;
}

static void
run_gamma_degamma_tests_for_pipe(data_t *data, enum pipe p,
		bool (*test_t)(data_t*, igt_plane_t*, struct chamelium_port*))
{
	int port_idx = test_setup(data, p);

	igt_require(port_idx >= 0);

	data->color_depth = 8;
	data->drm_format = DRM_FORMAT_XRGB8888;
	data->mode = igt_output_get_mode(data->output);

	igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(p), data->output->name)
		igt_assert(test_t(data, data->primary, data->ports[port_idx]));
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
	int port_idx = test_setup(data, p);

	igt_require(port_idx >= 0);
	/*
	 * CherryView generates values on 10bits that we
	 * produce with an 8 bits per color framebuffer.
	 */
	if (expected_colors[0].r == 1.0 && ctm[0] == 100)
		igt_require(!IS_CHERRYVIEW(data->devid));

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
			success = test_pipe_ctm(data, data->primary,
						red_green_blue,
						expected_colors, ctm,
						data->ports[port_idx]);

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
			if (test_pipe_ctm(data, data->primary,
					  red_green_blue, expected_colors,
					  ctm, data->ports[port_idx])) {
				success = true;
				break;
			}
		}
		igt_assert(success);
	}
}

static void
run_limited_range_ctm_test_for_pipe(data_t *data, enum pipe p,
		bool (*test_t)(data_t*, igt_plane_t*, struct chamelium_port*))
{
	int port_idx = test_setup(data, p);

	igt_require(port_idx >= 0);
	igt_require(igt_output_has_prop(data->output, IGT_CONNECTOR_BROADCAST_RGB));

	data->color_depth = 8;
	data->drm_format = DRM_FORMAT_XRGB8888;
	data->mode = igt_output_get_mode(data->output);

	igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(p), data->output->name)
		igt_assert(test_t(data, data->primary, data->ports[port_idx]));
}

static void
run_tests_for_pipe(data_t *data)
{
	enum pipe pipe;
	struct {
		const char *name;
		bool (*test_t)(data_t*, igt_plane_t*, struct chamelium_port*);
		const char *desc;
	} gamma_degamma_tests[] = {
		{ "degamma", test_pipe_degamma,
		  "Verify that degamma LUT transformation works correctly" },

		{ "gamma", test_pipe_gamma,
		  "Verify that gamma LUT transformation works correctly" },
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

	igt_describe("Compare after applying ctm matrix & identity matrix");
	igt_subtest_with_dynamic("ctm-limited-range") {
		for_each_pipe(&data->display, pipe) {
			run_limited_range_ctm_test_for_pipe(data, pipe,
							    test_pipe_limited_range_ctm);
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

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		igt_chamelium_allow_fsm_handling = false;

		/* we need to initalize chamelium after igt_display_require */
		data.chamelium = chamelium_init(data.drm_fd, &data.display);
		igt_require(data.chamelium);

		data.ports = chamelium_get_ports(data.chamelium,
						 &data.port_count);

		if (!data.port_count)
			igt_skip("No ports connected\n");

		kmstest_set_vt_graphics_mode();
	}

	igt_subtest_group
		run_tests_for_pipe(&data);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
