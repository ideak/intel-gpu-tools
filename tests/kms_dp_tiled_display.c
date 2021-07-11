/*
 * Copyright © 2019 Intel Corporation
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
 *  Madhumitha Tolakanahalli Pradeep
 *      <madhumitha.tolakanahalli.pradeep@intel.com>
 *  Manasi Navare <manasi.d.navare@intel.com>
 *
 * Display Port Tiled Display Test
 * This test parses the tile information of the connectors that have TILE
 * property set, sets up the framebuffer with correct offsets corresponding to
 * the tile offsets and does an atomic modeset with two CRTCs for two
 * connectors. Page flip event timestamp from each CRTC is collected and
 * compared to make sure that they occurred in a synchronous manner.
 *
 * This test currently supports only horizontally tiled displays, in line with
 * the displays supported by the kernel at the moment.
 */

#include "igt.h"
#include "poll.h"
#include "drm_mode.h"
#include "drm_fourcc.h"
#include "igt_edid.h"

IGT_TEST_DESCRIPTION("Test for Transcoder Port Sync for Display Port Tiled Displays");

typedef struct {
	igt_output_t *output;
	igt_tile_info_t tile;
	enum pipe pipe;
	drmModeConnectorPtr connector;
	bool got_page_flip;
} data_connector_t;

typedef struct {
	int drm_fd;
	int num_h_tiles;
	igt_fb_t fb_test_pattern;
	igt_display_t *display;
	data_connector_t *conns;
	enum igt_commit_style commit;
	struct timeval first_ts;

	#ifdef HAVE_CHAMELIUM
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	int port_count;
	struct chamelium_edid *edids[IGT_CUSTOM_EDID_COUNT];
	#endif

} data_t;

void basic_test(data_t *data, drmEventContext *drm_event, struct pollfd *pfd);

#ifdef HAVE_CHAMELIUM
static void test_with_chamelium(data_t *data);
#endif

static int drm_property_is_tile(drmModePropertyPtr prop)
{
	return (strcmp(prop->name, "TILE") ? 0 : 1) &&
			 drm_property_type_is(prop, DRM_MODE_PROP_BLOB);
}

static void get_connector_tile_props(data_t *data, drmModeConnectorPtr conn,
				     igt_tile_info_t *tile)
{
	for (int i = 0; i < conn->count_props; i++) {
		drmModePropertyBlobPtr blob;
		drmModePropertyPtr prop;

		prop = drmModeGetProperty(data->drm_fd, conn->props[i]);
		igt_assert(prop);

		blob = NULL;
		if (drm_property_is_tile(prop)) {
			blob = drmModeGetPropertyBlob(data->drm_fd,
						      conn->prop_values[i]);
			if (blob) {
				igt_parse_connector_tile_blob(blob, tile);
				drmModeFreePropertyBlob(blob);
			}
		}
		drmModeFreeProperty(prop);
		if (blob)
			return;
	}
}

static void get_number_of_h_tiles(data_t *data)
{
	igt_tile_info_t tile = {};
	drmModeResPtr res;

	igt_assert(res = drmModeGetResources(data->drm_fd));

	for (int i = 0; !data->num_h_tiles && i < res->count_connectors; i++) {
		drmModeConnectorPtr connector;

		connector = drmModeGetConnectorCurrent(data->drm_fd,
						       res->connectors[i]);
		igt_assert(connector);

		if (connector->connection == DRM_MODE_CONNECTED &&
		    connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
			get_connector_tile_props(data, connector, &tile);

			data->num_h_tiles = tile.num_h_tile;
		}

		drmModeFreeConnector(connector);
	}

	drmModeFreeResources(res);
}

static void get_connectors(data_t *data)
{
	int count = 0;
	igt_output_t *output;
	data_connector_t *conns = data->conns;

	for_each_connected_output(data->display, output) {
		conns[count].connector = drmModeGetConnector(data->display->drm_fd,
							     output->id);

		igt_assert(conns[count].connector);

		if (conns[count].connector->connector_type !=
		    DRM_MODE_CONNECTOR_DisplayPort) {
			drmModeFreeConnector(conns[count].connector);
			continue;
		}

		get_connector_tile_props(data, conns[count].connector,
					 &conns[count].tile);

		if (conns[count].tile.num_h_tile == 0) {
			drmModeFreeConnector(conns[count].connector);
			continue;
		}

		/* Check if the connectors belong to the same tile group */
		if (count > 0)
			igt_assert(conns[count].tile.tile_group_id ==
				   conns[count-1].tile.tile_group_id);

		count++;
	}
	igt_require_f(count == data->num_h_tiles,
		     "All the tiled connectors are not connected\n");
}

static void
reset_plane(igt_output_t *output)
{
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
}

static void reset_output(igt_output_t *output)
{
	igt_output_set_pipe(output, PIPE_NONE);
}

