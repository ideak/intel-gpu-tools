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
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "igt.h"
#include "igt_kmod.h"
#include "igt_psr.h"
#include "igt_sysfs.h"
#include "limits.h"
#include "time.h"

/* DC State Flags */
#define CHECK_DC5	(1 << 0)
#define CHECK_DC6	(1 << 1)
#define CHECK_DC3CO	(1 << 2)

typedef struct {
	double r, g, b;
} color_t;

typedef struct {
	int drm_fd;
	int msr_fd;
	int debugfs_fd;
	uint32_t devid;
	igt_display_t display;
	struct igt_fb fb_white, fb_rgb, fb_rgr;
	enum psr_mode op_psr_mode;
	drmModeModeInfo *mode;
	igt_output_t *output;
	bool runtime_suspend_disabled;
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

static bool edp_psr2_enabled(data_t *data)
{
	char buf[512];

	igt_debugfs_simple_read(data->debugfs_fd, "i915_edp_psr_status",
				buf, sizeof(buf));

	return strstr(buf, "PSR mode: PSR2 enabled") != NULL;
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

static void cleanup_dc3co_fbs(data_t *data)
{
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	/* Clear Frame Buffers */
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &data->fb_rgb);
	igt_remove_fb(data->drm_fd, &data->fb_rgr);
}

static void paint_rectangles(data_t *data,
			     drmModeModeInfo *mode,
			     color_t *colors,
			     igt_fb_t *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(data->drm_fd, fb);
	int i, l = mode->hdisplay / 3;
	int rows_remaining = mode->hdisplay % 3;

	/* Paint 3 solid rectangles. */
	for (i = 0 ; i < 3; i++) {
		igt_paint_color(cr, i * l, 0, l, mode->vdisplay,
				colors[i].r, colors[i].g, colors[i].b);
	}

	if (rows_remaining > 0)
		igt_paint_color(cr, i * l, 0, rows_remaining, mode->vdisplay,
				colors[i - 1].r, colors[i - 1].g,
				colors[i - 1].b);

	igt_put_cairo_ctx(data->drm_fd, fb, cr);
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

static void create_color_fb(data_t *data, igt_fb_t *fb, color_t *fb_color)
{
	int fb_id;

	fb_id = igt_create_fb(data->drm_fd,
			      data->mode->hdisplay,
			      data->mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE,
			      fb);
	igt_assert(fb_id);
	paint_rectangles(data, data->mode, fb_color, fb);
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

static uint32_t read_dc_counter(uint32_t debugfs_fd, int dc_flag)
{
	char buf[4096];
	char *str;

	igt_debugfs_simple_read(debugfs_fd, "i915_dmc_info", buf, sizeof(buf));

	if (dc_flag & CHECK_DC5) {
		str = strstr(buf, "DC3 -> DC5 count");
		igt_assert_f(str, "DC5 counter is not available\n");
	} else if (dc_flag & CHECK_DC6) {
		str = strstr(buf, "DC5 -> DC6 count");
		igt_assert_f(str, "DC6 counter is not available\n");
	} else if (dc_flag & CHECK_DC3CO) {
		str = strstr(buf, "DC3CO count");
		igt_assert_f(str, "DC3CO counter is not available\n");
	}

	return get_dc_counter(str);
}

static bool dc_state_wait_entry(int debugfs_fd, int dc_flag, int prev_dc_count)
{
	return igt_wait(read_dc_counter(debugfs_fd, dc_flag) >
			prev_dc_count, 3000, 100);
}

static void check_dc_counter(int debugfs_fd, int dc_flag, uint32_t prev_dc_count)
{
	char tmp[64];

	snprintf(tmp, sizeof(tmp), "%s", dc_flag & CHECK_DC3CO ? "DC3CO" :
		(dc_flag & CHECK_DC5 ? "DC5" : "DC6"));
	igt_assert_f(dc_state_wait_entry(debugfs_fd, dc_flag, prev_dc_count),
		     "%s state is not achieved\n", tmp);
}

static void setup_videoplayback(data_t *data)
{
	color_t red_green_blue[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 },
	};
	color_t red_green_red[] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 1.0, 0.0, 0.0 },
	};

	create_color_fb(data, &data->fb_rgb, red_green_blue);
	create_color_fb(data, &data->fb_rgr, red_green_red);
}

static void check_dc3co_with_videoplayback_like_load(data_t *data)
{
	igt_plane_t *primary;
	uint32_t dc3co_prev_cnt;
	int delay;
	time_t secs = 6;
	time_t startTime = time(NULL);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	dc3co_prev_cnt = read_dc_counter(data->debugfs_fd, CHECK_DC3CO);

	/* Calculate delay to generate idle frame in usec*/
	delay = 1.5 * ((1000 * 1000) / data->mode->vrefresh);

	while (time(NULL) - startTime < secs) {
		igt_plane_set_fb(primary, &data->fb_rgb);
		igt_display_commit(&data->display);
		usleep(delay);

		igt_plane_set_fb(primary, &data->fb_rgr);
		igt_display_commit(&data->display);
		usleep(delay);
	}

	check_dc_counter(data->debugfs_fd, CHECK_DC3CO, dc3co_prev_cnt);
}

static void require_dc_counter(int debugfs_fd, int dc_flag)
{
	char buf[4096];

	igt_debugfs_simple_read(debugfs_fd, "i915_dmc_info",
				buf, sizeof(buf));

	switch (dc_flag) {
	case CHECK_DC3CO:
		igt_skip_on_f(!strstr(buf, "DC3CO count"),
			      "DC3CO counter is not available\n");
		break;
	case CHECK_DC5:
		igt_skip_on_f(!strstr(buf, "DC3 -> DC5 count"),
			      "DC5 counter is not available\n");
		break;
	case CHECK_DC6:
		igt_skip_on_f(!strstr(buf, "DC5 -> DC6 count"),
			      "DC6 counter is not available\n");
		break;
	default:
		igt_assert_f(0, "Unknown DC counter %d\n", dc_flag);
	}
}

