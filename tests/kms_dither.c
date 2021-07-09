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
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *     Bhanuprakash Modem <bhanuprakash.modem@intel.com>
 *
 */

#include "igt.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

IGT_TEST_DESCRIPTION("Test Dithering block status");

/* Connector BPC */
#define IGT_CONNECTOR_BPC_6		6
#define IGT_CONNECTOR_BPC_8		8
#define IGT_CONNECTOR_BPC_10		10

/* Framebuffer BPC */
#define IGT_FRAME_BUFFER_BPC_8		8
#define IGT_FRAME_BUFFER_BPC_10		10
#define IGT_FRAME_BUFFER_BPC_16		16

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_pipe_t *pipe;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int drm_fd;
	igt_fb_t fb;
} data_t;

typedef struct {
	unsigned int bpc;
	unsigned int dither;
} dither_status_t;

/* Prepare test data. */
static void prepare_test(data_t *data, igt_output_t *output, enum pipe pipe)
{
	igt_display_t *display = &data->display;

	data->pipe_id = pipe;
	data->pipe = &data->display.pipes[data->pipe_id];
	igt_assert(data->pipe);

	igt_display_reset(display);

	data->output = output;
	igt_assert(data->output);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);

	data->primary =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_pipe(data->output, data->pipe_id);
}

/* Returns the current state of dithering from the crtc debugfs. */
static dither_status_t get_dither_state(data_t *data)
{
	char buf[512], tmp[5];
	char *start_loc;
	int dir, res;
	dither_status_t status;

	dir = igt_debugfs_dir(data->drm_fd);
	igt_assert(dir >= 0);

	res = igt_debugfs_simple_read(dir, "i915_display_info", buf, sizeof(buf));
	igt_require(res > 0);
	close(dir);

	igt_assert(start_loc = strstr(buf, ", bpp="));
	igt_assert_eq(sscanf(start_loc, ", bpp=%u", &status.bpc), 1);
	status.bpc /= 3;

	igt_assert(start_loc = strstr(buf, ", dither="));
	igt_assert_eq(sscanf(start_loc, ", dither=%s", tmp), 1);
	status.dither = !strcmp(tmp, "yes,");

	return status;
}

static void test_dithering(data_t *data, enum pipe pipe,
			   igt_output_t *output,
			   int fb_bpc, int fb_format,
			   int output_bpc)
{
	igt_display_t *display = &data->display;
	dither_status_t status;
	int bpc, ret;

	igt_info("Dithering test execution on %s PIPE_%s\n",
			output->name, kmstest_pipe_name(pipe));
	prepare_test(data, output, pipe);

	igt_assert(igt_create_fb(data->drm_fd, data->mode->hdisplay,
				 data->mode->vdisplay, fb_format,
				 LOCAL_DRM_FORMAT_MOD_NONE, &data->fb));
	igt_plane_set_fb(data->primary, &data->fb);
	igt_plane_set_size(data->primary, data->mode->hdisplay, data->mode->vdisplay);

	bpc = igt_output_get_prop(output, IGT_CONNECTOR_MAX_BPC);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, output_bpc);

	if (display->is_atomic)
		ret = igt_display_try_commit_atomic(display,
					DRM_MODE_ATOMIC_TEST_ONLY |
					DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL);
	else
		ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	igt_require_f(!ret, "%s don't support %d-bpc\n",
				output->name, output_bpc);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	/*
	 * Check the status of Dithering block:
	 *
	 * Preserve the result & compute later (after clean-up).
	 * If fb_bpc is greater than output_bpc, Dithering should be enabled
	 * Else disabled
	 */
	status = get_dither_state(data);

	igt_info("FB BPC:%d, Panel BPC:%d, Pipe BPC:%d, Expected Dither:%s, Actual result:%s\n",
		  fb_bpc, output_bpc, status.bpc,
		  (fb_bpc > output_bpc) ? "Enable": "Disable",
		  status.dither ? "Enable": "Disable");

       /*
	* We must update the Connector max_bpc property back
	* Otherwise, previously updated value will stay forever and
	* may cause the failures for next/other subtests.
	*/
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, bpc);
	igt_plane_set_fb(data->primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_remove_fb(data->drm_fd, &data->fb);

	/* Check if crtc bpc is updated with requested one. */
	igt_require_f((status.bpc == output_bpc),
			"%s can support max %u-bpc, but requested %d-bpc\n",
				output->name, status.bpc, output_bpc);

	/* Compute the result. */
	if (fb_bpc > output_bpc)
		igt_assert_f(status.dither, "(fb_%dbpc > output_%dbpc): Dither should be enabled\n",
				fb_bpc, output_bpc);
	else
		igt_assert_f(!status.dither, "(fb_%dbpc <= output_%dbpc): Dither should be disabled\n",
				fb_bpc, output_bpc);

	return;
}

/* Returns true if an output supports max bpc property. */
static bool is_supported(igt_output_t *output)
{
        return igt_output_has_prop(output, IGT_CONNECTOR_MAX_BPC) &&
		igt_output_get_prop(output, IGT_CONNECTOR_MAX_BPC);
}

static void
run_dither_test(data_t *data, int fb_bpc, int fb_format, int output_bpc)
{
	igt_output_t *output;
	igt_display_t *display = &data->display;

	for_each_connected_output(display, output) {
		enum pipe pipe;

		if (!is_supported(output))
			continue;

		for_each_pipe(display, pipe) {
			if (igt_pipe_connector_valid(pipe, output)) {
				igt_dynamic_f("%s-pipe-%s", output->name, kmstest_pipe_name(pipe))
					test_dithering(data, pipe, output, fb_bpc,
							fb_format, output_bpc);

				/* One pipe is enough */
				break;
			}
		}
	}
}

igt_main
{
	struct {
		int fb_bpc;
		int format;
		int output_bpc;
	} tests[] = {
		{ IGT_FRAME_BUFFER_BPC_8, DRM_FORMAT_XRGB8888, IGT_CONNECTOR_BPC_6 },
		{ IGT_FRAME_BUFFER_BPC_8, DRM_FORMAT_XRGB8888, IGT_CONNECTOR_BPC_8 },
	};
	int i;
	data_t data = { 0 };

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe_f("Framebuffer BPC:%d, Panel BPC:%d, Expected Dither:%s\n",
			       tests[i].fb_bpc, tests[i].output_bpc,
			       (tests[i].fb_bpc > tests[i].output_bpc) ?
							"Enable": "Disable");

		igt_subtest_with_dynamic_f("FB-%dBPC-Vs-Panel-%dBPC",
				tests[i].fb_bpc, tests[i].output_bpc)
			run_dither_test(&data,
					tests[i].fb_bpc,
					tests[i].format,
					tests[i].output_bpc);
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
