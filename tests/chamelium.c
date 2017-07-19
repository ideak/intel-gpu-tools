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

#include "config.h"
#include "igt.h"

#include <fcntl.h>
#include <string.h>

typedef struct {
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	int port_count;

	int drm_fd;

	int edid_id;
	int alt_edid_id;
} data_t;

#define HOTPLUG_TIMEOUT 20 /* seconds */

#define HPD_STORM_PULSE_INTERVAL_DP 100 /* ms */
#define HPD_STORM_PULSE_INTERVAL_HDMI 200 /* ms */

#define HPD_TOGGLE_COUNT_VGA 5
#define HPD_TOGGLE_COUNT_DP_HDMI 15

static void
get_connectors_link_status_failed(data_t *data, bool *link_status_failed)
{
	drmModeConnector *connector;
	uint64_t link_status;
	drmModePropertyPtr prop;
	int p;

	for (p = 0; p < data->port_count; p++) {
		connector = chamelium_port_get_connector(data->chamelium,
							 data->ports[p], false);

		igt_assert(kmstest_get_property(data->drm_fd,
						connector->connector_id,
						DRM_MODE_OBJECT_CONNECTOR,
						"link-status", NULL,
						&link_status, &prop));

		link_status_failed[p] = link_status == DRM_MODE_LINK_STATUS_BAD;

		drmModeFreeProperty(prop);
		drmModeFreeConnector(connector);
	}
}

static void
require_connector_present(data_t *data, unsigned int type)
{
	int i;
	bool found = false;

	for (i = 0; i < data->port_count && !found; i++) {
		if (chamelium_port_get_type(data->ports[i]) == type)
			found = true;
	}

	igt_require_f(found, "No port of type %s was found\n",
		      kmstest_connector_type_str(type));
}

static drmModeConnection
reprobe_connector(data_t *data, struct chamelium_port *port)
{
	drmModeConnector *connector;
	drmModeConnection status;

	igt_debug("Reprobing %s...\n", chamelium_port_get_name(port));
	connector = chamelium_port_get_connector(data->chamelium, port, true);
	igt_assert(connector);
	status = connector->connection;

	drmModeFreeConnector(connector);
	return status;
}

static void
wait_for_connector(data_t *data, struct chamelium_port *port,
		   drmModeConnection status)
{
	bool finished = false;

	igt_debug("Waiting for %s to %sconnect...\n",
		  chamelium_port_get_name(port),
		  status == DRM_MODE_DISCONNECTED ? "dis" : "");

	/*
	 * Rely on simple reprobing so we don't fail tests that don't require
	 * that hpd events work in the event that hpd doesn't work on the system
	 */
	igt_until_timeout(HOTPLUG_TIMEOUT) {
		if (reprobe_connector(data, port) == status) {
			finished = true;
			return;
		}

		usleep(50000);
	}

	igt_assert(finished);
}

static void
reset_state(data_t *data, struct chamelium_port *port)
{
	int p;

	chamelium_reset(data->chamelium);

	if (port) {
		wait_for_connector(data, port, DRM_MODE_DISCONNECTED);
	} else {
		for (p = 0; p < data->port_count; p++) {
			port = data->ports[p];
			wait_for_connector(data, port, DRM_MODE_DISCONNECTED);
		}
	}
}

static void
test_basic_hotplug(data_t *data, struct chamelium_port *port, int toggle_count)
{
	struct udev_monitor *mon = igt_watch_hotplug();
	int i;

	reset_state(data, NULL);
	igt_hpd_storm_set_threshold(data->drm_fd, 0);

	for (i = 0; i < toggle_count; i++) {
		igt_flush_hotplugs(mon);

		/* Check if we get a sysfs hotplug event */
		chamelium_plug(data->chamelium, port);
		igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));
		igt_assert_eq(reprobe_connector(data, port),
			      DRM_MODE_CONNECTED);

		igt_flush_hotplugs(mon);

		/* Now check if we get a hotplug from disconnection */
		chamelium_unplug(data->chamelium, port);
		igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));
		igt_assert_eq(reprobe_connector(data, port),
			      DRM_MODE_DISCONNECTED);
	}

	igt_cleanup_hotplug(mon);
	igt_hpd_storm_reset(data->drm_fd);
}

static void
test_edid_read(data_t *data, struct chamelium_port *port,
	       int edid_id, const unsigned char *edid)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeConnector *connector = chamelium_port_get_connector(
	    data->chamelium, port, false);
	uint64_t edid_blob_id;

	reset_state(data, port);

	chamelium_port_set_edid(data->chamelium, port, edid_id);
	chamelium_plug(data->chamelium, port);
	wait_for_connector(data, port, DRM_MODE_CONNECTED);

	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob = drmModeGetPropertyBlob(data->drm_fd,
						      edid_blob_id));

	igt_assert(memcmp(edid, edid_blob->data, EDID_LENGTH) == 0);

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);
}

