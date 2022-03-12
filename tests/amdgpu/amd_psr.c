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

#include "drm_mode.h"
#include "igt.h"
#include "igt_core.h"
#include "igt_kms.h"
#include "igt_amd.h"
#include <stdint.h>
#include <fcntl.h>
#include <xf86drmMode.h>

/* hardware requirements:
 * 1. eDP panel that supports PSR (multiple panel can be connected at the same time)
 * 2. Optional DP display for testing a regression condition (setting crtc to null)
 */
IGT_TEST_DESCRIPTION("Basic test for enabling Panel Self Refresh for eDP displays");

/* After a full update, a few fast updates are necessary for PSR to be enabled */
#define N_FLIPS 6
/* DMCUB takes some time to actually enable PSR. Worst case delay is 4 seconds */
#define PSR_SETTLE_DELAY 4

/* Common test data. */
typedef struct data {
        igt_display_t display;
        igt_plane_t *primary;
        igt_plane_t *cursor;
        igt_output_t *output;
        igt_pipe_t *pipe;
        igt_pipe_crc_t *pipe_crc;
        drmModeModeInfo *mode;
        enum pipe pipe_id;
        int fd;
        int w;
        int h;
} data_t;

/* Common test setup. */
static void test_init(data_t *data)
{
        igt_display_t *display = &data->display;

        /* It doesn't matter which pipe we choose on amdpgu. */
        data->pipe_id = PIPE_A;
        data->pipe = &data->display.pipes[data->pipe_id];

        igt_display_reset(display);

        data->output = igt_get_single_output_for_pipe(display, data->pipe_id);
        igt_require(data->output);

        data->mode = igt_output_get_mode(data->output);
        igt_assert(data->mode);

        data->primary =
                igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

        data->cursor =
                igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_CURSOR);

        data->pipe_crc = igt_pipe_crc_new(data->fd, data->pipe_id, "auto");

        igt_output_set_pipe(data->output, data->pipe_id);

        data->w = data->mode->hdisplay;
        data->h = data->mode->vdisplay;
}
/* Common test cleanup. */
static void test_fini(data_t *data)
{
        igt_display_t *display = &data->display;

        igt_pipe_crc_free(data->pipe_crc);
        igt_display_reset(display);
        igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

static int check_conn_type(data_t *data, uint32_t type) {
	int i;

	for (i = 0; i < data->display.n_outputs; i++) {
		uint32_t conn_type = data->display.outputs[i].config.connector->connector_type;
		if (conn_type == type)
			return i;
	}

	return -1;
}

static void run_check_psr(data_t *data, bool test_null_crtc) {
	int fd, edp_idx, dp_idx, ret, i, psr_state;
	igt_fb_t ref_fb, ref_fb2;
	igt_fb_t *flip_fb;
	enum pipe pipe;
	igt_output_t *output;

	test_init(data);

	edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	dp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_DisplayPort);
	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	for_each_pipe_with_single_output(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_create_color_fb(data->fd, data->mode->hdisplay,
				    data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 1.0,
				    0.0, 0.0, &ref_fb);
		igt_create_color_fb(data->fd, data->mode->hdisplay,
				    data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.0,
				    1.0, 0.0, &ref_fb2);

		igt_plane_set_fb(data->primary, &ref_fb);
		igt_output_set_pipe(output, pipe);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

		for (i = 0; i < N_FLIPS; i++) {
			if (i % 2 == 0)
				flip_fb = &ref_fb2;
			else
				flip_fb = &ref_fb;

			ret = drmModePageFlip(data->fd, output->config.crtc->crtc_id,
					      flip_fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
			igt_require(ret == 0);
			kmstest_wait_for_pageflip(data->fd);
		}
	}

	/* PSR state takes some time to settle its value on static screen */
	sleep(PSR_SETTLE_DELAY);

	for_each_pipe_with_single_output(&data->display, pipe, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		psr_state =  igt_amd_read_psr_state(data->fd, output->name);
		igt_fail_on_f(psr_state < 1, "PSR was not enabled for connector %s\n", output->name);
		igt_fail_on_f(psr_state == 0xff, "PSR is invalid for connector %s\n", output->name);
		igt_fail_on_f(psr_state != 5, "PSR state is expected to be at 5 on a "
			      "static screen for connector %s\n", output->name);
	}

	if (test_null_crtc) {
		/* check whether settings crtc to null generated any warning (eDP+DP) */
		igt_skip_on_f(dp_idx == -1, "no DP connector found\n");

		for_each_pipe_with_single_output(&data->display, pipe, output) {
			if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
				continue;

			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}
	}

	igt_remove_fb(data->fd, &ref_fb);
	igt_remove_fb(data->fd, &ref_fb2);
	close(fd);
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
		igt_require(&data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Test whether PSR can be enabled with static screen");
	igt_subtest("psr_enable") run_check_psr(&data, false);

	igt_describe("Test whether setting CRTC to null triggers any warning with PSR enabled");
	igt_subtest("psr_enable_null_crtc") run_check_psr(&data, true);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
