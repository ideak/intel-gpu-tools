/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#include "igt.h"
#include "igt_rand.h"
#include "gpgpu_fill.h"
#include "drmtest.h"
#include "sw_sync.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include "i915/gem.h"

IGT_TEST_DESCRIPTION("Stress test how gpu and cpu behaves if maximum amount of planes, "
		     "cpu and gpu utilization is achieved in order to reveal possible "
		     "bandwidth/watermark and similar problems.");

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

#define HAS_XELPD(drm_fd) (intel_display_ver(intel_get_drm_devid(drm_fd)) >= 13)

#define N_BLITS_PER_FRAME 10

#define N_FORMATS 1
static const uint32_t formats[N_FORMATS] = {
	DRM_FORMAT_XRGB8888,
};

#define N_TILING_METHODS 2
static const uint64_t tilings[N_TILING_METHODS] = {
	DRM_FORMAT_MOD_LINEAR,
	I915_FORMAT_MOD_Y_TILED,
};

static const char *format_str(int format_index)
{
	switch (formats[format_index]) {
	case DRM_FORMAT_RGB565:
		return "rgb565";
	case DRM_FORMAT_XRGB8888:
		return "xrgb8888";
	case DRM_FORMAT_XRGB2101010:
		return "xrgb2101010";
	default:
		igt_assert(false);
	}
}

static const char *tiling_str(int tiling_index)
{
	switch (tilings[tiling_index]) {
	case DRM_FORMAT_MOD_LINEAR:
		return "untiled";
	case I915_FORMAT_MOD_X_TILED:
		return "xtiled";
	case I915_FORMAT_MOD_Y_TILED:
		return "ytiled";
	default:
		igt_assert(false);
	}
}


#define MAX_CORES 8
#define MAX_PLANES 16

struct data;

struct thread_context {
	struct data *data;
	int id;
	void *buf1;
	void *buf2;
};

struct rect {
	int x;
	int y;
	int w;
	int h;
};

struct gpu_context {
	struct data *data;
	int pipe;
	int color;
	int num_rectangles;
	struct intel_buf *buf;
	struct igt_fb *fb_ptr;
	struct rect blt_rect;
	struct intel_batchbuffer *batch;
};

enum {
	RUNNING,
	STOPPED,
	PAUSED,
	LAST_STATE = PAUSED
} thread_state;

struct data {
	int drm_fd;
	igt_display_t display;
	int num_planes[IGT_MAX_PIPES];
	uint32_t format;
	uint64_t modifier;
	uint32_t devid;
	struct buf_ops *bops;
	drm_intel_bufmgr *bufmgr;
	drmModeModeInfo *last_mode[IGT_MAX_PIPES];
	struct igt_fb fb[IGT_MAX_PIPES * MAX_PLANES];
	struct igt_fb cursor_fb[IGT_MAX_PIPES];
	pthread_t cpu_thread[MAX_CORES];
	pthread_t gpu_thread[IGT_MAX_PIPES];
	bool cpu_thread_stop[MAX_CORES];
	int gpu_thread_state[IGT_MAX_PIPES];
	sem_t gpu_thread_pause_ack[IGT_MAX_PIPES];
	struct thread_context cpu_context[MAX_CORES];
	struct gpu_context gpu_context[IGT_MAX_PIPES];
	pthread_mutex_t gpu_fill_lock;
	drmModeModeInfo *highest_mode[IGT_MAX_PIPES];
	drmModeConnectorPtr *connectors;
	drmModeRes *mode_resources;
	int number_of_cores;
	igt_pipe_crc_t *pipe_crc[IGT_MAX_PIPES];
};

static void stop_gpu_threads(struct data *data);
static void start_gpu_threads(struct data *data);

struct base_crc {
	bool set;
	igt_crc_t crc;
};

#define BUF_SIZE (128 * 1024 * 1024)

