/*
 * Copyright 2020 Advanced Micro Devices, Inc.
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
#include "igt_amd.h"
#include <fcntl.h>

IGT_TEST_DESCRIPTION("Test EDID parsing and debugfs reporting on Freesync displays");

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	int fd;
} data_t;

typedef struct range {
	unsigned int min;
	unsigned int max;
} range_t;

/* Test flags. */
enum {
	TEST_NONE = 1 << 0,
	TEST_SUSPEND = 1 << 1,
};

struct {
	const char *name;
	uint32_t connector_type;
	const unsigned char edid[256];
	const range_t range;
} edid_database[] = {
	{
		/* DP EDID from Benq EL-2870u */
		"Benq EL-2870u DP",
		DRM_MODE_CONNECTOR_DisplayPort,
		{
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
		0x09, 0xd1, 0x49, 0x79, 0x45, 0x54, 0x00, 0x00,
		0x0c, 0x1e, 0x01, 0x04, 0xb5, 0x3e, 0x22, 0x78,
		0x3f, 0x08, 0xa5, 0xa2, 0x57, 0x4f, 0xa2, 0x28,
		0x0f, 0x50, 0x54, 0xa5, 0x6b, 0x80, 0xd1, 0xc0,
		0x81, 0xc0, 0x81, 0x00, 0x81, 0x80, 0xa9, 0xc0,
		0xb3, 0x00, 0xa9, 0x40, 0x01, 0x01, 0x4d, 0xd0,
		0x00, 0xa0, 0xf0, 0x70, 0x3e, 0x80, 0x30, 0x20,
		0x35, 0x00, 0x6d, 0x55, 0x21, 0x00, 0x00, 0x1a,
		0x00, 0x00, 0x00, 0xff, 0x00, 0x46, 0x33, 0x4c,
		0x30, 0x34, 0x33, 0x33, 0x33, 0x53, 0x4c, 0x30,
		0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x28,
		0x3c, 0x87, 0x87, 0x3c, 0x01, 0x0a, 0x20, 0x20,
		0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
		0x00, 0x42, 0x65, 0x6e, 0x51, 0x20, 0x45, 0x4c,
		0x32, 0x38, 0x37, 0x30, 0x55, 0x0a, 0x01, 0xa8,
		0x02, 0x03, 0x2e, 0xf1, 0x56, 0x61, 0x60, 0x5d,
		0x5e, 0x5f, 0x10, 0x05, 0x04, 0x03, 0x02, 0x07,
		0x06, 0x0f, 0x1f, 0x20, 0x21, 0x22, 0x14, 0x13,
		0x12, 0x16, 0x01, 0x23, 0x09, 0x07, 0x07, 0x83,
		0x01, 0x00, 0x00, 0xe3, 0x05, 0xc0, 0x00, 0xe6,
		0x06, 0x05, 0x01, 0x5a, 0x53, 0x44, 0x02, 0x3a,
		0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
		0x45, 0x00, 0x6d, 0x55, 0x21, 0x00, 0x00, 0x1e,
		0x56, 0x5e, 0x00, 0xa0, 0xa0, 0xa0, 0x29, 0x50,
		0x30, 0x20, 0x35, 0x00, 0x6d, 0x55, 0x21, 0x00,
		0x00, 0x1a, 0x8c, 0x64, 0x00, 0x50, 0xf0, 0x70,
		0x1f, 0x80, 0x08, 0x20, 0x18, 0x04, 0x6d, 0x55,
		0x21, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x93
		},
		{40, 60},
	},
	{
		/* HDMI EDID from ASUS VP249QGR */
		"ASUS VP249QGR HDMI",
		DRM_MODE_CONNECTOR_HDMIA,
		{
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
		0x06, 0xb3, 0xaf, 0x24, 0x01, 0x01, 0x01, 0x01,
		0x00, 0x1d, 0x01, 0x03, 0x80, 0x35, 0x1e, 0x78,
		0x2a, 0x51, 0xb5, 0xa4, 0x54, 0x4f, 0xa0, 0x26,
		0x0d, 0x50, 0x54, 0xbf, 0xcf, 0x00, 0x81, 0x40,
		0x81, 0x80, 0x95, 0x00, 0x71, 0x4f, 0x81, 0xc0,
		0xb3, 0x00, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a,
		0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
		0x45, 0x00, 0x0f, 0x28, 0x21, 0x00, 0x00, 0x1e,
		0xfc, 0x7e, 0x80, 0x88, 0x70, 0x38, 0x12, 0x40,
		0x18, 0x20, 0x35, 0x00, 0x0f, 0x28, 0x21, 0x00,
		0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30,
		0x90, 0x1e, 0xb4, 0x22, 0x00, 0x0a, 0x20, 0x20,
		0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
		0x00, 0x41, 0x53, 0x55, 0x53, 0x20, 0x56, 0x50,
		0x32, 0x34, 0x39, 0x0a, 0x20, 0x20, 0x01, 0x94,
		0x02, 0x03, 0x2d, 0xf1, 0x4f, 0x01, 0x03, 0x04,
		0x13, 0x1f, 0x12, 0x02, 0x11, 0x90, 0x0e, 0x0f,
		0x1d, 0x1e, 0x3f, 0x40, 0x23, 0x09, 0x07, 0x07,
		0x83, 0x01, 0x00, 0x00, 0x67, 0x03, 0x0c, 0x00,
		0x10, 0x00, 0x00, 0x44, 0x68, 0x1a, 0x00, 0x00,
		0x01, 0x01, 0x30, 0x90, 0xe6, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16
		},
		{48, 144},
	},
};

