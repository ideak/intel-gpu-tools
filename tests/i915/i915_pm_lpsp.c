/*
 * Copyright © 2013 Intel Corporation
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
 * Author: Paulo Zanoni <paulo.r.zanoni@intel.com>
 *
 */

#include "igt.h"
#include "igt_kmod.h"
#include "igt_pm.h"
#include "igt_sysfs.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_SINK_LPSP_INFO_BUF_LEN	4096

#define PWR_DOMAIN_INFO "i915_power_domain_info"

typedef struct {
	int drm_fd;
	int debugfs_fd;
	uint32_t devid;
	char *pwr_dmn_info;
	igt_display_t display;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	igt_output_t *output;
} data_t;

static bool lpsp_is_enabled(data_t *data)
{
	char buf[MAX_SINK_LPSP_INFO_BUF_LEN];
	int len;

	len = igt_debugfs_simple_read(data->debugfs_fd, "i915_lpsp_status",
				      buf, sizeof(buf));
	if (len < 0)
		igt_assert_eq(len, -ENODEV);

	igt_skip_on(strstr(buf, "LPSP: not supported"));

	return strstr(buf, "LPSP: enabled");
}

static bool dmc_supported(int debugfs)
{
	char buf[15];
	int len;

	len = igt_sysfs_read(debugfs, "i915_dmc_info", buf, sizeof(buf) - 1);

	if (len < 0)
		return false;
	else
		return true;
}

/*
 * The LPSP mode is all about an enabled pipe, but we expect to also be in the
 * low power mode when no pipes are enabled, so do this check anyway.
 */
static void screens_disabled_subtest(data_t *data)
{
	int valid_output = 0;

	for (int i = 0; i < data->display.n_outputs; i++) {
		data->output = &data->display.outputs[i];
		igt_output_set_pipe(data->output, PIPE_NONE);
		igt_display_commit(&data->display);
		valid_output++;
	}

	igt_require_f(valid_output, "No connected output found\n");
	/* eDP panel may have power_cycle_delay of 600ms, 1sec delay is safer */
	igt_assert_f(igt_wait(lpsp_is_enabled(data), 1000, 100),
		     "lpsp is not enabled\n%s:\n%s\n",
		     PWR_DOMAIN_INFO, data->pwr_dmn_info =
		     igt_sysfs_get(data->debugfs_fd, PWR_DOMAIN_INFO));
}

static void setup_lpsp_output(data_t *data)
{
	igt_plane_t *primary;

	/* set output pipe = PIPE_A for LPSP */
	igt_output_set_pipe(data->output, PIPE_A);
	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_create_pattern_fb(data->drm_fd,
			      data->mode->hdisplay, data->mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_NONE,
			      &data->fb);
	igt_plane_set_fb(primary, &data->fb);
	igt_display_commit(&data->display);
}

static void test_cleanup(data_t *data)
{
	igt_plane_t *primary;

	if (!data->output || data->output->pending_pipe == PIPE_NONE)
		return;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(data->output, PIPE_NONE);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &data->fb);
	data->output = NULL;
}

static void test_lpsp(data_t *data)
{
	drmModeConnectorPtr c = data->output->config.connector;
	int i;

	/* LPSP is low power single pipe usages i.e. PIPE_A */
	igt_require(igt_pipe_connector_valid(PIPE_A, data->output));
	igt_require_f(i915_output_is_lpsp_capable(data->drm_fd, data->output),
		      "output is not lpsp capable\n");

	data->mode = igt_output_get_mode(data->output);

	/* For LPSP avoid pipe big joiner by atleast 4k mode */
	if (data->mode->hdisplay > 3840 && data->mode->vdisplay > 2160)
		for (i = 0; i < c->count_modes; i++) {
			if (c->modes[i].hdisplay <= 3840 &&
			    c->modes[i].vdisplay <= 2160) {
				data->mode = &c->modes[i];
				igt_output_override_mode(data->output,
							 data->mode);
				break;
			}
		}

	igt_require(data->mode->hdisplay <= 3840 &&
		    data->mode->vdisplay <= 2160);

	setup_lpsp_output(data);
	igt_assert_f(igt_wait(lpsp_is_enabled(data), 1000, 100),
		     "%s: lpsp is not enabled\n%s:\n%s\n",
		     data->output->name, PWR_DOMAIN_INFO, data->pwr_dmn_info =
		     igt_sysfs_get(data->debugfs_fd, PWR_DOMAIN_INFO));
}

IGT_TEST_DESCRIPTION("These tests validates display Low Power Single Pipe configurations");
igt_main
{
	data_t data = {};

	igt_fixture {

		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require(data.drm_fd >= 0);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		igt_require(data.debugfs_fd >= 0);
		igt_pm_enable_audio_runtime_pm();
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(igt_pm_dmc_loaded(data.debugfs_fd));
	}

	igt_describe("This test validates lpsp while all crtc are disabled");
	igt_subtest("screens-disabled") {
		igt_require_f(!dmc_supported(data.debugfs_fd),
			      "DC states supported platform don't have ROI for this subtest\n");
		screens_disabled_subtest(&data);
	}

	igt_describe("This test validates lpsp on all connected outputs on low power PIPE_A");
	igt_subtest_with_dynamic_f("kms-lpsp") {
		igt_display_t *display = &data.display;
		igt_output_t *output;

		for_each_connected_output(display, output) {
			igt_dynamic_f("kms-lpsp-%s",
				      kmstest_connector_type_str(output->config.connector->connector_type)) {
				data.output = output;
				test_lpsp(&data);
			}

			test_cleanup(&data);
		}
	}

	igt_fixture {
		free(data.pwr_dmn_info);
		close(data.drm_fd);
		igt_display_fini(&data.display);
	}
}