static void *cpu_load(void *d)
{
	char *buf1, *buf2;
	struct thread_context *context = (struct thread_context *)d;
	struct data *data = context->data;

	buf1 = context->buf1;
	buf2 = context->buf2;

	data->cpu_thread_stop[context->id] = false;

	igt_info("CPU thread cpu id %d start\n", context->id);

	/* Just to make CPU busy... */
	while (!data->cpu_thread_stop[context->id]) {
		memcpy(buf1, buf2, BUF_SIZE);
		memcpy(buf2, buf1, BUF_SIZE);
	}

	igt_info("CPU thread cpu id %d stop\n", context->id);

	return NULL;
}

static struct intel_buf *
create_buf(struct data *data, int width, int height, uint32_t region)
{
	struct intel_buf *buf;
	uint32_t handle;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	/*
	 * Legacy code uses 32 bpp after buffer creation.
	 * Let's do the same due to keep shader intact.
	 */
	handle = gem_create_in_memory_regions(data->drm_fd, width * height, region);
	intel_buf_init_using_handle(data->bops, handle, buf,
				    width/4, height, 32, 0,
				    I915_TILING_NONE, 0);

	return buf;
}

static void fill_gpu(struct gpu_context *context,
		     unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height,
		     uint8_t color)
{
	struct data *data = context->data;
	igt_fillfunc_t fill_fn = NULL;

	pthread_mutex_lock(&data->gpu_fill_lock);

	fill_fn = igt_get_gpgpu_fillfunc(data->devid);

	igt_assert(fill_fn);
	igt_assert(context->buf);

	fill_fn(data->drm_fd, context->buf, x, y, width, height, color);

	pthread_mutex_unlock(&data->gpu_fill_lock);
}

static void *gpu_load(void *ptr)
{
	struct gpu_context *context = (struct gpu_context *)ptr;
	struct data *data = context->data;
	int pipe = context->pipe;
	int rect_divisor;
	int rect_width;
	int rect_height;
	int frame_width;
	int frame_height;
	drmModeModeInfo *mode;
	int frame = 0;
	int x, y;
	int rect = 0, total_rects = 0;
	int pixels = 0;

	mode = data->highest_mode[pipe];
	if (!mode)
		return NULL;

	frame_width = mode->hdisplay;
	frame_height = mode->vdisplay;

	igt_info("GPU thread pipe %d start\n", pipe);

	context->buf = create_buf(data, data->highest_mode[pipe]->hdisplay,
				  data->highest_mode[pipe]->vdisplay,
				  INTEL_MEMORY_REGION_ID(I915_SYSTEM_MEMORY, 0));

	while (data->gpu_thread_state[pipe] != STOPPED) {

		rect = 0;
		while (rect < context->num_rectangles) {
			/* divide at least by 2 and up to 8 */
			int x_rand, y_rand;

			rect_divisor = 1 << (hars_petruska_f54_1_random_unsafe_max(3) + 1);

			rect_width = frame_width / rect_divisor;
			rect_height = frame_height / rect_divisor;

			x_rand = hars_petruska_f54_1_random_unsafe_max(frame_width - rect_width);
			y_rand = hars_petruska_f54_1_random_unsafe_max(frame_height/2 - rect_height);

			context->blt_rect.x = x + x_rand;
			context->blt_rect.y = y + y_rand;

			/* Fill randomly sized and positioned rectangles */
			fill_gpu(context, context->blt_rect.x, context->blt_rect.y,
				 context->blt_rect.x + rect_width,
				 context->blt_rect.y + rect_height,
				 context->color);

			context->color += 4;

			++rect;

			pixels += rect_width * rect_height;
		}
		++frame;
		total_rects += rect;
	}

	intel_buf_close(data->bops, context->buf);

	igt_info("GPU thread pipe %d stop. Frames rendered: %d Rectangles: %d Pixels filled: %d\n",
		 pipe, frame, total_rects, pixels);

	return NULL;
}

