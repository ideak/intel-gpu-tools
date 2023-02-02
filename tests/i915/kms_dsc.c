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

#include "kms_dsc_helper.h"

IGT_TEST_DESCRIPTION("Test to validate display stream compression");

enum dsc_test_type {
	TEST_DSC_BASIC,
	TEST_DSC_BPC
};

typedef struct {
	int drm_fd;
	uint32_t devid;
	igt_display_t display;
	struct igt_fb fb_test_pattern;
	unsigned int plane_format;
	igt_output_t *output;
	int input_bpc;
	int n_pipes;
	int disp_ver;
	enum pipe pipe;
} data_t;

static int format_list[] =  {DRM_FORMAT_XYUV8888, DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB16161616F, DRM_FORMAT_YUYV};
static uint32_t bpc_list[] = {12, 10, 8};

static inline void manual(const char *expected)
{
	igt_debug_interactive_mode_check("all", expected);
}

static drmModeModeInfo *get_highres_mode(igt_output_t *output)
{
	drmModeConnector *connector = output->config.connector;
	drmModeModeInfo *highest_mode = NULL;

	igt_sort_connector_modes(connector, sort_drm_modes_by_clk_dsc);

	highest_mode = &connector->modes[0];

	return highest_mode;
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
	save_force_dsc_en(data->drm_fd, data->output);
	force_dsc_enable(data->drm_fd, data->output);

	if (test_type == TEST_DSC_BPC) {
		igt_debug("Trying to set input BPC to %d\n", data->input_bpc);
		force_dsc_enable_bpc(data->drm_fd, data->output, data->input_bpc);
	}

	igt_output_set_pipe(output, data->pipe);

	mode = get_highres_mode(output);
	igt_require(mode != NULL);
	igt_output_override_mode(output, mode);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_skip_on(!igt_plane_has_format_mod(primary, data->plane_format,
		    DRM_FORMAT_MOD_LINEAR));

	igt_create_pattern_fb(data->drm_fd,
			      mode->hdisplay,
			      mode->vdisplay,
			      data->plane_format,
			      DRM_FORMAT_MOD_LINEAR,
			      &data->fb_test_pattern);

	igt_plane_set_fb(primary, &data->fb_test_pattern);
	igt_display_commit(display);

	/* until we have CRC check support, manually check if RGB test
	 * pattern has no corruption.
	 */
	manual("RGB test pattern without corruption");

	enabled = igt_is_dsc_enabled(data->drm_fd, output->name);
	igt_info("Current mode is: %dx%d @%dHz -- DSC is: %s\n",
				mode->hdisplay,
				mode->vdisplay,
				mode->vrefresh,
				enabled ? "ON" : "OFF");

	restore_force_dsc_en();
	igt_debug("Reset compression BPC\n");
	data->input_bpc = 0;
	force_dsc_enable_bpc(data->drm_fd, data->output, data->input_bpc);

	igt_assert_f(enabled,
		     "Default DSC enable failed on connector: %s pipe: %s\n",
		     output->name,
		     kmstest_pipe_name(data->pipe));

	test_cleanup(data);
}

static void test_dsc(data_t *data, enum dsc_test_type test_type, int bpc,
		     unsigned int plane_format)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	char name[20];
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		data->plane_format = plane_format;
		data->input_bpc = bpc;
		data->output = output;
		data->pipe = pipe;

		if (!check_dsc_on_connector(data->drm_fd, data->output))
			continue;

		if (!check_gen11_dp_constraint(data->drm_fd, data->output, data->pipe))
			continue;

		if (!check_gen11_bpc_constraint(data->drm_fd, data->output, data->input_bpc))
			continue;

		if (!check_big_joiner_pipe_constraint(data))
			continue;

		if (test_type == TEST_DSC_BPC)
			snprintf(name, sizeof(name), "-%dbpc-%s", data->input_bpc, igt_format_str(data->plane_format));
		else
			snprintf(name, sizeof(name), "-%s", igt_format_str(data->plane_format));

		igt_dynamic_f("pipe-%s-%s%s",  kmstest_pipe_name(data->pipe), data->output->name, name)
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
		data.disp_ver = intel_display_ver(data.devid);
		kmstest_set_vt_graphics_mode();
		igt_install_exit_handler(kms_dsc_exit_handler);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		igt_require(data.disp_ver >= 11);
		data.n_pipes = 0;
		for_each_pipe(&data.display, i)
			data.n_pipes++;
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with default parameters");
	igt_subtest_with_dynamic("basic-dsc")
			test_dsc(&data, TEST_DSC_BASIC, 0, DRM_FORMAT_XRGB8888);

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with default parameters and creating fb with diff formats");
	igt_subtest_with_dynamic("dsc-with-formats") {
		for (int k = 0; k < ARRAY_SIZE(format_list); k++)
			test_dsc(&data, TEST_DSC_BASIC, 0, format_list[k]);
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with certain input BPC for the connector");
	igt_subtest_with_dynamic("dsc-with-bpc") {
		for (int j = 0; j < ARRAY_SIZE(bpc_list); j++)
			test_dsc(&data, TEST_DSC_BPC, bpc_list[j], DRM_FORMAT_XRGB8888);
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with certain input BPC for the connector with diff formats");
	igt_subtest_with_dynamic("dsc-with-bpc-formats") {
		for (int j = 0; j < ARRAY_SIZE(bpc_list); j++) {
			for (int k = 0; k < ARRAY_SIZE(format_list); k++) {
				test_dsc(&data, TEST_DSC_BPC, bpc_list[j], format_list[k]);
			}
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
