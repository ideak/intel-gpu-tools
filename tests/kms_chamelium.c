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
#include "igt_vc4.h"

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>

typedef struct {
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	igt_display_t display;
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
#define HPD_TOGGLE_COUNT_FAST 3

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

static int chamelium_vga_modes[][2] = {
	{ 1600, 1200 },
	{ 1920, 1200 },
	{ 1920, 1080 },
	{ 1680, 1050 },
	{ 1280, 1024 },
	{ 1280, 960 },
	{ 1440, 900 },
	{ 1280, 800 },
	{ 1024, 768 },
	{ 1360, 768 },
	{ 1280, 720 },
	{ 800, 600 },
	{ 640, 480 },
	{ -1, -1 },
};

static bool
prune_vga_mode(data_t *data, drmModeModeInfo *mode)
{
	int i = 0;

	while (chamelium_vga_modes[i][0] != -1) {
		if (mode->hdisplay == chamelium_vga_modes[i][0] &&
		    mode->vdisplay == chamelium_vga_modes[i][1])
			return false;

		i++;
	}

	return true;
}

static bool
check_analog_bridge(data_t *data, struct chamelium_port *port)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeConnector *connector = chamelium_port_get_connector(
	    data->chamelium, port, false);
	uint64_t edid_blob_id;
	unsigned char *edid;
	char edid_vendor[3];

	if (chamelium_port_get_type(port) != DRM_MODE_CONNECTOR_VGA) {
		drmModeFreeConnector(connector);
		return false;
	}

	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob = drmModeGetPropertyBlob(data->drm_fd,
						      edid_blob_id));

	edid = (unsigned char *) edid_blob->data;

	edid_vendor[0] = ((edid[8] & 0x7c) >> 2) + '@';
	edid_vendor[1] = (((edid[8] & 0x03) << 3) |
			  ((edid[9] & 0xe0) >> 5)) + '@';
	edid_vendor[2] = (edid[9] & 0x1f) + '@';

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);

	/* Analog bridges provide their own EDID */
	if (edid_vendor[0] != 'I' || edid_vendor[1] != 'G' ||
	    edid_vendor[2] != 'T')
		return true;

	return false;
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

	igt_skip_on(check_analog_bridge(data, port));

	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob = drmModeGetPropertyBlob(data->drm_fd,
						      edid_blob_id));

	igt_assert(memcmp(edid, edid_blob->data, EDID_LENGTH) == 0);

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);
}

/* Wait for hotplug and return the remaining time left from timeout */
static bool wait_for_hotplug(struct udev_monitor *mon, int *timeout)
{
	struct timespec start, end;
	int elapsed;
	bool detected;

	igt_assert_eq(igt_gettime(&start), 0);
	detected = igt_hotplug_detected(mon, *timeout);
	igt_assert_eq(igt_gettime(&end), 0);

	elapsed = igt_time_elapsed(&start, &end);
	igt_assert_lte(0, elapsed);
	*timeout = max(0, *timeout - elapsed);

	return detected;
}

static void
try_suspend_resume_hpd(data_t *data, struct chamelium_port *port,
		       enum igt_suspend_state state, enum igt_suspend_test test,
		       struct udev_monitor *mon, bool connected)
{
	drmModeConnection target_state = connected ? DRM_MODE_DISCONNECTED :
						     DRM_MODE_CONNECTED;
	int timeout = HOTPLUG_TIMEOUT;
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
	igt_assert(wait_for_hotplug(mon, &timeout));

	if (port) {
		igt_assert_eq(reprobe_connector(data, port), target_state);
	} else {
		for (p = 0; p < data->port_count; p++) {
			drmModeConnection current_state;

			port = data->ports[p];
			/*
			 * There could be as many hotplug events sent by
			 * driver as connectors we scheduled an HPD toggle on
			 * above, depending on timing. So if we're not seeing
			 * the expected connector state try to wait for an HPD
			 * event for each connector/port.
			 */
			current_state = reprobe_connector(data, port);
			if (p > 0 && current_state != target_state) {
				igt_assert(wait_for_hotplug(mon, &timeout));
				current_state = reprobe_connector(data, port);
			}

			igt_assert_eq(current_state, target_state);
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
	       struct chamelium_port *port, bool set_edid)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	drmModeRes *res;
	drmModeConnector *connector =
		chamelium_port_get_connector(data->chamelium, port, false);
	enum pipe pipe;
	bool found = false;

	igt_require(res = drmModeGetResources(data->drm_fd));

	/* The chamelium's default EDID has a lot of resolutions, way more then
	 * we need to test
	 */
	if (set_edid)
		chamelium_port_set_edid(data->chamelium, port, data->edid_id);

	chamelium_plug(data->chamelium, port);
	wait_for_connector(data, port, DRM_MODE_CONNECTED);

	igt_display_reset(display);

	output = igt_output_from_connector(display, connector);

	/* Refresh pipe to update connected status */
	igt_output_set_pipe(output, PIPE_NONE);

	for_each_pipe(display, pipe) {
		if (!igt_pipe_connector_valid(pipe, output))
			continue;

		found = true;
		break;
	}

	igt_assert_f(found, "No pipe found for output %s\n", igt_output_name(output));

	igt_output_set_pipe(output, pipe);

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

	/* Clear any color correction values that might be enabled */
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_DEGAMMA_LUT, NULL, 0);
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_GAMMA_LUT, NULL, 0);
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_CTM, NULL, 0);

	igt_display_commit2(display, COMMIT_ATOMIC);

	if (chamelium_port_get_type(port) == DRM_MODE_CONNECTOR_VGA)
		usleep(250000);

	drmModeFreeConnector(connector);
}

