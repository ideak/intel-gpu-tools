#include "igt.h"
#include "igt_sysfs.h"
#include "igt_psr.h"
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>

#define INVALIDATES_PER_SEC 15
#define FLIPS_PER_SEC 30
#define SECS_TO_COMPLETE_TEST 10

#define OVERLAY_SIZE 500
#define OVERLAY_POSITION 250

#define FRAMEBUFFERS_LEN 60

#define DRAW_METHOD IGT_DRAW_BLT

typedef struct {
	int drm_fd;
	int debugfs_fd;
	struct buf_ops *bops;
	igt_display_t display;
	drmModeModeInfo *mode;
	igt_output_t *output;

	struct igt_fb primary_fb[FRAMEBUFFERS_LEN];
	struct igt_fb overlay_fb[FRAMEBUFFERS_LEN];

	uint8_t flip_fb_in_use;
	uint8_t invalidate_progress;

	int invalidate_timerfd;
	int flip_timerfd;
	int completed_timerfd;

	/*
	 * There is 2 subtest, one that flips primary and invalidates overlay
	 * and other that invalidates primary and flips overlay.
	 */
	bool flip_primary;

	enum psr_mode initial_state;
} data_t;

struct color {
	union {
		uint32_t val;
		struct {
			uint8_t b;
			uint8_t g;
			uint8_t r;
			uint8_t a;
		};
	};
};

static void setup_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	igt_display_require(&data->display, data->drm_fd);

	for_each_pipe_with_valid_output(display, pipe, output) {
		drmModeConnectorPtr c = output->config.connector;

		if (c->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_output_set_pipe(output, pipe);
		data->output = output;
		data->mode = igt_output_get_mode(output);

		return;
	}

	igt_require(data->output);
}

static void primary_draw(data_t *data, struct igt_fb *fb, uint8_t i)
{
	uint32_t x, y, w, h;
	struct color cl;

	x = 0;
	y = 500;
	w = data->mode->hdisplay / FRAMEBUFFERS_LEN;
	w *= i;
	h = OVERLAY_SIZE;

	cl.a = 0xff;

	if (w == 0) {
		cl.r = cl.g = cl.b = 128;
		y = 0;
		w = data->mode->hdisplay;
		h = data->mode->vdisplay;
	} else {
		cl.r = cl.b = 0x00;
		cl.g = 0xff;
	}

	igt_draw_rect_fb(data->drm_fd, data->bops, 0, fb, DRAW_METHOD, x, y,
			 w, h, cl.val);
}

static void overlay_draw(data_t *data, struct igt_fb *fb, uint8_t i)
{
	uint32_t x, y, w, h;
	struct color cl;

	x = 0;
	y = 0;
	w = OVERLAY_SIZE;
	h = OVERLAY_SIZE / FRAMEBUFFERS_LEN;
	h *= i;

	cl.a = 0xff;

	if (h == 0) {
		cl.r = cl.g = cl.b = 0xff;
		h = OVERLAY_SIZE;
	} else {
		cl.r = 0xff;
		cl.g = cl.b = 0x0;
	}

	igt_draw_rect_fb(data->drm_fd, data->bops, 0, fb, DRAW_METHOD, x, y,
			 w, h, cl.val);
}

static void prepare(data_t *data)
{
	struct itimerspec interval;
	igt_plane_t *plane;
	int r, i;

	if (data->flip_primary) {
		for (i = 0; i < ARRAY_SIZE(data->primary_fb); i++) {
			igt_create_color_fb(data->drm_fd, data->mode->hdisplay,
					    data->mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0,
					    0.0, &data->primary_fb[i]);
			primary_draw(data, &data->primary_fb[i], 0);
			primary_draw(data, &data->primary_fb[i], i);
		}

		igt_create_color_fb(data->drm_fd, OVERLAY_SIZE, OVERLAY_SIZE,
				    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
				    0.0, 0.0, 0.0, &data->overlay_fb[0]);
		overlay_draw(data, &data->overlay_fb[0], 0);
	} else {
		igt_create_color_fb(data->drm_fd, data->mode->hdisplay,
				    data->mode->vdisplay, DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 0.0,
				    &data->primary_fb[0]);
		primary_draw(data, &data->primary_fb[0], 0);

		for (i = 0; i < ARRAY_SIZE(data->overlay_fb); i++) {
			igt_create_color_fb(data->drm_fd, OVERLAY_SIZE,
					    OVERLAY_SIZE, DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR, 0.0f, 0.0f,
					    0.0f, &data->overlay_fb[i]);
			overlay_draw(data, &data->overlay_fb[i], 0);
			overlay_draw(data, &data->overlay_fb[i], i);
		}
	}

	plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(plane, &data->primary_fb[0]);

	plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_OVERLAY);
	igt_plane_set_fb(plane, &data->overlay_fb[0]);
	igt_plane_set_position(plane, -(OVERLAY_SIZE / 2), 350);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	data->flip_fb_in_use = data->invalidate_progress = 0;

	/* Arm timers */
	interval.it_value.tv_nsec = NSEC_PER_SEC / INVALIDATES_PER_SEC;
	interval.it_value.tv_sec = 0;
	interval.it_interval.tv_nsec = interval.it_value.tv_nsec;
	interval.it_interval.tv_sec = interval.it_value.tv_sec;
	r = timerfd_settime(data->invalidate_timerfd, 0, &interval, NULL);
	igt_require_f(r != -1, "Error setting invalidate_timerfd\n");

	interval.it_value.tv_nsec = NSEC_PER_SEC / FLIPS_PER_SEC;
	interval.it_value.tv_sec = 0;
	interval.it_interval.tv_nsec = interval.it_value.tv_nsec;
	interval.it_interval.tv_sec = interval.it_value.tv_sec;
	r = timerfd_settime(data->flip_timerfd, 0, &interval, NULL);
	igt_require_f(r != -1, "Error setting flip_timerfd\n");

	interval.it_value.tv_nsec = 0;
	interval.it_value.tv_sec = SECS_TO_COMPLETE_TEST;
	interval.it_interval.tv_nsec = interval.it_value.tv_nsec;
	interval.it_interval.tv_sec = interval.it_value.tv_sec;
	r = timerfd_settime(data->completed_timerfd, 0, &interval, NULL);
	igt_require_f(r != -1, "Error setting completed_timerfd\n");

	data->initial_state = psr_get_mode(data->debugfs_fd);
	igt_require(data->initial_state != PSR_DISABLED);
	igt_require(psr_wait_entry(data->debugfs_fd, data->initial_state));
}

