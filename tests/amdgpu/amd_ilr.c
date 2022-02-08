/*
 * Copyright 2022 Advanced Micro Devices, Inc.
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
#include "igt_sysfs.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MULTIPLIER_TO_LR 270000

IGT_TEST_DESCRIPTION("This igt test validates ILR (Intermediate Link Rate) "
	"feature from two perspective: "
	"1. Test if we can sucessfully train link rate at all supported ILRs"
	"2. Iterate over all modes to see if we do use ILR to optimize the link "
	"rate to light up the mode.");

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_fb_t fb;
	igt_pipe_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t crc_dprx;
	enum pipe pipe_id;
	int connector_type;
	int supported_ilr[MAX_SUPPORTED_ILR];
	int lane_count[4], link_rate[4], link_spread_spectrum[4];
} data_t;

enum sub_test {
	ILR_LINK_TRAINING_CONFIGS,
	ILR_POLICY
};

enum link_settings {
	CURRENT,
	VERIFIED,
	REPORTED,
	PREFERRED
};

static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
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

	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe_id,
					  AMDGPU_PIPE_CRC_SOURCE_DPRX);

	igt_output_set_pipe(output, data->pipe_id);

	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
}

static void test_ilr_link_training_configs(data_t *data, igt_output_t *output)
{
	int reported_lc, idx;

	reported_lc = data->lane_count[REPORTED];

	/* If ILR is supported */
	if (data->supported_ilr[0] != 0) {
		for (idx = 0; idx < MAX_SUPPORTED_ILR && data->supported_ilr[idx] != 0; idx++) {
			igt_amd_write_ilr_setting(data->drm_fd, output->name,
				reported_lc, idx);
			igt_info("Write training setting - lane count:%d, supported link rate idx:%d\n",
				reported_lc, idx);

			igt_amd_read_link_settings(data->drm_fd, output->name, data->lane_count,
				   data->link_rate, data->link_spread_spectrum);
			igt_info("Actual link result - lane count:%d, link rate:0x%02X\n",
					data->lane_count[CURRENT], data->link_rate[CURRENT]);

			/* Check lane count and link rate are trained at desired config*/
			igt_assert(reported_lc == data->lane_count[CURRENT]);
			igt_assert(data->supported_ilr[idx] == data->link_rate[CURRENT] * MULTIPLIER_TO_LR);
		}
	}
}

static void test_ilr_policy(data_t *data, igt_output_t *output)
{
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	int idx = 0, link_rate_set = 0;
	int current_link_rate;
	char *crc_str;

	igt_info("Policy test on %s\n", output->name);

	connector = output->config.connector;
	for (idx = 0; idx < connector->count_modes; idx++) {
		mode = &connector->modes[idx];
		igt_info("[%d]: htotal:%d vtotal:%d vrefresh:%d clock:%d\n", idx, mode->hdisplay,
		     mode->vdisplay, mode->vrefresh, mode->clock);

		/* Set test pattern*/
		igt_output_override_mode(output, mode);
		igt_create_pattern_fb(data->drm_fd, mode->hdisplay,
				      mode->vdisplay, DRM_FORMAT_XRGB8888,
				      0, &data->fb);
		igt_plane_set_fb(data->primary, &data->fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		igt_amd_read_link_settings(data->drm_fd, output->name, data->lane_count,
					   data->link_rate, data->link_spread_spectrum);

		igt_info("link result - lane count:%d, link rate:0x%02X\n",
					data->lane_count[CURRENT], data->link_rate[CURRENT]);

		current_link_rate = data->link_rate[CURRENT] * MULTIPLIER_TO_LR;

		/* Get current link_rate_set index after link training*/
		for (link_rate_set = 0; link_rate_set < sizeof(data->supported_ilr) &&
		 data->supported_ilr[link_rate_set] != 0; link_rate_set++) {
			if (data->supported_ilr[link_rate_set] == current_link_rate)
				break;
		}

		/* Firstly check driver does use ILR link setting */
		igt_assert(link_rate_set < sizeof(data->supported_ilr));
		igt_assert(data->supported_ilr[link_rate_set] > 0);

		/* Secondly check trained BW is sufficient.
		 * If BW is insufficient, crc retrieving will timeout
		 */
		igt_wait_for_vblank_count(data->drm_fd,
					data->pipe->crtc_offset, 10);

		igt_pipe_crc_collect_crc(data->pipe_crc, &data->crc_dprx);
		crc_str = igt_crc_to_string(&data->crc_dprx);
		igt_info("DP_RX CRC: %s\n", crc_str);
	}


}

static void test_flow(data_t *data, enum sub_test option)
{
	drmModeModeInfo *mode;
	igt_output_t *output;

	igt_enable_connectors(data->drm_fd);

	for_each_connected_output(&data->display, output) {
		if (!igt_amd_output_has_ilr_setting(data->drm_fd, output->name) ||
			!igt_amd_output_has_link_settings(data->drm_fd, output->name)) {
			igt_info("Skipping output: %s\n", output->name);
			continue;
		}

		igt_info("Testing on output: %s\n", output->name);

		/* Init only if display supports ilr link settings */
		test_init(data, output);

		mode = igt_output_get_mode(output);
		igt_assert(mode);

		/* Set test pattern*/
		igt_create_pattern_fb(data->drm_fd, mode->hdisplay,
				      mode->vdisplay, DRM_FORMAT_XRGB8888,
				      0, &data->fb);
		igt_plane_set_fb(data->primary, &data->fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		/* Collect info of Reported Lane Count & ILR */
		igt_amd_read_link_settings(data->drm_fd, output->name, data->lane_count,
					   data->link_rate, data->link_spread_spectrum);
		igt_amd_read_ilr_setting(data->drm_fd, output->name, data->supported_ilr);

		switch (option) {
			case ILR_LINK_TRAINING_CONFIGS:
				test_ilr_link_training_configs(data, output);
				break;
			case ILR_POLICY:
				test_ilr_policy(data, output);
				break;
			default:
				break;
		}

		/* Reset preferred link settings*/
		memset(data->supported_ilr, 0, sizeof(data->supported_ilr));
		igt_amd_write_ilr_setting(data->drm_fd, output->name, 0, 0);

		igt_remove_fb(data->drm_fd, &data->fb);

		test_fini(data);
	}

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

	igt_describe("Test ILR by trying training link rate at all supported ILRs");
	igt_subtest("ilr-link-training-configs")
		test_flow(&data, ILR_LINK_TRAINING_CONFIGS);
	igt_describe("Test ILR by checking driver does use ILRs to train link rate");
	igt_subtest("ilr-policy")
		test_flow(&data, ILR_POLICY);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}

