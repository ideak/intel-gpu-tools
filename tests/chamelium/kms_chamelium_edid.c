/*
 * Copyright © 2016 Red Hat Inc.
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
 * Authors:
 *    Lyude Paul <lyude@redhat.com>
 */

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <xf86drmMode.h>

#include "config.h"
#include "igt.h"
#include "igt_chamelium.h"
#include "igt_edid.h"
#include "igt_eld.h"
#include "igt_vc4.h"
#include "igt_infoframe.h"
#include "kms_chamelium_helper.h"
#include "monitor_edids/dp_edids.h"
#include "monitor_edids/hdmi_edids.h"
#include "monitor_edids/monitor_edids_helper.h"

#define MODE_CLOCK_ACCURACY 0.05 /* 5% */

static void get_connectors_link_status_failed(chamelium_data_t *data,
					      bool *link_status_failed)
{
	drmModeConnector *connector;
	uint64_t link_status;
	drmModePropertyPtr prop;
	int p;

	for (p = 0; p < data->port_count; p++) {
		connector = chamelium_port_get_connector(data->chamelium,
							 data->ports[p], false);

		igt_assert(kmstest_get_property(
			data->drm_fd, connector->connector_id,
			DRM_MODE_OBJECT_CONNECTOR, "link-status", NULL,
			&link_status, &prop));

		link_status_failed[p] = link_status == DRM_MODE_LINK_STATUS_BAD;

		drmModeFreeProperty(prop);
		drmModeFreeConnector(connector);
	}
}

static void check_mode(struct chamelium *chamelium, struct chamelium_port *port,
		       drmModeModeInfo *mode)
{
	struct chamelium_video_params video_params = { 0 };
	double mode_clock;
	int mode_hsync_offset, mode_vsync_offset;
	int mode_hsync_width, mode_vsync_width;
	int mode_hsync_polarity, mode_vsync_polarity;

	chamelium_port_get_video_params(chamelium, port, &video_params);

	mode_clock = (double)mode->clock / 1000;

	if (chamelium_port_get_type(port) == DRM_MODE_CONNECTOR_DisplayPort) {
		/* this is what chamelium understands as offsets for DP */
		mode_hsync_offset = mode->htotal - mode->hsync_start;
		mode_vsync_offset = mode->vtotal - mode->vsync_start;
	} else {
		/* and this is what they are for other connectors */
		mode_hsync_offset = mode->hsync_start - mode->hdisplay;
		mode_vsync_offset = mode->vsync_start - mode->vdisplay;
	}

	mode_hsync_width = mode->hsync_end - mode->hsync_start;
	mode_vsync_width = mode->vsync_end - mode->vsync_start;

	mode_hsync_polarity = !!(mode->flags & DRM_MODE_FLAG_PHSYNC);
	mode_vsync_polarity = !!(mode->flags & DRM_MODE_FLAG_PVSYNC);

	igt_debug("Checking video mode:\n");
	igt_debug("clock: got %f, expected %f ± %f%%\n", video_params.clock,
		  mode_clock, MODE_CLOCK_ACCURACY * 100);
	igt_debug("hactive: got %d, expected %d\n", video_params.hactive,
		  mode->hdisplay);
	igt_debug("vactive: got %d, expected %d\n", video_params.vactive,
		  mode->vdisplay);
	igt_debug("hsync_offset: got %d, expected %d\n",
		  video_params.hsync_offset, mode_hsync_offset);
	igt_debug("vsync_offset: got %d, expected %d\n",
		  video_params.vsync_offset, mode_vsync_offset);
	igt_debug("htotal: got %d, expected %d\n", video_params.htotal,
		  mode->htotal);
	igt_debug("vtotal: got %d, expected %d\n", video_params.vtotal,
		  mode->vtotal);
	igt_debug("hsync_width: got %d, expected %d\n",
		  video_params.hsync_width, mode_hsync_width);
	igt_debug("vsync_width: got %d, expected %d\n",
		  video_params.vsync_width, mode_vsync_width);
	igt_debug("hsync_polarity: got %d, expected %d\n",
		  video_params.hsync_polarity, mode_hsync_polarity);
	igt_debug("vsync_polarity: got %d, expected %d\n",
		  video_params.vsync_polarity, mode_vsync_polarity);

	if (!isnan(video_params.clock)) {
		igt_assert(video_params.clock >
			   mode_clock * (1 - MODE_CLOCK_ACCURACY));
		igt_assert(video_params.clock <
			   mode_clock * (1 + MODE_CLOCK_ACCURACY));
	}
	igt_assert(video_params.hactive == mode->hdisplay);
	igt_assert(video_params.vactive == mode->vdisplay);
	igt_assert(video_params.hsync_offset == mode_hsync_offset);
	igt_assert(video_params.vsync_offset == mode_vsync_offset);
	igt_assert(video_params.htotal == mode->htotal);
	igt_assert(video_params.vtotal == mode->vtotal);
	igt_assert(video_params.hsync_width == mode_hsync_width);
	igt_assert(video_params.vsync_width == mode_vsync_width);
	igt_assert(video_params.hsync_polarity == mode_hsync_polarity);
	igt_assert(video_params.vsync_polarity == mode_vsync_polarity);
}

