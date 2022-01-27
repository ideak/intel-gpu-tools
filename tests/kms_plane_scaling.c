/*
 * Copyright © 2013,2014 Intel Corporation
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
#include "igt_vec.h"
#include <math.h>

IGT_TEST_DESCRIPTION("Test display plane scaling");

enum scaler_combo_test_type {
	TEST_PLANES_UPSCALE = 0,
	TEST_PLANES_DOWNSCALE,
	TEST_PLANES_UPSCALE_DOWNSCALE
};

typedef struct {
	uint32_t devid;
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb[4];
	bool extended;
} data_t;

const struct {
	const char * const describe;
	const char * const name;
	const double sf;
	const bool is_upscale;
} scaler_with_pixel_format_tests[] = {
	{
		"Tests upscaling with pixel formats, from 20x20 fb.",
		"plane-upscale-with-pixel-format-20x20",
		0.0,
		true,
	},
	{
		"Tests upscaling with pixel formats for 0.25 scaling factor.",
		"plane-upscale-with-pixel-format-factor-0-25",
		0.25,
		true,
	},
	{
		"Tests downscaling with pixel formats for 0.25 scaling factor.",
		"plane-downscale-with-pixel-format-factor-0-25",
		0.25,
		false,
	},
	{
		"Tests downscaling with pixel formats for 0.5 scaling factor.",
		"plane-downscale-with-pixel-format-factor-0-5",
		0.5,
		false,
	},
	{
		"Tests downscaling with pixel formats for 0.75 scaling factor.",
		"plane-downscale-with-pixel-format-factor-0-75",
		0.75,
		false,
	},
	{
		"Tests scaling with pixel formats, unity scaling.",
		"plane-scaler-with-pixel-format-unity-scaling",
		1.0,
		true,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf;
	const bool is_upscale;
} scaler_with_rotation_tests[] = {
	{
		"Tests upscaling with rotation, from 20x20 fb.",
		"plane-upscale-with-rotation-20x20",
		0.0,
		true,
	},
	{
		"Tests upscaling with rotation for 0.25 scaling factor.",
		"plane-upscale-with-rotation-factor-0-25",
		0.25,
		true,
	},
	{
		"Tests downscaling with rotation for 0.25 scaling factor.",
		"plane-downscale-with-rotation-factor-0-25",
		0.25,
		false,
	},
	{
		"Tests downscaling with rotation for 0.5 scaling factor.",
		"plane-downscale-with-rotation-factor-0-5",
		0.25,
		false,
	},
	{
		"Tests downscaling with rotation for 0.75 scaling factor.",
		"plane-downscale-with-rotation-factor-0-75",
		0.75,
		false,
	},
	{
		"Tests scaling with rotation, unity scaling.",
		"plane-scaler-with-rotation-unity-scaling",
		1.0,
		true,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf;
	const bool is_upscale;
} scaler_with_modifiers_tests[] = {
	{
		"Tests upscaling with modifiers, from 20x20 fb.",
		"plane-upscale-with-modifiers-20x20",
		0.0,
		true,
	},
	{
		"Tests upscaling with modifiers for 0.25 scaling factor.",
		"plane-upscale-with-modifiers-factor-0-25",
		0.25,
		true,
	},
	{
		"Tests downscaling with modifiers for 0.25 scaling factor.",
		"plane-downscale-with-modifiers-factor-0-25",
		0.25,
		false,
	},
	{
		"Tests downscaling with modifiers for 0.5 scaling factor.",
		"plane-downscale-with-modifiers-factor-0-5",
		0.5,
		false,
	},
	{
		"Tests downscaling with modifiers for 0.75 scaling factor.",
		"plane-downscale-with-modifiers-factor-0-75",
		0.75,
		false,
	},
	{
		"Tests scaling with modifiers, unity scaling.",
		"plane-scaler-with-modifiers-unity-scaling",
		1.0,
		true,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf_plane1;
	const double sf_plane2;
	const enum scaler_combo_test_type test_type;
} scaler_with_2_planes_tests[] = {
	{
		"Tests upscaling of 2 planes, from 20x20 fb.",
		"planes-upscale-20x20",
		0.0,
		0.0,
		TEST_PLANES_UPSCALE,
	},
	{
		"Tests upscaling of 2 planes for 0.25 scaling factor.",
		"planes-upscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_UPSCALE,
	},
	{
		"Tests scaling of 2 planes, unity scaling.",
		"planes-scaler-unity-scaling",
		1.0,
		1.0,
		TEST_PLANES_UPSCALE,
	},
	{
		"Tests downscaling of 2 planes for 0.25 scaling factor.",
		"planes-downscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_DOWNSCALE,
	},
	{
		"Tests downscaling of 2 planes for 0.5 scaling factor.",
		"planes-downscale-factor-0-5",
		0.5,
		0.5,
		TEST_PLANES_DOWNSCALE,
	},
	{
		"Tests downscaling of 2 planes for 0.75 scaling factor.",
		"planes-downscale-factor-0-75",
		0.75,
		0.75,
		TEST_PLANES_DOWNSCALE,
	},
	{
		"Tests upscaling (20x20) and downscaling (scaling factor 0.25) of 2 planes.",
		"planes-upscale-20x20-downscale-factor-0-25",
		0.0,
		0.25,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (20x20) and downscaling (scaling factor 0.5) of 2 planes.",
		"planes-upscale-20x20-downscale-factor-0-5",
		0.0,
		0.5,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (20x20) and downscaling (scaling factor 0.75) of 2 planes.",
		"planes-upscale-20x20-downscale-factor-0-75",
		0.0,
		0.75,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (scaling factor 0.25) and downscaling (scaling factor 0.25) of 2 planes.",
		"planes-upscale-factor-0-25-downscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (scaling factor 0.25) and downscaling (scaling factor 0.5) of 2 planes.",
		"planes-upscale-factor-0-25-downscale-factor-0-5",
		0.25,
		0.5,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (scaling factor 0.25) and downscaling (scaling factor 0.75) of 2 planes.",
		"planes-upscale-factor-0-25-downscale-factor-0-75",
		0.25,
		0.75,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests scaling (unity) and downscaling (scaling factor 0.25) of 2 planes.",
		"planes-unity-scaling-downscale-factor-0-25",
		1.0,
		0.25,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests scaling (unity) and downscaling (scaling factor 0.5) of 2 planes.",
		"planes-unity-scaling-downscale-factor-0-5",
		1.0,
		0.5,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests scaling (unity) and downscaling (scaling factor 0.75) of 2 planes.",
		"planes-unity-scaling-downscale-factor-0-75",
		1.0,
		0.75,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
};

static int get_width(drmModeModeInfo *mode, double scaling_factor)
{
	if (scaling_factor == 0.0)
		return 20;
	else
		return mode->hdisplay * scaling_factor;
}

static int get_height(drmModeModeInfo *mode, double scaling_factor)
{
	if (scaling_factor == 0.0)
		return 20;
	else
		return mode->vdisplay * scaling_factor;
}

static void cleanup_fbs(data_t *data)
{
	for (int i = 0; i < ARRAY_SIZE(data->fb); i++)
		igt_remove_fb(data->drm_fd, &data->fb[i]);
}

static void cleanup_crtc(data_t *data)
{
	igt_display_reset(&data->display);

	cleanup_fbs(data);
}

static void check_scaling_pipe_plane_rot(data_t *d, igt_plane_t *plane,
					 uint32_t pixel_format,
					 uint64_t modifier,
					 int width, int height,
					 bool is_upscale,
					 enum pipe pipe,
					 igt_output_t *output,
					 igt_rotation_t rot)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;
	int commit_ret;
	int w, h;

	mode = igt_output_get_mode(output);

	if (is_upscale) {
		w = width;
		h = height;
	} else {
		w = mode->hdisplay;
		h = mode->vdisplay;
	}

	/*
	 * guarantee even value width/height to avoid fractional
	 * uv component in chroma subsampling for yuv 4:2:0 formats
	 * */
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	igt_create_color_fb(display->drm_fd, w, h,
			    pixel_format, modifier, 0.0, 1.0, 0.0, &d->fb[0]);

	igt_plane_set_fb(plane, &d->fb[0]);
	igt_fb_set_position(&d->fb[0], plane, 0, 0);
	igt_fb_set_size(&d->fb[0], plane, w, h);
	igt_plane_set_position(plane, 0, 0);

	if (is_upscale)
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
	else
		igt_plane_set_size(plane, width, height);

	igt_plane_set_rotation(plane, rot);
	commit_ret = igt_display_try_commit2(display, COMMIT_ATOMIC);

	igt_plane_set_fb(plane, NULL);
	igt_plane_set_position(plane, 0, 0);

	igt_skip_on_f(commit_ret == -ERANGE || commit_ret == -EINVAL,
		      "Unsupported scaling factor with fb size %dx%d\n",
		      w, h);
	igt_assert_eq(commit_ret, 0);
}

