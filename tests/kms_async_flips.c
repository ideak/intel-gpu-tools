/*
 * Copyright © 2020 Intel Corporation
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
 *  Paulo Zanoni <paulo.r.zanoni@intel.com>
 *  Karthik B S <karthik.b.s@intel.com>
 */

#include "igt.h"
#include "igt_aux.h"
#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>

#define CURSOR_POS 128

/*
 * These constants can be tuned in case we start getting unexpected
 * results in CI.
 */

#define RUN_TIME 2
#define MIN_FLIPS_PER_FRAME 5

IGT_TEST_DESCRIPTION("Test asynchronous page flips.");

typedef struct {
	int drm_fd;
	uint32_t crtc_id;
	uint32_t refresh_rate;
	struct igt_fb bufs[4];
	igt_display_t display;
	drmModeConnectorPtr connector;
	unsigned long flip_timestamp_us;
	double flip_interval;
} data_t;

static drmModeConnectorPtr find_connector_for_modeset(data_t *data)
{
	igt_output_t *output;
	drmModeConnectorPtr ret = NULL;

	for_each_connected_output(&data->display, output) {
		if (output->config.connector->count_modes > 0) {
			ret = output->config.connector;
			break;
		}
	}

	igt_assert_f(ret, "Connector NOT found\n");
	return ret;
}

static void flip_handler(int fd_, unsigned int sequence, unsigned int tv_sec,
			 unsigned int tv_usec, void *_data)
{
	data_t *data = _data;
	static double last_ms;
	double cur_ms;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	cur_ms =  ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;

	if (last_ms)
		data->flip_interval = cur_ms - last_ms;
	else
		data->flip_interval = 0;

	last_ms = cur_ms;

	data->flip_timestamp_us = tv_sec * 1000000l + tv_usec;
}

static void wait_flip_event(data_t *data)
{
	int ret;
	drmEventContext evctx;
	struct pollfd pfd;

	evctx.version = 2;
	evctx.vblank_handler = NULL;
	evctx.page_flip_handler = flip_handler;

	pfd.fd = data->drm_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	ret = poll(&pfd, 1, 2000);

	switch (ret) {
	case 0:
		igt_assert_f(0, "Flip Timeout\n");
		break;
	case 1:
		ret = drmHandleEvent(data->drm_fd, &evctx);
		igt_assert(ret == 0);
		break;
	default:
		/* unexpected */
		igt_assert(0);
	}
}

static void make_fb(data_t *data, struct igt_fb *fb,
		    drmModeConnectorPtr connector, int index)
{
	uint32_t width, height;
	int rec_width;

	width = connector->modes[0].hdisplay;
	height = connector->modes[0].vdisplay;

	rec_width = width / (ARRAY_SIZE(data->bufs) * 2);

	igt_create_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
		      I915_FORMAT_MOD_X_TILED, fb);
	igt_draw_fill_fb(data->drm_fd, fb, 0x88);
	igt_draw_rect_fb(data->drm_fd, NULL, 0, fb, IGT_DRAW_MMAP_CPU,
			 rec_width * 2 + rec_width * index,
			 height / 4, rec_width,
			 height / 2, rand());
}

static void require_monotonic_timestamp(int fd)
{
	igt_require_f(igt_has_drm_cap(fd, DRM_CAP_TIMESTAMP_MONOTONIC),
		      "Monotonic timestamps not supported\n");
}