static void reset_mode(data_t *data)
{
	int count;
	igt_output_t *output;
	data_connector_t *conns = data->conns;

	for (count = 0; count < data->num_h_tiles; count++) {
		output = igt_output_from_connector(data->display,
						   conns[count].connector);
		igt_output_set_pipe(output, PIPE_NONE);
	}
	igt_display_commit2(data->display, data->commit);
}

static void test_cleanup(data_t *data)
{
	int count;
	data_connector_t *conns = data->conns;

	for (count = 0; count < data->num_h_tiles; count++) {
		if (conns[count].output) {
			reset_plane(conns[count].output);
			reset_output(conns[count].output);
		}
	}
	igt_remove_fb(data->drm_fd, &data->fb_test_pattern);
	igt_display_commit2(data->display, data->commit);
	memset(conns, 0, sizeof(data_connector_t) * data->num_h_tiles);
}
static void setup_mode(data_t *data)
{
	int count = 0, prev = 0, i = 0;
	bool pipe_in_use = false, found = false;
	enum pipe pipe;
	drmModeModeInfo *mode;
	igt_output_t *output;
	data_connector_t *conns = data->conns;

	/*
	 * The output is set to PIPE_NONE and then assigned a pipe.
	 * This is done to ensure a complete modeset occures every
	 * time the test is run.
	 */
	reset_mode(data);

	for (count = 0; count < data->num_h_tiles; count++) {
		output = igt_output_from_connector(data->display,
						   conns[count].connector);

		for_each_pipe(data->display, pipe) {
			pipe_in_use = false;
			found = false;

			if (count > 0) {
				for (prev = count - 1; prev >= 0; prev--) {
					if (pipe == conns[prev].pipe) {
						pipe_in_use = true;
						break;
					}
				}
				if (pipe_in_use)
					continue;
			}

			if (igt_pipe_connector_valid(pipe, output)) {
				conns[count].pipe = pipe;
				conns[count].output = output;

				igt_output_set_pipe(conns[count].output,
						    conns[count].pipe);
				break;
			}
		}
		igt_require(conns[count].pipe != PIPE_NONE);

		for (i = 0; i < conns[count].connector->count_modes; i++) {
			mode = &conns[count].connector->modes[i];
			if (mode->vdisplay == conns[count].tile.tile_v_size &&
			    mode->hdisplay == conns[count].tile.tile_h_size) {
				found = true;
				break;
			}
		}
		igt_require(found);
		igt_output_override_mode(output, mode);
	}
	igt_display_commit_atomic(data->display, DRM_MODE_ATOMIC_ALLOW_MODESET,
				  NULL);
}

static void setup_framebuffer(data_t *data)
{
	int count;
	igt_plane_t *primary;
	int fb_h_size = 0, fb_v_size = 0;
	data_connector_t *conns = data->conns;

	for (count = 0; count < data->num_h_tiles; count++) {

		fb_h_size += conns[count].tile.tile_h_size;
		/* We support only horizontal tiles, so vertical size is same
		 * for all tiles and needs to be assigned only once.
		 */
		if (!fb_v_size)
			fb_v_size = conns[count].tile.tile_v_size;

		if (count > 0)
			igt_assert(conns[count].tile.tile_v_size ==
				   conns[count-1].tile.tile_v_size);
	}

	igt_create_pattern_fb(data->drm_fd,
			      fb_h_size,
			      fb_v_size,
			      DRM_FORMAT_XBGR8888,
			      DRM_FORMAT_MOD_NONE,
			      &data->fb_test_pattern);

	for (count = 0; count < data->num_h_tiles; count++) {
		primary = igt_output_get_plane_type(conns[count].output,
						    DRM_PLANE_TYPE_PRIMARY);

		igt_plane_set_fb(primary, &data->fb_test_pattern);

		igt_fb_set_size(&data->fb_test_pattern, primary,
				conns[count].tile.tile_h_size,
				conns[count].tile.tile_v_size);

		igt_fb_set_position(&data->fb_test_pattern, primary,
				    (conns[count].tile.tile_h_size *
				     conns[count].tile.tile_h_loc),
				    (conns[count].tile.tile_v_size *
				     conns[count].tile.tile_v_loc));

		igt_plane_set_size(primary,
				   conns[count].tile.tile_h_size,
				   conns[count].tile.tile_v_size);
	}
}

static data_connector_t *conn_for_crtc(data_t *data, unsigned int crtc_id)
{
	for (int i = 0; i < data->num_h_tiles; i++) {
		data_connector_t *conn = &data->conns[i];

		if (data->display->pipes[conn->pipe].crtc_id == crtc_id)
			return conn;
	}

	return NULL;
}