static const igt_rotation_t rotations[] = {
	IGT_ROTATION_0,
	IGT_ROTATION_90,
	IGT_ROTATION_180,
	IGT_ROTATION_270,
};

static bool can_scale(data_t *d, unsigned format)
{
	if (!is_i915_device(d->drm_fd))
		return true;

	switch (format) {
	case DRM_FORMAT_XRGB16161616F:
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ARGB16161616F:
	case DRM_FORMAT_ABGR16161616F:
		if (intel_display_ver(d->devid) >= 11)
			return true;
		/* fall through */
	case DRM_FORMAT_C8:
		return false;
	default:
		return true;
	}
}

static bool test_format(data_t *data,
			struct igt_vec *tested_formats,
			uint32_t format)
{
	if (!igt_fb_supported_format(format))
		return false;

	if (!is_i915_device(data->drm_fd) ||
	    data->extended)
		return true;

	format = igt_reduce_format(format);

	/* only test each format "class" once */
	if (igt_vec_index(tested_formats, &format) >= 0)
		return false;

	igt_vec_push(tested_formats, &format);

	return true;
}

static bool test_pipe_iteration(data_t *data, enum pipe pipe, int iteration)
{
	if (!is_i915_device(data->drm_fd) ||
	    data->extended)
		return true;

	if ((pipe > PIPE_B) && (iteration >= 2))
		return false;

	return true;
}

