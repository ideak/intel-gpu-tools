/*
 * (C) COPYRIGHT 2017 ARM Limited. All rights reserved.
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

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_fb.h"

static drmModePropertyBlobRes *get_writeback_formats_blob(igt_output_t *output)
{
	drmModePropertyBlobRes *blob = NULL;
	uint64_t blob_id;
	int ret;

	ret = kmstest_get_property(output->display->drm_fd,
				   output->config.connector->connector_id,
				   DRM_MODE_OBJECT_CONNECTOR,
				   igt_connector_prop_names[IGT_CONNECTOR_WRITEBACK_PIXEL_FORMATS],
				   NULL, &blob_id, NULL);
	if (ret)
		blob = drmModeGetPropertyBlob(output->display->drm_fd, blob_id);

	igt_assert(blob);

	return blob;
}

static bool check_writeback_config(igt_display_t *display, igt_output_t *output)
{
	igt_fb_t input_fb, output_fb;
	igt_plane_t *plane;
	uint32_t writeback_format = DRM_FORMAT_XRGB8888;
	uint64_t tiling = DRM_FORMAT_MOD_LINEAR;
	int width, height, ret;
	drmModeModeInfo override_mode = {
		.clock = 25175,
		.hdisplay = 640,
		.hsync_start = 656,
		.hsync_end = 752,
		.htotal = 800,
		.hskew = 0,
		.vdisplay = 480,
		.vsync_start = 490,
		.vsync_end = 492,
		.vtotal = 525,
		.vscan = 0,
		.vrefresh = 60,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
		.name = {"640x480-60"},
	};
	igt_output_override_mode(output, &override_mode);

	width = override_mode.hdisplay;
	height = override_mode.vdisplay;

	ret = igt_create_fb(display->drm_fd, width, height, DRM_FORMAT_XRGB8888, tiling, &input_fb);
	igt_assert(ret >= 0);

	ret = igt_create_fb(display->drm_fd, width, height, writeback_format, tiling, &output_fb);
	igt_assert(ret >= 0);

	plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(plane, &input_fb);
	igt_output_set_writeback_fb(output, &output_fb);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_plane_set_fb(plane, NULL);
	igt_remove_fb(display->drm_fd, &input_fb);
	igt_remove_fb(display->drm_fd, &output_fb);

	return !ret;
}

static igt_output_t *kms_writeback_get_output(igt_display_t *display)
{
	int i;

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];
		int j;

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		for (j = 0; j < igt_display_get_n_pipes(display); j++) {
			igt_output_set_pipe(output, j);

			if (check_writeback_config(display, output)) {
				igt_debug("Using connector %u:%s on pipe %d\n",
					  output->config.connector->connector_id,
					  output->name, j);
				return output;
			}
		}

		igt_debug("We found %u:%s, but this test will not be able to use it.\n",
			  output->config.connector->connector_id, output->name);

		/* Restore any connectors we don't use, so we don't trip on them later */
		kmstest_force_connector(display->drm_fd, output->config.connector, FORCE_CONNECTOR_UNSPECIFIED);
	}

	return NULL;
}

static void check_writeback_fb_id(igt_output_t *output)
{
	uint64_t check_fb_id;

	check_fb_id = igt_output_get_prop(output, IGT_CONNECTOR_WRITEBACK_FB_ID);
	igt_assert(check_fb_id == 0);
}

static int do_writeback_test(igt_output_t *output, uint32_t fb_id,
			     int32_t *out_fence_ptr, bool ptr_valid)
{
	int ret;
	igt_display_t *display = output->display;
	struct kmstest_connector_config *config = &output->config;

	igt_output_set_prop_value(output, IGT_CONNECTOR_CRTC_ID, config->crtc->crtc_id);
	igt_output_set_prop_value(output, IGT_CONNECTOR_WRITEBACK_FB_ID, fb_id);
	igt_output_set_prop_value(output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR, to_user_pointer(out_fence_ptr));

	if (ptr_valid)
		*out_fence_ptr = 0;

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	if (ptr_valid)
		igt_assert(*out_fence_ptr == -1);

	/* WRITEBACK_FB_ID must always read as zero */
	check_writeback_fb_id(output);

	return ret;
}