static void
try_suspend_resume_hpd(data_t *data, struct chamelium_port *port,
		       enum igt_suspend_state state, enum igt_suspend_test test,
		       struct udev_monitor *mon, bool connected)
{
	int delay;
	int p;

	igt_flush_hotplugs(mon);

	delay = igt_get_autoresume_delay(state) * 1000 / 2;

	if (port) {
		chamelium_schedule_hpd_toggle(data->chamelium, port, delay,
					      !connected);
	} else {
		for (p = 0; p < data->port_count; p++) {
			port = data->ports[p];
			chamelium_schedule_hpd_toggle(data->chamelium, port,
						      delay, !connected);
		}

		port = NULL;
	}

	igt_system_suspend_autoresume(state, test);

	igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));
	if (port) {
		igt_assert_eq(reprobe_connector(data, port), connected ?
			      DRM_MODE_DISCONNECTED : DRM_MODE_CONNECTED);
	} else {
		for (p = 0; p < data->port_count; p++) {
			port = data->ports[p];
			igt_assert_eq(reprobe_connector(data, port), connected ?
				      DRM_MODE_DISCONNECTED :
				      DRM_MODE_CONNECTED);
		}

		port = NULL;
	}
}

static void
test_suspend_resume_hpd(data_t *data, struct chamelium_port *port,
			enum igt_suspend_state state,
			enum igt_suspend_test test)
{
	struct udev_monitor *mon = igt_watch_hotplug();

	reset_state(data, port);

	/* Make sure we notice new connectors after resuming */
	try_suspend_resume_hpd(data, port, state, test, mon, false);

	/* Now make sure we notice disconnected connectors after resuming */
	try_suspend_resume_hpd(data, port, state, test, mon, true);

	igt_cleanup_hotplug(mon);
}

static void
test_suspend_resume_hpd_common(data_t *data, enum igt_suspend_state state,
			       enum igt_suspend_test test)
{
	struct udev_monitor *mon = igt_watch_hotplug();
	struct chamelium_port *port;
	int p;

	for (p = 0; p < data->port_count; p++) {
		port = data->ports[p];
		igt_debug("Testing port %s\n", chamelium_port_get_name(port));
	}

	reset_state(data, NULL);

	/* Make sure we notice new connectors after resuming */
	try_suspend_resume_hpd(data, NULL, state, test, mon, false);

	/* Now make sure we notice disconnected connectors after resuming */
	try_suspend_resume_hpd(data, NULL, state, test, mon, true);

	igt_cleanup_hotplug(mon);
}

static void
test_suspend_resume_edid_change(data_t *data, struct chamelium_port *port,
				enum igt_suspend_state state,
				enum igt_suspend_test test,
				int edid_id,
				int alt_edid_id)
{
	struct udev_monitor *mon = igt_watch_hotplug();
	bool link_status_failed[2][data->port_count];
	int p;

	reset_state(data, port);

	/* Catch the event and flush all remaining ones. */
	igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));
	igt_flush_hotplugs(mon);

	/* First plug in the port */
	chamelium_port_set_edid(data->chamelium, port, edid_id);
	chamelium_plug(data->chamelium, port);
	igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));

	wait_for_connector(data, port, DRM_MODE_CONNECTED);

	/*
	 * Change the edid before we suspend. On resume, the machine should
	 * notice the EDID change and fire a hotplug event.
	 */
	chamelium_port_set_edid(data->chamelium, port, alt_edid_id);

	get_connectors_link_status_failed(data, link_status_failed[0]);

	igt_flush_hotplugs(mon);

	igt_system_suspend_autoresume(state, test);

	igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));

	get_connectors_link_status_failed(data, link_status_failed[1]);

	for (p = 0; p < data->port_count; p++)
		igt_skip_on(!link_status_failed[0][p] && link_status_failed[1][p]);
}

static igt_output_t *
prepare_output(data_t *data,
	       igt_display_t *display,
	       struct chamelium_port *port)
{
	igt_output_t *output;
	drmModeRes *res;
	drmModeConnector *connector =
		chamelium_port_get_connector(data->chamelium, port, false);

	chamelium_reset(data->chamelium);

	igt_assert(res = drmModeGetResources(data->drm_fd));
	kmstest_unset_all_crtcs(data->drm_fd, res);

