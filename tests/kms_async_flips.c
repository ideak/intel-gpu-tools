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
#include "igt_psr.h"
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
	igt_output_t *output;
	unsigned long flip_timestamp_us;
	double flip_interval;
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t ref_crc;
	int flip_count;
	int frame_count;
	bool flip_pending;
	bool extended;
	enum pipe pipe;
	bool alternate_sync_async;
} data_t;

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
		    uint32_t width, uint32_t height, int index)
{
	int rec_width;
	cairo_t *cr;

	rec_width = width / (ARRAY_SIZE(data->bufs) * 2);

	if (is_i915_device(data->drm_fd)) {
		igt_create_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
			      I915_FORMAT_MOD_X_TILED, fb);
		igt_draw_fill_fb(data->drm_fd, fb, 0x88);
	} else {
		igt_create_color_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 0.5, fb);
	}

	cr = igt_get_cairo_ctx(data->drm_fd, fb);
	igt_paint_color_rand(cr, rec_width * 2 + rec_width * index, height / 4, rec_width, height / 2);
}

static void require_monotonic_timestamp(int fd)
{
	igt_require_f(igt_has_drm_cap(fd, DRM_CAP_TIMESTAMP_MONOTONIC),
		      "Monotonic timestamps not supported\n");
}

static void test_init(data_t *data)
{
	int i;
	uint32_t width, height;
	igt_plane_t *plane;
	static uint32_t prev_output_id;
	drmModeModeInfo *mode;

	igt_display_reset(&data->display);
	igt_display_commit(&data->display);

	mode = igt_output_get_mode(data->output);
	width = mode->hdisplay;
	height = mode->vdisplay;

	data->crtc_id = data->display.pipes[data->pipe].crtc_id;
	data->refresh_rate = mode->vrefresh;

	igt_output_set_pipe(data->output, data->pipe);
	plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);

	if (prev_output_id != data->output->id) {
		prev_output_id = data->output->id;

		if (data->bufs[0].fb_id) {
			for (i = 0; i < ARRAY_SIZE(data->bufs); i++)
				igt_remove_fb(data->drm_fd, &data->bufs[i]);
		}

		for (i = 0; i < ARRAY_SIZE(data->bufs); i++)
			make_fb(data, &data->bufs[i], width, height, i);
	}

	igt_plane_set_fb(plane, &data->bufs[0]);
	igt_plane_set_size(plane, width, height);

	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void test_async_flip(data_t *data)
{
	int ret, frame;
	long long int fps;
	struct timeval start, end, diff;

	test_init(data);

	gettimeofday(&start, NULL);
	frame = 1;
	do {
		int flags = DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT;

		if (data->alternate_sync_async) {
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
			 * In platforms greater than DISPLAY13 the first async flip will be discarded
			 * in order to change the watermark levels as per the optimization. Hence the
			 * subsequent async flips will actually do the asynchronous flips.
			 */
			if (is_i915_device(data->drm_fd)) {
				uint32_t devid = intel_get_drm_devid(data->drm_fd);

				if (IS_GEN9(devid) || IS_GEN10(devid) || AT_LEAST_GEN(devid, 12)) {
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

		if (data->alternate_sync_async) {
			igt_assert_f(data->flip_interval < 1000.0 / (data->refresh_rate * MIN_FLIPS_PER_FRAME),
				     "Flip interval not significantly smaller than vblank interval\n"
				     "Flip interval: %lfms, Refresh Rate = %dHz, Threshold = %d\n",
				     data->flip_interval, data->refresh_rate, MIN_FLIPS_PER_FRAME);
		}

		frame++;
	} while (diff.tv_sec < RUN_TIME);

	if (!data->alternate_sync_async) {
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

	test_init(data);

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

	/*
	 * Intel's PSR2 selective fetch adds other planes to state when
	 * necessary, causing the async flip to fail because async flip is not
	 * supported in cursor plane.
	 */
	igt_skip_on_f(i915_psr2_selective_fetch_check(data->drm_fd),
		      "PSR2 sel fetch causes cursor to be added to primary plane " \
		      "pages flips and async flip is not supported in cursor\n");

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &width));
	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &height));

	test_init(data);

	igt_create_color_fb(data->drm_fd, width, height, DRM_FORMAT_ARGB8888,
			    DRM_FORMAT_MOD_LINEAR, 1., 1., 1., &cursor_fb);

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
	drmModeModeInfo *mode;

	mode = igt_output_get_mode(data->output);
	width = mode->hdisplay;
	height = mode->vdisplay;

	test_init(data);

	igt_create_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
		      I915_FORMAT_MOD_Y_TILED, &fb);

	/* Flip with a different fb modifier which is expected to be rejected */
	ret = drmModePageFlip(data->drm_fd, data->crtc_id,
			      fb.fb_id, flags, data);

	igt_assert(ret == -EINVAL);

	/* TODO: Add verification for changes in stride, pixel format */

	igt_remove_fb(data->drm_fd, &fb);
}

static void queue_vblank(data_t *data)
{
	int pipe = kmstest_get_pipe_from_crtc_id(data->drm_fd, data->crtc_id);
	drmVBlank wait_vbl = {
		.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT |
			kmstest_get_vbl_flag(pipe),
		.request.sequence = 1,
		.request.signal = (long)data,
	};

	igt_assert(drmIoctl(data->drm_fd, DRM_IOCTL_WAIT_VBLANK, &wait_vbl) == 0);
}

static void vblank_handler_crc(int fd_, unsigned int sequence, unsigned int tv_sec,
			       unsigned int tv_usec, void *_data)
{
	data_t *data = _data;
	igt_crc_t crc;

	data->frame_count++;

	igt_pipe_crc_get_single(data->pipe_crc, &crc);
	igt_assert_crc_equal(&data->ref_crc, &crc);

	/* check again next vblank */
	queue_vblank(data);
}