/* Common test setup. */
static void test_init(data_t *data, uint32_t connector_type)
{
	igt_display_t *display = &data->display;

	igt_display_reset(display);

	/* find connected outputs */
	data->output = NULL;
	for (int i=0; i < data->display.n_outputs; ++i) {
		drmModeConnector *connector = data->display.outputs[i].config.connector;
		if (connector->connection == DRM_MODE_CONNECTED &&
			connector->connector_type == connector_type) {
			data->output = &data->display.outputs[i];
		}
	}
	igt_assert_f(data->output, "Requires connected output\n");

}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_reset(&data->display);
}

static int find_test_edid_index(uint32_t connector_type)
{
	int i;

	for(i = 0; i < sizeof(edid_database)/sizeof(edid_database[0]); ++i) {
		if (edid_database[i].connector_type == connector_type) {
			return i;
		}
	}

	igt_assert_f(0, "should not reach here");
	return -1;
}

/* Returns the min and max vrr range from the connector debugfs. */
static range_t get_freesync_range(data_t *data, igt_output_t *output)
{
	char buf[256];
	char *start_loc;
	int fd, res;
	range_t range;

	fd = igt_debugfs_connector_dir(data->fd, output->name, O_RDONLY);
	igt_assert(fd >= 0);

	res = igt_debugfs_simple_read(fd, "vrr_range", buf, sizeof(buf));
	igt_require(res > 0);

	close(fd);

	igt_assert(start_loc = strstr(buf, "Min: "));
	igt_assert_eq(sscanf(start_loc, "Min: %u", &range.min), 1);

	igt_assert(start_loc = strstr(buf, "Max: "));
	igt_assert_eq(sscanf(start_loc, "Max: %u", &range.max), 1);

	return range;
}

static void trigger_edid_parse(data_t *data, uint32_t test_flags)
{
	if (test_flags & TEST_SUSPEND)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
	else
		igt_amd_trigger_hotplug(data->fd, data->output->name);

	/* more safe margin until resume and hotplug is completed */
	usleep(1500000);
}

/* Check if EDID parsing is correctly reporting Freesync capability
 * by overriding EDID with ones from golden sample. Display under test
 * must still support Freesync.
 */