static const uint64_t modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	I915_FORMAT_MOD_X_TILED,
	I915_FORMAT_MOD_Y_TILED,
	I915_FORMAT_MOD_Yf_TILED,
	I915_FORMAT_MOD_4_TILED
};

static void test_scaler_with_modifier_pipe(data_t *d,
					   int width, int height,
					   bool is_upscale,
					   enum pipe pipe,
					   igt_output_t *output)
{
	igt_display_t *display = &d->display;
	unsigned format = DRM_FORMAT_XRGB8888;
	igt_plane_t *plane;

	cleanup_crtc(d);

	igt_output_set_pipe(output, pipe);

	for_each_plane_on_pipe(display, pipe, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		for (int i = 0; i < ARRAY_SIZE(modifiers); i++) {
			uint64_t modifier = modifiers[i];

			if (igt_plane_has_format_mod(plane, format, modifier))
				check_scaling_pipe_plane_rot(d, plane,
							     format, modifier,
							     width, height,
							     is_upscale,
							     pipe, output,
							     IGT_ROTATION_0);
		}
	}
}

static void test_scaler_with_rotation_pipe(data_t *d,
					   int width, int height,
					   bool is_upscale,
					   enum pipe pipe,
					   igt_output_t *output)
{
	igt_display_t *display = &d->display;
	unsigned format = DRM_FORMAT_XRGB8888;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	igt_plane_t *plane;

	cleanup_crtc(d);

	igt_output_set_pipe(output, pipe);

	for_each_plane_on_pipe(display, pipe, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		for (int i = 0; i < ARRAY_SIZE(rotations); i++) {
			igt_rotation_t rot = rotations[i];

			if (igt_plane_has_rotation(plane, rot))
				check_scaling_pipe_plane_rot(d, plane,
							     format, modifier,
							     width, height,
							     is_upscale,
							     pipe, output,
							     rot);
		}
	}
}