static void flip_handler_crc(int fd_, unsigned int sequence, unsigned int tv_sec,
			     unsigned int tv_usec, void *_data)
{
	data_t *data = _data;

	data->flip_pending = false;
	data->flip_count++;
}

static void wait_events_crc(data_t *data)
{
	drmEventContext evctx = {
		.version = 2,
		.vblank_handler = vblank_handler_crc,
		.page_flip_handler = flip_handler_crc,
	};

	while (data->flip_pending) {
		struct pollfd pfd = {
			.fd = data->drm_fd,
			.events = POLLIN,
		};
		int ret;

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
}

static unsigned int clock_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_crc(data_t *data)
{
	unsigned int frame = 0;
	unsigned int start;
	cairo_t *cr;
	int ret;

	data->flip_count = 0;
	data->frame_count = 0;
	data->flip_pending = false;

	test_init(data);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->bufs[frame]);
	igt_paint_color(cr, 0, 0, data->bufs[frame].width, data->bufs[frame].height, 1.0, 0.0, 0.0);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->bufs[!frame]);
	igt_paint_color(cr, 0, 0, data->bufs[!frame].width, data->bufs[!frame].height, 1.0, 0.0, 0.0);

	ret = drmModeSetCrtc(data->drm_fd, data->crtc_id, data->bufs[frame].fb_id, 0, 0,
			     &data->output->config.connector->connector_id, 1,
			     &data->output->config.connector->modes[0]);
	igt_assert_eq(ret, 0);

	data->pipe_crc = igt_pipe_crc_new(data->drm_fd,
					  kmstest_get_pipe_from_crtc_id(data->drm_fd, data->crtc_id),
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_single(data->pipe_crc, &data->ref_crc);

	queue_vblank(data);

	start = clock_ms();

	while (clock_ms() - start < 2000) {
		/* fill the next fb with the expected color */
		cr = igt_get_cairo_ctx(data->drm_fd, &data->bufs[frame]);
		igt_paint_color(cr, 0, 0, 1, data->bufs[frame].height, 1.0, 0.0, 0.0);

		data->flip_pending = true;
		ret = drmModePageFlip(data->drm_fd, data->crtc_id, data->bufs[frame].fb_id,
				      DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT, data);
		igt_assert_eq(ret, 0);

		wait_events_crc(data);

		/* clobber the previous fb which should no longer be scanned out */
		frame = !frame;
		cr = igt_get_cairo_ctx(data->drm_fd, &data->bufs[frame]);
		igt_paint_color_rand(cr, 0, 0, 1, data->bufs[frame].height);
	}

	igt_pipe_crc_stop(data->pipe_crc);
	igt_pipe_crc_free(data->pipe_crc);

	/* make sure we got at a reasonable number of async flips done */
	igt_assert_lt(data->frame_count * 2, data->flip_count);
}

static void run_test(data_t *data, void (*test)(data_t *))
{
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe(&data->display, pipe) {
		for_each_valid_output_on_pipe(&data->display, pipe, output) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), output->name) {
				data->output = output;
				data->pipe = pipe;
				test(data);
			}

			if (!data->extended)
				break;
		}
	}
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'e':
		data->extended = true;
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const char help_str[] =
	"  --e \t\tRun the extended tests\n";

static data_t data;

igt_main_args("e", NULL, help_str, opt_handler, &data)
{
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
			require_monotonic_timestamp(data.drm_fd);

		igt_describe("Wait for page flip events in between successive asynchronous flips");
		igt_subtest_with_dynamic("async-flip-with-page-flip-events") {
			data.alternate_sync_async = false;
			run_test(&data, test_async_flip);
		}

		igt_describe("Alternate between sync and async flips");
		igt_subtest_with_dynamic("alternate-sync-async-flip") {
			data.alternate_sync_async = true;
			run_test(&data, test_async_flip);
		}

		igt_describe("Verify that the async flip timestamp does not coincide with either previous or next vblank");
		igt_subtest_with_dynamic("test-time-stamp")
			run_test(&data, test_timestamp);
	}

	igt_describe("Verify that the DRM_IOCTL_MODE_CURSOR passes after async flip");
	igt_subtest_with_dynamic("test-cursor") {
		/*
		 * Intel's PSR2 selective fetch adds other planes to state when
		 * necessary, causing the async flip to fail because async flip is not
		 * supported in cursor plane.
		 */
		igt_skip_on_f(i915_psr2_selective_fetch_check(data.drm_fd),
			      "PSR2 sel fetch causes cursor to be added to primary plane " \
			      "pages flips and async flip is not supported in cursor\n");

		run_test(&data, test_cursor);
	}

	igt_describe("Negative case to verify if changes in fb are rejected from kernel as expected");
	igt_subtest_with_dynamic("invalid-async-flip") {
		/* TODO: support more vendors */
		igt_require(is_i915_device(data.drm_fd));
		igt_require(igt_display_has_format_mod(&data.display, DRM_FORMAT_XRGB8888,
						       I915_FORMAT_MOD_Y_TILED));

		run_test(&data, test_invalid);
	}

	igt_describe("Use CRC to verify async flip scans out the correct framebuffer");
	igt_subtest_with_dynamic("crc") {
		/* Devices without CRC can't run this test */
		igt_require_pipe_crc(data.drm_fd);

		run_test(&data, test_crc);
	}

	igt_fixture {
		for (i = 0; i < ARRAY_SIZE(data.bufs); i++)
			igt_remove_fb(data.drm_fd, &data.bufs[i]);

		igt_display_reset(&data.display);
		igt_display_commit(&data.display);
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