static void test_freesync_parsing(data_t *data, uint32_t connector_type,
		uint32_t test_flags)
{
	const struct edid *edid;
	range_t range, expected_range;
	int i;

	test_init(data, connector_type);

	igt_amd_require_hpd(&data->display, data->fd);

	/* find a test EDID */
	i = find_test_edid_index(connector_type);
	edid = (const struct edid *)edid_database[i].edid;
	expected_range = edid_database[i].range;

	kmstest_force_edid(data->fd, data->output->config.connector, edid);

	trigger_edid_parse(data, test_flags);

	range = get_freesync_range(data, data->output);

	/* undo EDID override and trigger a re-parsing of EDID */
	kmstest_force_edid(data->fd, data->output->config.connector, NULL);
	igt_amd_trigger_hotplug(data->fd, data->output->name);

	test_fini(data);

	igt_assert_f(range.min == expected_range.min &&
			range.max == expected_range.max,
			"Expecting Freesync range %d-%d, got %d-%d\n",
			expected_range.min, expected_range.max,
			range.min, range.max);
	igt_info("Freesync range: %d-%d\n", range.min, range.max);
}

/* Returns true if an output supports VRR. */
static bool has_vrr(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_VRR_CAPABLE) &&
	       igt_output_get_prop(output, IGT_CONNECTOR_VRR_CAPABLE);
}

/* More relaxed checking on Freesync capability.
 * Only checks if frame rate range is within legal range.
 */
static void test_freesync_range(data_t *data, uint32_t connector_type,
		uint32_t test_flags)
{
	range_t range;

	test_init(data, connector_type);

	igt_amd_require_hpd(&data->display, data->fd);

	igt_assert_f(has_vrr(data->output),
			"connector %s is not VRR capable\n",
			data->output->name);

	trigger_edid_parse(data, test_flags);

	range = get_freesync_range(data, data->output);

	test_fini(data);

	igt_assert_f(range.min != 0 &&
			range.max != 0 &&
			range.max - range.min > 10,
			"Invalid Freesync range %d-%d\n",
			range.min, range.max);
	igt_info("Freesync range: %d-%d\n", range.min, range.max);
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
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Freesync EDID parsing on HDMI");
	igt_subtest("freesync-parsing-hdmi") test_freesync_parsing(&data,
			DRM_MODE_CONNECTOR_HDMIA, TEST_NONE);
	igt_describe("Freesync EDID parsing on DP");
	igt_subtest("freesync-parsing-dp") test_freesync_parsing(&data,
			DRM_MODE_CONNECTOR_DisplayPort, TEST_NONE);

	igt_describe("Freesync EDID parsing on HDMI after suspend");
	igt_subtest("freesync-parsing-hdmi-suspend") test_freesync_parsing(&data,
			DRM_MODE_CONNECTOR_HDMIA, TEST_SUSPEND);
	igt_describe("Freesync EDID parsing on DP after suspend");
	igt_subtest("freesync-parsing-dp-suspend") test_freesync_parsing(&data,
			DRM_MODE_CONNECTOR_DisplayPort, TEST_SUSPEND);

	igt_describe("Freesync range on HDMI");
	igt_subtest("freesync-range-hdmi") test_freesync_range(&data,
			DRM_MODE_CONNECTOR_HDMIA, TEST_NONE);
	igt_describe("Freesync range on DP");
	igt_subtest("freesync-range-dp") test_freesync_range(&data,
			DRM_MODE_CONNECTOR_DisplayPort, TEST_NONE);

	igt_describe("Freesync range on HDMI after suspend");
	igt_subtest("freesync-range-hdmi-suspend") test_freesync_range(&data,
			DRM_MODE_CONNECTOR_HDMIA, TEST_SUSPEND);
	igt_describe("Freesync range on DP after suspend");
	igt_subtest("freesync-range-dp-suspend") test_freesync_range(&data,
			DRM_MODE_CONNECTOR_DisplayPort, TEST_SUSPEND);

	igt_fixture
	{
		igt_display_fini(&data.display);
	}
}
