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

IGT_TEST_DESCRIPTION("Test to validate display stream compression");

#define HDISPLAY_5K	5120
#define XRGB8888_DRM_FORMAT_MIN_BPP	8

enum dsc_test_type {
	TEST_BASIC_DSC_ENABLE,
	TEST_DSC_COMPRESSION_BPP
};

typedef struct {
	int drm_fd;
	uint32_t devid;
	igt_display_t display;
	struct igt_fb fb_test_pattern;
	igt_output_t *output;
	int compression_bpp;
	int n_pipes;
	enum pipe pipe;
} data_t;

bool force_dsc_en_orig;
int force_dsc_restore_fd = -1;

static inline void manual(const char *expected)
{
	igt_debug_interactive_mode_check("all", expected);
}

static void force_dsc_enable(data_t *data)
{
	int ret;

	igt_debug("Forcing DSC enable on %s\n", data->output->name);
	ret = igt_force_dsc_enable(data->drm_fd,
				   data->output->name);
	igt_assert_f(ret > 0, "debugfs_write failed");
}

static void force_dsc_enable_bpp(data_t *data)
{
	int ret;

	igt_debug("Forcing DSC BPP to %d on %s\n",
		  data->compression_bpp, data->output->name);
	ret = igt_force_dsc_enable_bpp(data->drm_fd,
				       data->output->name,
				       data->compression_bpp);
	igt_assert_f(ret > 0, "debugfs_write failed");
}

static void save_force_dsc_en(data_t *data)
{
	force_dsc_en_orig =
		igt_is_force_dsc_enabled(data->drm_fd,
					 data->output->name);
	force_dsc_restore_fd =
		igt_get_dsc_debugfs_fd(data->drm_fd,
				       data->output->name);
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

static void kms_dsc_exit_handler(int sig)
{
	restore_force_dsc_en();
}

static int sort_drm_modes(const void *a, const void *b)
{
	const drmModeModeInfo *mode1 = a, *mode2 = b;

	return (mode1->clock < mode2->clock) - (mode2->clock < mode1->clock);
}

static drmModeModeInfo *get_highres_mode(igt_output_t *output)
{
	drmModeConnector *connector = output->config.connector;
	drmModeModeInfo *highest_mode = NULL;

	qsort(connector->modes,
	      connector->count_modes,
	      sizeof(drmModeModeInfo),
	      sort_drm_modes);

	highest_mode = &connector->modes[0];

	return highest_mode;
}

static bool check_dsc_on_connector(data_t *data)
{
	igt_output_t *output = data->output;

	if (!igt_is_dsc_supported(data->drm_fd, output->name)) {
		igt_debug("DSC not supported on connector %s\n",
			  output->name);
		return false;
	}

	if (!output_is_internal_panel(output) &&
	    !igt_is_fec_supported(data->drm_fd, output->name)) {
		igt_debug("DSC cannot be enabled without FEC on %s\n",
			  output->name);
		return false;
	}

	return true;
}

static bool check_big_joiner_test_constraint(data_t *data,
					     enum dsc_test_type test_type)
{
	igt_output_t *output = data->output;
	drmModeModeInfo *mode = get_highres_mode(output);

	if (test_type == TEST_DSC_COMPRESSION_BPP &&
	    mode->hdisplay >= HDISPLAY_5K) {
		igt_debug("Bigjoiner does not support force bpp on %s\n",
			   output->name);
		return false;
	}

	return true;
}

static bool check_big_joiner_pipe_constraint(data_t *data)
{
	igt_output_t *output = data->output;
	drmModeModeInfo *mode = get_highres_mode(output);

	if (mode->hdisplay >= HDISPLAY_5K &&
	    data->pipe == (data->n_pipes - 1)) {
		igt_debug("Pipe-%s not supported due to bigjoiner limitation\n",
			   kmstest_pipe_name(data->pipe));
		return false;
	}

	return true;
}

static bool check_dp_gen11_constraint(data_t *data)
{
	igt_output_t *output = data->output;
	uint32_t devid = intel_get_drm_devid(data->drm_fd);
	drmModeConnector *connector = output->config.connector;

	if ((connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort) &&
	    (data->pipe == PIPE_A) && IS_GEN11(devid)) {
		igt_debug("DSC not supported on pipe A on external DP in gen11 platforms\n");
		return false;
	}

	return true;
}

static void test_cleanup(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);

	igt_output_set_pipe(output, PIPE_NONE);
	igt_remove_fb(data->drm_fd, &data->fb_test_pattern);
}

