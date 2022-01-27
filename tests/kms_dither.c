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
	int drm_fd;
	igt_fb_t fb;
} data_t;

typedef struct {
	unsigned int bpc;
	unsigned int dither;
} dither_status_t;

/* Prepare test data. */
static void prepare_test(data_t *data, igt_output_t *output, enum pipe p)
{
	igt_display_t *display = &data->display;
	igt_pipe_t *pipe = &data->display.pipes[p];

	igt_assert(pipe);

	igt_display_reset(display);

	data->primary =
		igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_pipe(output, p);
}

/* Returns the current state of dithering from the crtc debugfs. */
static dither_status_t get_dither_state(data_t *data, enum pipe pipe)
{
	char buf[512], tmp[5];
	char *start_loc;
	int dir, res;
	dither_status_t status;

	dir = igt_debugfs_dir(data->drm_fd);
	igt_assert(dir >= 0);

	igt_require_intel(data->drm_fd);

	res = igt_debugfs_simple_read(dir, "i915_display_info", buf, sizeof(buf));
	igt_require(res > 0);
	close(dir);

	igt_assert(start_loc = strstr(buf, ", dither="));
	igt_assert_eq(sscanf(start_loc, ", dither=%s", tmp), 1);
	status.dither = !strcmp(tmp, "yes,");

	status.bpc = igt_get_pipe_current_bpc(data->drm_fd, pipe);

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
	bool constraint;

	igt_info("Dithering test execution on %s PIPE_%s\n",
			output->name, kmstest_pipe_name(pipe));
	prepare_test(data, output, pipe);

	igt_assert(igt_create_fb(data->drm_fd, 512, 512, fb_format,
				 DRM_FORMAT_MOD_LINEAR, &data->fb));
	igt_plane_set_fb(data->primary, &data->fb);

	bpc = igt_output_get_prop(output, IGT_CONNECTOR_MAX_BPC);
	igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, output_bpc);

	if (display->is_atomic)
		ret = igt_display_try_commit_atomic(display,
					DRM_MODE_ATOMIC_TEST_ONLY |
					DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL);
	else
		ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	if (ret)
		goto cleanup;

	constraint = igt_max_bpc_constraint(display, pipe, output, output_bpc);
	if (!constraint)
		goto cleanup;

	/*
	 * Check the status of Dithering block:
	 *
	 * Preserve the result & compute later (after clean-up).
	 * If fb_bpc is greater than output_bpc, Dithering should be enabled
	 * Else disabled
	 */
	status = get_dither_state(data, pipe);

	igt_info("FB BPC:%d, Panel BPC:%d, Pipe BPC:%d, Expected Dither:%s, Actual result:%s\n",
		  fb_bpc, output_bpc, status.bpc,
		  (fb_bpc > output_bpc) ? "Enable": "Disable",
		  status.dither ? "Enable": "Disable");

       /*
	* We must update the Connector max_bpc property back
	* Otherwise, previously updated value will stay forever and
	* may cause the failures for next/other subtests.
	*/
cleanup:
	igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, bpc);
	igt_plane_set_fb(data->primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_remove_fb(data->drm_fd, &data->fb);

	igt_require_f(!ret, "%s don't support %d-bpc\n", output->name, output_bpc);
	igt_require_f(constraint, "No supported mode found to use %d-bpc on %s\n",
				  output_bpc, output->name);

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

		if (igt_get_output_max_bpc(data->drm_fd, output->name) < output_bpc)
			continue;

		for_each_pipe(display, pipe) {
			if (igt_pipe_connector_valid(pipe, output)) {
				igt_dynamic_f("pipe-%s-%s",
					      kmstest_pipe_name(pipe), output->name)
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
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
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
		close(data.drm_fd);
	}
}