static void test_async_flip(data_t *data, bool alternate_sync_async)
{
	int ret, frame;
	long long int fps;
	struct timeval start, end, diff;

	require_monotonic_timestamp(data->drm_fd);

	gettimeofday(&start, NULL);
	frame = 1;
	do {
		int flags = DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT;

		if (alternate_sync_async) {
			flags &= ~DRM_MODE_PAGE_FLIP_ASYNC;

			ret = drmModePageFlip(data->drm_fd, data->crtc_id,
					      data->bufs[frame % 4].fb_id,
					      flags, data);

			igt_assert(ret == 0);

			wait_flip_event(data);

			flags |= DRM_MODE_PAGE_FLIP_ASYNC;

			/*
			 * In older platforms (<= Gen10), async address update bit is double buffered.
			 * So flip timestamp can be verified only from the second flip.
			 * The first async flip just enables the async address update.
			 */
			if (is_i915_device(data->drm_fd)) {
				uint32_t devid = intel_get_drm_devid(data->drm_fd);

				if (IS_GEN9(devid) || IS_GEN10(devid)) {
					ret = drmModePageFlip(data->drm_fd, data->crtc_id,
							      data->bufs[frame % 4].fb_id,
							      flags, data);

					igt_assert(ret == 0);

					wait_flip_event(data);
				}
			}
		}

		ret = drmModePageFlip(data->drm_fd, data->crtc_id,
				      data->bufs[frame % 4].fb_id,
				      flags, data);

		igt_assert(ret == 0);

		wait_flip_event(data);

		gettimeofday(&end, NULL);
		timersub(&end, &start, &diff);

		if (alternate_sync_async) {
			igt_assert_f(data->flip_interval < 1000.0 / (data->refresh_rate * MIN_FLIPS_PER_FRAME),
				     "Flip interval not significantly smaller than vblank interval\n"
				     "Flip interval: %lfms, Refresh Rate = %dHz, Threshold = %d\n",
				     data->flip_interval, data->refresh_rate, MIN_FLIPS_PER_FRAME);
		}

		frame++;
	} while (diff.tv_sec < RUN_TIME);

	if (!alternate_sync_async) {
		fps = frame * 1000 / RUN_TIME;
		igt_assert_f((fps / 1000) > (data->refresh_rate * MIN_FLIPS_PER_FRAME),
			     "FPS should be significantly higher than the refresh rate\n");
	}
}

static void wait_for_vblank(data_t *data, unsigned long *vbl_time, unsigned int *seq)
{
	drmVBlank wait_vbl;
	uint32_t pipe_id_flag;
	int pipe;

	memset(&wait_vbl, 0, sizeof(wait_vbl));
	pipe = kmstest_get_pipe_from_crtc_id(data->drm_fd, data->crtc_id);
	pipe_id_flag = kmstest_get_vbl_flag(pipe);

	wait_vbl.request.type = DRM_VBLANK_RELATIVE | pipe_id_flag;
	wait_vbl.request.sequence = 1;

	igt_assert(drmIoctl(data->drm_fd, DRM_IOCTL_WAIT_VBLANK, &wait_vbl) == 0);
	*vbl_time = wait_vbl.reply.tval_sec * 1000000 + wait_vbl.reply.tval_usec;
	*seq = wait_vbl.reply.sequence;
}

static void test_timestamp(data_t *data)
{
	int flags = DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT;
	unsigned long vbl_time, vbl_time1;
	unsigned int seq, seq1;
	int ret;

	require_monotonic_timestamp(data->drm_fd);

	/*
	 * In older platforms(<= gen10), async address update bit is double buffered.
	 * So flip timestamp can be verified only from the second flip.
	 * The first async flip just enables the async address update.
	 */
	ret = drmModePageFlip(data->drm_fd, data->crtc_id,
			      data->bufs[0].fb_id,
			      flags, data);

	igt_assert(ret == 0);

	wait_flip_event(data);

	wait_for_vblank(data, &vbl_time, &seq);

	ret = drmModePageFlip(data->drm_fd, data->crtc_id,
			      data->bufs[0].fb_id,
			      flags, data);

	igt_assert(ret == 0);

	wait_flip_event(data);

	wait_for_vblank(data, &vbl_time1, &seq1);

	/* TODO: Make changes to do as many flips as possbile between two vblanks */

	igt_assert_f(seq1 == seq + 1,
		     "Vblank sequence is expected to be incremented by one(%d != (%d + 1)\n", seq1, seq);

	igt_info("vbl1_timestamp = %ldus\nflip_timestamp = %ldus\nvbl2_timestamp = %ldus\n",
		 vbl_time, data->flip_timestamp_us, vbl_time1);

	igt_assert_f(vbl_time <= data->flip_timestamp_us && vbl_time1 > data->flip_timestamp_us,
		     "Async flip time stamp is expected to be in between 2 vblank time stamps\n");
}