static void test_scaler_with_pixel_format_pipe(data_t *d, int width, int height, bool is_upscale,
					       enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &d->display;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	igt_plane_t *plane;

	cleanup_crtc(d);

	igt_output_set_pipe(output, pipe);

	for_each_plane_on_pipe(display, pipe, plane) {
		struct igt_vec tested_formats;

		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		igt_vec_init(&tested_formats, sizeof(uint32_t));

		for (int j = 0; j < plane->drm_plane->count_formats; j++) {
			uint32_t format = plane->drm_plane->formats[j];

			if (!test_pipe_iteration(d, pipe, j))
				continue;

			if (test_format(d, &tested_formats, format) &&
			    igt_plane_has_format_mod(plane, format, modifier) &&
			    can_scale(d, format))
			    check_scaling_pipe_plane_rot(d, plane,
							 format, modifier,
							 width, height,
							 is_upscale,
							 pipe, output, IGT_ROTATION_0);
		}

		igt_vec_fini(&tested_formats);
	}
}

static void find_connected_pipe(igt_display_t *display, bool second, enum pipe *pipe, igt_output_t **output)
{
	enum pipe first = PIPE_NONE;
	igt_output_t *first_output = NULL;
	bool found = false;

	for_each_pipe_with_valid_output(display, *pipe, *output) {
		if (first == *pipe || *output == first_output)
			continue;

		if (second) {
			first = *pipe;
			first_output = *output;
			second = false;
			continue;
		}

		return;
	}

	if (first_output)
		igt_require_f(found, "No second valid output found\n");
	else
		igt_require_f(found, "No valid outputs found\n");
}

static void
__test_planes_scaling_combo(data_t *d, int w1, int h1, int w2, int h2,
			    enum pipe pipe, igt_output_t *output,
			    igt_plane_t *p1, igt_plane_t *p2,
			    struct igt_fb *fb1, struct igt_fb *fb2,
			    enum scaler_combo_test_type test_type)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;
	int ret;

	mode = igt_output_get_mode(output);

	igt_plane_set_fb(p1, fb1);
	igt_plane_set_fb(p2, fb2);

	if (test_type == TEST_PLANES_UPSCALE) {
		/* first plane upscaling */
		igt_plane_set_size(p1, mode->hdisplay, mode->vdisplay);
		/* second plane upscaling */
		igt_plane_set_size(p2, mode->hdisplay - 20, mode->vdisplay - 20);
	}
	if (test_type == TEST_PLANES_DOWNSCALE) {
		/* first plane downscaling */
		igt_plane_set_size(p1, w1, h1);
		/* second plane downscaling */
		igt_plane_set_size(p2, w2, h2);
	}
	if (test_type == TEST_PLANES_UPSCALE_DOWNSCALE) {
		/* first plane upscaling */
		igt_plane_set_size(p1, mode->hdisplay, mode->vdisplay);
		/* second plane downscaling */
		igt_plane_set_size(p2, w2, h2);
	}

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_plane_set_fb(p1, NULL);
	igt_plane_set_fb(p2, NULL);

	igt_skip_on_f(ret == -EINVAL || ret == -ERANGE,
		      "Scaling op not supported by driver\n");
	igt_assert_eq(ret, 0);
}

static void setup_fb(int fd, int width, int height,
		     double r, double g, double b,
		     struct igt_fb *fb)
{
	igt_create_color_pattern_fb(fd, width, height,
				    DRM_FORMAT_XRGB8888,
				    I915_TILING_NONE,
				    r, g, b, fb);
}

