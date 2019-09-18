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
} data_t;

static int drm_property_is_tile(drmModePropertyPtr prop)
{
	return (strcmp(prop->name, "TILE") ? 0 : 1) &&
			 drm_property_type_is(prop, DRM_MODE_PROP_BLOB);
}

static void get_connector_tile_props(data_t *data, drmModeConnectorPtr conn,
				     igt_tile_info_t *tile)
{
	int i = 0;
	drmModePropertyPtr prop;
	drmModePropertyBlobPtr blob;

	for (i = 0; i < conn->count_props; i++) {
		prop = drmModeGetProperty(data->drm_fd, conn->props[i]);

		igt_assert(prop);

		if (!drm_property_is_tile(prop)) {
			drmModeFreeProperty(prop);
			continue;
		}

		blob = drmModeGetPropertyBlob(data->drm_fd,
				conn->prop_values[i]);

		if (!blob)
			goto cleanup;

		igt_parse_connector_tile_blob(blob, tile);
		break;
	}

cleanup:
	drmModeFreeProperty(prop);
	drmModeFreePropertyBlob(blob);
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
	int count = 0, prev = 0;
	bool pipe_in_use = false;
	enum pipe pipe;
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
			      LOCAL_DRM_FORMAT_MOD_NONE,
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

static void page_flip_handler(int fd, unsigned int seq,
			      unsigned int tv_sec, unsigned int tv_usec,
			      unsigned int crtc_id, void *_data)
{
	data_t *data = _data;
	data_connector_t *conn;
	bool is_on_time = false;
	static unsigned int _tv_sec, _tv_usec;
	int i;

	igt_debug("Page Flip Event received from CRTC:%d at %u:%u\n", crtc_id,
		  tv_sec, tv_usec);

	for (i = 0; i < data->num_h_tiles; i++) {

		conn = &data->conns[i];
		if (data->display->pipes[conn->pipe].crtc_id == crtc_id) {
			igt_assert_f(!conn->got_page_flip,
				     "Got two page-flips for CRTC %u\n",
				     crtc_id);
			conn->got_page_flip = true;

			/* Skip the following checks for the first page flip event */
			if (_tv_sec == 0 || _tv_usec == 0) {
				_tv_sec = tv_sec;
				_tv_usec = tv_usec;
				return;
			}
			/*
			 * For seamless tear-free display, the page flip event timestamps
			 * from all the tiles should not differ by more than 10us.
			 */
			is_on_time = tv_sec == _tv_sec && (abs(tv_usec - _tv_usec) < 10);

			igt_fail_on_f(!is_on_time, "Delayed page flip event from CRTC:%d at %u:%u\n",
				      crtc_id, tv_sec, tv_usec);
			return;
		}
	}

	igt_assert_f(false, "Got page-flip event for unexpected CRTC %u\n",
		     crtc_id);
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

igt_main
{
	igt_display_t display;
	data_t data = {0};
	struct pollfd pfd = {0};
	drmEventContext drm_event = {0};
	int ret;

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

		get_number_of_h_tiles(&data);
		igt_debug("Number of Horizontal Tiles: %d\n", data.num_h_tiles);
		igt_require(data.num_h_tiles > 0);
		data.conns = calloc(data.num_h_tiles, sizeof(data_connector_t));
	}

	igt_describe("Make sure the Tiled CRTCs are synchronized and we get "
		     "page flips for all tiled CRTCs in one vblank.");
	igt_subtest("basic-test-pattern") {
		igt_assert(data.conns);

		get_connectors(&data);
		setup_mode(&data);
		setup_framebuffer(&data);
		igt_display_commit_atomic(data.display, DRM_MODE_ATOMIC_NONBLOCK |
					  DRM_MODE_PAGE_FLIP_EVENT, &data);
		while (!got_all_page_flips(&data)) {
			ret = poll(&pfd, 1, 1000);
			igt_assert(ret == 1);
			drmHandleEvent(data.drm_fd, &drm_event);
		}

		test_cleanup(&data);
	}

	igt_fixture {
		free(data.conns);
		close(data.drm_fd);
		kmstest_restore_vt_mode();
		igt_display_fini(data.display);
	}
}