static void chamelium_paint_xr24_pattern(uint32_t *data,
					 size_t width, size_t height,
					 size_t stride, size_t block_size)
{
	uint32_t colors[] = { 0xff000000,
			      0xffff0000,
			      0xff00ff00,
			      0xff0000ff,
			      0xffffffff };
	unsigned i, j;

	for (i = 0; i < height; i++)
		for (j = 0; j < width; j++)
			*(data + i * stride / 4 + j) = colors[((j / block_size) + (i / block_size)) % 5];
}

static int chamelium_get_pattern_fb(data_t *data, size_t width, size_t height,
				    uint32_t fourcc, size_t block_size,
				    struct igt_fb *fb)
{
	int fb_id;
	void *ptr;

	igt_assert(fourcc == DRM_FORMAT_XRGB8888);

	fb_id = igt_create_fb(data->drm_fd, width, height, fourcc,
			      LOCAL_DRM_FORMAT_MOD_NONE, fb);
	igt_assert(fb_id > 0);

	ptr = igt_fb_map_buffer(fb->fd, fb);
	igt_assert(ptr);

	chamelium_paint_xr24_pattern(ptr, width, height, fb->strides[0],
				     block_size);
	igt_fb_unmap_buffer(fb, ptr);

	return fb_id;
}

static void do_test_display(data_t *data, struct chamelium_port *port,
			    igt_output_t *output, drmModeModeInfo *mode,
			    uint32_t fourcc, enum chamelium_check check,
			    int count)
{
	struct chamelium_fb_crc_async_data *fb_crc;
	struct igt_fb frame_fb, fb;
	int i, fb_id, captured_frame_count;
	int frame_id;

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, &fb);
	igt_assert(fb_id > 0);

	frame_id = igt_fb_convert(&frame_fb, &fb, fourcc,
				  LOCAL_DRM_FORMAT_MOD_NONE);
	igt_assert(frame_id > 0);

	if (check == CHAMELIUM_CHECK_CRC)
		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&fb);

	enable_output(data, port, output, mode, &frame_fb);

	if (check == CHAMELIUM_CHECK_CRC) {
		igt_crc_t *expected_crc;
		igt_crc_t *crc;

		/* We want to keep the display running for a little bit, since
		 * there's always the potential the driver isn't able to keep
		 * the display running properly for very long
		 */
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, count);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_assert(captured_frame_count == count);

		igt_debug("Captured %d frames\n", captured_frame_count);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		for (i = 0; i < captured_frame_count; i++)
			chamelium_assert_crc_eq_or_dump(data->chamelium,
							expected_crc, &crc[i],
							&fb, i);

		free(expected_crc);
		free(crc);
	} else if (check == CHAMELIUM_CHECK_ANALOG ||
		   check == CHAMELIUM_CHECK_CHECKERBOARD) {
		struct chamelium_frame_dump *dump;

		igt_assert(count == 1);

		dump = chamelium_port_dump_pixels(data->chamelium, port, 0, 0,
						  0, 0);

		if (check == CHAMELIUM_CHECK_ANALOG)
			chamelium_crop_analog_frame(dump, mode->hdisplay,
						    mode->vdisplay);

		chamelium_assert_frame_match_or_dump(data->chamelium, port,
						     dump, &fb, check);
		chamelium_destroy_frame_dump(dump);
	}

	igt_remove_fb(data->drm_fd, &frame_fb);
	igt_remove_fb(data->drm_fd, &fb);
}

static void test_display_one_mode(data_t *data, struct chamelium_port *port,
				  uint32_t fourcc, enum chamelium_check check,
				  int count)
{
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	igt_output_t *output;
	igt_plane_t *primary;

	reset_state(data, port);

	output = prepare_output(data, port, true);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_require(igt_plane_has_format_mod(primary, fourcc, LOCAL_DRM_FORMAT_MOD_NONE));

	mode = &connector->modes[0];
	if (check == CHAMELIUM_CHECK_ANALOG) {
		bool bridge = check_analog_bridge(data, port);

		igt_assert(!(bridge && prune_vga_mode(data, mode)));
	}

	do_test_display(data, port, output, mode, fourcc, check, count);

	drmModeFreeConnector(connector);
}