static const char igt_custom_edid_type_read_desc[] =
	"Make sure the EDID exposed by KMS is the same as the screen's";
static void igt_custom_edid_type_read(chamelium_data_t *data,
				      struct chamelium_port *port,
				      enum igt_custom_edid_type edid)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeConnector *connector;
	size_t raw_edid_size;
	const struct edid *raw_edid;
	uint64_t edid_blob_id;

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	chamelium_set_edid(data, port, edid);
	chamelium_plug(data->chamelium, port);
	chamelium_wait_for_conn_status_change(&data->display, data->chamelium,
					      port, DRM_MODE_CONNECTED);

	igt_skip_on(chamelium_check_analog_bridge(data, port));

	connector = chamelium_port_get_connector(data->chamelium, port, true);
	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob_id != 0);
	edid_blob = drmModeGetPropertyBlob(data->drm_fd, edid_blob_id);
	igt_assert(edid_blob);

	raw_edid = chamelium_edid_get_raw(data->edids[edid], port);
	raw_edid_size = edid_get_size(raw_edid);
	igt_assert(memcmp(raw_edid, edid_blob->data, raw_edid_size) == 0);

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);
}

static const char igt_edid_stress_resolution_desc[] =
	"Stress test the DUT by testing multiple EDIDs, one right after the other, "
	"and ensure their validity by check the real screen resolution vs the "
	"advertised mode resultion.";
static void edid_stress_resolution(chamelium_data_t *data,
				   struct chamelium_port *port,
				   monitor_edid edids_list[],
				   size_t edids_list_len)
{
	int i;
	struct chamelium *chamelium = data->chamelium;
	struct udev_monitor *mon = igt_watch_uevents();
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);


	for (i = 0; i < edids_list_len; ++i) {
		struct chamelium_edid *chamelium_edid;
		drmModeModeInfo mode;
		struct igt_fb fb = { 0 };
		igt_output_t *output;
		enum pipe pipe;
		bool is_video_stable;
		int screen_res_w, screen_res_h;

		monitor_edid *edid = &edids_list[i];
		igt_info("Testing out the EDID for %s\n",
			 monitor_edid_get_name(edid));

		/* Getting and Setting the EDID on Chamelium. */
		chamelium_edid =
			get_chameleon_edid_from_monitor_edid(chamelium, edid);
		chamelium_port_set_edid(data->chamelium, port, chamelium_edid);
		free_chamelium_edid_from_monitor_edid(chamelium_edid);

		igt_flush_uevents(mon);
		chamelium_plug(chamelium, port);
		chamelium_wait_for_connector_after_hotplug(data, mon, port,
							   DRM_MODE_CONNECTED);
		igt_flush_uevents(mon);

		/* Setting an output on the screen to turn it on. */
		mode = chamelium_get_mode_for_port(chamelium, port);
		chamelium_create_fb_for_mode(data, &fb, &mode);
		output = chamelium_get_output_for_port(data, port);
		pipe = chamelium_get_pipe_for_output(&data->display, output);
		igt_output_set_pipe(output, pipe);
		chamelium_enable_output(data, port, output, &mode, &fb);

		/* Capture the screen resolution and verify. */
		is_video_stable = chamelium_port_wait_video_input_stable(
			chamelium, port, 5);
		igt_assert(is_video_stable);

		chamelium_port_get_resolution(chamelium, port, &screen_res_w,
					      &screen_res_h);
		igt_assert(screen_res_w == fb.width);
		igt_assert(screen_res_h == fb.height);

		// Clean up
		igt_remove_fb(data->drm_fd, &fb);
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_unplug(chamelium, port);
	}

	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);
}

static const char igt_edid_resolution_list_desc[] =
	"Get an EDID with many modes of different configurations, set them on the screen and check the"
	" screen resolution matches the mode resolution.";

