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
#include "sw_sync.h"
#include <fcntl.h>
#include <signal.h>

#define NUM_SLICE_SLOTS 4

/* Maximumm pipes on any AMD ASIC. */
#define MAX_PIPES 6

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary[MAX_PIPES];
	igt_output_t *output[MAX_PIPES];
	igt_pipe_t *pipe[MAX_PIPES];
	igt_pipe_crc_t *pipe_crc[MAX_PIPES];
	drmModeModeInfo mode[MAX_PIPES];
	enum pipe pipe_id[MAX_PIPES];
	int fd;
} data_t;

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_t *display = &data->display;
	int i;

	for (i = 0; i < display->n_pipes; ++i) {
		igt_pipe_crc_free(data->pipe_crc[i]);
	}

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	int i, n;

	for (i = 0; i < display->n_pipes; ++i) {
		data->pipe_id[i] = PIPE_A + i;
		data->pipe[i] = &data->display.pipes[data->pipe_id[i]];
		data->primary[i] = igt_pipe_get_plane_type(
				data->pipe[i], DRM_PLANE_TYPE_PRIMARY);
		data->pipe_crc[i] =
				igt_pipe_crc_new(data->fd, data->pipe_id[i],
						 IGT_PIPE_CRC_SOURCE_AUTO);
	}

	for (i = 0, n = 0; i < display->n_outputs && n < display->n_pipes; ++i) {
		igt_output_t *output = &display->outputs[i];
		data->output[n] = output;

		/* Only allow physically connected displays for the tests. */
		if (!igt_output_is_connected(output))
				continue;

		/* Ensure that outpus are DP, DSC & FEC capable*/
		if (!(is_dp_fec_supported(data->fd, output->name) &&
			is_dp_dsc_supported(data->fd, output->name)))
			continue;

		if (output->config.connector->connector_type !=
			DRM_MODE_CONNECTOR_DisplayPort)
			continue;

		igt_assert(kmstest_get_connector_default_mode(
				data->fd, output->config.connector, &data->mode[n]));

		n += 1;
	}

	igt_display_reset(display);
}