static void test_display_all_modes(data_t *data, struct chamelium_port *port,
				   uint32_t fourcc, enum chamelium_check check,
				   int count)
{
	igt_output_t *output;
	igt_plane_t *primary;
	drmModeConnector *connector;
	bool bridge;
	int i;

	reset_state(data, port);

	output = prepare_output(data, port, true);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);
	igt_require(igt_plane_has_format_mod(primary, fourcc, LOCAL_DRM_FORMAT_MOD_NONE));

	if (check == CHAMELIUM_CHECK_ANALOG)
		bridge = check_analog_bridge(data, port);

	for (i = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *mode = &connector->modes[i];

		if (check == CHAMELIUM_CHECK_ANALOG && bridge &&
		    prune_vga_mode(data, mode))
			continue;

		do_test_display(data, port, output, mode, fourcc, check, count);
	}

	drmModeFreeConnector(connector);
}

static void
test_display_frame_dump(data_t *data, struct chamelium_port *port)
{
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	struct chamelium_frame_dump *frame;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i, j;

	reset_state(data, port);

	output = prepare_output(data, port, true);
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

		igt_remove_fb(data->drm_fd, &fb);
	}

	drmModeFreeConnector(connector);
}


/* Playback parameters control the audio signal we synthesize and send */
#define PLAYBACK_CHANNELS 2
#define PLAYBACK_SAMPLES 1024

/* Capture paremeters control the audio signal we receive */
#define CAPTURE_SAMPLES 2048

#define AUDIO_TIMEOUT 2000 /* ms */
/* A streak of 3 gives confidence that the signal is good. */
#define MIN_STREAK 3

static int sampling_rates[] = {
	32000,
	44100,
	48000,
	88200,
	96000,
	176400,
	192000,
};

static int sampling_rates_count = sizeof(sampling_rates) / sizeof(int);

static int test_frequencies[] = {
	300,
	600,
	1200,
	80000,
	10000,
};

static int test_frequencies_count = sizeof(test_frequencies) / sizeof(int);

struct audio_state {
	struct audio_signal *signal;
	atomic_bool run;
};

static int
audio_output_callback(void *data, short *buffer, int frames)
{
	struct audio_state *state = data;

	audio_signal_fill(state->signal, buffer, frames);

	return state->run ? 0 : -1;
}

static void *
run_audio_thread(void *data)
{
	struct alsa *alsa = data;

	alsa_run(alsa, -1);
	return NULL;
}

