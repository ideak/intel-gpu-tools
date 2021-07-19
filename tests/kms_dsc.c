/*
 * Copyright © 2018 Intel Corporation
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
 * Displayport Display Stream Compression test
 * Until the CRC support is added this needs to be invoked with --interactive
 * to manually verify if the test pattern is seen without corruption for each
 * subtest.
 *
 * Authors:
 * Manasi Navare <manasi.d.navare@intel.com>
 *
 */
#include "igt.h"
#include "igt_sysfs.h"
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>

/* currently dsc compression is verifying on 8bpc frame only */
#define XRGB8888_DRM_FORMAT_MIN_BPP 8

enum dsc_test_type
{
	test_basic_dsc_enable,
	test_dsc_compression_bpp
};

typedef struct {
	int drm_fd;
	uint32_t devid;
	igt_display_t display;
	struct igt_fb fb_test_pattern;
	igt_output_t *output;
	int mode_valid;
	drmModeEncoder *encoder;
	int crtc;
	int compression_bpp;
	enum pipe pipe;
	char conn_name[128];
} data_t;

bool force_dsc_en_orig;
int force_dsc_restore_fd = -1;

static inline void manual(const char *expected)
{
	igt_debug_manual_check("all", expected);
}

static void force_dsc_enable(data_t *data)
{
	int ret;

	igt_debug ("Forcing DSC enable on %s\n", data->conn_name);
	ret = igt_force_dsc_enable(data->drm_fd,
				      data->output->config.connector);
	igt_assert_f(ret > 0, "debugfs_write failed");
}

static void force_dsc_enable_bpp(data_t *data)
{
	int ret;

	igt_debug("Forcing DSC BPP to %d on %s\n",
		  data->compression_bpp, data->conn_name);
	ret = igt_force_dsc_enable_bpp(data->drm_fd,
					  data->output->config.connector,
					  data->compression_bpp);
	igt_assert_f(ret > 0, "debugfs_write failed");
}

static void save_force_dsc_en(data_t *data)
{
	force_dsc_en_orig =
		igt_is_force_dsc_enabled(data->drm_fd,
					 data->output->config.connector);
	force_dsc_restore_fd =
		igt_get_dsc_debugfs_fd(data->drm_fd,
					  data->output->config.connector);
	igt_assert(force_dsc_restore_fd >= 0);
}

static void restore_force_dsc_en(void)
{
	if (force_dsc_restore_fd < 0)
		return;

	igt_debug("Restoring DSC enable\n");
	igt_assert(write(force_dsc_restore_fd, force_dsc_en_orig ? "1" : "0", 1) == 1);

	close(force_dsc_restore_fd);
	force_dsc_restore_fd = -1;
}

static void test_cleanup(data_t *data)
{
	igt_plane_t *primary;

	if (data->output) {
		primary = igt_output_get_plane_type(data->output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(data->output, PIPE_NONE);
		igt_display_commit(&data->display);
	}
}

static void kms_dsc_exit_handler(int sig)
{
	restore_force_dsc_en();
}

static bool is_external_panel(drmModeConnector *connector)
{
	switch (connector->connector_type) {
		case DRM_MODE_CONNECTOR_LVDS:
		case DRM_MODE_CONNECTOR_eDP:
		case DRM_MODE_CONNECTOR_DSI:
		case DRM_MODE_CONNECTOR_DPI:
			return false;
		default:
			return true;
	}
}

static bool check_dsc_on_connector(data_t *data, uint32_t drmConnector)
{
	drmModeConnector *connector;
	igt_output_t *output;

	connector = drmModeGetConnectorCurrent(data->drm_fd,
					       drmConnector);
	if (connector->connection != DRM_MODE_CONNECTED)
		return false;

	output = igt_output_from_connector(&data->display, connector);
	sprintf(data->conn_name, "%s-%d",
		kmstest_connector_type_str(connector->connector_type),
		connector->connector_type_id);

	if (!igt_is_dsc_supported(data->drm_fd, connector)) {
		igt_debug("DSC not supported on connector %s\n",
			  data->conn_name);
		return false;
	}
	if (is_external_panel(connector) &&
	    !igt_is_fec_supported(data->drm_fd, connector)) {
		igt_debug("DSC cannot be enabled without FEC on %s\n",
			  data->conn_name);
		return false;
	}
	data->output = output;
	return true;
}

/*
 * Re-probe connectors and do a modeset with DSC
 *
 */
static void update_display(data_t *data, enum dsc_test_type test_type)
{
	bool enabled;
	igt_plane_t *primary;

	/* Disable the output first */
	igt_output_set_pipe(data->output, PIPE_NONE);
	igt_display_commit(&data->display);

	igt_debug("DSC is supported on %s\n", data->conn_name);
	save_force_dsc_en(data);
	force_dsc_enable(data);
	if (test_type == test_dsc_compression_bpp) {
		igt_debug("Trying to set BPP to %d\n", data->compression_bpp);
		force_dsc_enable_bpp(data);
	}

	igt_output_set_pipe(data->output, data->pipe);
	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);

	/* Now set the output to the desired mode */
	igt_plane_set_fb(primary, &data->fb_test_pattern);
	igt_display_commit(&data->display);

	/*
	 * Until we have CRC check support, manually check if RGB test
	 * pattern has no corruption.
	 */
	manual("RGB test pattern without corruption");

	enabled = igt_is_dsc_enabled(data->drm_fd,
					data->output->config.connector);
	restore_force_dsc_en();
	if (test_type == test_dsc_compression_bpp) {
		igt_debug("Rest compression BPP \n");
		data->compression_bpp = 0;
		force_dsc_enable_bpp(data);
	}

	igt_assert_f(enabled,
		     "Default DSC enable failed on Connector: %s Pipe: %s\n",
		     data->conn_name,
		     kmstest_pipe_name(data->pipe));
}