static void test_invalid_parameters(igt_output_t *output, igt_fb_t *valid_fb, igt_fb_t *invalid_fb)
{
	int i, ret;
	int32_t out_fence;
	struct {
		uint32_t fb_id;
		bool ptr_valid;
		int32_t *out_fence_ptr;
	} invalid_tests[] = {
		{
			/* No output buffer, but the WRITEBACK_OUT_FENCE_PTR set. */
			.fb_id = 0,
			.ptr_valid = true,
			.out_fence_ptr = &out_fence,
		},
		{
			/* Invalid output buffer. */
			.fb_id = invalid_fb->fb_id,
			.ptr_valid = true,
			.out_fence_ptr = &out_fence,
		},
		{
			/* Invalid WRITEBACK_OUT_FENCE_PTR. */
			.fb_id = valid_fb->fb_id,
			.ptr_valid = false,
			.out_fence_ptr = (int32_t *)0x8,
		},
	};

	for (i = 0; i < ARRAY_SIZE(invalid_tests); i++) {
		ret = do_writeback_test(output, invalid_tests[i].fb_id,
					invalid_tests[i].out_fence_ptr,
					invalid_tests[i].ptr_valid);
		igt_assert(ret != 0);
	}
}

static void writeback_fb_id(igt_output_t *output, igt_fb_t *valid_fb, igt_fb_t *invalid_fb)
{

	int ret;

	/* Invalid object for WRITEBACK_FB_ID */
	ret = do_writeback_test(output, output->id, NULL, false);
	igt_assert(ret == -EINVAL);

	/* Zero WRITEBACK_FB_ID */
	ret = do_writeback_test(output, 0, NULL, false);
	igt_assert(ret == 0);

	/* Valid output buffer */
	ret = do_writeback_test(output, valid_fb->fb_id, NULL, false);
	igt_assert(ret == 0);
}

igt_main
{
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *plane;
	igt_fb_t input_fb;
	drmModeModeInfo mode;
	unsigned int fb_id;

	memset(&display, 0, sizeof(display));

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&display, display.drm_fd);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, display.drm_fd);

		igt_require(display.is_atomic);

		output = kms_writeback_get_output(&display);
		igt_require(output);

		if (output->use_override_mode)
			memcpy(&mode, &output->override_mode, sizeof(mode));
		else
			memcpy(&mode, &output->config.default_mode, sizeof(mode));

		plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_assert(plane);

		fb_id = igt_create_fb(display.drm_fd, mode.hdisplay,
				      mode.vdisplay,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR,
				      &input_fb);
		igt_assert(fb_id >= 0);
		igt_plane_set_fb(plane, &input_fb);
	}

	igt_describe("Check the writeback format");
	igt_subtest("writeback-pixel-formats") {
		drmModePropertyBlobRes *formats_blob = get_writeback_formats_blob(output);
		const char *valid_chars = "0123456 ABCGNRUVXY";
		unsigned int i;
		char *c;

		/*
		 * We don't have a comprehensive list of formats, so just check
		 * that the blob length is sensible and that it doesn't contain
		 * any outlandish characters
		 */
		igt_assert(!(formats_blob->length % 4));
		c = formats_blob->data;
		for (i = 0; i < formats_blob->length; i++)
			igt_assert_f(strchr(valid_chars, c[i]),
				     "Unexpected character %c\n", c[i]);
		drmModeFreePropertyBlob(formats_blob);
	}

	igt_describe("Writeback has a couple of parameters linked together"
		     "(output framebuffer and fence); this test goes through"
		     "the combination of possible bad options");
	igt_subtest("writeback-invalid-parameters") {
		igt_fb_t invalid_output_fb;
		fb_id = igt_create_fb(display.drm_fd, mode.hdisplay / 2,
				      mode.vdisplay / 2,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR,
				      &invalid_output_fb);
		igt_require(fb_id > 0);

		test_invalid_parameters(output, &input_fb, &invalid_output_fb);

		igt_remove_fb(display.drm_fd, &invalid_output_fb);
	}

	igt_describe("Validate WRITEBACK_FB_ID with valid and invalid options");
	igt_subtest("writeback-fb-id") {
		igt_fb_t output_fb;
		fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR,
				      &output_fb);
		igt_require(fb_id > 0);

		writeback_fb_id(output, &input_fb, &output_fb);

		igt_remove_fb(display.drm_fd, &output_fb);
	}

	igt_fixture {
		igt_remove_fb(display.drm_fd, &input_fb);
		igt_display_fini(&display);
	}
}