static void test_dsc_enable(data_t *data)
{
	bool dsc_on, dsc_after, dsc_before;
	igt_display_t *display = &data->display;
	igt_output_t *output;
	igt_fb_t ref_fb;
	int i, test_conn_cnt = 0;

	test_init(data);
	igt_enable_connectors(data->fd);

	for (i = 0; i < display->n_pipes; i++) {
		/* Setup the output */
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_create_pattern_fb(data->fd,
					data->mode[i].hdisplay,
					data->mode[i].vdisplay,
					DRM_FORMAT_XRGB8888,
					0,
					&ref_fb);
		igt_output_set_pipe(output, data->pipe_id[i]);
		igt_plane_set_fb(data->primary[i], &ref_fb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

		test_conn_cnt++;

		/* Save pipe's initial DSC state */
		dsc_before = igt_amd_read_dsc_clock_status(data->fd, output->name);

		/* Force enable DSC */
		igt_amd_write_dsc_clock_en(data->fd, output->name, DSC_FORCE_ON);

		igt_plane_set_fb(data->primary[i], &ref_fb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		/* Check if DSC is enabled */
		dsc_on = igt_amd_read_dsc_clock_status(data->fd, output->name) == 1;

		/* Revert DSC to automatic state */
		igt_amd_write_dsc_clock_en(data->fd, output->name, DSC_FORCE_OFF);

		igt_plane_set_fb(data->primary[i], &ref_fb);
		igt_display_commit_atomic(display,DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		dsc_after = igt_amd_read_dsc_clock_status(data->fd, output->name);

		/* Revert DSC back to automatic mechanism by disabling state overwrites*/
		igt_plane_set_fb(data->primary[i], &ref_fb);

		igt_amd_write_dsc_clock_en(data->fd, output->name, DSC_AUTOMATIC);

		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		igt_assert_f(dsc_on, "Enabling DSC on pipe failed.\n");
		igt_assert_f(dsc_after == dsc_before, "Reverting DSC to initial state failed.\n");

		/* Cleanup fb */
		igt_remove_fb(data->fd, &ref_fb);
	}

	test_fini(data);
	igt_skip_on(test_conn_cnt == 0);
}

static bool update_slice_height(data_t *data, int v_addressable,
					  int *num_slices, igt_output_t *output, int conn_idx, igt_fb_t ref_fb)
{
	int i;
	bool pass = true;

	for(i = 0; i < NUM_SLICE_SLOTS; i++) {
		int act_slice_height;
		int slice_height = v_addressable / num_slices[i] + (v_addressable % num_slices[i]);

		/* Overwrite DSC slice height */
		igt_amd_write_dsc_param_slice_height(data->fd, output->name, slice_height);
		igt_plane_set_fb(data->primary[conn_idx], &ref_fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		igt_info("Forcing slice height: slice height %d num slices vertical %d\n", slice_height, num_slices[i]);

		act_slice_height = igt_amd_read_dsc_param_slice_height(data->fd, output->name);

		igt_info("Reading slice height: actual slice height %d VS assigned slice height %d\n", act_slice_height, slice_height);

		pass = (slice_height == act_slice_height);

		if (!pass)
			break;
	}

	igt_amd_write_dsc_param_slice_height(data->fd, output->name, 0);
	igt_plane_set_fb(data->primary[conn_idx], &ref_fb);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	return pass;
}

static bool update_slice_width(data_t *data, int h_addressable,
					  int *num_slices, igt_output_t *output, int conn_idx, igt_fb_t ref_fb)
{
	int i;
	bool pass = true;

	for(i = 0; i < NUM_SLICE_SLOTS; i++) {
		int act_slice_width;
		int slice_width = h_addressable / num_slices[i] + (h_addressable % num_slices[i]);

		/* Overwrite DSC slice width */
		igt_amd_write_dsc_param_slice_width(data->fd, output->name, slice_width);
		igt_plane_set_fb(data->primary[conn_idx], &ref_fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		igt_info("Forcing slice width: slice width %d num slices horisontal %d\n", slice_width, num_slices[i]);

		act_slice_width = igt_amd_read_dsc_param_slice_width(data->fd, output->name);

		igt_info("Reading slice width: actual slice width %d VS assigned slice width %d\n", act_slice_width, slice_width);

		pass = (slice_width == act_slice_width);

		if (!pass)
			break;
	}

	igt_amd_write_dsc_param_slice_width(data->fd, output->name, 0);
	igt_plane_set_fb(data->primary[conn_idx], &ref_fb);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	return pass;
}

static void test_dsc_slice_dimensions_change(data_t *data)
{
	bool dsc_on, dsc_after, dsc_before;
	igt_output_t *output;
	igt_display_t *display = &data->display;
	igt_fb_t ref_fb;
	int num_slices [] = { 1, 2, 4, 8 };
	int h_addressable, v_addressable;
	bool ret_slice_height= false, ret_slice_width = false;
	int i, test_conn_cnt = 0;

	test_init(data);
	igt_enable_connectors(data->fd);

	for (i = 0; i < display->n_pipes; i++) {
		/* Setup the output */
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_create_pattern_fb(data->fd,
					data->mode[i].hdisplay,
					data->mode[i].vdisplay,
					DRM_FORMAT_XRGB8888,
					0,
					&ref_fb);
		igt_output_set_pipe(output, data->pipe_id[i]);
		igt_plane_set_fb(data->primary[i], &ref_fb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

		test_conn_cnt++;

		h_addressable = data->mode->hdisplay;
		v_addressable = data->mode->vdisplay;

		igt_info("Mode info: v_ative %d  h_active %d\n", v_addressable, h_addressable);

		/* Save pipe's initial DSC state */
		dsc_before = igt_amd_read_dsc_clock_status(data->fd, output->name);

		/* Force enable DSC */
		igt_amd_write_dsc_clock_en(data->fd, output->name, DSC_FORCE_ON);

		igt_plane_set_fb(data->primary[i], &ref_fb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		/* Check if DSC is enabled */
		dsc_on = igt_amd_read_dsc_clock_status(data->fd, output->name) == 1;

		if (dsc_on) {
			ret_slice_height = update_slice_height(data, v_addressable, num_slices, output, i, ref_fb);
			ret_slice_width = update_slice_width(data, h_addressable, num_slices, output, i, ref_fb);
		}

		/* Force disable DSC */
		igt_amd_write_dsc_clock_en(data->fd, output->name, DSC_FORCE_OFF);

		igt_plane_set_fb(data->primary[i], &ref_fb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		dsc_after = igt_amd_read_dsc_clock_status(data->fd, output->name);

		/* Revert DSC back to automatic mechanism by disabling state overwrites*/
		igt_plane_set_fb(data->primary[i], &ref_fb);

		igt_amd_write_dsc_clock_en(data->fd, output->name, DSC_AUTOMATIC);

		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		igt_assert_f(dsc_on, "Enabling DSC on pipe failed.\n");
		igt_assert_f(ret_slice_height, "Changing slice height failed.\n");
		igt_assert_f(ret_slice_width, "Changing slice width failed.\n");
		igt_assert_f(dsc_after == dsc_before, "Reverting DSC to initial state failed.\n");

		/* Cleanup fb */
		igt_remove_fb(data->fd, &ref_fb);
	}

	test_fini(data);
	igt_skip_on(test_conn_cnt == 0);
}

static void test_dsc_link_settings(data_t *data)
{
	igt_output_t *output;
	igt_fb_t ref_fb[MAX_PIPES];
	igt_crc_t ref_crc[MAX_PIPES], new_crc[MAX_PIPES];
    int lane_count[4], link_rate[4], link_spread[4];
	igt_display_t *display = &data->display;
	int i, lc, lr;
    bool dsc_on;
	const enum dc_lane_count lane_count_vals[] =
	{
		LANE_COUNT_TWO,
		LANE_COUNT_FOUR
	};
	const enum dc_link_rate link_rate_vals[] =
	{
		LINK_RATE_LOW,
		LINK_RATE_HIGH,
		LINK_RATE_HIGH2,
		LINK_RATE_HIGH3
	};

    test_init(data);

    /* Setup all outputs */
	for (i = 0; i < display->n_pipes; i++) {
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;

        igt_create_pattern_fb(data->fd,
                    data->mode[i].hdisplay,
                    data->mode[i].vdisplay,
                    DRM_FORMAT_XRGB8888,
                    0,
                    &ref_fb[i]);
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

	for (lc = 0; lc < ARRAY_SIZE(lane_count_vals); lc++) {
		for (lr = 0; lr < ARRAY_SIZE(link_rate_vals); lr++) {
			/* Write new link_settings */
			for (i = 0; i < display->n_pipes; i++) {
				output = data->output[i];
				if (!output || !igt_output_is_connected(output))
					continue;

				/* Write lower link settings */
				igt_info("Applying lane count: %d, link rate 0x%02x, on default training\n",
						lane_count_vals[lc], link_rate_vals[lr]);
				igt_amd_write_link_settings(data->fd, output->name,
							lane_count_vals[lc],
							link_rate_vals[lr],
							LINK_TRAINING_DEFAULT);
				usleep(500 * MSEC_PER_SEC);
			}

			/* Trigger commit after writing new link settings */
			igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

			for (i = 0; i < display->n_pipes; i++) {
				output = data->output[i];
				if (!output || !igt_output_is_connected(output))
					continue;

				/* Verify lower link settings */
				igt_amd_read_link_settings(data->fd, output->name,
							lane_count,
							link_rate,
							link_spread);

				igt_assert_f(lane_count[0] == lane_count_vals[lc], "Lowering lane count settings failed\n");
				igt_assert_f(link_rate[0] == link_rate_vals[lr], "Lowering link rate settings failed\n");

				/* Log current mode and DSC status */
				dsc_on = igt_amd_read_dsc_clock_status(data->fd, output->name) == 1;
				igt_info("Current mode is: %dx%d @%dHz -- DSC is: %s\n",
							data->mode[i].hdisplay,
							data->mode[i].vdisplay,
							data->mode[i].vrefresh,
							dsc_on ? "ON" : "OFF");

				igt_pipe_crc_collect_crc(data->pipe_crc[i], &new_crc[i]);
				igt_assert_crc_equal(&ref_crc[i], &new_crc[i]);
			}
		}
	}

	/* Cleanup all fbs */
	for (i = 0; i < display->n_pipes; i++) {
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;
		igt_remove_fb(data->fd, &ref_fb[i]);
	}

    test_fini(data);
}

static void test_dsc_bpc(data_t *data)
{
	igt_output_t *output;
	igt_fb_t ref_fb[MAX_PIPES];
	igt_crc_t test_crc;
	igt_display_t *display = &data->display;
	int i, bpc, max_supported_bpc[MAX_PIPES];
    bool dsc_on;
	const int bpc_vals[] = {12, 10, 8};

    test_init(data);

	/* Find max supported bpc */
	for (i = 0; i < display->n_pipes; i++) {
		output = data->output[i];
		if (!output || !igt_output_is_connected(output))
			continue;
		igt_info("Checking bpc support of conn %s\n", output->name);
		max_supported_bpc[i] = igt_get_output_max_bpc(data->fd, output->name);
	}

    /* Setup all outputs */
	for (bpc = 0; bpc < ARRAY_SIZE(bpc_vals); bpc++) {
		igt_info("Testing bpc = %d\n", bpc_vals[bpc]);

		for (i = 0; i < display->n_pipes; i++) {
			output = data->output[i];
			if (!output || !igt_output_is_connected(output))
				continue;

			if (max_supported_bpc[i] < bpc_vals[bpc]) {
				igt_info("Display doesn't support bpc of %d, max is %d. Skipping to next bpc value.\n", bpc_vals[bpc], max_supported_bpc[i]);
				continue;
			}
			igt_info("Setting bpc = %d\n", bpc_vals[bpc]);
			igt_output_set_prop_value(output, IGT_CONNECTOR_MAX_BPC, bpc_vals[bpc]);
			igt_create_pattern_fb(data->fd,
						data->mode[i].hdisplay,
						data->mode[i].vdisplay,
						DRM_FORMAT_XRGB8888,
						0,
						&ref_fb[i]);
			igt_output_set_pipe(output, data->pipe_id[i]);
			igt_plane_set_fb(data->primary[i], &ref_fb[i]);
		}

		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

		for (i = 0; i < display->n_pipes; i++) {
			output = data->output[i];
			if (!output || !igt_output_is_connected(output))
				continue;

			if (max_supported_bpc[i] < bpc_vals[bpc])
				continue;

			/* Check that crc is non-zero */
			igt_pipe_crc_collect_crc(data->pipe_crc[i], &test_crc);
			igt_assert(test_crc.crc[0] && test_crc.crc[1] && test_crc.crc[2]);

			/* Check current bpc */
			igt_info("Verifying display %s has correct bpc\n", output->name);
			igt_assert_output_bpc_equal(data->fd, data->pipe_id[i],
						    output->name, bpc_vals[bpc]);

			/* Log current mode and DSC status */
			dsc_on = igt_amd_read_dsc_clock_status(data->fd, output->name) == 1;
			igt_info("Current mode is: %dx%d @%dHz -- DSC is: %s\n",
						data->mode[i].hdisplay,
						data->mode[i].vdisplay,
						data->mode[i].vrefresh,
						dsc_on ? "ON" : "OFF");
		}

		/* Cleanup all fbs */
		for (i = 0; i < display->n_pipes; i++) {
			output = data->output[i];
			if (!output || !igt_output_is_connected(output))
				continue;

			if (max_supported_bpc[i] < bpc_vals[bpc])
				continue;

			igt_remove_fb(data->fd, &ref_fb[i]);
		}
	}

    test_fini(data);
}

igt_main
{
	data_t data = { 0 };

	igt_skip_on_simulation();

	igt_fixture
	{
		data.fd = drm_open_driver_master(DRIVER_ANY);

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);

		igt_amd_require_dsc(&data.display, data.fd);
		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Forces DSC on/off & ensures it is reset properly");
	igt_subtest("dsc-enable-basic")
		    test_dsc_enable(&data);

	igt_describe("Tests various DSC slice dimensions");
	igt_subtest("dsc-slice-dimensions-change")
		    test_dsc_slice_dimensions_change(&data);

	igt_describe("Tests various combinations of link_rate + lane_count and logs if DSC enabled/disabled");
	igt_subtest("dsc-link-settings")
		    test_dsc_link_settings(&data);

	igt_describe("Tests different bpc settings and logs if DSC is enabled/disabled");
	igt_subtest("dsc-bpc")
			test_dsc_bpc(&data);

	igt_fixture
	{
		igt_reset_connectors();
		igt_display_fini(&data.display);
	}
}