/* re-probe connectors and do a modeset with DSC */
static void update_display(data_t *data, enum dsc_test_type test_type)
{
	bool enabled;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	igt_output_t *output = data->output;
	igt_display_t *display = &data->display;

	/* sanitize the state before starting the subtest */
	igt_display_reset(display);
	igt_display_commit(display);

	igt_debug("DSC is supported on %s\n", data->output->name);
	save_force_dsc_en(data);
	force_dsc_enable(data);

	if (test_type == TEST_DSC_COMPRESSION_BPP) {
		igt_debug("Trying to set BPP to %d\n", data->compression_bpp);
		force_dsc_enable_bpp(data);
	}

	igt_output_set_pipe(output, data->pipe);

	mode = get_highres_mode(output);
	igt_require(mode != NULL);
	igt_output_override_mode(output, mode);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_create_pattern_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR,
			      &data->fb_test_pattern);

	igt_plane_set_fb(primary, &data->fb_test_pattern);
	igt_display_commit(display);

	/* until we have CRC check support, manually check if RGB test
	 * pattern has no corruption.
	 */
	manual("RGB test pattern without corruption");

	enabled = igt_is_dsc_enabled(data->drm_fd, output->name);
	restore_force_dsc_en();
	igt_debug("Reset compression BPP\n");
	data->compression_bpp = 0;
	force_dsc_enable_bpp(data);

	igt_assert_f(enabled,
		     "Default DSC enable failed on connector: %s pipe: %s\n",
		     output->name,
		     kmstest_pipe_name(data->pipe));

	test_cleanup(data);
}

static void test_dsc(data_t *data, enum dsc_test_type test_type, int bpp)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	char test_name[10];
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		data->compression_bpp = bpp;
		data->output = output;
		data->pipe = pipe;

		if (!check_dsc_on_connector(data))
			continue;

		if (!check_big_joiner_test_constraint(data, test_type))
			continue;

		if (!check_dp_gen11_constraint(data))
			continue;

		if (!check_big_joiner_pipe_constraint(data))
			continue;

		snprintf(test_name, sizeof(test_name), "-%dbpp", data->compression_bpp);
		igt_dynamic_f("pipe-%s-%s%s",  kmstest_pipe_name(data->pipe), data->output->name,
			     (test_type == TEST_DSC_COMPRESSION_BPP) ? test_name : "")
			update_display(data, test_type);
	}
}

igt_main
{
	data_t data = {};
	int i;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.devid = intel_get_drm_devid(data.drm_fd);
		kmstest_set_vt_graphics_mode();
		igt_install_exit_handler(kms_dsc_exit_handler);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		data.n_pipes = 0;
		for_each_pipe(&data.display, i)
			data.n_pipes++;
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with default parameters");
	igt_subtest_with_dynamic("basic-dsc-enable")
			test_dsc(&data, TEST_BASIC_DSC_ENABLE, 0);

	igt_fixture
		igt_require(intel_display_ver(data.devid) >= 13);

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with certain BPP as the output BPP for the connector");
	igt_subtest_with_dynamic("XRGB8888-dsc-compression") {
		uint32_t bpp_list[] = {
			XRGB8888_DRM_FORMAT_MIN_BPP,
			(XRGB8888_DRM_FORMAT_MIN_BPP  +
			(XRGB8888_DRM_FORMAT_MIN_BPP * 3) - 1) / 2,
			(XRGB8888_DRM_FORMAT_MIN_BPP * 3) - 1
		};

		for (int j = 0; j < ARRAY_SIZE(bpp_list); j++)
			test_dsc(&data, TEST_DSC_COMPRESSION_BPP, bpp_list[j]);
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