static void edid_resolution_list(chamelium_data_t *data,
				 struct chamelium_port *port)
{
	struct chamelium *chamelium = data->chamelium;
	struct udev_monitor *mon = igt_watch_uevents();
	drmModeConnector *connector;
	drmModeModeInfoPtr modes;
	int count_modes;
	int i;
	igt_output_t *output;
	enum pipe pipe;

	chamelium_unplug(chamelium, port);
	chamelium_set_edid(data, port, IGT_CUSTOM_EDID_FULL);

	igt_flush_uevents(mon);
	chamelium_plug(chamelium, port);
	chamelium_wait_for_connector_after_hotplug(data, mon, port,
						   DRM_MODE_CONNECTED);
	igt_flush_uevents(mon);

	connector = chamelium_port_get_connector(chamelium, port, true);
	modes = connector->modes;
	count_modes = connector->count_modes;

	output = chamelium_get_output_for_port(data, port);
	pipe = chamelium_get_pipe_for_output(&data->display, output);
	igt_output_set_pipe(output, pipe);

	for (i = 0; i < count_modes; ++i)
		igt_debug("#%d %s %uHz\n", i, modes[i].name, modes[i].vrefresh);

	for (i = 0; i < count_modes; ++i) {
		struct igt_fb fb = { 0 };
		bool is_video_stable;
		int screen_res_w, screen_res_h;

		igt_info("Testing #%d %s %uHz\n", i, modes[i].name,
			 modes[i].vrefresh);

		/* Set the screen mode with the one we chose. */
		chamelium_create_fb_for_mode(data, &fb, &modes[i]);
		chamelium_enable_output(data, port, output, &modes[i], &fb);
		is_video_stable = chamelium_port_wait_video_input_stable(
			chamelium, port, 10);
		igt_assert(is_video_stable);

		chamelium_port_get_resolution(chamelium, port, &screen_res_w,
					      &screen_res_h);
		igt_assert_eq(screen_res_w, modes[i].hdisplay);
		igt_assert_eq(screen_res_h, modes[i].vdisplay);

		igt_remove_fb(data->drm_fd, &fb);
	}

	igt_modeset_disable_all_outputs(&data->display);
	drmModeFreeConnector(connector);
}

static const char test_suspend_resume_edid_change_desc[] =
	"Simulate a screen being unplugged and another screen being plugged "
	"during suspend, check that a uevent is sent and connector status is "
	"updated";
static void test_suspend_resume_edid_change(chamelium_data_t *data,
					    struct chamelium_port *port,
					    enum igt_suspend_state state,
					    enum igt_suspend_test test,
					    enum igt_custom_edid_type edid,
					    enum igt_custom_edid_type alt_edid)
{
	struct udev_monitor *mon = igt_watch_uevents();
	bool link_status_failed[2][data->port_count];
	int p;

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	/* Catch the event and flush all remaining ones. */
	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));
	igt_flush_uevents(mon);

	/* First plug in the port */
	chamelium_set_edid(data, port, edid);
	chamelium_plug(data->chamelium, port);
	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));

	chamelium_wait_for_conn_status_change(&data->display, data->chamelium,
					      port, DRM_MODE_CONNECTED);

	/*
	 * Change the edid before we suspend. On resume, the machine should
	 * notice the EDID change and fire a hotplug event.
	 */
	chamelium_set_edid(data, port, alt_edid);

	get_connectors_link_status_failed(data, link_status_failed[0]);

	igt_flush_uevents(mon);

	igt_system_suspend_autoresume(state, test);
	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));
	chamelium_assert_reachable(data->chamelium, ONLINE_TIMEOUT);

	get_connectors_link_status_failed(data, link_status_failed[1]);

	for (p = 0; p < data->port_count; p++)
		igt_skip_on(!link_status_failed[0][p] &&
			    link_status_failed[1][p]);
}

static const char test_mode_timings_desc[] =
	"For each mode of the IGT base EDID, perform a modeset and check the "
	"mode detected by the Chamelium receiver matches the mode we set";
static void test_mode_timings(chamelium_data_t *data,
			      struct chamelium_port *port)
{
	int i, count_modes;