static bool
do_test_display_audio(data_t *data, struct chamelium_port *port,
		      struct alsa *alsa, int playback_channels,
		      int playback_rate)
{
	int ret, capture_rate, capture_channels, msec, freq, step;
	struct chamelium_audio_file *audio_file;
	struct chamelium_stream *stream;
	enum chamelium_stream_realtime_mode stream_mode;
	struct audio_signal *signal;
	int32_t *recv, *buf;
	double *channel;
	size_t i, j, streak, page_count;
	size_t recv_len, buf_len, buf_cap, buf_size, channel_len;
	bool ok, success;
	char dump_suffix[64];
	char *dump_path = NULL;
	int dump_fd = -1;
	pthread_t thread;
	struct audio_state state = {};
	int channel_mapping[8], capture_chan;

	if (!alsa_test_output_configuration(alsa, playback_channels,
					    playback_rate)) {
		igt_debug("Skipping test with sample rate %d Hz and %d channels "
			  "because at least one of the selected output devices "
			  "doesn't support this configuration\n",
			  playback_rate, playback_channels);
		return false;
	}

	igt_debug("Testing with playback sampling rate %d Hz and %d channels\n",
		  playback_rate, playback_channels);
	alsa_configure_output(alsa, playback_channels, playback_rate);

	chamelium_start_capturing_audio(data->chamelium, port, false);

	stream = chamelium_stream_init();
	igt_assert(stream);

	stream_mode = CHAMELIUM_STREAM_REALTIME_STOP_WHEN_OVERFLOW;
	ok = chamelium_stream_dump_realtime_audio(stream, stream_mode);
	igt_assert(ok);

	signal = audio_signal_init(playback_channels, playback_rate);
	igt_assert(signal);

	/* We'll choose different frequencies per channel to make sure they are
	 * independent from each other. To do so, we'll add a different offset
	 * to the base frequencies for each channel. We need to choose a big
	 * enough offset so that we're sure to detect mixed up channels. We
	 * choose an offset of two 2 bins in the final FFT to enforce a clear
	 * difference.
	 *
	 * Note that we assume capture_rate == playback_rate. We'll assert this
	 * later on. We cannot retrieve the capture rate before starting
	 * playing audio, so we don't really have the choice.
	 */
	step = 2 * playback_rate / CAPTURE_SAMPLES;
	for (i = 0; i < test_frequencies_count; i++) {
		for (j = 0; j < playback_channels; j++) {
			freq = test_frequencies[i] + j * step;
			audio_signal_add_frequency(signal, freq, j);
		}
	}
	audio_signal_synthesize(signal);

	state.signal = signal;
	state.run = true;
	alsa_register_output_callback(alsa, audio_output_callback, &state,
				      PLAYBACK_SAMPLES);

	/* Start playing audio */
	ret = pthread_create(&thread, NULL, run_audio_thread, alsa);
	igt_assert(ret == 0);

	/* Only after we've started playing audio, we can retrieve the capture
	 * format used by the Chamelium device. */
	chamelium_get_audio_format(data->chamelium, port,
				   &capture_rate, &capture_channels);
	if (capture_rate == 0) {
		igt_debug("Audio receiver doesn't indicate the capture "
			 "sampling rate, assuming it's %d Hz\n", playback_rate);
		capture_rate = playback_rate;
	} else
		igt_assert(capture_rate == playback_rate);

	chamelium_get_audio_channel_mapping(data->chamelium, port,
					    channel_mapping);
	/* Make sure we can capture all channels we send. */
	for (i = 0; i < playback_channels; i++) {
		ok = false;
		for (j = 0; j < capture_channels; j++) {
			if (channel_mapping[j] == i) {
				ok = true;
				break;
			}
		}
		igt_assert(ok);
	}

	if (igt_frame_dump_is_enabled()) {
		snprintf(dump_suffix, sizeof(dump_suffix), "capture-%dch-%d",
			 playback_channels, playback_rate);

		dump_fd = audio_create_wav_file_s32_le(dump_suffix,
						       capture_rate,
						       capture_channels,
						       &dump_path);
		igt_assert(dump_fd >= 0);
	}

	/* Needs to be a multiple of 128, because that's the number of samples
	 * we get per channel each time we receive an audio page from the
	 * Chamelium device. */
	channel_len = CAPTURE_SAMPLES;
	channel = malloc(sizeof(double) * channel_len);

	buf_cap = capture_channels * channel_len;
	buf = malloc(sizeof(int32_t) * buf_cap);
	buf_len = 0;

	recv = NULL;
	recv_len = 0;

	success = false;
	streak = 0;
	msec = 0;
	i = 0;
	while (!success && msec < AUDIO_TIMEOUT) {
		ok = chamelium_stream_receive_realtime_audio(stream,
							     &page_count,
							     &recv, &recv_len);
		igt_assert(ok);

		memcpy(&buf[buf_len], recv, recv_len * sizeof(int32_t));
		buf_len += recv_len;

		if (buf_len < buf_cap)
			continue;
		igt_assert(buf_len == buf_cap);

		if (dump_fd >= 0) {
			buf_size = buf_len * sizeof(int32_t);
			igt_assert(write(dump_fd, buf, buf_size) == buf_size);
		}

		msec = i * channel_len / (double) capture_rate * 1000;
		igt_debug("Detecting audio signal, t=%d msec\n", msec);

		for (j = 0; j < playback_channels; j++) {
			capture_chan = channel_mapping[j];
			igt_assert(capture_chan >= 0);
			igt_debug("Processing channel %zu (captured as "
				  "channel %d)\n", j, capture_chan);

			audio_extract_channel_s32_le(channel, channel_len,
						     buf, buf_len,
						     capture_channels,
						     capture_chan);

			if (audio_signal_detect(signal, capture_rate, j,
						channel, channel_len))
				streak++;
			else
				streak = 0;
		}

		buf_len = 0;
		i++;

		success = streak == MIN_STREAK * playback_channels;
	}

	igt_debug("Stopping audio playback\n");
	state.run = false;
	ret = pthread_join(thread, NULL);
	igt_assert(ret == 0);

	alsa_close_output(alsa);

	if (dump_fd >= 0) {
		close(dump_fd);
		if (success) {
			/* Test succeeded, no need to keep the captured data */
			unlink(dump_path);
		} else
			igt_debug("Saved captured audio data to %s\n", dump_path);
		free(dump_path);
	}

	free(recv);
	free(buf);
	free(channel);

	ok = chamelium_stream_stop_realtime_audio(stream);
	igt_assert(ok);

	audio_file = chamelium_stop_capturing_audio(data->chamelium,
						    port);
	if (audio_file) {
		igt_debug("Audio file saved on the Chamelium in %s\n",
			  audio_file->path);
		chamelium_destroy_audio_file(audio_file);
	}

	audio_signal_fini(signal);
	chamelium_stream_deinit(stream);

	igt_assert(success);
	return true;
}