static inline uint32_t pipe_select(enum pipe pipe)
{
	if (pipe > 1)
		return pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;
	else if (pipe > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static unsigned get_vblank(int fd, enum pipe pipe, unsigned flags)
{
	union drm_wait_vblank vbl;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | pipe_select(pipe) | flags;
	if (drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl))
		return 0;

	return vbl.reply.sequence;
}

static int commit_mode(struct data *data, igt_output_t *output,
		       enum pipe pipe, drmModeModeInfo *mode)
{
	int ret;

	igt_output_override_mode(output, mode);
	igt_output_set_pipe(output, pipe);

	ret = igt_display_try_commit_atomic(&data->display,
					    DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret) {
		igt_warn("Could not commit mode: \n");
		kmstest_dump_mode(mode);
		return ret;
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	return 0;
}

static void cursor_plane_set_fb(igt_plane_t *plane, struct igt_fb *fb,
				int width, int height)
{
	igt_plane_set_fb(plane, fb);
	igt_fb_set_size(fb, plane, width, height);
}

static void universal_plane_set_fb(igt_plane_t *plane, struct igt_fb *fb,
				int width, int height)
{
	igt_plane_set_fb(plane, fb);
	igt_plane_set_position(plane, 0, 0);
	igt_fb_set_size(fb, plane, width, height);
}

static bool plane_needs_rotation(int drm_fd, uint64_t modifier, igt_plane_t *plane)
{
	return !HAS_XELPD(drm_fd) && !IS_DG2(intel_get_drm_devid(drm_fd)) &&
		modifier == I915_FORMAT_MOD_Y_TILED &&
		plane->type != DRM_PLANE_TYPE_CURSOR;
}

static int try_plane_scaling(struct data *data, igt_plane_t *plane,
			     int width, int height)
{
	int ret;

	if (plane_needs_rotation(data->drm_fd, data->modifier, plane)) {
		igt_plane_set_rotation(plane, IGT_ROTATION_90);
		igt_plane_set_size(plane, height, width);
	} else {
		igt_plane_set_size(plane, width, height);
	}

	ret = igt_display_try_commit_atomic(&data->display,
					    DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET,
					    NULL);

	return ret;
}

static void cleanup_plane_fbs(struct data *data, enum pipe pipe, int start, int end)
{
	int i = start;

	while (i < end) {
		igt_remove_fb(data->display.drm_fd,
			      &data->fb[pipe * MAX_PLANES + i]);
		data->fb[pipe * MAX_PLANES + i].fb_id = 0;
		i++;
	}
}

static int pipe_stress(struct data *data, igt_output_t *output,
		       enum pipe pipe, drmModeModeInfo *mode)
{
	igt_plane_t *plane;
	int i = 0;
	int ret;
	bool new_mode = false;
	uint64_t cursor_width, cursor_height;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width));
	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height));

	if (!mode)
		mode = igt_output_get_mode(output);

	if (data->last_mode[pipe] != mode) {
		ret = commit_mode(data, output, pipe, mode);

		if (!ret)
			return ret;

		data->last_mode[pipe] = mode;
		new_mode = true;
	}

	/*
	 * Looks like we can't have planes on that pipe at all
	 * or mode hasn't changed
	 */
	if (!data->num_planes[pipe] || !new_mode)
		return 0;

	for_each_plane_on_pipe(&data->display, pipe, plane) {
		int plane_width, plane_height;
		if (plane->type == DRM_PLANE_TYPE_CURSOR) {
			cursor_plane_set_fb(plane, &data->cursor_fb[pipe],
					    cursor_width, cursor_height);
			plane_width = cursor_width;
			plane_height = cursor_height;
		} else {
			universal_plane_set_fb(plane, &data->fb[pipe * MAX_PLANES + i],
					       mode->hdisplay, mode->vdisplay);

			plane_width = (mode->hdisplay * 3) / 4;
			plane_height = (mode->vdisplay * 3) / 4;

			ret = try_plane_scaling(data, plane, plane_width, plane_height);

			while (ret) {
				if (plane_width <= cursor_width || plane_height <= cursor_height)
					break;

				plane_width /= 2;
				plane_height /= 2;

				ret = try_plane_scaling(data, plane, plane_width, plane_height);

				igt_info("Reduced plane %d size to %dx%d\n",
					 plane->index, plane_width, plane_height);
			}
			if (ret) {
				igt_info("Plane %d pipe %d try commit failed, exiting\n", i, pipe);
				data->num_planes[pipe] = i;
				igt_info("Max num planes for pipe %d set to %d\n", pipe, i);
				/*
				 * We have now determined max amount of full sized planes, we will just
				 * keep it in mind and be smarter next time. Also lets remove unneeded fbs.
				 * Don't destroy cursor_fb as will take care about it at the end.
				 */
				igt_plane_set_fb(plane, NULL);
				cleanup_plane_fbs(data, pipe, i, MAX_PLANES);
			}

			if (++i >= data->num_planes[pipe])
				break;
		}
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	return 0;
}