	i = 0;
	igt_require(chamelium_supports_get_video_params(data->chamelium));
	do {
		igt_output_t *output;
		igt_plane_t *primary;
		drmModeConnector *connector;
		drmModeModeInfo *mode;
		int fb_id;
		struct igt_fb fb;

		/*
		 * let's reset state each mode so we will get the
		 * HPD pulses realibably
		 */
		igt_modeset_disable_all_outputs(&data->display);
		chamelium_reset_state(&data->display, data->chamelium, port,
				      data->ports, data->port_count);

		/*
		 * modes may change due to mode pruining and link issues, so we
		 * need to refresh the connector
		 */
		output = chamelium_prepare_output(data, port,
						  IGT_CUSTOM_EDID_BASE);
		connector = chamelium_port_get_connector(data->chamelium, port,
							 false);
		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_assert(primary);

		/* we may skip some modes due to above but that's ok */
		count_modes = connector->count_modes;
		if (i >= count_modes)
			break;

		mode = &connector->modes[i];

		fb_id = igt_create_color_pattern_fb(
			data->drm_fd, mode->hdisplay, mode->vdisplay,
			DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 0,
			&fb);
		igt_assert(fb_id > 0);

		chamelium_enable_output(data, port, output, mode, &fb);

		/* Trigger the FSM */
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 0);

		check_mode(data->chamelium, port, mode);

		igt_remove_fb(data->drm_fd, &fb);
		drmModeFreeConnector(connector);
	} while (++i < count_modes);
}

IGT_TEST_DESCRIPTION("Testing EDID with a Chamelium board");
igt_main
{
	chamelium_data_t data;
	struct chamelium_port *port;
	int p;

	igt_fixture {
		chamelium_init_test(&data);
	}

	igt_describe("DisplayPort tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_DisplayPort,
				data.port_count, 1);
		}

		igt_describe(igt_custom_edid_type_read_desc);
		connector_subtest("dp-edid-read", DisplayPort)
		{
			igt_custom_edid_type_read(&data, port,
						  IGT_CUSTOM_EDID_BASE);
			igt_custom_edid_type_read(&data, port,
						  IGT_CUSTOM_EDID_ALT);
		}

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("dp-edid-stress-resolution-4k", DisplayPort)
			edid_stress_resolution(&data, port, DP_EDIDS_4K,
					       ARRAY_SIZE(DP_EDIDS_4K));

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("dp-edid-stress-resolution-non-4k",
				  DisplayPort)
			edid_stress_resolution(&data, port, DP_EDIDS_NON_4K,
					       ARRAY_SIZE(DP_EDIDS_NON_4K));

		igt_describe(igt_edid_resolution_list_desc);
		connector_subtest("dp-edid-resolution-list", DisplayPort)
			edid_resolution_list(&data, port);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("dp-edid-change-during-suspend", DisplayPort)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_MEM,
							SUSPEND_TEST_NONE,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("dp-edid-change-during-hibernate",
				  DisplayPort)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_DISK,
							SUSPEND_TEST_DEVICES,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_mode_timings_desc);
		connector_subtest("dp-mode-timings", DisplayPort)
			test_mode_timings(&data, port);
	}

	igt_describe("HDMI tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_HDMIA,
				data.port_count, 1);
		}

		igt_describe(igt_custom_edid_type_read_desc);
		connector_subtest("hdmi-edid-read", HDMIA)
		{
			igt_custom_edid_type_read(&data, port,
						  IGT_CUSTOM_EDID_BASE);
			igt_custom_edid_type_read(&data, port,
						  IGT_CUSTOM_EDID_ALT);
		}

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("hdmi-edid-stress-resolution-4k", HDMIA)
			edid_stress_resolution(&data, port, HDMI_EDIDS_4K,
					       ARRAY_SIZE(HDMI_EDIDS_4K));

		igt_describe(igt_edid_stress_resolution_desc);
		connector_subtest("hdmi-edid-stress-resolution-non-4k", HDMIA)
			edid_stress_resolution(&data, port, HDMI_EDIDS_NON_4K,
					       ARRAY_SIZE(HDMI_EDIDS_NON_4K));

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("hdmi-edid-change-during-suspend", HDMIA)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_MEM,
							SUSPEND_TEST_NONE,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_suspend_resume_edid_change_desc);
		connector_subtest("hdmi-edid-change-during-hibernate", HDMIA)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_DISK,
							SUSPEND_TEST_DEVICES,
							IGT_CUSTOM_EDID_BASE,
							IGT_CUSTOM_EDID_ALT);

		igt_describe(test_mode_timings_desc);
		connector_subtest("hdmi-mode-timings", HDMIA)
			test_mode_timings(&data, port);
	}

	igt_describe("VGA tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_VGA,
				data.port_count, 1);
		}

		igt_describe(igt_custom_edid_type_read_desc);
		connector_subtest("vga-edid-read", VGA)
		{
			igt_custom_edid_type_read(&data, port,
						  IGT_CUSTOM_EDID_BASE);
			igt_custom_edid_type_read(&data, port,
						  IGT_CUSTOM_EDID_ALT);
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