	/* The chamelium's default EDID has a lot of resolutions, way more then
	 * we need to test
	 */
	chamelium_port_set_edid(data->chamelium, port, data->edid_id);

	chamelium_plug(data->chamelium, port);
	wait_for_connector(data, port, DRM_MODE_CONNECTED);

	igt_display_init(display, data->drm_fd);
	output = igt_output_from_connector(display, connector);

	igt_assert(kmstest_probe_connector_config(
		data->drm_fd, connector->connector_id, ~0, &output->config));
	igt_output_set_pipe(output, output->config.pipe);

	drmModeFreeConnector(connector);
	drmModeFreeResources(res);

	return output;
}

static void
enable_output(data_t *data,
	      struct chamelium_port *port,
	      igt_output_t *output,
	      drmModeModeInfo *mode,
	      struct igt_fb *fb)
{
	igt_display_t *display = output->display;
	igt_plane_t *primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	drmModeConnector *connector = chamelium_port_get_connector(
	    data->chamelium, port, false);

	igt_assert(primary);

	igt_plane_set_size(primary, mode->hdisplay, mode->vdisplay);
	igt_plane_set_fb(primary, fb);
	igt_output_override_mode(output, mode);

	igt_output_set_pipe(output, output->config.pipe);

	/* Clear any color correction values that might be enabled */
	igt_pipe_set_degamma_lut(primary->pipe, NULL, 0);
	igt_pipe_set_gamma_lut(primary->pipe, NULL, 0);
	igt_pipe_set_ctm_matrix(primary->pipe, NULL, 0);

	kmstest_set_connector_broadcast_rgb(display->drm_fd, connector,
					    BROADCAST_RGB_FULL);

	igt_display_commit(display);
	chamelium_port_wait_video_input_stable(data->chamelium, port,
					       HOTPLUG_TIMEOUT);

	drmModeFreeConnector(connector);
}

static void
disable_output(data_t *data,
	       struct chamelium_port *port,
	       igt_output_t *output)
{
	igt_display_t *display = output->display;
	igt_plane_t *primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	/* Disable the display */
	igt_plane_set_fb(primary, NULL);
	igt_display_commit(display);
}

static void
test_display_crc_single(data_t *data, struct chamelium_port *port)
{
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *primary;
	igt_crc_t *crc;
	igt_crc_t *expected_crc;
	struct chamelium_fb_crc_async_data *fb_crc;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i, captured_frame_count;

	reset_state(data, port);

	output = prepare_output(data, &display, port);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];
		fb_id = igt_create_color_pattern_fb(data->drm_fd,
						    mode->hdisplay,
						    mode->vdisplay,
						    DRM_FORMAT_XRGB8888,
						    LOCAL_DRM_FORMAT_MOD_NONE,
						    0, 0, 0, &fb);
		igt_assert(fb_id > 0);

		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&fb);
		enable_output(data, port, output, mode, &fb);

		igt_debug("Testing single CRC fetch\n");

		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		chamelium_assert_crc_eq_or_dump(data->chamelium, expected_crc,
						crc, &fb, 0);

		igt_assert_crc_equal(crc, expected_crc);
		free(expected_crc);
		free(crc);

		disable_output(data, port, output);
		igt_remove_fb(data->drm_fd, &fb);
	}

	drmModeFreeConnector(connector);
	igt_display_fini(&display);
}

static void
test_display_crc_multiple(data_t *data, struct chamelium_port *port)
{
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *primary;
	igt_crc_t *crc;
	igt_crc_t *expected_crc;
	struct chamelium_fb_crc_async_data *fb_crc;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i, j, captured_frame_count;

	reset_state(data, port);

	output = prepare_output(data, &display, port);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];
		fb_id = igt_create_color_pattern_fb(data->drm_fd,
						    mode->hdisplay,
						    mode->vdisplay,
						    DRM_FORMAT_XRGB8888,
						    LOCAL_DRM_FORMAT_MOD_NONE,
						    0, 0, 0, &fb);
		igt_assert(fb_id > 0);

		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&fb);

		enable_output(data, port, output, mode, &fb);

		/* We want to keep the display running for a little bit, since
		 * there's always the potential the driver isn't able to keep
		 * the display running properly for very long
		 */
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 3);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_debug("Captured %d frames\n", captured_frame_count);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		for (j = 0; j < captured_frame_count; j++)
			chamelium_assert_crc_eq_or_dump(data->chamelium,
							expected_crc, &crc[j],
							&fb, j);

		free(expected_crc);
		free(crc);

		disable_output(data, port, output);
		igt_remove_fb(data->drm_fd, &fb);
	}

	drmModeFreeConnector(connector);
	igt_display_fini(&display);
}