static void
test_planes_scaling_combo(data_t *d, int w1, int h1, int w2, int h2,
			  enum pipe pipe, igt_output_t *output,
			  enum scaler_combo_test_type test_type)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;

	cleanup_crtc(d);

	igt_output_set_pipe(output, pipe);
	mode = igt_output_get_mode(output);

	if (test_type == TEST_PLANES_UPSCALE) {
		setup_fb(display->drm_fd, w1, h1, 1.0, 0.0, 0.0, &d->fb[1]);
		setup_fb(display->drm_fd, w2, h2, 0.0, 1.0, 0.0, &d->fb[2]);
	}
	if (test_type == TEST_PLANES_DOWNSCALE) {
		setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, 1.0, 0.0, 0.0, &d->fb[1]);
		setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, 0.0, 1.0, 0.0, &d->fb[2]);
	}
	if (test_type == TEST_PLANES_UPSCALE_DOWNSCALE) {
		setup_fb(display->drm_fd, w1, h1, 1.0, 0.0, 0.0, &d->fb[1]);
		setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, 0.0, 1.0, 0.0, &d->fb[2]);
	}

	for (int k = 0; k < display->pipes[pipe].n_planes; k++) {
		igt_plane_t *p1, *p2;

		p1 = &display->pipes[pipe].planes[k];
		igt_require(p1);
		p2 = &display->pipes[pipe].planes[k+1];
		igt_require(p2);

		if (p1->type == DRM_PLANE_TYPE_CURSOR || p2->type == DRM_PLANE_TYPE_CURSOR)
				continue;

		__test_planes_scaling_combo(d, w1, h1, w2, h2,
					    pipe, output, p1, p2,
					    &d->fb[1], &d->fb[2],
					    test_type);
	}

	igt_remove_fb(display->drm_fd, &d->fb[1]);
	igt_remove_fb(display->drm_fd, &d->fb[2]);
}

static void
test_invalid_num_scalers(data_t *d, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &d->display;
	igt_pipe_t *pipe_obj = &display->pipes[pipe];
	int width, height;
	igt_plane_t *plane[3];
	drmModeModeInfo *mode;
	int ret;

	cleanup_crtc(d);

	igt_output_set_pipe(output, pipe);

	width = height = 20;
	mode = igt_output_get_mode(output);

	plane[0] = igt_pipe_get_plane_type_index(pipe_obj, DRM_PLANE_TYPE_OVERLAY, 0);
	igt_require(plane[0]);
	plane[1] = igt_pipe_get_plane_type_index(pipe_obj, DRM_PLANE_TYPE_OVERLAY, 1);
	igt_require(plane[1]);
	plane[2] = igt_pipe_get_plane_type_index(pipe_obj, DRM_PLANE_TYPE_OVERLAY, 2);
	igt_require(plane[2]);

	igt_create_color_pattern_fb(display->drm_fd,
                                    width, height,
                                    DRM_FORMAT_XRGB8888,
                                    I915_TILING_NONE,
                                    1.0, 0.0, 0.0, &d->fb[0]);
	igt_create_color_pattern_fb(display->drm_fd,
                                    width, height,
                                    DRM_FORMAT_XRGB8888,
                                    I915_TILING_NONE,
                                    0.0, 1.0, 0.0, &d->fb[1]);
	igt_create_color_pattern_fb(display->drm_fd,
                                    width, height,
                                    DRM_FORMAT_XRGB8888,
                                    I915_TILING_NONE,
                                    0.0, 0.0, 1.0, &d->fb[2]);

	igt_plane_set_fb(plane[0], &d->fb[0]);
	igt_plane_set_fb(plane[1], &d->fb[1]);
	igt_plane_set_fb(plane[2], &d->fb[2]);

	igt_plane_set_size(plane[0], mode->hdisplay, mode->vdisplay);
	igt_plane_set_size(plane[1], mode->hdisplay, mode->vdisplay);
	igt_plane_set_size(plane[2], mode->hdisplay, mode->vdisplay);

	/* This commit is expected to fail for i915 devices. i915 devices support
	 * max 2 scalers/pipe. In dmesg we can find: Too many scaling requests 3 > 2.
	 * For devices (non-i915, or possible future i915) that are able to perform this
	 * amount of scaling; handle that case aswell.
	 * */
	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_skip_on_f(ret == 0, "Cannot test handling of too many scaling ops, the device supports a large amount.\n");
	igt_assert_eq(ret, -EINVAL);

	/* cleanup */
	igt_plane_set_fb(plane[0], NULL);
	igt_plane_set_fb(plane[1], NULL);
	igt_plane_set_fb(plane[2], NULL);
	igt_remove_fb(display->drm_fd, &d->fb[0]);
	igt_remove_fb(display->drm_fd, &d->fb[1]);
	igt_remove_fb(display->drm_fd, &d->fb[2]);
}