static __u64 get_mode_data_rate(drmModeModeInfo *mode)
{
	__u64 data_rate = (__u64)mode->hdisplay * (__u64)mode->vdisplay * (__u64)mode->vrefresh;

	return data_rate;
}


static drmModeModeInfo *find_highest_mode(drmModeConnector *connector)
{
	drmModeModeInfo *highest_mode = NULL;
	int j;

	for (j = 0; j < connector->count_modes; j++) {
		if (!highest_mode) {
			highest_mode = &connector->modes[j];
		} else if (connector->modes[j].vdisplay && connector->modes[j].hdisplay) {
			__u64 highest_data_rate = get_mode_data_rate(highest_mode);
			__u64 data_rate = get_mode_data_rate(&connector->modes[j]);

			if (highest_data_rate < data_rate)
				highest_mode = &connector->modes[j];
		}
	}

	return highest_mode;
}

typedef drmModeConnector *drmModeConnectorPtr;

static void fill_connector_to_pipe_array(struct data *data,
					 drmModeRes *mode_resources,
					 drmModeConnectorPtr *connectors)
{
	int pipe = 0;
	int i;

	memset(connectors, 0, sizeof(drmModeConnectorPtr) *
	       mode_resources->count_connectors);

	igt_info("Got %d connectors\n", mode_resources->count_connectors);

	for (i = 0; i < mode_resources->count_connectors; i++) {
		drmModeConnector *connector;

		connector = drmModeGetConnector(data->drm_fd,
						mode_resources->connectors[i]);

		if (!connector) {
			igt_warn("could not get connector %i: %s\n",
				 mode_resources->connectors[i], strerror(errno));
			continue;
		}

		if (connector->connection == DRM_MODE_CONNECTED) {
			igt_info("Connector %d connected to pipe %d\n", i, pipe);
			connectors[pipe] = (drmModeConnectorPtr)connector;
			++pipe;
			if (pipe == IGT_MAX_PIPES)
				break;
		} else {
			igt_info("Connector %d connection status %d\n",
				 i, connector->connection);
			drmModeFreeConnector(connector);
		}
	}
}

static void release_connectors(drmModeConnectorPtr *connectors)
{
	int i;

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		if (connectors[i])
			drmModeFreeConnector(connectors[i]);
	}
	free(connectors);
}

static void stress_pipes(struct data *data, struct timespec *start,
			 struct timespec *end)
{
	int pipe = 0;
	int ret = 0;
	igt_output_t *output;
	igt_crc_t crc, crc2;

	for_each_connected_output(&data->display, output) {

		if (!data->highest_mode[pipe])
			continue;

		igt_assert_f(data->display.pipes[pipe].n_planes < MAX_PLANES,
			    "Currently we don't support more than %d planes!",
			     MAX_PLANES);

		ret = pipe_stress(data, output, pipe,
				 data->highest_mode[pipe]);
		if (ret)
			break;

		igt_pipe_crc_start(data->pipe_crc[pipe]);
		igt_pipe_crc_get_current(data->display.drm_fd, data->pipe_crc[pipe], &crc);
		get_vblank(data->display.drm_fd, pipe,
			   DRM_VBLANK_NEXTONMISS);
		igt_pipe_crc_get_current(data->display.drm_fd, data->pipe_crc[pipe], &crc2);
		igt_pipe_crc_stop(data->pipe_crc[pipe]);
		igt_assert_crc_equal(&crc, &crc2);

		++pipe;
	}
}

#define MIN_DURATION_SEC 5.0
#define MIN_ITERATIONS 20

static void stress(struct data *data)
{
	struct timespec start, end;
	int iterations = 0;
	bool need_continue;

	igt_gettime(&start);

	do {
		igt_gettime(&end);
		stress_pipes(data, &start, &end);
		iterations++;
		need_continue =
			igt_time_elapsed(&start, &end) < MIN_DURATION_SEC;
	} while ((need_continue || iterations < MIN_ITERATIONS));
}