static void run_test(data_t *data, enum dsc_test_type test_type)
{
	enum pipe pipe;
	char test_name[10];
	drmModeModeInfo *mode = igt_output_get_mode(data->output);

	igt_create_pattern_fb(data->drm_fd, mode->hdisplay,
			      mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_NONE,
			      &data->fb_test_pattern);

	for_each_pipe(&data->display, pipe) {
		if (is_i915_device(data->drm_fd)) {
			uint32_t devid = intel_get_drm_devid(data->drm_fd);

			if (data->output->config.connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
			    pipe == PIPE_A && IS_GEN11(devid)) {
				igt_debug("DSC not supported on Pipe A on external DP in Gen11 platforms\n");
				continue;
			}
		}

		snprintf(test_name, sizeof(test_name), "-%dbpp", data->compression_bpp);
		if (igt_pipe_connector_valid(pipe, data->output)) {
			data->pipe = pipe;

			igt_dynamic_f("%s-pipe-%s%s", data->output->name,
					kmstest_pipe_name(pipe),
					(test_type == test_dsc_compression_bpp) ?
					 test_name : "")
				update_display(data, test_type);
		}

		if (test_type == test_dsc_compression_bpp)
			break;
	}

	igt_remove_fb(data->drm_fd, &data->fb_test_pattern);
}

igt_main
{
	data_t data = {};
	drmModeRes *res;
	drmModeConnector *connector = NULL;
	int i, j;
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_require_intel(data.drm_fd);
		data.devid = intel_get_drm_devid(data.drm_fd);
		kmstest_set_vt_graphics_mode();
		igt_install_exit_handler(kms_dsc_exit_handler);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(res = drmModeGetResources(data.drm_fd));
	}
	igt_subtest_with_dynamic("basic-dsc-enable") {
		for (j = 0; j < res->count_connectors; j++) {
			if (!check_dsc_on_connector(&data, res->connectors[j]))
				continue;
			run_test(&data, test_basic_dsc_enable);
		}
	}
	/* currently we are validating compression bpp on XRGB8888 format only */
	igt_subtest_with_dynamic("XRGB8888-dsc-compression") {
		uint32_t bpp_list[] = {
			XRGB8888_DRM_FORMAT_MIN_BPP,
			(XRGB8888_DRM_FORMAT_MIN_BPP  +
			 (XRGB8888_DRM_FORMAT_MIN_BPP * 3) - 1) / 2,
			(XRGB8888_DRM_FORMAT_MIN_BPP * 3) - 1
		};

		igt_require(intel_display_ver(data.devid) >= 13);

		for (j = 0; j < res->count_connectors; j++) {
			if (!check_dsc_on_connector(&data, res->connectors[j]))
				continue;

			for (i = 0; i < ARRAY_SIZE(bpp_list); i++) {
				data.compression_bpp = bpp_list[i];
				run_test(&data, test_dsc_compression_bpp);
			}
		}
	}

	igt_fixture {
		if (connector)
			drmModeFreeConnector(connector);
		test_cleanup(&data);
		drmModeFreeResources(res);
		close(data.drm_fd);
		igt_display_fini(&data.display);
	}
}
