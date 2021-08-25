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
#include "igt_sysfs.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

IGT_TEST_DESCRIPTION("Check if ASSR is enabled on eDP links that support "
		     "the display authentication by changing scrambling sequence. "
		     "The test also covers embedded and non-removable "
		     "displays that appear as DP.");

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	int fd;
} data_t;

/* Test flags. */
enum {
	TEST_NONE = 1 << 0,
	TEST_DPMS = 1 << 1,
	TEST_SUSPEND = 1 << 2,
};


/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_reset(&data->display);
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_reset(&data->display);
}

static char *find_aux_dev(data_t *data, igt_output_t *output,
				char *aux_dev, size_t max_aux_dev_len)
{
	char sysfs_name[PATH_MAX] = { 0 };
	/* +7 only to get rid of snprintf_chk warning.
	 * Path name cannot exceed the size of PATH_MAX anyway.
	 */
	char conn_dir_name[PATH_MAX+7] = { 0 };
	DIR *dir;
	struct dirent *dirent;

	aux_dev[0] = 0;

	if(igt_sysfs_path(data->fd, sysfs_name, sizeof(sysfs_name))) {
			snprintf(conn_dir_name, sizeof(conn_dir_name),
					"%s%scard0%s%s",
					sysfs_name, "/", "-", output->name);
	}

	dir = opendir(conn_dir_name);
	if (!dir)
		return NULL;

	while((dirent = readdir(dir))) {
		if (strncmp(dirent->d_name, "drm_dp_aux", sizeof("drm_dp_aux")-1))
			continue;

		strncpy(aux_dev, dirent->d_name, max_aux_dev_len);
		break;
	}

	closedir(dir);

	if (aux_dev[0])
		return aux_dev;
	else
		return NULL;
}

static void parse_dpcd(const char *aux_dev, bool *assr_supported, bool *assr_enabled)
{
	char aux_name[PATH_MAX+6]; /* +6 only to get rid of snprintf_chk warning */
	char dpcd[2];
	int aux_fd;

	snprintf(aux_name, sizeof(aux_name), "/dev/%s", aux_dev);

	igt_assert((aux_fd = open(aux_name, O_RDONLY)) >= 0);

	/* Refer to section 3.5 of VESA eDP standard v1.4b:
	 * Display Authentication and Content Protection Support
	 */

	/* DPCD register 0x0D, eDP_CONFIGURATION_CAP
	 * Bit 0 is ALTERNATE_SCRAMBLER_RESET_CAPABLE,
	 * indicating if eDP device can use ASSR.
	 */
	igt_assert(lseek(aux_fd, 0x0D, SEEK_SET));
	igt_assert(read(aux_fd, &dpcd[0], 1) == 1);
	*assr_supported = dpcd[0] & 0x01;

	/* DPCD register 0x10A, eDP_CONFIGURATION_SET
	 * Bit 0 is ALTERNATE_SCRAMBLER_RESET_ENABLE,
	 * indicating if ASSR is enabled on the eDP device
	 */
	igt_assert(lseek(aux_fd, 0x10A, SEEK_SET));
	igt_assert(read(aux_fd, &dpcd[1], 1) == 1);
	*assr_enabled = dpcd[1] & 0x01;

	close(aux_fd);
}

static bool get_internal_display_flag(data_t *data, igt_output_t *output)
{
	char buf[256];
	char *start_loc;
	int fd, res;
	int internal_flag;

	fd = igt_debugfs_connector_dir(data->fd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	res = igt_debugfs_simple_read(fd, "internal_display", buf, sizeof(buf));
	if (res <= 0) {
		close(fd);
		return false;
	}

	close(fd);

	igt_assert(start_loc = strstr(buf, "Internal: "));
	igt_assert_eq(sscanf(start_loc, "Internal: %u", &internal_flag), 1);

	return (bool)internal_flag;
}

static void present_visual_pattern(data_t *data, igt_output_t *output)
{
	igt_plane_t *primary;
	igt_pipe_t *pipe;
	drmModeModeInfo *mode;
	igt_fb_t fb;
	cairo_t *cr;

	mode = igt_output_get_mode(output);
	igt_assert(mode);

	pipe = &data->display.pipes[PIPE_A];
	primary =
		igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
	igt_output_set_pipe(output, PIPE_A);

	igt_create_fb(data->fd, mode->hdisplay, mode->vdisplay,
			DRM_FORMAT_XRGB8888, 0, &fb);
	cr = igt_get_cairo_ctx(fb.fd, &fb);
	igt_paint_test_pattern(cr, fb.width, fb.height);

	igt_put_cairo_ctx(cr);

	igt_plane_set_fb(primary, &fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	/* useful for visual inspection on artifacts */
	igt_debug_wait_for_keypress("assr");

	igt_plane_set_fb(primary, NULL);
	igt_remove_fb(data->fd, &fb);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void test_cycle_flags(data_t *data, igt_output_t *output,
				uint32_t test_flags)
{
	if (test_flags & TEST_DPMS) {
		igt_info("Link DPMS off then on\n");
		kmstest_set_connector_dpms(data->fd,
					   output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->fd,
					   output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	if (test_flags & TEST_SUSPEND)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
}

static void test_assr(data_t *data, igt_output_t *output, uint32_t test_flags)
{
	drmModeConnector *connector = output->config.connector;
	int connector_type = connector->connector_type;
	char aux_dev[PATH_MAX];
	bool assr_supported = false, assr_enabled = false;
	bool is_internal_display;

	igt_info("Test ASSR on link %s\n", output->name);

	test_cycle_flags(data, output, test_flags);

	igt_assert_f(find_aux_dev(data, output, aux_dev, sizeof(aux_dev)),
			"Cannot find AUX device for link %s\n", output->name);
	igt_info("Link %s aux %s\n", output->name, aux_dev);

	parse_dpcd(aux_dev, &assr_supported, &assr_enabled);

	is_internal_display = get_internal_display_flag(data, output);

	igt_info("Link %s internal: %d, ASSR supported: %d, ASSR enabled: %d\n",
			output->name,
			is_internal_display,
			assr_supported, assr_enabled);

	present_visual_pattern(data, output);

	if (connector_type == DRM_MODE_CONNECTOR_eDP ||
		(connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
		 is_internal_display))
		igt_assert(assr_supported == assr_enabled);
	else
		igt_assert(!assr_enabled);
}

static void test_assr_links(data_t *data, uint32_t test_flags)
{
	for (int i = 0; i < data->display.n_outputs; ++i) {
		igt_output_t *output = &data->display.outputs[i];
		drmModeConnector *connector = output->config.connector;

		if (connector->connection != DRM_MODE_CONNECTED)
			continue;

		if (connector->connector_type != DRM_MODE_CONNECTOR_eDP &&
			connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort)
			continue;

		test_init(data);

		test_assr(data, output, test_flags);

		test_fini(data);
	}

}

igt_main
{
	data_t data;

	igt_skip_on_simulation();

	memset(&data, 0, sizeof(data));

	igt_fixture
	{
		data.fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Test ASSR on connected DP/eDP links");
	igt_subtest("assr-links")
		test_assr_links(&data, TEST_NONE);
	igt_describe("Test ASSR with DPMS ");
	igt_subtest("assr-links-dpms")
		test_assr_links(&data, TEST_DPMS);
	igt_describe("Test ASSR with suspend ");
	igt_subtest("assr-links-suspend")
		test_assr_links(&data, TEST_SUSPEND);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
