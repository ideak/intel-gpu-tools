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

#include "igt_edid.h"
#include "kms_chamelium_helper.h"

void chamelium_init_test(chamelium_data_t *data)
{
	int i;

	/* So fbcon doesn't try to reprobe things itself */
	kmstest_set_vt_graphics_mode();

	data->drm_fd = drm_open_driver_master(DRIVER_ANY);
	igt_display_require(&data->display, data->drm_fd);
	igt_require(data->display.is_atomic);

	/*
	 * XXX: disabling modeset, can be removed when
	 * igt_display_require will start doing this for us
	 */
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	/* we need to initalize chamelium after igt_display_require */
	data->chamelium = chamelium_init(data->drm_fd, &data->display);
	igt_require(data->chamelium);

	data->ports = chamelium_get_ports(data->chamelium, &data->port_count);

	for (i = 0; i < IGT_CUSTOM_EDID_COUNT; ++i) {
		data->edids[i] = chamelium_new_edid(data->chamelium,
						    igt_kms_get_custom_edid(i));
	}
}

/* Wait for hotplug and return the remaining time left from timeout */
bool chamelium_wait_for_hotplug(struct udev_monitor *mon, int *timeout)
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

/**
 * chamelium_wait_for_connector_after_hotplug:
 *
 * Waits for the connector attached to @port to have a status of @status after
 * it's plugged/unplugged.
 *
 */
void chamelium_wait_for_connector_after_hotplug(chamelium_data_t *data,
						struct udev_monitor *mon,
						struct chamelium_port *port,
						drmModeConnection status)
{
	int timeout = CHAMELIUM_HOTPLUG_TIMEOUT;
	int hotplug_count = 0;

	igt_debug("Waiting for %s to get %s after a hotplug event...\n",
		  chamelium_port_get_name(port),
		  kmstest_connector_status_str(status));

	while (timeout > 0) {
		if (!chamelium_wait_for_hotplug(mon, &timeout))
			break;

		hotplug_count++;

		if (chamelium_reprobe_connector(&data->display, data->chamelium,
						port) == status)
			return;
	}

	igt_assert_f(
		false,
		"Timed out waiting for %s to get %s after a hotplug. Current state %s hotplug_count %d\n",
		chamelium_port_get_name(port),
		kmstest_connector_status_str(status),
		kmstest_connector_status_str(chamelium_reprobe_connector(
			&data->display, data->chamelium, port)),
		hotplug_count);
}

/**
 * chamelium_port_get_connector:
 * @data: The Chamelium data instance to use
 * @port: The chamelium port to prepare its connector
 * @edid: The chamelium's default EDID has a lot of resolutions, way more then
 * 		  we need to test. Additionally the default EDID doesn't support
 *        HDMI audio.
 *
 * Makes sure the output display of the connector attached to @port is connected
 * and ready for use.
 *
 * Returns: a pointer to the enabled igt_output_t
 */
igt_output_t *chamelium_prepare_output(chamelium_data_t *data,
				       struct chamelium_port *port,
				       enum igt_custom_edid_type edid)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	/* The chamelium's default EDID has a lot of resolutions, way more then
	 * we need to test. Additionally the default EDID doesn't support HDMI
	 * audio.
	 */
	chamelium_set_edid(data, port, edid);

	chamelium_plug(data->chamelium, port);
	chamelium_wait_for_conn_status_change(&data->display, data->chamelium,
					      port, DRM_MODE_CONNECTED);

	igt_display_reset(display);

	output = chamelium_get_output_for_port(data, port);

	/* Refresh pipe to update connected status */
	igt_output_set_pipe(output, PIPE_NONE);

	pipe = chamelium_get_pipe_for_output(display, output);
	igt_output_set_pipe(output, pipe);

	return output;
}

/**
 * chamelium_enable_output:
 *
 * Modesets the connector attached to @port for the assigned @mode and draws the
 * @fb.
 *
 */
void chamelium_enable_output(chamelium_data_t *data,
			     struct chamelium_port *port, igt_output_t *output,
			     drmModeModeInfo *mode, struct igt_fb *fb)
{
	igt_display_t *display = output->display;
	igt_plane_t *primary =
		igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	drmModeConnector *connector =
		chamelium_port_get_connector(data->chamelium, port, false);

	igt_assert(primary);

	igt_plane_set_size(primary, mode->hdisplay, mode->vdisplay);
	igt_plane_set_fb(primary, fb);
	igt_output_override_mode(output, mode);

	/* Clear any color correction values that might be enabled */
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_DEGAMMA_LUT))
		igt_pipe_obj_replace_prop_blob(primary->pipe,
					       IGT_CRTC_DEGAMMA_LUT, NULL, 0);
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_GAMMA_LUT))
		igt_pipe_obj_replace_prop_blob(primary->pipe,
					       IGT_CRTC_GAMMA_LUT, NULL, 0);
	if (igt_pipe_obj_has_prop(primary->pipe, IGT_CRTC_CTM))
		igt_pipe_obj_replace_prop_blob(primary->pipe, IGT_CRTC_CTM,
					       NULL, 0);

	igt_display_commit2(display, COMMIT_ATOMIC);

	if (chamelium_port_get_type(port) == DRM_MODE_CONNECTOR_VGA)
		usleep(250000);

	drmModeFreeConnector(connector);
}