static void
test_display_audio(data_t *data, struct chamelium_port *port,
		   const char *audio_device)
{
	bool run = false;
	struct alsa *alsa;
	int ret;
	igt_output_t *output;
	igt_plane_t *primary;
	struct igt_fb fb;
	drmModeModeInfo *mode;
	drmModeConnector *connector;
	int fb_id, i;

	igt_require(alsa_has_exclusive_access());

	alsa = alsa_init();
	igt_assert(alsa);

	reset_state(data, port);

	/* Use the default Chamelium EDID for this test, as the base IGT EDID
	 * doesn't advertise audio support (see drm_detect_monitor_audio in
	 * the kernel tree). */
	output = prepare_output(data, port, false);
	connector = chamelium_port_get_connector(data->chamelium, port, false);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	/* Enable the output because the receiver won't try to receive audio if
	 * it doesn't receive video. */
	igt_assert(connector->count_modes > 0);
	mode = &connector->modes[0];

	fb_id = igt_create_color_pattern_fb(data->drm_fd,
					    mode->hdisplay, mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    LOCAL_DRM_FORMAT_MOD_NONE,
					    0, 0, 0, &fb);
	igt_assert(fb_id > 0);

	enable_output(data, port, output, mode, &fb);

	for (i = 0; i < sampling_rates_count; i++) {
		ret = alsa_open_output(alsa, audio_device);
		igt_assert(ret >= 0);

		/* TODO: playback on all 8 available channels */
		run |= do_test_display_audio(data, port, alsa,
					     PLAYBACK_CHANNELS,
					     sampling_rates[i]);

		alsa_close_output(alsa);
	}

	/* Make sure we tested at least one frequency. */
	igt_assert(run);

	igt_remove_fb(data->drm_fd, &fb);

	drmModeFreeConnector(connector);

	free(alsa);
}


static void select_tiled_modifier(igt_plane_t *plane, uint32_t width,
				  uint32_t height, uint32_t format,
				  uint64_t *modifier)
{
	if (igt_plane_has_format_mod(plane, format,
				     DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED)) {
		igt_debug("Selecting VC4 T-tiling\n");

		*modifier = DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED;
	} else if (igt_plane_has_format_mod(plane, format,
					    DRM_FORMAT_MOD_BROADCOM_SAND256)) {
		/* Randomize the column height to less than twice the minimum. */
		size_t column_height = (rand() % height) + height;

		igt_debug("Selecting VC4 SAND256 tiling with column height %ld\n",
			  column_height);

		*modifier = DRM_FORMAT_MOD_BROADCOM_SAND256_COL_HEIGHT(column_height);
	} else {
		*modifier = DRM_FORMAT_MOD_LINEAR;
	}
}

static void randomize_plane_format_stride(igt_plane_t *plane,
					  uint32_t width, uint32_t height,
					  uint32_t *format, uint64_t *modifier,
					  size_t *stride, bool allow_yuv)
{
	size_t stride_min;
	uint32_t *formats_array;
	unsigned int formats_count;
	unsigned int count = 0;
	unsigned int i;
	bool tiled;
	int index;

	igt_format_array_fill(&formats_array, &formats_count, allow_yuv);

	/* First pass to count the supported formats. */
	for (i = 0; i < formats_count; i++)
		if (igt_plane_has_format_mod(plane, formats_array[i],
					     DRM_FORMAT_MOD_LINEAR))
			count++;

	igt_assert(count > 0);

	index = rand() % count;

	/* Second pass to get the index-th supported format. */
	for (i = 0; i < formats_count; i++) {
		if (!igt_plane_has_format_mod(plane, formats_array[i],
					      DRM_FORMAT_MOD_LINEAR))
			continue;

		if (!index--) {
			*format = formats_array[i];
			break;
		}
	}

	free(formats_array);

	igt_assert(index < 0);

	stride_min = width * igt_format_plane_bpp(*format, 0) / 8;

	/* Randomize the stride to less than twice the minimum. */
	*stride = (rand() % stride_min) + stride_min;

	/* Pixman requires the stride to be aligned to 32-byte words. */
	*stride = ALIGN(*stride, sizeof(uint32_t));

	/* Randomize the use of a tiled mode with a 1/4 probability. */
	tiled = ((rand() % 4) == 0);

	if (tiled)
		select_tiled_modifier(plane, width, height, *format, modifier);
	else
		*modifier = DRM_FORMAT_MOD_LINEAR;
}

