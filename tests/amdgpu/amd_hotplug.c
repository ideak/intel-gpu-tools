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
#include "igt_amd.h"

IGT_TEST_DESCRIPTION("Test simulated hotplugging on connectors");

/* Maximum pipes on any AMD ASIC. */
#define MAX_PIPES 6

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary[MAX_PIPES];
	igt_plane_t *overlay[MAX_PIPES];
	igt_plane_t *cursor[MAX_PIPES];
	igt_output_t *output[MAX_PIPES];
	igt_pipe_t *pipe[MAX_PIPES];
	igt_pipe_crc_t *pipe_crc[MAX_PIPES];
	drmModeModeInfo mode[MAX_PIPES];
	enum pipe pipe_id[MAX_PIPES];
	int w[MAX_PIPES];
	int h[MAX_PIPES];
	int fd;
} data_t;

static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	int i, n, max_pipes = display->n_pipes;

	for_each_pipe(display, i) {
		data->pipe_id[i] = PIPE_A + i;
		data->pipe[i] = &data->display.pipes[data->pipe_id[i]];
		data->primary[i] = igt_pipe_get_plane_type(
			data->pipe[i], DRM_PLANE_TYPE_PRIMARY);
		data->overlay[i] = igt_pipe_get_plane_type_index(
			data->pipe[i], DRM_PLANE_TYPE_OVERLAY, 0);
		data->cursor[i] = igt_pipe_get_plane_type(
			data->pipe[i], DRM_PLANE_TYPE_CURSOR);
		data->pipe_crc[i] =
			igt_pipe_crc_new(data->fd, data->pipe_id[i],
					 IGT_PIPE_CRC_SOURCE_AUTO);
	}

	for (i = 0, n = 0; i < display->n_outputs && n < max_pipes; ++i) {
		igt_output_t *output = &display->outputs[i];

		data->output[n] = output;

		/* Only allow physically connected displays for the tests. */
		if (!igt_output_is_connected(output))
			continue;

		igt_assert(kmstest_get_connector_default_mode(
			data->fd, output->config.connector, &data->mode[n]));

		data->w[n] = data->mode[n].hdisplay;
		data->h[n] = data->mode[n].vdisplay;

		n += 1;
	}

	igt_require(data->output[0]);
	igt_display_reset(display);
}

static void test_fini(data_t *data)
{
	igt_display_t *display = &data->display;
	int i;

	for_each_pipe(display, i) {
		igt_pipe_crc_free(data->pipe_crc[i]);
	}

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

static void test_hotplug_basic(data_t *data, bool suspend)
{
	igt_output_t *output;
	igt_fb_t ref_fb[MAX_PIPES];
	igt_crc_t ref_crc[MAX_PIPES], new_crc[MAX_PIPES];
	igt_display_t *display = &data->display;
	int i = 0;

	test_init(data);

	/* Setup all outputs */
	for (i = 0; i < display->n_pipes; i++) {
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_create_pattern_fb(data->fd, data->w[i], data->h[i],
				      DRM_FORMAT_XRGB8888, 0, &ref_fb[i]);
		igt_output_set_pipe(output, data->pipe_id[i]);
		igt_plane_set_fb(data->primary[i], &ref_fb[i]);
	}
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	/* Collect reference CRCs */
	for (i = 0; i < display->n_pipes; i++) {
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_pipe_crc_collect_crc(data->pipe_crc[i], &ref_crc[i]);
	}

	if (suspend) {
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
	}

	/* Trigger hotplug and confirm reference image is the same. */
	for (i = 0; i < display->n_pipes; i++) {
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_amd_trigger_hotplug(data->fd, output->name);

		igt_pipe_crc_collect_crc(data->pipe_crc[i], &new_crc[i]);
		igt_assert_crc_equal(&ref_crc[i], &new_crc[i]);
		igt_remove_fb(data->fd, &ref_fb[i]);
	}

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

		igt_amd_require_hpd(&data.display, data.fd);
	}

	igt_describe("Tests HPD on each connected output");
	igt_subtest("basic") test_hotplug_basic(&data, false);

	igt_describe("Tests HPD on each connected output after a suspend sequence");
	igt_subtest("basic-suspend") test_hotplug_basic(&data, true);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