static void cleanup(data_t *data)
{
	struct itimerspec interval;
	igt_plane_t *plane;
	uint8_t i;

	plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(plane, NULL);
	plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_OVERLAY);
	igt_plane_set_fb(plane, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < ARRAY_SIZE(data->primary_fb); i++)
		igt_remove_fb(data->drm_fd, &data->primary_fb[i]);

	for (i = 0; i < ARRAY_SIZE(data->overlay_fb); i++)
		igt_remove_fb(data->drm_fd, &data->overlay_fb[i]);

	/* Disarm timers */
	interval.it_value.tv_nsec = 0;
	interval.it_value.tv_sec = 0;
	interval.it_interval.tv_nsec = interval.it_value.tv_nsec;
	interval.it_interval.tv_sec = interval.it_value.tv_sec;
	timerfd_settime(data->invalidate_timerfd, 0, &interval, NULL);
	timerfd_settime(data->flip_timerfd, 0, &interval, NULL);
	timerfd_settime(data->completed_timerfd, 0, &interval, NULL);
}

static void invalidate(data_t *data)
{
	if (data->flip_primary)
		overlay_draw(data, &data->overlay_fb[0], data->invalidate_progress);
	else
		primary_draw(data, &data->primary_fb[0], data->invalidate_progress);

	data->invalidate_progress++;
	if (data->invalidate_progress == (FRAMEBUFFERS_LEN + 1))
		data->invalidate_progress = 0;
}

static void flip(data_t *data)
{
	uint8_t next = data->flip_fb_in_use + 1;
	struct igt_fb *fb;
	igt_plane_t *plane;

	if (next == FRAMEBUFFERS_LEN)
		next = 0;

	if (data->flip_primary) {
		plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
		fb = &data->primary_fb[next];
	} else {
		plane = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_OVERLAY);
		fb = &data->overlay_fb[next];
	}

	igt_plane_set_fb(plane, fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	data->flip_fb_in_use = next;
}

static void run(data_t *data)
{
	struct pollfd pfd[3];
	bool loop = true;

	pfd[0].fd = data->invalidate_timerfd;
	pfd[0].events = POLLIN;
	pfd[0].revents = 0;

	pfd[1].fd = data->flip_timerfd;
	pfd[1].events = POLLIN;
	pfd[1].revents = 0;

	pfd[2].fd = data->completed_timerfd;
	pfd[2].events = POLLIN;
	pfd[2].revents = 0;

	while (loop) {
		int i, r = poll(pfd, ARRAY_SIZE(pfd), -1);

		if (r < 0)
			break;
		if (r == 0)
			continue;

		for (i = 0; i < ARRAY_SIZE(pfd); i++) {
			uint64_t exp;

			if (pfd[i].revents == 0)
				continue;

			pfd[i].revents = 0;
			r = read(pfd[i].fd, &exp, sizeof(exp));
			if (r != sizeof(uint64_t) || exp == 0)
				continue;

			if (pfd[i].fd == data->invalidate_timerfd)
				invalidate(data);
			else if (pfd[i].fd == data->flip_timerfd)
				flip(data);
			else if (pfd[i].fd == data->completed_timerfd)
				loop = false;
		}
	}

	/* Check if after all this stress the PSR is still in the same state */
	igt_assert(psr_get_mode(data->debugfs_fd) == data->initial_state);
}

igt_main
{
	data_t data = {};

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		igt_require_f(psr_sink_support(data.drm_fd, data.debugfs_fd,
					       PSR_MODE_1),
			      "Sink does not support PSR\n");

		setup_output(&data);

		data.invalidate_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
		igt_require(data.invalidate_timerfd != -1);
		data.flip_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
		igt_require(data.flip_timerfd != -1);
		data.completed_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
		igt_require(data.completed_timerfd != -1);
	}

	/*
	 * TODO: add cursor plane to the test to mimic even more real user
	 * usage cases
	 */
	igt_describe("Mix page flips in primary plane and frontbuffer writes "
		     "to overlay plane and check for warnings, underruns or "
		     "PSR state changes");
	igt_subtest("flip-primary-invalidate-overlay") {
		data.flip_primary = true;
		prepare(&data);
		run(&data);
		cleanup(&data);
	}

	igt_describe("Mix frontbuffer writes to the primary plane and page "
		     "flips in the overlay plane and check for warnings, "
		     "underruns or PSR state changes");
	igt_subtest("invalidate-primary-flip-overlay") {
		data.flip_primary = false;
		prepare(&data);
		run(&data);
		cleanup(&data);
	}

	igt_fixture {
		buf_ops_destroy(data.bops);
		igt_display_fini(&data.display);
		close(data.debugfs_fd);
		close(data.drm_fd);
	}
}