static void randomize_plane_dimensions(drmModeModeInfo *mode,
				       uint32_t *width, uint32_t *height,
				       uint32_t *src_w, uint32_t *src_h,
				       uint32_t *src_x, uint32_t *src_y,
				       uint32_t *crtc_w, uint32_t *crtc_h,
				       int32_t *crtc_x, int32_t *crtc_y,
				       bool allow_scaling)
{
	double ratio;

	/* Randomize width and height in the mode dimensions range. */
	*width = (rand() % mode->hdisplay) + 1;
	*height = (rand() % mode->vdisplay) + 1;

	/* Randomize source offset in the first half of the original size. */
	*src_x = rand() % (*width / 2);
	*src_y = rand() % (*height / 2);

	/* The source size only includes the active source area. */
	*src_w = *width - *src_x;
	*src_h = *height - *src_y;

	if (allow_scaling) {
		*crtc_w = (rand() % mode->hdisplay) + 1;
		*crtc_h = (rand() % mode->vdisplay) + 1;

		/*
		 * Don't bother with scaling if dimensions are quite close in
		 * order to get non-scaling cases more frequently. Also limit
		 * scaling to 3x to avoid agressive filtering that makes
		 * comparison less reliable.
		 */

		ratio = ((double) *crtc_w / *src_w);
		if (ratio > 0.8 && ratio < 1.2)
			*crtc_w = *src_w;
		else if (ratio > 3.0)
			*crtc_w = *src_w * 3;

		ratio = ((double) *crtc_h / *src_h);
		if (ratio > 0.8 && ratio < 1.2)
			*crtc_h = *src_h;
		else if (ratio > 3.0)
			*crtc_h = *src_h * 3;
	} else {
		*crtc_w = *src_w;
		*crtc_h = *src_h;
	}

	if (*crtc_w != *src_w || *crtc_h != *src_h) {
		/*
		 * When scaling is involved, make sure to not go off-bounds or
		 * scaled clipping may result in decimal dimensions, that most
		 * drivers don't support.
		 */
		*crtc_x = rand() % (mode->hdisplay - *crtc_w);
		*crtc_y = rand() % (mode->vdisplay - *crtc_h);
	} else {
		/*
		 * Randomize the on-crtc position and allow the plane to go
		 * off-display by less than half of its on-crtc dimensions.
		 */
		*crtc_x = (rand() % mode->hdisplay) - *crtc_w / 2;
		*crtc_y = (rand() % mode->vdisplay) - *crtc_h / 2;
	}
}

static void blit_plane_cairo(data_t *data, cairo_surface_t *result,
			     uint32_t src_w, uint32_t src_h,
			     uint32_t src_x, uint32_t src_y,
			     uint32_t crtc_w, uint32_t crtc_h,
			     int32_t crtc_x, int32_t crtc_y,
			     struct igt_fb *fb)
{
	cairo_surface_t *surface;
	cairo_surface_t *clipped_surface;
	cairo_t *cr;

	surface = igt_get_cairo_surface(data->drm_fd, fb);

	if (src_x || src_y) {
		clipped_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
							     src_w, src_h);

		cr = cairo_create(clipped_surface);

		cairo_translate(cr, -1. * src_x, -1. * src_y);

		cairo_set_source_surface(cr, surface, 0, 0);

		cairo_paint(cr);
		cairo_surface_flush(clipped_surface);

		cairo_destroy(cr);
	} else {
		clipped_surface = surface;
	}

	cr = cairo_create(result);

	cairo_translate(cr, crtc_x, crtc_y);

	if (src_w != crtc_w || src_h != crtc_h) {
		cairo_scale(cr, (double) crtc_w / src_w,
			    (double) crtc_h / src_h);
	}

	cairo_set_source_surface(cr, clipped_surface, 0, 0);
	cairo_surface_destroy(clipped_surface);

	if (src_w != crtc_w || src_h != crtc_h) {
		cairo_pattern_set_filter(cairo_get_source(cr),
					 CAIRO_FILTER_BILINEAR);
		cairo_pattern_set_extend(cairo_get_source(cr),
					 CAIRO_EXTEND_NONE);
	}

	cairo_paint(cr);
	cairo_surface_flush(result);

	cairo_destroy(cr);
}

static void configure_plane(igt_plane_t *plane, uint32_t src_w, uint32_t src_h,
			    uint32_t src_x, uint32_t src_y, uint32_t crtc_w,
			    uint32_t crtc_h, int32_t crtc_x, int32_t crtc_y,
			    struct igt_fb *fb)
{
	igt_plane_set_fb(plane, fb);

	igt_plane_set_position(plane, crtc_x, crtc_y);
	igt_plane_set_size(plane, crtc_w, crtc_h);

	igt_fb_set_position(fb, plane, src_x, src_y);
	igt_fb_set_size(fb, plane, src_w, src_h);
}

