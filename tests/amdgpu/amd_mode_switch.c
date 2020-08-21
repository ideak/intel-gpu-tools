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
#include <xf86drmMode.h>

#define MAX_PIPES 6

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary[MAX_PIPES];
	igt_output_t *output[MAX_PIPES];
	int fd;
} data_t;

static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	int i;

	for_each_pipe(display, i) {
		igt_output_t *output = &display->outputs[i];

		data->primary[i] = igt_pipe_get_plane_type(
			&data->display.pipes[i], DRM_PLANE_TYPE_PRIMARY);

		data->output[i] = output;
	}

	igt_require(data->output[0]);
	igt_display_reset(display);
}

static void test_fini(data_t *data)
{
	igt_display_t *display = &data->display;

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

/* Forces a mode for a connector. */
static void force_output_mode(data_t *d, igt_output_t *output,
			      const drmModeModeInfo *mode)
{
	/* This allows us to create a virtual sink. */
	if (!igt_output_is_connected(output)) {
		kmstest_force_edid(d->fd, output->config.connector,
				   igt_kms_get_4k_edid());

		kmstest_force_connector(d->fd, output->config.connector,
					FORCE_CONNECTOR_DIGITAL);
	}

	igt_output_override_mode(output, mode);
}

static void run_mode_switch_first_last(data_t *data, int num_pipes)
{
	igt_output_t *output;
	struct igt_fb *buffer1[MAX_PIPES] = { NULL };
	struct igt_fb *buffer2[MAX_PIPES] = { NULL };
	drmModeConnectorPtr conn;
	drmModeModeInfoPtr kmode;
	void *user_data = NULL;
	int i = 0;
	int j = 0;

	test_init(data);

	igt_skip_on_f(num_pipes > igt_display_get_n_pipes(&data->display) ||
			      num_pipes > data->display.n_outputs,
		      "ASIC does not have %d outputs/pipes\n", num_pipes);

	/* First supported mode */

	for (j = 0; j < num_pipes; j++) {
		output = data->output[j];
		if (!igt_output_is_connected(output))
			continue;

		conn = drmModeGetConnector(
			data->fd, output->config.connector->connector_id);

		kmode = &conn->modes[0];
		if (buffer1[j] == NULL) {
			igt_fb_t fb;
			buffer1[j] = &fb;
			igt_create_color_fb(data->fd, kmode->hdisplay,
					    kmode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_NONE, 1.f, 0.f,
					    0.f, buffer1[j]);
		}
		igt_output_set_pipe(output, j);
		force_output_mode(data, output, kmode);
		igt_plane_set_fb(data->primary[j], buffer1[j]);
		drmModeFreeConnector(conn);
	}

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET,
				  user_data);

	/* Last supported mode */

	for (j = 0; j < num_pipes; j++) {
		output = data->output[j];
		if (!igt_output_is_connected(output))
			continue;

		conn = drmModeGetConnector(
			data->fd, output->config.connector->connector_id);

		kmode = &conn->modes[conn->count_modes - 1];
		if (buffer2[j] == NULL) {
			igt_fb_t fb;
			buffer2[j] = &fb;
			igt_create_color_fb(data->fd, kmode->hdisplay,
					    kmode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_NONE, 1.f, 0.f,
					    0.f, buffer2[j]);
		}
		force_output_mode(data, output, kmode);
		igt_plane_set_fb(data->primary[j], buffer2[j]);
		drmModeFreeConnector(conn);
	}

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET,
				  user_data);

	/* First supported again */
	for (j = 0; j < num_pipes; j++) {
		output = data->output[j];
		if (!igt_output_is_connected(output))
			continue;

		conn = drmModeGetConnector(
			data->fd, output->config.connector->connector_id);

		kmode = &conn->modes[0];
		force_output_mode(data, output, kmode);
		igt_plane_set_fb(data->primary[j], buffer1[j]);
		drmModeFreeConnector(conn);
	}

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET,
				  user_data);

	test_fini(data);

	for (i = 0; i <= num_pipes; i++) {
		igt_remove_fb(data->fd, buffer1[i]);
		igt_remove_fb(data->fd, buffer2[i]);
	}
}

IGT_TEST_DESCRIPTION("Test switching between supported modes");
igt_main
{
	data_t data;
	int i = 0;

	igt_skip_on_simulation();

	memset(&data, 0, sizeof(data));

	igt_fixture
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(&data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	for (i = 0; i < MAX_PIPES; i++) {
		igt_describe(
			"Test between switching highest and lowest supported mode");
		igt_subtest_f("mode-switch-first-last-pipe-%d", i)
			run_mode_switch_first_last(&data, i + 1);
	}

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