static void test_scaler_with_multi_pipe_plane(data_t *d)
{
	igt_display_t *display = &d->display;
	igt_output_t *output1, *output2;
	drmModeModeInfo *mode1, *mode2;
	igt_plane_t *plane[4];
	enum pipe pipe1, pipe2;
	int ret1, ret2;

	cleanup_crtc(d);

	find_connected_pipe(display, false, &pipe1, &output1);
	find_connected_pipe(display, true, &pipe2, &output2);

	igt_skip_on(!output1 || !output2);

	igt_output_set_pipe(output1, pipe1);
	igt_output_set_pipe(output2, pipe2);

	plane[0] = igt_output_get_plane(output1, 0);
	igt_require(plane[0]);
	plane[1] = igt_output_get_plane(output1, 0);
	igt_require(plane[1]);
	plane[2] = igt_output_get_plane(output2, 1);
	igt_require(plane[2]);
	plane[3] = igt_output_get_plane(output2, 1);
	igt_require(plane[3]);

	igt_create_pattern_fb(d->drm_fd, 600, 600,
			      DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &d->fb[0]);
	igt_create_pattern_fb(d->drm_fd, 500, 500,
			      DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &d->fb[1]);
	igt_create_pattern_fb(d->drm_fd, 700, 700,
			      DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &d->fb[2]);
	igt_create_pattern_fb(d->drm_fd, 400, 400,
			      DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &d->fb[3]);

	igt_plane_set_fb(plane[0], &d->fb[0]);
	igt_plane_set_fb(plane[1], &d->fb[1]);
	igt_plane_set_fb(plane[2], &d->fb[2]);
	igt_plane_set_fb(plane[3], &d->fb[3]);

	if (igt_display_try_commit_atomic(display,
				DRM_MODE_ATOMIC_TEST_ONLY |
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) != 0) {
		bool found = igt_override_all_active_output_modes_to_fit_bw(display);
		igt_require_f(found, "No valid mode combo found.\n");
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

	mode1 = igt_output_get_mode(output1);
	mode2 = igt_output_get_mode(output2);

	/* upscaling primary */
	igt_plane_set_size(plane[0], mode1->hdisplay, mode1->vdisplay);
	igt_plane_set_size(plane[2], mode2->hdisplay, mode2->vdisplay);
	ret1 = igt_display_try_commit2(display, COMMIT_ATOMIC);

	/* upscaling sprites */
	igt_plane_set_size(plane[1], mode1->hdisplay, mode1->vdisplay);
	igt_plane_set_size(plane[3], mode2->hdisplay, mode2->vdisplay);
	ret2 = igt_display_try_commit2(display, COMMIT_ATOMIC);

	igt_plane_set_fb(plane[0], NULL);
	igt_plane_set_fb(plane[1], NULL);
	igt_plane_set_fb(plane[2], NULL);
	igt_plane_set_fb(plane[3], NULL);

	igt_skip_on_f(ret1 == -ERANGE || ret1 == -EINVAL ||
		      ret2 == -ERANGE || ret1 == -EINVAL,
		      "Scaling op is not supported by driver\n");
	igt_assert_eq(ret1 && ret2, 0);
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'e':
		data->extended = true;
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{}
};

static const char help_str[] =
	"  --extended\t\tRun the extended tests\n";

static data_t data;

igt_main_args("", long_opts, help_str, opt_handler, &data)
{
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&data.display, data.drm_fd);
		data.devid = is_i915_device(data.drm_fd) ?
			intel_get_drm_devid(data.drm_fd) : 0;
		igt_require(data.display.is_atomic);
	}

	igt_subtest_group {
		igt_output_t *output;

		for (int index = 0; index < ARRAY_SIZE(scaler_with_pixel_format_tests); index++) {
			igt_describe(scaler_with_pixel_format_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_pixel_format_tests[index].name) {
			for_each_pipe_with_single_output(&data.display, pipe, output)
				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output)) {
					drmModeModeInfo *mode;

					mode = igt_output_get_mode(output);

					test_scaler_with_pixel_format_pipe(&data,
							get_width(mode, scaler_with_pixel_format_tests[index].sf),
							get_height(mode, scaler_with_pixel_format_tests[index].sf),
							scaler_with_pixel_format_tests[index].is_upscale,
							pipe, output);
				}
			}
		}

		for (int index = 0; index < ARRAY_SIZE(scaler_with_rotation_tests); index++) {
			igt_describe(scaler_with_rotation_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_rotation_tests[index].name) {
			for_each_pipe_with_single_output(&data.display, pipe, output)
				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output)) {
					drmModeModeInfo *mode;

					mode = igt_output_get_mode(output);

					test_scaler_with_rotation_pipe(&data,
							get_width(mode, scaler_with_rotation_tests[index].sf),
							get_height(mode, scaler_with_rotation_tests[index].sf),
							scaler_with_rotation_tests[index].is_upscale,
							pipe, output);
				}
			}
		}

		for (int index = 0; index < ARRAY_SIZE(scaler_with_modifiers_tests); index++) {
			igt_describe(scaler_with_modifiers_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_modifiers_tests[index].name) {
			for_each_pipe_with_single_output(&data.display, pipe, output)
				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output)) {
					drmModeModeInfo *mode;

					mode = igt_output_get_mode(output);

					test_scaler_with_modifier_pipe(&data,
							get_width(mode, scaler_with_rotation_tests[index].sf),
							get_height(mode, scaler_with_rotation_tests[index].sf),
							scaler_with_rotation_tests[index].is_upscale,
							pipe, output);
				}
			}
		}

		igt_describe("Tests scaling with clipping and clamping, pixel formats.");
		igt_subtest_with_dynamic("plane-scaler-with-clipping-clamping-pixel-formats") {
			for_each_pipe_with_single_output(&data.display, pipe, output) {
				drmModeModeInfo *mode;

				mode = igt_output_get_mode(output);

				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output))
					test_scaler_with_pixel_format_pipe(&data, mode->hdisplay + 100,
							mode->vdisplay + 100, false, pipe, output);
			}
		}

		igt_describe("Tests scaling with clipping and clamping, rotation.");
		igt_subtest_with_dynamic("plane-scaler-with-clipping-clamping-rotation") {
			for_each_pipe_with_single_output(&data.display, pipe, output) {
				drmModeModeInfo *mode;

				mode = igt_output_get_mode(output);

				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output))
					test_scaler_with_rotation_pipe(&data, mode->hdisplay + 100,
							mode->vdisplay + 100, false, pipe, output);
			}
		}

		igt_describe("Tests scaling with clipping and clamping, modifiers.");
		igt_subtest_with_dynamic("plane-scaler-with-clipping-clamping-modifiers") {
			for_each_pipe_with_single_output(&data.display, pipe, output) {
				drmModeModeInfo *mode;

				mode = igt_output_get_mode(output);

				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output))
					test_scaler_with_modifier_pipe(&data, mode->hdisplay + 100,
							mode->vdisplay + 100, false, pipe, output);
			}
		}

		for (int index = 0; index < ARRAY_SIZE(scaler_with_2_planes_tests); index++) {
			igt_describe(scaler_with_2_planes_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_2_planes_tests[index].name) {
			for_each_pipe_with_single_output(&data.display, pipe, output)
				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output)) {
					drmModeModeInfo *mode;

					mode = igt_output_get_mode(output);

					test_planes_scaling_combo(&data,
							get_width(mode, scaler_with_2_planes_tests[index].sf_plane1),
							get_height(mode, scaler_with_2_planes_tests[index].sf_plane1),
							get_width(mode, scaler_with_2_planes_tests[index].sf_plane2),
							get_height(mode, scaler_with_2_planes_tests[index].sf_plane2),
							pipe, output, scaler_with_2_planes_tests[index].test_type);
				}
			}
		}

		igt_describe("Negative test for number of scalers per pipe.");
		igt_subtest_with_dynamic("invalid-num-scalers") {
			for_each_pipe_with_valid_output(&data.display, pipe, output)
				igt_dynamic_f("pipe-%s-%s-invalid-num-scalers",
					       kmstest_pipe_name(pipe), igt_output_name(output))
					test_invalid_num_scalers(&data, pipe, output);
		}
	}

	igt_describe("Tests scaling with multi-pipe.");
	igt_subtest_f("2x-scaler-multi-pipe")
		test_scaler_with_multi_pipe_plane(&data);

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
