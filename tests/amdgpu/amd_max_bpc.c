/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "igt.h"
#include <fcntl.h>

IGT_TEST_DESCRIPTION("Test 4K HDMI regression if max bpc is too high");

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_pipe_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	igt_pipe_crc_t *pipe_crc_dither;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int fd;
	int w;
	int h;
} data_t;

static drmModeModeInfo uhd_mode = {
	  594000,
	  3840, 4016, 4104, 4400, 0,
	  2160, 2168, 2178, 2250, 0,
	  60, 0x5|DRM_MODE_FLAG_PIC_AR_64_27,
	  0x40,
	  "3840x2160@60", /* VIC 107 */
	  };

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = &data->display.pipes[data->pipe_id];

	igt_display_reset(display);

	/* find a connected HDMI output */
	data->output = NULL;
	for (int i=0; i < data->display.n_outputs; ++i) {
		drmModeConnector *connector = data->display.outputs[i].config.connector;
		if (connector->connection == DRM_MODE_CONNECTED &&
				(connector->connector_type == DRM_MODE_CONNECTOR_HDMIA)) {
			data->output = &data->display.outputs[i];
		}
	}
	igt_require_f(data->output, "Requires connected HDMI output\n");

	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
	igt_output_override_mode(data->output, &uhd_mode);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);
	igt_assert_output_bpc_equal(data->fd, data->pipe_id,
				    data->output->name, 8);

	data->primary =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	data->pipe_crc = igt_pipe_crc_new(data->fd, data->pipe_id,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_output_set_pipe(data->output, data->pipe_id);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	igt_display_reset(&data->display);
}

static void test_4k_mode_max_bpc(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_fb_t rfb;
	int max_bpc = 16;

	test_init(data);

	igt_info("Setting output max bpc to %d\n", max_bpc);

	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, max_bpc);

	igt_create_pattern_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888, 0, &rfb);
	igt_plane_set_fb(data->primary, &rfb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_remove_fb(data->fd, &rfb);
	test_fini(data);
}

igt_main
{
	data_t data;

	igt_skip_on_simulation();

	memset(&data, 0, sizeof(data));

	igt_fixture
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Tests overly high 'max bpc' should not affect 4K modes on HDMI");
	igt_subtest("4k-mode-max-bpc") test_4k_mode_max_bpc(&data);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