/* Return pipe attached to @outpu.t */
enum pipe chamelium_get_pipe_for_output(igt_display_t *display,
					igt_output_t *output)
{
	enum pipe pipe;

	for_each_pipe(display, pipe) {
		if (igt_pipe_connector_valid(pipe, output)) {
			return pipe;
		}
	}

	igt_assert_f(false, "No pipe found for output %s\n",
		     igt_output_name(output));
}

static void chamelium_paint_xr24_pattern(uint32_t *data, size_t width,
					 size_t height, size_t stride,
					 size_t block_size)
{
	uint32_t colors[] = { 0xff000000, 0xffff0000, 0xff00ff00, 0xff0000ff,
			      0xffffffff };
	unsigned i, j;

	for (i = 0; i < height; i++)
		for (j = 0; j < width; j++)
			*(data + i * stride / 4 +
			  j) = colors[((j / block_size) + (i / block_size)) % 5];
}

/**
 * chamelium_get_pattern_fb:
 *
 * Creates an @fb with an xr24 pattern and returns the fb_id.
 *
 */
int chamelium_get_pattern_fb(chamelium_data_t *data, size_t width,
			     size_t height, uint32_t fourcc, size_t block_size,
			     struct igt_fb *fb)
{
	int fb_id;
	void *ptr;

	igt_assert(fourcc == DRM_FORMAT_XRGB8888);

	fb_id = igt_create_fb(data->drm_fd, width, height, fourcc,
			      DRM_FORMAT_MOD_LINEAR, fb);
	igt_assert(fb_id > 0);

	ptr = igt_fb_map_buffer(fb->fd, fb);
	igt_assert(ptr);

	chamelium_paint_xr24_pattern(ptr, width, height, fb->strides[0],
				     block_size);
	igt_fb_unmap_buffer(fb, ptr);

	return fb_id;
}

/* Generate a simple @fb for the size of @mode. */
void chamelium_create_fb_for_mode(chamelium_data_t *data, struct igt_fb *fb,
				  drmModeModeInfo *mode)
{
	int fb_id;

	fb_id = chamelium_get_pattern_fb(data, mode->hdisplay, mode->vdisplay,
					 DRM_FORMAT_XRGB8888, 64, fb);

	igt_assert(fb_id > 0);
}

/* Returns the first preferred mode for the connector attached to @port. */
drmModeModeInfo chamelium_get_mode_for_port(struct chamelium *chamelium,
					    struct chamelium_port *port)
{
	drmModeConnector *connector =
		chamelium_port_get_connector(chamelium, port, false);
	drmModeModeInfo mode;
	igt_assert(&connector->modes[0] != NULL);
	memcpy(&mode, &connector->modes[0], sizeof(mode));
	drmModeFreeConnector(connector);
	return mode;
}

/* Returns the igt display output for the connector attached to @port. */
igt_output_t *chamelium_get_output_for_port(chamelium_data_t *data,
					    struct chamelium_port *port)
{
	drmModeConnector *connector =
		chamelium_port_get_connector(data->chamelium, port, true);
	igt_output_t *output =
		igt_output_from_connector(&data->display, connector);
	drmModeFreeConnector(connector);
	igt_assert(output != NULL);
	return output;
}

/* Set the EDID of index @edid to Chamelium's @port. */
void chamelium_set_edid(chamelium_data_t *data, struct chamelium_port *port,
			enum igt_custom_edid_type edid)
{
	chamelium_port_set_edid(data->chamelium, port, data->edids[edid]);
}

/**
 * chamelium_check_analog_bridge:
 *
 * Check if the connector associalted to @port is an analog bridge by checking
 * if it has its own EDID.
 *
 */
bool chamelium_check_analog_bridge(chamelium_data_t *data,
				   struct chamelium_port *port)
{
	drmModePropertyBlobPtr edid_blob = NULL;
	drmModeConnector *connector =
		chamelium_port_get_connector(data->chamelium, port, false);
	uint64_t edid_blob_id;
	const struct edid *edid;
	char edid_vendor[3];

	if (chamelium_port_get_type(port) != DRM_MODE_CONNECTOR_VGA) {
		drmModeFreeConnector(connector);
		return false;
	}

	igt_assert(kmstest_get_property(data->drm_fd, connector->connector_id,
					DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL,
					&edid_blob_id, NULL));
	igt_assert(edid_blob =
			   drmModeGetPropertyBlob(data->drm_fd, edid_blob_id));

	edid = (const struct edid *)edid_blob->data;
	edid_get_mfg(edid, edid_vendor);

	drmModeFreePropertyBlob(edid_blob);
	drmModeFreeConnector(connector);

	/* Analog bridges provide their own EDID */
	if (edid_vendor[0] != 'I' || edid_vendor[1] != 'G' ||
	    edid_vendor[2] != 'T')
		return true;

	return false;
}