static void start_gpu_threads(struct data *data)
{
	int i;

	data->bops = buf_ops_create(data->drm_fd);

	pthread_mutex_init(&data->gpu_fill_lock, NULL);

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		if (!data->highest_mode[i])
			continue;
		data->gpu_context[i].data = data;
		data->gpu_context[i].pipe = i;
		data->gpu_context[i].fb_ptr = NULL;
		data->gpu_context[i].blt_rect.x = 0;
		data->gpu_context[i].blt_rect.y = 0;
		data->gpu_context[i].blt_rect.w = 0;
		data->gpu_context[i].blt_rect.h = 0;
		data->gpu_context[i].num_rectangles = N_BLITS_PER_FRAME;
		data->gpu_thread_state[i] = RUNNING;
		data->gpu_context[i].buf = NULL;
		igt_info("Starting GPU thread %d\n", i);

		pthread_create(&data->gpu_thread[i], NULL, gpu_load,
			       (void *)&data->gpu_context[i]);

		igt_info("GPU thread %d started\n", i);
	}
}

static void stop_gpu_threads(struct data *data)
{
	int i;

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		if (!data->highest_mode[i])
			continue;
		igt_info("Stopping GPU thread %d\n", i);
		data->gpu_thread_state[i] = STOPPED;
		pthread_join(data->gpu_thread[i], NULL);
		igt_info("Stopped GPU thread %d\n", i);
		data->gpu_context[i].fb_ptr = NULL;\
		data->gpu_context[i].buf = NULL;
	}

	buf_ops_destroy(data->bops);
}

static void start_cpu_threads(struct data *data)
{
	int i;

	for (i = 0; i < data->number_of_cores; i++) {
		data->cpu_context[i].buf1 = malloc(BUF_SIZE);
		data->cpu_context[i].buf2 = malloc(BUF_SIZE);
		data->cpu_context[i].id = i;
		data->cpu_context[i].data = data;
		pthread_create(&data->cpu_thread[i], NULL, cpu_load,
			       (void *)&data->cpu_context[i]);
	}
}

static void stop_cpu_threads(struct data *data)
{
	int i;

	for (i = 0; i < data->number_of_cores; i++) {
		data->cpu_thread_stop[i] = true;
		pthread_join(data->cpu_thread[i], NULL);
		free(data->cpu_context[i].buf1);
		free(data->cpu_context[i].buf2);
	}
}

static void create_framebuffers(struct data *data)
{
	int i, j;
	uint64_t cursor_width, cursor_height;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width));
	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height));

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		if (!data->highest_mode[i])
			continue;

		if (!data->cursor_fb[i].fb_id) {
			igt_create_color_fb(data->drm_fd,
					    cursor_width, cursor_height,
					    data->format,
					    data->modifier,
					    1.0, 0.0, 0.0,
					    &data->cursor_fb[i]);
		}

		for (j = 0; j < data->num_planes[i]; j++) {
			if (!data->fb[i * MAX_PLANES + j].fb_id) {
				igt_create_color_pattern_fb(data->drm_fd,
							    data->highest_mode[i]->hdisplay,
							    data->highest_mode[i]->vdisplay,
							    data->format,
							    data->modifier,
							    0.0, 1.0, 0.0, &data->fb[i * MAX_PLANES + j]);
			}
		}
	}
}

static void destroy_framebuffers(struct data *data)
{
	int i, j;

	for (i = 0; i < IGT_MAX_PIPES; i++) {

		if (!data->highest_mode[i])
			continue;

		for (j = 0; j < MAX_PLANES; j++) {
			if (data->fb[i * MAX_PLANES + j].fb_id) {
				igt_plane_set_fb(&data->display.pipes[i].planes[j], NULL);
				igt_remove_fb(data->display.drm_fd, &data->fb[i * MAX_PLANES + j]);
				data->fb[i * MAX_PLANES + j].fb_id = 0;
			}
		}
		if (data->cursor_fb[i].fb_id) {
			igt_remove_fb(data->display.drm_fd, &data->cursor_fb[i]);
			data->cursor_fb[i].fb_id = 0;
		}
	}
}