static void page_flip_handler(int fd, unsigned int seq,
			      unsigned int tv_sec, unsigned int tv_usec,
			      unsigned int crtc_id, void *_data)
{
	data_t *data = _data;
	data_connector_t *conn = conn_for_crtc(data, crtc_id);
	struct timeval current_ts = {
		.tv_sec = tv_sec,
		.tv_usec = tv_usec,
	};
	struct timeval diff;
	long usec;

	if (!timerisset(&data->first_ts))
		data->first_ts = current_ts;

	igt_assert_f(conn, "Got page-flip event for unexpected CRTC %u\n",
		     crtc_id);

	igt_assert_f(!conn->got_page_flip, "Got two page-flips for CRTC %u\n",
		     crtc_id);

	igt_debug("Page Flip Event received from CRTC:%d at %u:%u\n", crtc_id,
		  tv_sec, tv_usec);

	conn->got_page_flip = true;

	timersub(&current_ts, &data->first_ts, &diff);
	usec = diff.tv_sec * 1000000 + diff.tv_usec;

	/*
	 * For seamless tear-free display, the page flip event timestamps
	 * from all the tiles should not differ by more than 10us.
	 */
	igt_fail_on_f(labs(usec) >= 10, "Delayed page flip event from CRTC:%d at %u:%u\n",
		      crtc_id, tv_sec, tv_usec);
}

static bool got_all_page_flips(data_t *data)
{
	int i;

	for (i = 0; i < data->num_h_tiles; i++) {
		if (!data->conns[i].got_page_flip)
			return false;
	}

	return true;
}

#ifdef HAVE_CHAMELIUM
static void test_with_chamelium(data_t *data)
{
	int i, count = 0;
	uint8_t htile = 2, vtile = 1;
	struct edid **edid;

	data->chamelium = chamelium_init(data->drm_fd);
	igt_require(data->chamelium);
	data->ports = chamelium_get_ports
		(data->chamelium, &data->port_count);
	chamelium_require_connector_present(data->ports,
					    DRM_MODE_CONNECTOR_DisplayPort,
					    data->port_count, 2);
	edid = igt_kms_get_tiled_edid(htile-1, vtile-1);

	for (i = 0; i < 2; i++)
		data->edids[i] =
			chamelium_new_edid(data->chamelium, edid[i]);

	for (i = 0; i < data->port_count; i++) {
		if (chamelium_port_get_type(data->ports[i]) ==
				DRM_MODE_CONNECTOR_DisplayPort) {

			chamelium_port_set_tiled_edid(data->chamelium,
				data->ports[i], data->edids[i]);
			chamelium_plug(data->chamelium,
				       data->ports[i]);
			chamelium_wait_for_conn_status_change(data->display,
							      data->chamelium,
							      data->ports[i],
							      DRM_MODE_CONNECTED);
			count++;
		}
		if (count == 2)
			break;
	}
}
#endif

void basic_test(data_t *data, drmEventContext *drm_event, struct pollfd *pfd)
{
		int ret;

		get_number_of_h_tiles(data);
		igt_debug("Number of Horizontal Tiles: %d\n",
			  data->num_h_tiles);
		igt_require(data->num_h_tiles > 0);
		data->conns = calloc(data->num_h_tiles,
				     sizeof(data_connector_t));
		igt_assert(data->conns);

		get_connectors(data);
		setup_mode(data);
		setup_framebuffer(data);
		timerclear(&data->first_ts);
		igt_display_commit_atomic(data->display,
			DRM_MODE_ATOMIC_NONBLOCK |
			DRM_MODE_PAGE_FLIP_EVENT, data);
		while (!got_all_page_flips(data)) {
			ret = poll(pfd, 1, 1000);
			igt_assert(ret == 1);
			drmHandleEvent(data->drm_fd, drm_event);
		}
}

igt_main
{
	igt_display_t display;
	data_t data = {0};
	struct pollfd pfd = {0};
	drmEventContext drm_event = {0};
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&display, data.drm_fd);
		igt_display_reset(&display);
		data.display = &display;
		pfd.fd = data.drm_fd;
		pfd.events = POLLIN;
		drm_event.version = 3;
		drm_event.page_flip_handler2 = page_flip_handler;
		data.commit = data.display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY;
		igt_require(data.commit == COMMIT_ATOMIC);
	}

	igt_describe("Make sure the Tiled CRTCs are synchronized and we get "
		     "page flips for all tiled CRTCs in one vblank.");
	igt_subtest("basic-test-pattern") {
		basic_test(&data, &drm_event, &pfd);
		test_cleanup(&data);
	}

	#ifdef HAVE_CHAMELIUM
        igt_describe("Make sure the Tiled CRTCs are synchronized and we get "
                     "page flips for all tiled CRTCs in one vblank (executes on chamelium).");
	igt_subtest_f("basic-test-pattern-with-chamelium") {
		int i;

		test_with_chamelium(&data);
		basic_test(&data, &drm_event, &pfd);
		test_cleanup(&data);
		for (i = 0; i < data.port_count; i++)
			chamelium_reset_state(data.display, data.chamelium,
					      data.ports[i], data.ports,
					      data.port_count);
	}
	#endif
	igt_fixture {
		free(data.conns);
		close(data.drm_fd);
		kmstest_restore_vt_mode();
		igt_display_fini(data.display);
	}
}
