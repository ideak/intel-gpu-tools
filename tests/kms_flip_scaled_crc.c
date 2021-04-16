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

IGT_TEST_DESCRIPTION("Test flipping between scaled/nonscaled framebuffers");

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	uint32_t gen;
	struct igt_fb small_fb;
	struct igt_fb big_fb;
	igt_pipe_crc_t *pipe_crc;
	uint32_t attemptmodewidth;
	uint32_t attemptmodeheight;
} data_t;

const struct {
	const char * const name;
	const char * const describe;
	const uint64_t firstmodifier;
	const uint32_t firstformat;
	const uint64_t secondmodifier;
	const uint32_t secondformat;
} flip_scenario_test[] = {
	{
		"flip-32bpp-ytile-to-64bpp-ytile",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F
	},
	{
		"flip-64bpp-ytile-to-32bpp-ytile",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888
	},
	{
		"flip-64bpp-ytile-to-16bpp-ytile",
		"Flip from 64bpp non scaled fb to 16bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_RGB565
	},
	{
		"flip-32bpp-ytileccs-to-64bpp-ytile",
		"Flip from 32bpp non scaled fb to 64bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_XRGB8888,
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F
	},
	{
		"flip-32bpp-ytile-to-32bpp-ytilegen12rcccs",
		"Flip from 32bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		LOCAL_I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, DRM_FORMAT_XRGB8888
	},
	{
		"flip-32bpp-ytile-to-32bpp-ytileccs",
		"Flip from 32bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB8888,
		LOCAL_I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_XRGB8888
	},
	{
		"flip-64bpp-ytile-to-32bpp-ytilercccs",
		"Flip from 64bpp non scaled fb to 32bpp downscaled fb to stress CD clock programming",
		LOCAL_I915_FORMAT_MOD_Y_TILED, DRM_FORMAT_XRGB16161616F,
		LOCAL_I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, DRM_FORMAT_XRGB8888
	},
};

static void setup_fb(data_t *data, struct igt_fb *newfb, uint32_t width,
		     uint32_t height, uint64_t format, uint64_t modifier)
{
	igt_require(igt_display_has_format_mod(&data->display, format,
					       modifier));

	igt_create_color_fb(data->drm_fd, width, height,
			    format, modifier, 0, 1, 0, newfb);
}

static void free_fbs(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->small_fb);
	igt_remove_fb(data->drm_fd, &data->big_fb);
}

static void test_flip_to_scaled(data_t *data, uint32_t index, enum pipe pipe,
				igt_output_t *output)
{
	igt_plane_t *primary;
	igt_crc_t small_crc, big_crc;
	drmModeModeInfoPtr modetoset = NULL;
	struct drm_event_vblank ev;
	int ret;

	igt_display_reset(&data->display);

	igt_debug("running on output %s pipe %s\n", output->name,
		  kmstest_pipe_name(pipe));

	if (data->big_fb.fb_id == 0) {
		setup_fb(data, &data->small_fb,
				data->attemptmodewidth,
				data->attemptmodeheight,
				flip_scenario_test[index].firstformat,
				flip_scenario_test[index].firstmodifier);

		setup_fb(data, &data->big_fb,
				data->attemptmodewidth * 2,
				data->attemptmodeheight * 2,
				flip_scenario_test[index].secondformat,
				flip_scenario_test[index].secondmodifier);

		igt_debug("small fb %dx%d\n", data->small_fb.width,
				data->small_fb.height);
		igt_debug("big fb %dx%d\n", data->big_fb.width,
				data->big_fb.height);
	}

	igt_output_set_pipe(output, pipe);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	if (!igt_plane_has_format_mod(primary, data->small_fb.drm_format,
				      data->small_fb.modifier) ||
	    !igt_plane_has_format_mod(primary, data->big_fb.drm_format,
				      data->big_fb.modifier))
		return;

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET,
				  NULL);
	if (data->pipe_crc) {
		igt_pipe_crc_stop(data->pipe_crc);
		igt_pipe_crc_free(data->pipe_crc);
	}
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe,
					  INTEL_PIPE_CRC_SOURCE_AUTO);

	for (int i = 0; i < output->config.connector->count_modes; i++) {
		if (output->config.connector->modes[i].hdisplay == data->attemptmodewidth &&
		   output->config.connector->modes[i].vdisplay == data->attemptmodeheight) {
			if (modetoset &&
			    modetoset->vrefresh < output->config.connector->modes[i].vrefresh)
				continue;

			modetoset = &output->config.connector->modes[i];
		}
	}

	if (!modetoset)
		igt_debug("%dp mode was not found from connector, will continue with default. This may cause cdclk to fail this test.\n",
			  data->attemptmodeheight);
	else
		igt_output_override_mode(output, modetoset);

	igt_plane_set_position(primary, 0, 0);
	igt_plane_set_fb(primary, &data->small_fb);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &small_crc);

	igt_plane_set_fb(primary, &data->big_fb);
	igt_plane_set_size(primary, data->small_fb.width,
			   data->small_fb.height);
	ret = igt_display_try_commit_atomic(&data->display,
					    DRM_MODE_ATOMIC_ALLOW_MODESET  |
					    DRM_MODE_PAGE_FLIP_EVENT, NULL);

	igt_require_f(ret != -ERANGE,
		      "Platform scaling limits exceeded, skipping.\n");
	igt_require_f(!(ret == -EINVAL && !modetoset),
		      "No %dp and default mode too big, cdclk limits exceeded. Skipping.\n",
		      data->attemptmodeheight);
	igt_assert_eq(ret, 0);

	igt_assert(read(data->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &big_crc);
	igt_assert_crc_equal(&small_crc, &big_crc);

	igt_pipe_crc_stop(data->pipe_crc);
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;
}

igt_main
{
	enum pipe pipe;
	data_t data = {};
	igt_output_t *output;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.gen = intel_gen(intel_get_drm_devid(data.drm_fd));
		igt_require(data.gen >= 9);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_require_pipe_crc(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		if (data.gen < 11) {
			data.attemptmodewidth = 640;
			data.attemptmodeheight = 480;
		} else {
			data.attemptmodewidth = 1920;
			data.attemptmodeheight = 1080;
		}
	}

	for (int index = 0; index < ARRAY_SIZE(flip_scenario_test); index++) {
		igt_describe(flip_scenario_test[index].describe);
		igt_subtest(flip_scenario_test[index].name) {
			free_fbs(&data);
			for_each_pipe_with_single_output(&data.display, pipe,
							 output)
				test_flip_to_scaled(&data, index, pipe, output);

		}
	}
	igt_fixture {
		free_fbs(&data);
		if (data.pipe_crc) {
			igt_pipe_crc_stop(data.pipe_crc);
			igt_pipe_crc_free(data.pipe_crc);
			data.pipe_crc = NULL;
		}
		kmstest_set_vt_text_mode();
		igt_display_fini(&data.display);
	}
}