static void
test_display_frame_dump(data_t *data, struct chamelium_port *port)
{
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	struct chamelium_frame_dump *frame;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i, j;

	reset_state(data, port);

	output = prepare_output(data, &display, port);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];
		fb_id = igt_create_color_pattern_fb(data->drm_fd,
						    mode->hdisplay, mode->vdisplay,
						    DRM_FORMAT_XRGB8888,
						    LOCAL_DRM_FORMAT_MOD_NONE,
						    0, 0, 0, &fb);
		igt_assert(fb_id > 0);

		enable_output(data, port, output, mode, &fb);

		igt_debug("Reading frame dumps from Chamelium...\n");
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 5);
		for (j = 0; j < 5; j++) {
			frame = chamelium_read_captured_frame(
			    data->chamelium, j);
			chamelium_assert_frame_eq(data->chamelium, frame, &fb);
			chamelium_destroy_frame_dump(frame);
		}

		disable_output(data, port, output);
		igt_remove_fb(data->drm_fd, &fb);
	}

	drmModeFreeConnector(connector);
	igt_display_fini(&display);
}

static void
test_hpd_without_ddc(data_t *data, struct chamelium_port *port)
{
	struct udev_monitor *mon = igt_watch_hotplug();

	reset_state(data, port);
	igt_flush_hotplugs(mon);

	/* Disable the DDC on the connector and make sure we still get a
	 * hotplug
	 */
	chamelium_port_set_ddc_state(data->chamelium, port, false);
	chamelium_plug(data->chamelium, port);

	igt_assert(igt_hotplug_detected(mon, HOTPLUG_TIMEOUT));
	igt_assert_eq(reprobe_connector(data, port), DRM_MODE_CONNECTED);

	igt_cleanup_hotplug(mon);
}

static void
test_hpd_storm_detect(data_t *data, struct chamelium_port *port, int width)
{
	struct udev_monitor *mon;
	int count = 0;

	igt_require_hpd_storm_ctl(data->drm_fd);
	reset_state(data, port);

	igt_hpd_storm_set_threshold(data->drm_fd, 1);
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);
	igt_assert(igt_hpd_storm_detected(data->drm_fd));

	mon = igt_watch_hotplug();
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);

	/*
	 * Polling should have been enabled by the HPD storm at this point,
	 * so we should only get at most 1 hotplug event
	 */
	igt_until_timeout(5)
		count += igt_hotplug_detected(mon, 1);
	igt_assert_lt(count, 2);

	igt_cleanup_hotplug(mon);
	igt_hpd_storm_reset(data->drm_fd);
}

static void
test_hpd_storm_disable(data_t *data, struct chamelium_port *port, int width)
{
	igt_require_hpd_storm_ctl(data->drm_fd);
	reset_state(data, port);

	igt_hpd_storm_set_threshold(data->drm_fd, 0);
	chamelium_fire_hpd_pulses(data->chamelium, port,
				  width, 10);
	igt_assert(!igt_hpd_storm_detected(data->drm_fd));

	igt_hpd_storm_reset(data->drm_fd);
}

#define for_each_port(p, port)            \
	for (p = 0, port = data.ports[p]; \
	     p < data.port_count;         \
	     p++, port = data.ports[p])