static void prepare_randomized_plane(data_t *data,
				     drmModeModeInfo *mode,
				     igt_plane_t *plane,
				     struct igt_fb *overlay_fb,
				     unsigned int index,
				     cairo_surface_t *result_surface,
				     bool allow_scaling, bool allow_yuv)
{
	struct igt_fb pattern_fb;
	uint32_t overlay_fb_w, overlay_fb_h;
	uint32_t overlay_src_w, overlay_src_h;
	uint32_t overlay_src_x, overlay_src_y;
	int32_t overlay_crtc_x, overlay_crtc_y;
	uint32_t overlay_crtc_w, overlay_crtc_h;
	uint32_t format;
	uint64_t modifier;
	size_t stride;
	bool tiled;
	int fb_id;

	randomize_plane_dimensions(mode, &overlay_fb_w, &overlay_fb_h,
				   &overlay_src_w, &overlay_src_h,
				   &overlay_src_x, &overlay_src_y,
				   &overlay_crtc_w, &overlay_crtc_h,
				   &overlay_crtc_x, &overlay_crtc_y,
				   allow_scaling);

	igt_debug("Plane %d: framebuffer size %dx%d\n", index,
		  overlay_fb_w, overlay_fb_h);
	igt_debug("Plane %d: on-crtc size %dx%d\n", index,
		  overlay_crtc_w, overlay_crtc_h);
	igt_debug("Plane %d: on-crtc position %dx%d\n", index,
		  overlay_crtc_x, overlay_crtc_y);
	igt_debug("Plane %d: in-framebuffer size %dx%d\n", index,
		  overlay_src_w, overlay_src_h);
	igt_debug("Plane %d: in-framebuffer position %dx%d\n", index,
		  overlay_src_x, overlay_src_y);

	/* Get a pattern framebuffer for the overlay plane. */
	fb_id = chamelium_get_pattern_fb(data, overlay_fb_w, overlay_fb_h,
					 DRM_FORMAT_XRGB8888, 32, &pattern_fb);
	igt_assert(fb_id > 0);

	randomize_plane_format_stride(plane, overlay_fb_w, overlay_fb_h,
				      &format, &modifier, &stride, allow_yuv);

	tiled = (modifier != LOCAL_DRM_FORMAT_MOD_NONE);

	igt_debug("Plane %d: %s format (%s) with stride %ld\n", index,
		  igt_format_str(format), tiled ? "tiled" : "linear", stride);

	fb_id = igt_fb_convert_with_stride(overlay_fb, &pattern_fb, format,
					   modifier, stride);
	igt_assert(fb_id > 0);

	blit_plane_cairo(data, result_surface, overlay_src_w, overlay_src_h,
			 overlay_src_x, overlay_src_y,
			 overlay_crtc_w, overlay_crtc_h,
			 overlay_crtc_x, overlay_crtc_y, &pattern_fb);

	configure_plane(plane, overlay_src_w, overlay_src_h,
			overlay_src_x, overlay_src_y,
			overlay_crtc_w, overlay_crtc_h,
			overlay_crtc_x, overlay_crtc_y, overlay_fb);

	/* Remove the original pattern framebuffer. */
	igt_remove_fb(data->drm_fd, &pattern_fb);
}

