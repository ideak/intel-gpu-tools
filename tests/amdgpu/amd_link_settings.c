/*
 * Copyright 2020 Advanced Micro Devices, Inc.
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

typedef struct
{
	int drm_fd;
        igt_display_t display;
        igt_plane_t *primary;
        igt_output_t *output;
        igt_fb_t fb;
	igt_pipe_t *pipe;
        enum pipe pipe_id;
	int connector_type;
	int w, h;
} data_t;

const enum dc_lane_count lane_count_values[] =
{
	LANE_COUNT_ONE,
	LANE_COUNT_TWO,
	LANE_COUNT_FOUR,
};

const enum dc_link_rate dp_link_rate_values[] =
{
	LINK_RATE_LOW,
	LINK_RATE_HIGH,
	LINK_RATE_HIGH2,
	LINK_RATE_HIGH3
};

const enum dc_link_rate edp_link_rate_values[] =
{
	LINK_RATE_LOW,
	LINK_RATE_HIGH,
	LINK_RATE_RBR2,
	LINK_RATE_HIGH2
};

static void test_fini(data_t *data)
{
	igt_display_reset(&data->display);
}

static void set_all_output_pipe_to_none(data_t *data)
{
	igt_output_t *output;

	for_each_connected_output(&data->display, output) {
		igt_output_set_pipe(output, PIPE_NONE);
	}

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static void test_init(data_t *data, igt_output_t *output)
{
	enum pipe pipe;

	igt_require(output->config.connector->count_modes >= 1);

	set_all_output_pipe_to_none(data);

	for_each_pipe(&data->display, pipe) {
		if (igt_pipe_connector_valid(pipe, output)) {
			data->pipe_id = pipe;
			break;
		}
	}

	data->connector_type = output->config.connector->connector_type;

	igt_require(data->pipe_id != PIPE_NONE);

	data->pipe = &data->display.pipes[data->pipe_id];

	igt_output_set_pipe(output, data->pipe_id);

	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
}

static void run_link_training_config(data_t *data, igt_output_t *output)
{
	int lane_count[4], link_rate[4], link_spread[4];
	int max_lc, max_lr;
	const int current = 0;
	const int verified = 1;
	char *connector_name = output->name;
	const enum dc_link_rate *link_rate_values;
	int num_link_rates;
	if (data->connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
		link_rate_values = dp_link_rate_values;
		num_link_rates = ARRAY_SIZE(dp_link_rate_values);
	} else if (data->connector_type == DRM_MODE_CONNECTOR_eDP) {
		link_rate_values = edp_link_rate_values;
		num_link_rates = ARRAY_SIZE(edp_link_rate_values);
	} else {
		igt_info("Not a DP or eDP connector\n");
		return;
	}

	igt_amd_read_link_settings(data->drm_fd, connector_name, lane_count,
			           link_rate, link_spread);

	max_lc = lane_count[verified];
	max_lr = link_rate[verified];

	for (int i = 0; i < ARRAY_SIZE(lane_count_values); i++)
	{
		if (lane_count_values[i] > max_lc)
			continue;

		for (int j = 0; j < num_link_rates; j++)
		{
			if (link_rate_values[j] > max_lr)
				continue;

			/* Write link settings */
			igt_info("Applying lane count: %d, link rate 0x%02x, on default training\n",
				  lane_count_values[i], link_rate_values[j]);
			igt_amd_write_link_settings(data->drm_fd, connector_name,
						    lane_count_values[i],
						    link_rate_values[j],
						    LINK_TRAINING_DEFAULT);

			/* Verify */
			igt_amd_read_link_settings(data->drm_fd, connector_name,
						   lane_count, link_rate,
						   link_spread);

			igt_info("Trained lane count: %d; link rate: 0x%02x\n",
				lane_count[current], link_rate[current]);
			igt_assert(lane_count[current] == lane_count_values[i]);
			igt_assert(link_rate[current] == link_rate_values[j]);

		}
	}
}

static void test_link_training_configs(data_t *data)
{
	const drmModeModeInfo *orig_mode;
	igt_output_t *output;

	igt_enable_connectors(data->drm_fd);

	for_each_connected_output(&data->display, output) {
		if (!igt_amd_output_has_link_settings(data->drm_fd, output->name)) {
			igt_info("Skipping output: %s\n", output->name);
			continue;
		}

		igt_info("Testing on output: %s\n", output->name);

		/* Init only if display supports link_settings */
		test_init(data, output);

		orig_mode = igt_output_get_mode(output);
		igt_assert(orig_mode);
		igt_output_override_mode(output, orig_mode);

		/* Set display pattern */
		igt_create_pattern_fb(data->drm_fd, orig_mode->hdisplay,
				      orig_mode->vdisplay, DRM_FORMAT_XRGB8888,
				      0, &data->fb);
		igt_plane_set_fb(data->primary, &data->fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		/* Change link settings. */
		run_link_training_config(data, output);

		/* Clean up preferred link_setting of driver */
		igt_info("%s: Clean up preferred link_setting\n", output->name);
		igt_amd_write_link_settings(data->drm_fd, output->name, 0, 0,
			LINK_TRAINING_DEFAULT);

		igt_remove_fb(data->drm_fd, &data->fb);
	}

	test_fini(data);
}

igt_main
{
	data_t data;
	memset(&data, 0, sizeof(data));

	igt_skip_on_simulation();

	igt_fixture
	{
		data.drm_fd = drm_open_driver_master(DRIVER_AMDGPU);
		if (data.drm_fd == -1)
			igt_skip("Not an amdgpu driver.\n");

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Retrieves all link settings configurations and retrains "
		     "links on all possible configurations with different "
		     "types of link training.");
	igt_subtest("link-training-configs")
		test_link_training_configs(&data);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