static void setup_dc3co(data_t *data)
{
	data->op_psr_mode = PSR_MODE_2;
	psr_enable(data->debugfs_fd, data->op_psr_mode);
	igt_require_f(edp_psr2_enabled(data),
		      "PSR2 is not enabled\n");
}

static void test_dc3co_vpb_simulation(data_t *data)
{
	require_dc_counter(data->debugfs_fd, CHECK_DC3CO);
	setup_output(data);
	setup_dc3co(data);
	setup_videoplayback(data);
	check_dc3co_with_videoplayback_like_load(data);
	cleanup_dc3co_fbs(data);
}

static void test_dc_state_psr(data_t *data, int dc_flag)
{
	uint32_t dc_counter_before_psr;

	require_dc_counter(data->debugfs_fd, dc_flag);
	dc_counter_before_psr = read_dc_counter(data->debugfs_fd, dc_flag);
	setup_output(data);
	setup_primary(data);
	igt_assert(psr_wait_entry(data->debugfs_fd, data->op_psr_mode));
	check_dc_counter(data->debugfs_fd, dc_flag, dc_counter_before_psr);
	cleanup_dc_psr(data);
}

static void cleanup_dc_dpms(data_t *data)
{
	/*
	 * if runtime PM is disabled for i915 restore it,
	 * so any other sub-test can use runtime-PM.
	 */
	if (data->runtime_suspend_disabled) {
		igt_restore_runtime_pm();
		igt_setup_runtime_pm();
	}
}

static void setup_dc_dpms(data_t *data)
{
	if (IS_BROXTON(data->devid) || IS_GEMINILAKE(data->devid) ||
	    AT_LEAST_GEN(data->devid, 11)) {
		igt_disable_runtime_pm();
		data->runtime_suspend_disabled = true;
	} else {
		data->runtime_suspend_disabled = false;
	}
}

static void dpms_off(data_t *data)
{
	for (int i = 0; i < data->display.n_outputs; i++) {
		kmstest_set_connector_dpms(data->drm_fd,
					   data->display.outputs[i].config.connector,
					   DRM_MODE_DPMS_OFF);
	}

	if (!data->runtime_suspend_disabled)
		igt_assert(igt_wait_for_pm_status
			   (IGT_RUNTIME_PM_STATUS_SUSPENDED));
}

static void dpms_on(data_t *data)
{
	for (int i = 0; i < data->display.n_outputs; i++) {
		kmstest_set_connector_dpms(data->drm_fd,
					   data->display.outputs[i].config.connector,
					   DRM_MODE_DPMS_ON);
	}

	if (!data->runtime_suspend_disabled)
		igt_assert(igt_wait_for_pm_status
			   (IGT_RUNTIME_PM_STATUS_ACTIVE));
}

static void test_dc_state_dpms(data_t *data, int dc_flag)
{
	uint32_t dc_counter;

	require_dc_counter(data->debugfs_fd, dc_flag);
	setup_dc_dpms(data);
	dc_counter = read_dc_counter(data->debugfs_fd, dc_flag);
	dpms_off(data);
	check_dc_counter(data->debugfs_fd, dc_flag, dc_counter);
	dpms_on(data);
	cleanup_dc_dpms(data);
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
		igt_require(psr_sink_support(data.debugfs_fd, PSR_MODE_1));
		/* Make sure our Kernel supports MSR and the module is loaded */
		igt_require(igt_kmod_load("msr", NULL) == 0);

		data.msr_fd = open("/dev/cpu/0/msr", O_RDONLY);
		igt_assert_f(data.msr_fd >= 0,
			     "Can't open /dev/cpu/0/msr.\n");
	}

	igt_describe("In this test we make sure that system enters DC3CO "
		     "when PSR2 is active and system is in SLEEP state");
	igt_subtest("dc3co-vpb-simulation") {
		test_dc3co_vpb_simulation(&data);
	}

	igt_describe("This test validates display engine entry to DC5 state "
		     "while PSR is active");
	igt_subtest("dc5-psr") {
		data.op_psr_mode = PSR_MODE_1;
		psr_enable(data.debugfs_fd, data.op_psr_mode);
		test_dc_state_psr(&data, CHECK_DC5);
	}

	igt_describe("This test validates display engine entry to DC6 state "
		     "while PSR is active");
	igt_subtest("dc6-psr") {
		data.op_psr_mode = PSR_MODE_1;
		psr_enable(data.debugfs_fd, data.op_psr_mode);
		igt_require_f(igt_pm_pc8_plus_residencies_enabled(data.msr_fd),
			      "PC8+ residencies not supported\n");
		test_dc_state_psr(&data, CHECK_DC6);
	}

	igt_describe("This test validates display engine entry to DC5 state "
		     "while all connectors's DPMS property set to OFF");
	igt_subtest("dc5-dpms") {
		test_dc_state_dpms(&data, CHECK_DC5);
	}

	igt_describe("This test validates display engine entry to DC5 state "
		     "while all connectors's DPMS property set to OFF");
	igt_subtest("dc6-dpms") {
		igt_require_f(igt_pm_pc8_plus_residencies_enabled(data.msr_fd),
			      "PC8+ residencies not supported\n");
		test_dc_state_dpms(&data, CHECK_DC6);
	}

	igt_fixture {
		close(data.debugfs_fd);
		close(data.msr_fd);
		display_fini(&data);
	}

	igt_exit();
}
