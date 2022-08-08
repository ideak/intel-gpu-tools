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
#include <limits.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_fb.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION(
   "This test validates the expected behavior of the writeback connectors "
   "feature by checking if the target device support writeback; it validates "
   "bad and good combination, check color format, and check the output result "
   "by using CRC."
);

typedef struct {
	bool builtin_mode;
	bool custom_mode;
	bool list_modes;
	bool dump_check;
	int mode_index;
	drmModeModeInfo user_mode;
} data_t;

static data_t data;

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

static bool check_writeback_config(igt_display_t *display, igt_output_t *output,
				    drmModeModeInfo override_mode)
{
	igt_fb_t input_fb, output_fb;
	igt_plane_t *plane;
	uint32_t writeback_format = DRM_FORMAT_XRGB8888;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	int width, height, ret;

	igt_output_override_mode(output, &override_mode);

	width = override_mode.hdisplay;
	height = override_mode.vdisplay;

	ret = igt_create_fb(display->drm_fd, width, height,
			    DRM_FORMAT_XRGB8888, modifier, &input_fb);
	igt_assert(ret >= 0);

	ret = igt_create_fb(display->drm_fd, width, height,
			    writeback_format, modifier, &output_fb);
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
	enum pipe pipe;

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

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		for_each_pipe(display, pipe) {
			igt_output_set_pipe(output, pipe);

			if (data.custom_mode)
				override_mode = data.user_mode;
			if (data.builtin_mode)
				override_mode = output->config.connector->modes[data.mode_index];

			if (check_writeback_config(display, output, override_mode)) {
				igt_debug("Using connector %u:%s on pipe %d\n",
					  output->config.connector->connector_id,
					  output->name, pipe);
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

static uint64_t get_writeback_fb_id(igt_output_t *output)
{
	return igt_output_get_prop(output, IGT_CONNECTOR_WRITEBACK_FB_ID);
}

static void detach_crtc(igt_display_t *display, igt_output_t *output)
{
	if (get_writeback_fb_id(output) == 0)
		return;

	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, COMMIT_ATOMIC);
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

static void fill_fb(igt_fb_t *fb, uint32_t pixel)
{
	uint32_t *ptr;
	int64_t pixel_count, i;

	igt_assert(fb->drm_format == DRM_FORMAT_XRGB8888);

	ptr = igt_fb_map_buffer(fb->fd, fb);
	igt_assert(ptr);

	pixel_count = fb->strides[0] * fb->height / sizeof(uint32_t);
	for (i = 0; i < pixel_count; i++)
		ptr[i] = cpu_to_le32(pixel);

	igt_fb_unmap_buffer(fb, ptr);
}

static void get_and_wait_out_fence(igt_output_t *output)
{
	int ret;

	igt_assert(output->writeback_out_fence_fd >= 0);

	ret = sync_fence_wait(output->writeback_out_fence_fd, 1000);
	igt_assert_f(ret == 0, "sync_fence_wait failed: %s\n", strerror(-ret));
	close(output->writeback_out_fence_fd);
	output->writeback_out_fence_fd = -1;
}

static void writeback_sequence(igt_output_t *output, igt_plane_t *plane,
				igt_fb_t *in_fb, igt_fb_t *out_fbs[], int n_commits)
{
	int i = 0;
	uint32_t in_fb_colors[2] = { 0x42ff0000, 0x4200ff00 };
	uint32_t clear_color = 0xffffffff;

	igt_crc_t cleared_crc, out_expected;

	for (i = 0; i < n_commits; i++) {
		/* Change the input color each time */
		fill_fb(in_fb, in_fb_colors[i % 2]);

		if (out_fbs[i]) {
			igt_crc_t out_before;

			/* Get the expected CRC */
			igt_fb_get_fnv1a_crc(in_fb, &out_expected);
			fill_fb(out_fbs[i], clear_color);

			if (i == 0)
				igt_fb_get_fnv1a_crc(out_fbs[i], &cleared_crc);
			igt_fb_get_fnv1a_crc(out_fbs[i], &out_before);
			igt_assert_crc_equal(&cleared_crc, &out_before);
		}

		/* Commit */
		igt_plane_set_fb(plane, in_fb);
		igt_output_set_writeback_fb(output, out_fbs[i]);

		igt_display_commit_atomic(output->display,
					  DRM_MODE_ATOMIC_ALLOW_MODESET,
					  NULL);
		if (out_fbs[i])
			get_and_wait_out_fence(output);

		/* Make sure the old output buffer is untouched */
		if (i > 0 && out_fbs[i - 1] && out_fbs[i] != out_fbs[i - 1]) {
			igt_crc_t out_prev;
			igt_fb_get_fnv1a_crc(out_fbs[i - 1], &out_prev);
			igt_assert_crc_equal(&cleared_crc, &out_prev);
		}

		/* Make sure this output buffer is written */
		if (out_fbs[i]) {
			igt_crc_t out_after;
			igt_fb_get_fnv1a_crc(out_fbs[i], &out_after);
			igt_assert_crc_equal(&out_expected, &out_after);

			/* And clear it, for the next time */
			fill_fb(out_fbs[i], clear_color);
		}
	}
}

static void writeback_check_output(igt_output_t *output, igt_plane_t *plane,
				   igt_fb_t *input_fb, igt_fb_t *output_fb)
{
	igt_fb_t *out_fbs[2] = { 0 };
	igt_fb_t second_out_fb;
	unsigned int fb_id;

	/* One commit, with a writeback. */
	writeback_sequence(output, plane, input_fb, &output_fb, 1);

	/* Two commits, the second with no writeback */
	out_fbs[0] = output_fb;
	writeback_sequence(output, plane, input_fb, out_fbs, 2);

	/* Two commits, both with writeback */
	out_fbs[1] = output_fb;
	writeback_sequence(output, plane, input_fb, out_fbs, 2);

	fb_id = igt_create_fb(output_fb->fd, output_fb->width, output_fb->height,
			      DRM_FORMAT_XRGB8888,
			      igt_fb_mod_to_tiling(0),
			      &second_out_fb);
	igt_require(fb_id > 0);

	/* Two commits, with different writeback buffers */
	out_fbs[1] = &second_out_fb;
	writeback_sequence(output, plane, input_fb, out_fbs, 2);

	igt_remove_fb(output_fb->fd, &second_out_fb);
}

static void do_single_commit(igt_output_t *output, igt_plane_t *plane, igt_fb_t *in_fb,
			      igt_fb_t *out_fb)
{
	uint32_t in_fb_color = 0xffff0000;

	fill_fb(in_fb, in_fb_color);

	igt_plane_set_fb(plane, in_fb);
	igt_output_set_writeback_fb(output, out_fb);

	igt_display_commit_atomic(output->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (out_fb)
		get_and_wait_out_fence(output);
}

static void commit_and_dump_fb(igt_display_t *display, igt_output_t *output, igt_plane_t *plane,
			        igt_fb_t *input_fb, drmModeModeInfo *mode)
{
	cairo_surface_t *fb_surface_out;
	char filepath_out[PATH_MAX];
	cairo_status_t status;
	char *path_name;
	char *file_name;
	unsigned int fb_id;
	igt_fb_t output_fb;

	path_name = getenv("IGT_FRAME_DUMP_PATH");
	file_name = getenv("FRAME_PNG_FILE_NAME");
	fb_id = igt_create_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
				igt_fb_mod_to_tiling(0), &output_fb);
	igt_require(fb_id > 0);

	do_single_commit(output, plane, input_fb, &output_fb);

	fb_surface_out = igt_get_cairo_surface(display->drm_fd, &output_fb);
	snprintf(filepath_out, PATH_MAX, "%s/%s.png", path_name, file_name);
	status = cairo_surface_write_to_png(fb_surface_out, filepath_out);
	igt_assert_eq(status, CAIRO_STATUS_SUCCESS);

	igt_remove_fb(display->drm_fd, &output_fb);
}

static igt_output_t *list_writeback_modes(igt_display_t *display)
{
	for (int i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (output->config.connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			igt_info("\tname  vref hdis hss hse htot vdis vss vse vtot flags type clock\n");
			for (int j = 0; j < output->config.connector->count_modes; j++) {
				igt_info("[%d]", j);
				kmstest_dump_mode(&output->config.connector->modes[j]);
			}
			break;
		}
	}
	return NULL;
}

static int opt_handler(int option, int option_index, void *_data)
{
	switch (option) {
	case 'l':
		data.list_modes = true;
		break;
	case 'b':
		data.builtin_mode = true;
		data.mode_index = atoi(optarg);
		break;
	case 'c':
		data.custom_mode = true;
		if (!igt_parse_mode_string(optarg, &data.user_mode))
			return IGT_OPT_HANDLER_ERROR;
		break;
	case 'd':
		data.dump_check = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}
	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	" --list-modes | -l List of writeback connector modes\n"
	" --built-in | -b Commits a built-in mode\n"
	" --custom | -c Commits a custom mode inputted by user"
	" <clock MHz>,<hdisp>,<hsync-start>,<hsync-end>,<htotal>,"
	"<vdisp>,<vsync-start>,<vsync-end>,<vtotal>\n"
	" --dump | -d Prints buffer to file location $IGT_FRAME_DUMP_PATH"
	"/$FRAME_PNG_FILE_NAME "
	"before running dump. Will skip all other tests.\n";

static const struct option long_options[] = {
	{ .name = "list-modes", .has_arg = false, .val = 'l', },
	{ .name = "built-in", .has_arg = true, .val = 'b', },
	{ .name = "custom", .has_arg = true, .val = 'c', },
	{ .name = "dump", .has_arg = false, .val = 'd', },
	{}
};

igt_main_args("b:c:dl", long_options, help_str, opt_handler, NULL)
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

		if (data.list_modes)
			list_writeback_modes(&display);
		if (data.dump_check)
			commit_and_dump_fb(&display, output, plane, &input_fb, &mode);
	}
	/*
	 * When dump_check or list_modes flag is high, then the following subtests will be skipped
	 * as we do not want to do CRC validation.
	 */
	igt_describe("Check the writeback format");
	igt_subtest("writeback-pixel-formats") {
		unsigned int i;
		char *c;
		drmModePropertyBlobRes *formats_blob;
		const char *valid_chars;

		igt_skip_on(data.dump_check || data.list_modes);
		formats_blob = get_writeback_formats_blob(output);
		valid_chars = "01234568 ABCGNRUVXY";

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

		igt_skip_on(data.dump_check || data.list_modes);
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

		igt_skip_on(data.dump_check || data.list_modes);
		fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR,
				      &output_fb);
		igt_require(fb_id > 0);

		writeback_fb_id(output, &input_fb, &output_fb);

		igt_remove_fb(display.drm_fd, &output_fb);
	}

	igt_describe("Check writeback output with CRC validation");
	igt_subtest("writeback-check-output") {
		igt_fb_t output_fb;

		igt_skip_on(data.dump_check || data.list_modes);
		fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
				      DRM_FORMAT_XRGB8888,
				      igt_fb_mod_to_tiling(0),
				      &output_fb);
		igt_require(fb_id > 0);

		writeback_check_output(output, plane, &input_fb, &output_fb);

		igt_remove_fb(display.drm_fd, &output_fb);
	}

	igt_fixture {
		detach_crtc(&display, output);
		igt_remove_fb(display.drm_fd, &input_fb);
		igt_display_fini(&display);
	}
}