#define connector_subtest(name__, type__)                    \
	igt_subtest(name__)                                  \
		for_each_port(p, port)                       \
			if (chamelium_port_get_type(port) == \
			    DRM_MODE_CONNECTOR_ ## type__)

static data_t data;

igt_main
{
	struct chamelium_port *port;
	int edid_id, alt_edid_id, p;

	igt_fixture {
		igt_skip_on_simulation();

		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		data.chamelium = chamelium_init(data.drm_fd);
		igt_require(data.chamelium);

		data.ports = chamelium_get_ports(data.chamelium,
						 &data.port_count);

		edid_id = chamelium_new_edid(data.chamelium,
					     igt_kms_get_base_edid());
		alt_edid_id = chamelium_new_edid(data.chamelium,
						 igt_kms_get_alt_edid());
		data.edid_id = edid_id;
		data.alt_edid_id = alt_edid_id;

		/* So fbcon doesn't try to reprobe things itself */
		kmstest_set_vt_graphics_mode();
	}

	igt_subtest_group {
		igt_fixture {
			require_connector_present(
			    &data, DRM_MODE_CONNECTOR_DisplayPort);
		}

		connector_subtest("dp-hpd", DisplayPort)
			test_basic_hotplug(&data, port,
					   HPD_TOGGLE_COUNT_DP_HDMI);

		connector_subtest("dp-edid-read", DisplayPort) {
			test_edid_read(&data, port, edid_id,
				       igt_kms_get_base_edid());
			test_edid_read(&data, port, alt_edid_id,
				       igt_kms_get_alt_edid());
		}

		connector_subtest("dp-hpd-after-suspend", DisplayPort)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		connector_subtest("dp-hpd-after-hibernate", DisplayPort)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		connector_subtest("dp-hpd-storm", DisplayPort)
			test_hpd_storm_detect(&data, port,
					      HPD_STORM_PULSE_INTERVAL_DP);

		connector_subtest("dp-hpd-storm-disable", DisplayPort)
			test_hpd_storm_disable(&data, port,
					       HPD_STORM_PULSE_INTERVAL_DP);

		connector_subtest("dp-edid-change-during-suspend", DisplayPort)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_MEM,
							SUSPEND_TEST_NONE,
							edid_id, alt_edid_id);

		connector_subtest("dp-edid-change-during-hibernate", DisplayPort)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_DISK,
							SUSPEND_TEST_DEVICES,
							edid_id, alt_edid_id);

		connector_subtest("dp-crc-single", DisplayPort)
			test_display_crc_single(&data, port);

		connector_subtest("dp-crc-multiple", DisplayPort)
			test_display_crc_multiple(&data, port);

		connector_subtest("dp-frame-dump", DisplayPort)
			test_display_frame_dump(&data, port);
	}

	igt_subtest_group {
		igt_fixture {
			require_connector_present(
			    &data, DRM_MODE_CONNECTOR_HDMIA);
		}

		connector_subtest("hdmi-hpd", HDMIA)
			test_basic_hotplug(&data, port,
					   HPD_TOGGLE_COUNT_DP_HDMI);

		connector_subtest("hdmi-edid-read", HDMIA) {
			test_edid_read(&data, port, edid_id,
				       igt_kms_get_base_edid());
			test_edid_read(&data, port, alt_edid_id,
				       igt_kms_get_alt_edid());
		}

		connector_subtest("hdmi-hpd-after-suspend", HDMIA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		connector_subtest("hdmi-hpd-after-hibernate", HDMIA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		connector_subtest("hdmi-hpd-storm", HDMIA)
			test_hpd_storm_detect(&data, port,
					      HPD_STORM_PULSE_INTERVAL_HDMI);

		connector_subtest("hdmi-hpd-storm-disable", HDMIA)
			test_hpd_storm_disable(&data, port,
					       HPD_STORM_PULSE_INTERVAL_HDMI);

		connector_subtest("hdmi-edid-change-during-suspend", HDMIA)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_MEM,
							SUSPEND_TEST_NONE,
							edid_id, alt_edid_id);

		connector_subtest("hdmi-edid-change-during-hibernate", HDMIA)
			test_suspend_resume_edid_change(&data, port,
							SUSPEND_STATE_DISK,
							SUSPEND_TEST_DEVICES,
							edid_id, alt_edid_id);

		connector_subtest("hdmi-crc-single", HDMIA)
			test_display_crc_single(&data, port);

		connector_subtest("hdmi-crc-multiple", HDMIA)
			test_display_crc_multiple(&data, port);

		connector_subtest("hdmi-frame-dump", HDMIA)
			test_display_frame_dump(&data, port);
	}

	igt_subtest_group {
		igt_fixture {
			require_connector_present(
			    &data, DRM_MODE_CONNECTOR_VGA);
		}

		connector_subtest("vga-hpd", VGA)
			test_basic_hotplug(&data, port, HPD_TOGGLE_COUNT_VGA);

		connector_subtest("vga-edid-read", VGA) {
			test_edid_read(&data, port, edid_id,
				       igt_kms_get_base_edid());
			test_edid_read(&data, port, alt_edid_id,
				       igt_kms_get_alt_edid());
		}

		connector_subtest("vga-hpd-after-suspend", VGA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		connector_subtest("vga-hpd-after-hibernate", VGA)
			test_suspend_resume_hpd(&data, port,
						SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		connector_subtest("vga-hpd-without-ddc", VGA)
			test_hpd_without_ddc(&data, port);
	}

	igt_subtest_group {
		igt_subtest("common-hpd-after-suspend")
			test_suspend_resume_hpd_common(&data,
						       SUSPEND_STATE_MEM,
						       SUSPEND_TEST_NONE);

		igt_subtest("common-hpd-after-hibernate")
			test_suspend_resume_hpd_common(&data,
						       SUSPEND_STATE_DISK,
						       SUSPEND_TEST_DEVICES);
	}

	igt_fixture {
		close(data.drm_fd);
	}
}
