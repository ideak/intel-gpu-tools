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
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "igt.h"
#include "igt_psr.h"
#include "igt_sysfs.h"
#include "limits.h"

/* DC State Flags */
#define CHECK_DC5	1
#define CHECK_DC6	2

typedef struct {
	int drm_fd;
	int debugfs_fd;
	uint32_t devid;
	igt_display_t display;
	struct igt_fb fb_white;
	enum psr_mode op_psr_mode;
	drmModeModeInfo *mode;
	igt_output_t *output;
} data_t;

static bool dc_state_wait_entry(int drm_fd, int dc_flag, int prev_dc_count);
static void check_dc_counter(int drm_fd, int dc_flag, uint32_t prev_dc_count);

static void setup_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		drmModeConnectorPtr c = output->config.connector;

		if (c->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_output_set_pipe(output, pipe);
		data->output = output;
		data->mode = igt_output_get_mode(output);

		return;
	}
}

static void display_fini(data_t *data)
{
	igt_display_fini(&data->display);
}

static bool edp_psr_sink_support(data_t *data)
{
	char buf[512];

	igt_debugfs_simple_read(data->debugfs_fd, "i915_edp_psr_status",
				buf, sizeof(buf));

	return strstr(buf, "Sink support: yes");
}

static void cleanup_dc_psr(data_t *data)
{
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &data->fb_white);
}

static void setup_primary(data_t *data)
{
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_create_color_fb(data->drm_fd,
			    data->mode->hdisplay, data->mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_I915_FORMAT_MOD_X_TILED,
			    1.0, 1.0, 1.0,
			    &data->fb_white);
	igt_plane_set_fb(primary, &data->fb_white);
	igt_display_commit(&data->display);
}

static uint32_t get_dc_counter(char *dc_data)
{
	char *e;
	long ret;
	char *s = strchr(dc_data, ':');

	assert(s);
	s++;
	ret = strtol(s, &e, 10);
	assert(((ret != LONG_MIN && ret != LONG_MAX) || errno != ERANGE) &&
	       e > s && *e == '\n' && ret >= 0);
	return ret;
}

static uint32_t read_dc_counter(uint32_t drm_fd, int dc_flag)
{
	char buf[4096];
	char *str;

	igt_debugfs_read(drm_fd, "i915_dmc_info", buf);

	if (dc_flag & CHECK_DC5)
		str = strstr(buf, "DC3 -> DC5 count");
	else if (dc_flag & CHECK_DC6)
		str = strstr(buf, "DC5 -> DC6 count");

	/* Check DC5/DC6 counter is available for the platform.
	 * Skip the test if counter is not available.
	 */
	igt_skip_on_f(!str, "DC%d counter is not available\n",
		      dc_flag & CHECK_DC5 ? 5 : 6);
	return get_dc_counter(str);
}

static bool dc_state_wait_entry(int drm_fd, int dc_flag, int prev_dc_count)
{
	return igt_wait(read_dc_counter(drm_fd, dc_flag) >
			prev_dc_count, 3000, 100);
}

static void check_dc_counter(int drm_fd, int dc_flag, uint32_t prev_dc_count)
{
	igt_assert_f(dc_state_wait_entry(drm_fd, dc_flag, prev_dc_count),
		     "DC%d state is not achieved\n",
		     dc_flag & CHECK_DC5 ? 5 : 6);
}

static void test_dc_state_psr(data_t *data, int dc_flag)
{
	uint32_t dc_counter_before_psr;

	dc_counter_before_psr = read_dc_counter(data->drm_fd, dc_flag);
	setup_output(data);
	setup_primary(data);
	igt_assert(psr_wait_entry(data->debugfs_fd, data->op_psr_mode));
	check_dc_counter(data->drm_fd, dc_flag, dc_counter_before_psr);
	cleanup_dc_psr(data);
}

IGT_TEST_DESCRIPTION("These tests validate Display Power DC states");
int main(int argc, char *argv[])
{
	bool has_runtime_pm;
	data_t data = {};

	igt_skip_on_simulation();
	igt_subtest_init(argc, argv);
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		igt_require(data.debugfs_fd != -1);
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_pm_enable_sata_link_power_management();
		has_runtime_pm = igt_setup_runtime_pm();
		igt_info("Runtime PM support: %d\n", has_runtime_pm);
		igt_require(has_runtime_pm);
		igt_require(igt_pm_dmc_loaded(data.debugfs_fd));
		igt_display_require(&data.display, data.drm_fd);
	}

	igt_describe("This test validates display engine entry to DC5 state "
		     "while PSR is active");
	igt_subtest("dc5-psr") {
		data.op_psr_mode = PSR_MODE_1;
		psr_enable(data.debugfs_fd, data.op_psr_mode);
		igt_require_f(edp_psr_sink_support(&data),
			      "Sink does not support PSR\n");
		test_dc_state_psr(&data, CHECK_DC5);
	}

	igt_fixture {
		close(data.debugfs_fd);
		display_fini(&data);
	}

	igt_exit();
}