static void prepare_test(struct data *data)
{
	int i, j;
	int num_connectors;
	int num_cpus = (int) sysconf(_SC_NPROCESSORS_ONLN);

	data->number_of_cores = min(num_cpus, MAX_CORES);

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		for (j = 0; j < MAX_PLANES; j++)
			data->fb[i * MAX_PLANES + j].fb_id = 0;
		data->cursor_fb[i].fb_id = 0;
		data->num_planes[i] = -1;
		data->last_mode[i] = NULL;
		sem_init(&data->gpu_thread_pause_ack[i], 0, 0);
	}

	start_cpu_threads(data);
	data->mode_resources = drmModeGetResources(data->drm_fd);
	if (!data->mode_resources) {
		igt_warn("drmModeGetResources failed: %s\n", strerror(errno));
		return;
	}

	num_connectors = data->mode_resources->count_connectors;
	num_connectors = max(num_connectors, IGT_MAX_PIPES);
	memset(data->highest_mode, 0, sizeof(drmModeModeInfo *) * IGT_MAX_PIPES);
	data->connectors =
		(drmModeConnectorPtr *)calloc(sizeof(drmModeConnectorPtr) * num_connectors, 1);
	fill_connector_to_pipe_array(data, data->mode_resources, data->connectors);

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		drmModeConnector *connector = (drmModeConnector *)data->connectors[i];

		if (!connector)
			continue;

		if (!data->highest_mode[i]) {
			if (connector->count_modes)
				data->highest_mode[i] = find_highest_mode(connector);
		}
		igt_assert(data->highest_mode[i]);

		if (data->highest_mode[i]) {
			igt_info("Using mode: \n");
			kmstest_dump_mode(data->highest_mode[i]);
			data->pipe_crc[i] = igt_pipe_crc_new(data->drm_fd, i,
							     IGT_PIPE_CRC_SOURCE_AUTO);
		} else
			data->pipe_crc[i] = NULL;

		if (data->num_planes[i] == -1)
			data->num_planes[i] = data->display.pipes[i].n_planes;

		igt_info("Max number of planes is %d for pipe %d\n",
			 data->num_planes[i], i);
	}

	create_framebuffers(data);

	if (intel_gen(intel_get_drm_devid(data->drm_fd)) > 9)
		start_gpu_threads(data);
}

static void finish_test(struct data *data)
{
	int i;

	if (intel_gen(intel_get_drm_devid(data->drm_fd)) > 9)
		stop_gpu_threads(data);

	/*
	 * As we change tiling/format we need a new FB
	 */
	destroy_framebuffers(data);

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		data->num_planes[i] = -1;
		data->last_mode[i] = NULL;
		if (data->pipe_crc[i])
			igt_pipe_crc_free(data->pipe_crc[i]);
	}

	stop_cpu_threads(data);
	release_connectors(data->connectors);
	drmModeFreeResources(data->mode_resources);
}

struct data data = {
	.format = DRM_FORMAT_XRGB8888,
	.modifier = DRM_FORMAT_MOD_LINEAR,
	.devid = 0,
	.bops = NULL
};


igt_main {
	uint8_t format_idx = 0, tiling_idx = 0;

	igt_fixture {
		data.drm_fd = data.display.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.display.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_require_gem(data.drm_fd);
	}

	for (format_idx = 0; format_idx < N_FORMATS; format_idx++) {
		for (tiling_idx = 0; tiling_idx < N_TILING_METHODS; tiling_idx++) {
			data.format = formats[format_idx];
			data.modifier = tilings[tiling_idx];

			igt_describe("Start pipe stress test, utilizing cpu and gpu "
				     "simultaneously with maximum amount of planes "
				     "and resolution.");
			igt_subtest_f("stress-%s-%s",
				      format_str(format_idx),
				      tiling_str(tiling_idx)) {

				igt_skip_on(!igt_display_has_format_mod(&data.display,
									formats[format_idx],
									tilings[tiling_idx]));

				prepare_test(&data);
				stress(&data);
				finish_test(&data);
			}
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