static void test_cursor(data_t *data)
{
	int flags = DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT;
	int ret;
	uint64_t width, height;
	struct igt_fb cursor_fb;
	struct drm_mode_cursor cur;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &width));
	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &height));

	igt_create_color_fb(data->drm_fd, width, height, DRM_FORMAT_ARGB8888,
			    DRM_FORMAT_MOD_NONE, 1., 1., 1., &cursor_fb);

	cur.flags = DRM_MODE_CURSOR_BO;
	cur.crtc_id = data->crtc_id;
	cur.width = width;
	cur.height = height;
	cur.handle = cursor_fb.gem_handle;

	do_ioctl(data->drm_fd, DRM_IOCTL_MODE_CURSOR, &cur);

	ret = drmModePageFlip(data->drm_fd, data->crtc_id,
			      data->bufs[0].fb_id,
			      flags, data);

	igt_assert(ret == 0);

	wait_flip_event(data);

	cur.flags = DRM_MODE_CURSOR_MOVE;
	cur.x = CURSOR_POS;
	cur.y = CURSOR_POS;

	do_ioctl(data->drm_fd, DRM_IOCTL_MODE_CURSOR, &cur);

	igt_remove_fb(data->drm_fd, &cursor_fb);
}

static void test_invalid(data_t *data)
{
	int flags = DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT;
	int ret;
	uint32_t width, height;
	struct igt_fb fb;

	width = data->connector->modes[0].hdisplay;
	height = data->connector->modes[0].vdisplay;

	igt_require(igt_display_has_format_mod(&data->display, DRM_FORMAT_XRGB8888,
					       I915_FORMAT_MOD_Y_TILED));

	igt_create_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
		      I915_FORMAT_MOD_Y_TILED, &fb);

	/* Flip with a different fb modifier which is expected to be rejected */
	ret = drmModePageFlip(data->drm_fd, data->crtc_id,
			      fb.fb_id, flags, data);

	igt_assert(ret == -EINVAL);

	/* TODO: Add verification for changes in stride, pixel format */

	igt_remove_fb(data->drm_fd, &fb);
}

static void test_init(data_t *data)
{
	drmModeResPtr res;
	int i, ret;

	res = drmModeGetResources(data->drm_fd);
	igt_assert(res);

	kmstest_unset_all_crtcs(data->drm_fd, res);

	data->connector = find_connector_for_modeset(data);
	data->crtc_id = kmstest_find_crtc_for_connector(data->drm_fd,
							res, data->connector, 0);

	data->refresh_rate = data->connector->modes[0].vrefresh;

	for (i = 0; i < ARRAY_SIZE(data->bufs); i++)
		make_fb(data, &data->bufs[i], data->connector, i);

	ret = drmModeSetCrtc(data->drm_fd, data->crtc_id, data->bufs[0].fb_id, 0, 0,
			     &data->connector->connector_id, 1, &data->connector->modes[0]);

	igt_assert(ret == 0);

	drmModeFreeResources(res);
}

igt_main
{
	static data_t data;
	int i;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);

		igt_require_f(igt_has_drm_cap(data.drm_fd, DRM_CAP_ASYNC_PAGE_FLIP),
			      "Async Flip is not supported\n");
	}

	igt_describe("Verify the async flip functionality and the fps during async flips");
	igt_subtest_group {
		igt_fixture
			test_init(&data);

		igt_describe("Wait for page flip events in between successive asynchronous flips");
		igt_subtest("async-flip-with-page-flip-events")
			test_async_flip(&data, false);

		igt_describe("Alternate between sync and async flips");
		igt_subtest("alternate-sync-async-flip")
			test_async_flip(&data, true);

		igt_describe("Verify that the async flip timestamp does not coincide with either previous or next vblank");
		igt_subtest("test-time-stamp")
			test_timestamp(&data);

		igt_describe("Verify that the DRM_IOCTL_MODE_CURSOR passes after async flip");
		igt_subtest("test-cursor")
			test_cursor(&data);

		igt_describe("Negative case to verify if changes in fb are rejected from kernel as expected");
		igt_subtest("invalid-async-flip")
			test_invalid(&data);

		igt_fixture {
			for (i = 0; i < ARRAY_SIZE(data.bufs); i++)
				igt_remove_fb(data.drm_fd, &data.bufs[i]);
		}
	}

	igt_fixture
		igt_display_fini(&data.display);
}