static void test_display_planes_random(data_t *data,
				       struct chamelium_port *port,
				       enum chamelium_check check)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	igt_plane_t *primary_plane;
	struct igt_fb primary_fb;
	struct igt_fb result_fb;
	struct igt_fb *overlay_fbs;
	igt_crc_t *crc;
	igt_crc_t *expected_crc;
	struct chamelium_fb_crc_async_data *fb_crc;
	unsigned int overlay_planes_max = 0;
	unsigned int overlay_planes_count;
	cairo_surface_t *result_surface;
	int captured_frame_count;
	bool allow_scaling;
	bool allow_yuv;
	unsigned int i;
	unsigned int fb_id;

	switch (check) {
	case CHAMELIUM_CHECK_CRC:
		allow_scaling = false;
		allow_yuv = false;
		break;
	case CHAMELIUM_CHECK_CHECKERBOARD:
		allow_scaling = true;
		allow_yuv = true;
		break;
	default:
		igt_assert(false);
	}

	srand(time(NULL));

	reset_state(data, port);

	/* Find the connector and pipe. */
	output = prepare_output(data, port, true);

	mode = igt_output_get_mode(output);

	/* Get a framebuffer for the primary plane. */
	primary_plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary_plane);

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, &primary_fb);
	igt_assert(fb_id > 0);

	/* Get a framebuffer for the cairo composition result. */
	fb_id = igt_create_fb(data->drm_fd, mode->hdisplay,
			      mode->vdisplay, DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE, &result_fb);
	igt_assert(fb_id > 0);

	result_surface = igt_get_cairo_surface(data->drm_fd, &result_fb);

	/* Paint the primary framebuffer on the result surface. */
	blit_plane_cairo(data, result_surface, 0, 0, 0, 0, 0, 0, 0, 0,
			 &primary_fb);

	/* Configure the primary plane. */
	igt_plane_set_fb(primary_plane, &primary_fb);

	overlay_planes_max =
		igt_output_count_plane_type(output, DRM_PLANE_TYPE_OVERLAY);

	/* Limit the number of planes to a reasonable scene. */
	overlay_planes_max = max(overlay_planes_max, 4);

	overlay_planes_count = (rand() % overlay_planes_max) + 1;
	igt_debug("Using %d overlay planes\n", overlay_planes_count);

	overlay_fbs = calloc(sizeof(struct igt_fb), overlay_planes_count);

	for (i = 0; i < overlay_planes_count; i++) {
		struct igt_fb *overlay_fb = &overlay_fbs[i];
		igt_plane_t *plane =
			igt_output_get_plane_type_index(output,
							DRM_PLANE_TYPE_OVERLAY,
							i);
		igt_assert(plane);

		prepare_randomized_plane(data, mode, plane, overlay_fb, i,
					 result_surface, allow_scaling,
					 allow_yuv);
	}

	cairo_surface_destroy(result_surface);

	if (check == CHAMELIUM_CHECK_CRC)
		fb_crc = chamelium_calculate_fb_crc_async_start(data->drm_fd,
								&result_fb);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (check == CHAMELIUM_CHECK_CRC) {
		chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
		crc = chamelium_read_captured_crcs(data->chamelium,
						   &captured_frame_count);

		igt_assert(captured_frame_count == 1);

		expected_crc = chamelium_calculate_fb_crc_async_finish(fb_crc);

		chamelium_assert_crc_eq_or_dump(data->chamelium,
						expected_crc, crc,
						&result_fb, 0);

		free(expected_crc);
		free(crc);
	} else if (check == CHAMELIUM_CHECK_CHECKERBOARD) {
		struct chamelium_frame_dump *dump;

		dump = chamelium_port_dump_pixels(data->chamelium, port, 0, 0,
						  0, 0);
		chamelium_assert_frame_match_or_dump(data->chamelium, port,
						     dump, &result_fb, check);
		chamelium_destroy_frame_dump(dump);
	}

	for (i = 0; i < overlay_planes_count; i++) {
		struct igt_fb *overlay_fb = &overlay_fbs[i];
		igt_plane_t *plane;

		plane = igt_output_get_plane_type_index(output,
							DRM_PLANE_TYPE_OVERLAY,
							i);
		igt_assert(plane);

		igt_remove_fb(data->drm_fd, overlay_fb);
	}

	free(overlay_fbs);

	igt_remove_fb(data->drm_fd, &primary_fb);
	igt_remove_fb(data->drm_fd, &result_fb);
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

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
	}

	igt_subtest_group {
		igt_fixture {
			require_connector_present(
			    &data, DRM_MODE_CONNECTOR_DisplayPort);
		}

		connector_subtest("dp-hpd", DisplayPort)
			test_basic_hotplug(&data, port,
					   HPD_TOGGLE_COUNT_DP_HDMI);

		connector_subtest("dp-hpd-fast", DisplayPort)
			test_basic_hotplug(&data, port,
					   HPD_TOGGLE_COUNT_FAST);

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
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("dp-crc-fast", DisplayPort)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("dp-crc-multiple", DisplayPort)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 3);

		connector_subtest("dp-frame-dump", DisplayPort)
			test_display_frame_dump(&data, port);

		connector_subtest("dp-audio", DisplayPort)
			test_display_audio(&data, port, "HDMI");
	}

	igt_subtest_group {
		igt_fixture {
			require_connector_present(
			    &data, DRM_MODE_CONNECTOR_HDMIA);
		}

		connector_subtest("hdmi-hpd", HDMIA)
			test_basic_hotplug(&data, port,
					   HPD_TOGGLE_COUNT_DP_HDMI);

		connector_subtest("hdmi-hpd-fast", HDMIA)
			test_basic_hotplug(&data, port,
					   HPD_TOGGLE_COUNT_FAST);

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
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-fast", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-multiple", HDMIA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_CRC, 3);

		connector_subtest("hdmi-crc-argb8888", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_ARGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-abgr8888", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_ABGR8888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-xrgb8888", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB8888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-xbgr8888", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_XBGR8888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-rgb888", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_RGB888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-bgr888", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_BGR888,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-rgb565", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_RGB565,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-bgr565", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_BGR565,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-argb1555", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_ARGB1555,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-xrgb1555", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_XRGB1555,
					      CHAMELIUM_CHECK_CRC, 1);

		connector_subtest("hdmi-crc-planes-random", HDMIA)
			test_display_planes_random(&data, port,
						   CHAMELIUM_CHECK_CRC);

		connector_subtest("hdmi-cmp-nv12", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_NV12,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-nv16", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_NV16,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-nv21", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_NV21,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-nv61", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_NV61,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-yu12", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_YUV420,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-yu16", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_YUV422,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-yv12", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_YVU420,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-yv16", HDMIA)
			test_display_one_mode(&data, port, DRM_FORMAT_YVU422,
					      CHAMELIUM_CHECK_CHECKERBOARD, 1);

		connector_subtest("hdmi-cmp-planes-random", HDMIA)
			test_display_planes_random(&data, port,
						   CHAMELIUM_CHECK_CHECKERBOARD);

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

		connector_subtest("vga-hpd-fast", VGA)
			test_basic_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST);

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

		connector_subtest("vga-frame-dump", VGA)
			test_display_all_modes(&data, port, DRM_FORMAT_XRGB8888,
					       CHAMELIUM_CHECK_ANALOG, 1);
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
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
