/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "igt.h"
#include "sw_sync.h"
#include <fcntl.h>
#include <signal.h>

#define NSECS_PER_SEC		(1000000000ull)
#define TEST_DURATION_NS	(10 * NSECS_PER_SEC)

#define BYTES_PER_PIXEL         4
#define MK_COLOR(r, g, b)	((0 << 24) | (r << 16) | (g << 8) | b)

/*
 * The Display Core of amdgpu will add a set of modes derived from the
 * base FreeSync video mode into the corresponding connector’s mode list based
 * on commonly used refresh rates and VRR range of the connected display.
 * From the userspace's perspective, they can see a seamless mode change
 * experience when the change between different refresh rates under the same
 * resolution. Additionally, userspace applications such as Video playback can
 * read this modeset list and change the refresh rate based on the video frame
 * rate. Finally, the userspace can also derive an appropriate mode for
 * a particular refresh rate based on the FreeSync Mode and add it to the
 * connector’s mode list.
*/
IGT_TEST_DESCRIPTION("This tests transition between normal and FreeSync-Video"
		     "modes and measures the FPS to ensure vblank events are"
		     "happening at the expected rate.");
typedef struct range {
	unsigned int min;
	unsigned int max;
} range_t;

typedef struct data {
	int		drm_fd;
	igt_display_t	display;
	igt_plane_t	*primary;
	igt_fb_t	fbs[2];
	uint32_t	*fb_mem[2];
	int		front;
	bool		fb_initialized;
	range_t		range;

	drmModeConnector *connector;
	drmModeModeInfo *modes;
	int		count_modes;

	uint32_t	preferred_mode_index;
        uint32_t	base_mode_index;
	uint32_t	hdisplay;
	uint32_t	vdisplay;
} data_t;

struct fsv_sprite {
        uint32_t        w;
	uint32_t	h;
        uint32_t        *data;
};
static struct fsv_sprite cicle_sprite;

enum {
        FSV_PREFERRED_MODE,
        FSV_BASE_MODE,
        FSV_FREESYNC_VIDEO_MODE,
        FSV_NON_FREESYNC_VIDEO_MODE,
};

enum {
	ANIM_TYPE_SMPTE,
	ANIM_TYPE_CIRCLE_WAVE,

	ANIM_TYPE_COUNT,
};

enum {
	SCENE_BASE_MODE_TO_VARIOUS_FSV_MODE ,
	SCENE_LOWER_FSV_MODE_TO_HIGHER_FSV_MODE ,
	SCENE_NON_FSV_MODE_TO_FSV_MODE ,
	SCENE_BASE_MODE_TO_CUSTUM_MODE ,
	SCENE_NON_FSV_MODE_TO_NON_FSV_MODE,

	SCENE_COUNT,
};

/*----------------------------------------------------------------------------*/

/* Converts a timespec structure to nanoseconds. */
static uint64_t timespec_to_ns(struct timespec *ts)
{
	return ts->tv_sec * NSECS_PER_SEC + ts->tv_nsec;
}

/*
 * Gets an event from DRM and returns its timestamp in nanoseconds.
 * Asserts if the event from DRM is not matched with requested one.
 *
 * This blocks until the event is received.
 */
static uint64_t get_kernel_event_ns(data_t *data, uint32_t event)
{
	struct drm_event_vblank ev;

	igt_set_timeout(1, "Waiting for an event\n");
	igt_assert_eq(read(data->drm_fd, &ev, sizeof(ev)), sizeof(ev));
	igt_assert_eq(ev.base.type, event);
	igt_reset_timeout();

	return ev.tv_sec * NSECS_PER_SEC + ev.tv_usec * 1000ull;
}

/*
 * Returns the current CLOCK_MONOTONIC time in nanoseconds.
 * The regular IGT helpers can't be used since they default to
 * CLOCK_MONOTONIC_RAW - which isn't what the kernel uses for its timestamps.
 */
static uint64_t get_time_ns(void)
{
	struct timespec ts;
	memset(&ts, 0, sizeof(ts));
	errno = 0;

	if (!clock_gettime(CLOCK_MONOTONIC, &ts))
		return timespec_to_ns(&ts);

	igt_warn("Could not read monotonic time: %s\n", strerror(errno));
	igt_fail(-errno);

	return 0;
}

static void fbmem_draw_rect(
		uint32_t *fbmem,
		uint32_t stride,
		uint32_t x,
		uint32_t y,
		uint32_t w,
		uint32_t h,
		uint32_t color)
{
        uint32_t offset = y * stride + x;

        for (uint32_t j = 0; j < h; j++) {
                for (uint32_t i = 0; i < w; i++) {
                        fbmem[offset + i] = color;
                }
                offset += stride;
        }
}

static void fbmem_draw_smpte_pattern(uint32_t *fbmem, int width, int height)
{
	uint32_t x, y;
        uint32_t colors_top[] = {
                MK_COLOR(192, 192, 192), /* grey */
                MK_COLOR(192, 192, 0),   /* yellow */
                MK_COLOR(0, 192, 192),   /* cyan */
                MK_COLOR(0, 192, 0),     /* green */
                MK_COLOR(192, 0, 192),   /* magenta */
                MK_COLOR(192, 0, 0),     /* red */
                MK_COLOR(0, 0, 192),     /* blue */
        };
        uint32_t colors_middle[] = {
                MK_COLOR(0, 0, 192),     /* blue */
                MK_COLOR(19, 19, 19),    /* black */
                MK_COLOR(192, 0, 192),   /* magenta */
                MK_COLOR(19, 19, 19),    /* black */
                MK_COLOR(0, 192, 192),   /* cyan */
                MK_COLOR(19, 19, 19),    /* black */
                MK_COLOR(192, 192, 192), /* grey */
        };
        uint32_t colors_bottom[] = {
                MK_COLOR(0, 33, 76),     /* in-phase */
                MK_COLOR(255, 255, 255), /* super white */
                MK_COLOR(50, 0, 106),    /* quadrature */
                MK_COLOR(19, 19, 19),    /* black */
                MK_COLOR(9, 9, 9),       /* 3.5% */
                MK_COLOR(19, 19, 19),    /* 7.5% */
                MK_COLOR(29, 29, 29),    /* 11.5% */
                MK_COLOR(19, 19, 19),    /* black */
        };

        for (y = 0; y < height * 6 / 9; ++y) {
                for (x = 0; x < width; ++x)
                        fbmem[x] =
                                colors_top[x * 7 / width];
                fbmem += width;
        }

        for (; y < height * 7 / 9; ++y) {
                for (x = 0; x < width; ++x)
                        fbmem[x] =
                                colors_middle[x * 7 / width];
                fbmem += width;
        }

        for (; y < height; ++y) {
                for (x = 0; x < width * 5 / 7; ++x)
                        fbmem[x] =
                                colors_bottom[x * 4 / (width * 5 / 7)];
                for (; x < width * 6 / 7; ++x)
                        fbmem[x] =
                                colors_bottom[(x - width * 5 / 7) * 3
                                              / (width / 7) + 4];
                for (; x < width; ++x)
                        fbmem[x] = colors_bottom[7];
                fbmem += width;
        }
}

static void sprite_init(
		struct fsv_sprite *sprite,
		uint32_t w,
		uint32_t h)
{
        igt_assert(sprite);

        sprite->data = (uint32_t *)malloc(w * h * BYTES_PER_PIXEL);
        igt_assert(sprite->data);

        sprite->w = w;
        sprite->h = h;
}

static void sprite_paste(
		uint32_t *fbmem,
		uint32_t fb_stride,
		struct fsv_sprite *sprite,
		uint32_t x,
		uint32_t y)
{
        uint32_t fb_offset = y * fb_stride + x;
        uint32_t sprite_offset = 0;

        for (int j = 0; j < sprite->h; j++) {
                memcpy(fbmem + fb_offset, sprite->data + sprite_offset, sprite->w * 4);
                sprite_offset += sprite->w;
                fb_offset += fb_stride;
        }
}

static void sprite_draw_rect(
		struct fsv_sprite *sprite,
		uint32_t x,
		uint32_t y,
		uint32_t w,
		uint32_t h,
		uint32_t color)
{
        uint32_t offset = y * sprite->w + x;
        uint32_t *addr = (uint32_t *)sprite->data;

        for (uint32_t j = 0; j < h; j++) {
                addr = (uint32_t *)(sprite->data + offset);
                for (uint32_t i = 0; i < w; i++) {
                        addr[i] = color;
                }
                offset += sprite->w;
        }
}

/* drawing horizontal line in the sprite */
static void sprite_draw_hline(
		struct fsv_sprite *sprite,
		uint32_t x1,
		uint32_t y1,
		uint32_t x2,
		uint32_t color)
{
	uint32_t offset = y1 * sprite->w;
        for (int x = x1 ; x < x2; x++) {
                sprite->data[offset + x] = color;
        }
}

/* drawing filled circle with Bresenham's algorithm */
static void sprite_draw_circle(
		struct fsv_sprite *sprite,
		uint32_t x,
		uint32_t y,
		uint32_t radius,
		uint32_t color)
{
        int offsetx = 0, offsety = radius, d = radius -1;

        while (offsety >= offsetx) {
                sprite_draw_hline(sprite, x - offsety, y + offsetx,
                                x + offsety, color);
                sprite_draw_hline(sprite, x - offsetx, y + offsety,
                                x + offsetx, color);
                sprite_draw_hline(sprite, x - offsetx, y - offsety,
                                x + offsetx, color);
                sprite_draw_hline(sprite, x - offsety, y - offsetx,
                                x + offsety, color);

                if (d >= 2 * offsetx) {
                        d -= 2 * offsetx + 1;
                        offsetx += 1;
                } else if (d < 2 * (radius - offsety)) {
                        d += 2 * offsety - 1;
                        offsety -= 1;
                } else {
                        d += 2 * (offsety - offsetx - 1);
                        offsety -= 1;
                        offsetx += 1;
                }
        }
}

static void sprite_anim_init(void)
{
        memset(&cicle_sprite, 0, sizeof(cicle_sprite));
        sprite_init(&cicle_sprite, 100, 100);

        sprite_draw_rect(&cicle_sprite, 0, 0, 100, 100, MK_COLOR(128, 128, 128));
	/* draw filled circle with center (50, 50), radius 50. */
        sprite_draw_circle(&cicle_sprite, 50, 50, 50, MK_COLOR(0, 0, 255));
}

static void sprite_anim(data_t *data, uint32_t *addr)
{
        struct timeval tv1, tv2, tv_delta;
        uint64_t frame_ns = get_time_ns();
        double now = frame_ns / (double)NSECS_PER_SEC;

        gettimeofday(&tv1, NULL);

        fbmem_draw_rect(addr, data->hdisplay, 0, 0,
			data->hdisplay, data->vdisplay, MK_COLOR(128, 128, 128));
	/* red rectangle for checking tearing effect*/
        if (data->front) {
                fbmem_draw_rect(addr, data->hdisplay, 0, 0,
			30, data->vdisplay, MK_COLOR(191, 0, 0));
        }

	/* draw 16 filled circles */
        for (int i = 0; i < 16; ++i) {
                double tv = now + i * 0.25;
                float x, y;
                x = data->hdisplay - 10.0f - 118.0f * i - 100.0f;
                y = data->vdisplay * 0.5f + cos(tv) * data->vdisplay * 0.35;
                sprite_paste(addr, data->hdisplay, &cicle_sprite, (uint32_t)x, (uint32_t)y);
        }

        gettimeofday(&tv2, NULL);
        timersub(&tv2, &tv1, &tv_delta);

        igt_debug("time of drawing: %ld ms\n", tv_delta.tv_usec / 1000);
}

/*----------------------------------------------------------------------------*/

/* The freesync video modes is derived from the base mode(the mode with the
   highest clock rate, and has the same resolution with preferred mode) by
   amdgpu driver. They have the same clock rate with base mode, and the
   type of mode has been set as DRM_MODE_TYPE_DRIVER"
*/
static bool is_freesync_video_mode(data_t *data, drmModeModeInfo *mode)
{
        drmModeModeInfo *base_mode = &data->modes[data->base_mode_index];
        uint32_t bm_clock = base_mode->clock;

        if (    mode->hdisplay == data->hdisplay &&
                mode->vdisplay == data->vdisplay &&
                mode->clock == bm_clock &&
		mode->type & DRM_MODE_TYPE_DRIVER) {
                return true;
        }

        return false;
}

static drmModeModeInfo* select_mode(
        data_t *data,
        uint32_t mode_type,
        int refresh_rate)
{
	int i;
        int index;
        drmModeModeInfo *mode = NULL;
	igt_debug("select_mode: type=%d, refresh_rate=%d\n", mode_type, refresh_rate);

        switch (mode_type) {
        case FSV_BASE_MODE:
                index = data->base_mode_index;
                mode = &data->modes[index];
                break;

        case FSV_PREFERRED_MODE:
                index = data->preferred_mode_index;
                mode = &data->modes[index];
                break;

        case FSV_FREESYNC_VIDEO_MODE:
                for (i = 0; i < data->count_modes; i++) {
                        mode = &data->modes[i];
                        if (    mode->vrefresh == refresh_rate &&
                                is_freesync_video_mode(data, mode)) {
                                break;
                        }
                }
		if (i == data->count_modes)
			mode = NULL;
                break;

        case FSV_NON_FREESYNC_VIDEO_MODE:
                for (i = 0; i < data->count_modes; i++) {
                        mode = &data->modes[i];
                        if (    mode->vrefresh == refresh_rate &&
                                !is_freesync_video_mode(data, mode)) {
                                break;
                        }
                }
		if (i == data->count_modes)
			mode = NULL;
                break;

        default:
                igt_assert("Cannot find mode with specified rate and type.");
                break;
        }

	if (mode) {
		igt_info("selected mode:\n");
		kmstest_dump_mode(mode);
	}

        return mode;
}

static int prepare_custom_mode(
        data_t *data,
	drmModeModeInfo *custom_mode,
	uint32_t refresh_rate)
{
	uint64_t num, den;
	uint64_t target_vtotal, target_vtotal_diff;
	drmModeModeInfo *base_mode;

	igt_info("prepare custom mode:\n");

	base_mode = &data->modes[data->base_mode_index];
	if (base_mode->vrefresh < refresh_rate) {
		igt_warn("The given refresh rate is large than base mode's one:" \
				" base_mode->vrefresh=%d, refresh_rate=%u\n",
				base_mode->vrefresh, refresh_rate);
		return -1;
	}

	if (refresh_rate < data->range.min ||
			refresh_rate > data->range.max) {
		igt_warn("The given refresh rate(%u) should be between the rage of: min=%d, max=%d\n",
				refresh_rate, data->range.min, data->range.max);
		return -1;
	}

	num = (unsigned long long)base_mode->clock * 1000 * 1000;
	den = refresh_rate * 1000 * (unsigned long long)base_mode->htotal;
	target_vtotal = num / den;
	target_vtotal_diff = target_vtotal - base_mode->vtotal;
	igt_debug("num=%lu, den=%lu, " \
                  "target_vtotal=%lu, target_vtotal_diff=%lu, base_mode->vtotal=%d\n",
		  num, den, target_vtotal, target_vtotal_diff, base_mode->vtotal
		);

	/* Check for illegal modes */
	if (base_mode->vsync_start + target_vtotal_diff < base_mode->vdisplay ||
			base_mode->vsync_end + target_vtotal_diff < base_mode->vsync_start ||
			base_mode->vtotal + target_vtotal_diff < base_mode->vsync_end)
		return -1;

	*custom_mode = *base_mode;
	custom_mode->vtotal += (uint16_t)target_vtotal_diff;
	custom_mode->vsync_start += (uint16_t)target_vtotal_diff;
	custom_mode->vsync_end += (uint16_t)target_vtotal_diff;
	custom_mode->type &= ~DRM_MODE_TYPE_PREFERRED;
	custom_mode->type |= DRM_MODE_TYPE_DRIVER;
	custom_mode->vrefresh = refresh_rate;

	igt_info("custom mode:\n");
	kmstest_dump_mode(custom_mode);

	return 0;
}

/* Returns the rate duration in nanoseconds for the given refresh rate. */
static uint64_t nsec_per_frame(uint64_t refresh)
{
	return NSECS_PER_SEC / refresh;
}

/* Read min and max vrr range from the connector debugfs. */
static range_t
get_vrr_range(data_t *data, igt_output_t *output)
{
	char buf[256];
	char *start_loc;
	int fd, res;
	range_t range;

	fd = igt_debugfs_connector_dir(data->drm_fd, output->name, O_RDONLY);
	igt_assert(fd >= 0);

	res = igt_debugfs_simple_read(fd, "vrr_range", buf, sizeof(buf));
	igt_require(res > 0);

	close(fd);

	igt_assert(start_loc = strstr(buf, "Min: "));
	igt_assert_eq(sscanf(start_loc, "Min: %u", &range.min), 1);

	igt_assert(start_loc = strstr(buf, "Max: "));
	igt_assert_eq(sscanf(start_loc, "Max: %u", &range.max), 1);

	return range;
}

/* Returns true if an output supports VRR. */
static bool has_vrr(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_VRR_CAPABLE) &&
	       igt_output_get_prop(output, IGT_CONNECTOR_VRR_CAPABLE);
}

/* Toggles variable refresh rate on the pipe. */
static void set_vrr_on_pipe(data_t *data, enum pipe pipe, bool enabled)
{
	igt_pipe_set_prop_value(&data->display, pipe, IGT_CRTC_VRR_ENABLED,
				enabled);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void prepare_test(
		data_t *data,
		igt_output_t *output,
		enum pipe pipe,
		drmModeModeInfo *mode)
{
	/* Reset output */
	igt_display_reset(&data->display);
	igt_output_set_pipe(output, pipe);

	igt_output_override_mode(output, mode);

	/* Prepare resources */
	if (!data->fb_initialized) {
		igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
				DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &data->fbs[0]);

		igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
				DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &data->fbs[1]);
		data->fb_mem[0] = igt_fb_map_buffer(data->drm_fd, &data->fbs[0]);
		data->fb_mem[1] = igt_fb_map_buffer(data->drm_fd, &data->fbs[1]);
		data->fb_initialized = true;
	}

	fbmem_draw_smpte_pattern(data->fb_mem[0], data->hdisplay, data->vdisplay);
	fbmem_draw_smpte_pattern(data->fb_mem[1], data->hdisplay, data->vdisplay);

	/* Take care of any required modesetting before the test begins. */
	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(data->primary, &data->fbs[0]);

	/* Clear vrr_enabled state before enabling it, because
	 * it might be left enabled if the previous test fails.
	 */
	igt_pipe_set_prop_value(&data->display, pipe, IGT_CRTC_VRR_ENABLED, 0);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

/* Performs an atomic non-blocking page-flip on a pipe. */
static void
do_flip(data_t *data)
{
	int ret;
	igt_fb_t *fb = &(data->fbs[data->front]);

	igt_set_timeout(1, "Scheduling page flip\n");
	igt_plane_set_fb(data->primary, fb);

	do {
		ret = igt_display_try_commit_atomic(&data->display,
				  DRM_MODE_ATOMIC_NONBLOCK |
				  DRM_MODE_PAGE_FLIP_EVENT,
				  data);
	} while (ret == -EBUSY);

	igt_assert_eq(ret, 0);
	igt_reset_timeout();
}

/*
 * Flips at the given rate and measures against the expected value.
 * Returns the pass rate as a percentage from 0 - 100.
 *
 * The VRR API is quite flexible in terms of definition - the driver
 * can arbitrarily restrict the bounds further than the absolute
 * min and max range. But VRR is really about extending the flip
 * to prevent stuttering or to match a source content rate.
 */
static uint32_t
flip_and_measure(
		data_t *data,
		igt_output_t *output,
		enum pipe pipe,
		uint64_t interval_ns,
		uint64_t duration_ns,
		int anim_type)
{
	uint64_t start_ns, last_event_ns, target_ns;
	uint32_t total_flip = 0, total_pass = 0;

	/* Align with the flip completion event to speed up convergence. */
	do_flip(data);
	start_ns = last_event_ns = target_ns = get_kernel_event_ns(data,
							DRM_EVENT_FLIP_COMPLETE);
	igt_info("interval_ns=%lu\n", interval_ns);

	for (;;) {
		uint64_t event_ns;
		int64_t diff_ns;

		data->front = !data->front;
		if (anim_type == ANIM_TYPE_CIRCLE_WAVE)
			sprite_anim(data, data->fb_mem[data->front]);
		do_flip(data);

		/* We need to capture flip event instead of vblank event,
		 * because vblank is triggered after each frame, but depending
		 * on the vblank evasion time flip might or might not happen in
		 * that same frame.
		 */
		event_ns = get_kernel_event_ns(data, DRM_EVENT_FLIP_COMPLETE);
		igt_debug("event_ns - last_event_ns: %"PRIu64"\n",
						(event_ns - last_event_ns));

		/*
		 * Check if the difference between the two flip timestamps
		 * was within the required threshold from the expected rate.
		 *
		 * A ~50us threshold is arbitrary, but it's roughly the
		 * difference between 144Hz and 143Hz which should give this
		 * enough accuracy for most use cases.
		 */
		diff_ns = interval_ns;
		diff_ns -= event_ns - last_event_ns;
		if (llabs(diff_ns) < 50000ll)
			total_pass += 1;

		last_event_ns = event_ns;
		total_flip += 1;

		if (event_ns - start_ns > duration_ns)
			break;
	}

	igt_info("Completed %u flips, %u were in threshold for (%llu Hz) %"PRIu64"ns.\n",
		 total_flip, total_pass, (NSECS_PER_SEC / interval_ns), interval_ns);

	return total_flip ? ((total_pass * 100) / total_flip) : 0;
}

static void init_data(data_t *data, igt_output_t *output) {
	int i;
	uint32_t pm_hdisplay, pm_vdisplay, max_clk = 0;
	drmModeModeInfo *preferred_mode;
	drmModeConnector *connector;

	connector = data->connector = output->config.connector;
	data->count_modes = connector->count_modes;
	data->modes = (drmModeModeInfo *)malloc(sizeof(drmModeModeInfo) * data->count_modes);

	for (i = 0; i < data->count_modes; i++) {
		data->modes[i] = connector->modes[i];
#ifdef FSV_DEBUG
		igt_info("mode %d:", i);
		kmstest_dump_mode(&data->modes[i]);
#endif
	}

	/* searching the preferred mode */
        for (i = 0; i < connector->count_modes; i++) {
                drmModeModeInfo *mode = &connector->modes[i];

                if (mode->type & DRM_MODE_TYPE_PREFERRED) {
                        data->preferred_mode_index = i;
			data->hdisplay = mode->hdisplay;
			data->vdisplay = mode->vdisplay;
			pm_hdisplay = preferred_mode->hdisplay;
			pm_vdisplay = preferred_mode->vdisplay;
			break;
                }
        }

        /* searching the base mode; */
        for (i = 0; i < connector->count_modes; i++) {
                drmModeModeInfo *mode = &connector->modes[i];
                if (mode->hdisplay == pm_hdisplay && mode->vdisplay == pm_vdisplay) {
                        if (mode->clock > max_clk) {
                                max_clk = mode->clock;
                                data->base_mode_index = i;
                        }
                }
        }
        igt_info("preferred=%d, base=%d\n", data->preferred_mode_index, data->base_mode_index);

        for (i = 0; i < connector->count_modes; i++) {
                drmModeModeInfo *mode = &connector->modes[i];
                if (is_freesync_video_mode(data, mode))
                        igt_debug("mode[%d] is freesync video mode.\n", i);
        }

	data->range = get_vrr_range(data, output);
}

static void finish_test(data_t *data, enum pipe pipe, igt_output_t *output)
{
	set_vrr_on_pipe(data, pipe, 0);
	igt_plane_set_fb(data->primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_output_override_mode(output, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_fb_unmap_buffer(&data->fbs[1], data->fb_mem[1]);
	igt_fb_unmap_buffer(&data->fbs[0], data->fb_mem[0]);
	igt_remove_fb(data->drm_fd, &data->fbs[1]);
	igt_remove_fb(data->drm_fd, &data->fbs[0]);
}

static void
mode_transition(data_t *data, enum pipe pipe, igt_output_t *output, uint32_t scene)
{
	uint32_t result;
	uint64_t interval;
	drmModeModeInfo *mode_start = NULL, *mode_playback = NULL, mode_custom;

	init_data(data, output);
	sprite_anim_init();

	igt_info("stage-1:\n");
	switch(scene) {
        case SCENE_BASE_MODE_TO_VARIOUS_FSV_MODE:
		mode_start = select_mode(data, FSV_BASE_MODE, 0);
                mode_playback  = select_mode(data, FSV_FREESYNC_VIDEO_MODE, 60);
		break;
        case SCENE_LOWER_FSV_MODE_TO_HIGHER_FSV_MODE:
		mode_start = select_mode(data, FSV_FREESYNC_VIDEO_MODE, 60);
                mode_playback = select_mode(data, FSV_FREESYNC_VIDEO_MODE, 120);
		break;
        case SCENE_NON_FSV_MODE_TO_FSV_MODE:
		mode_start = select_mode(data, FSV_NON_FREESYNC_VIDEO_MODE, 60);
                mode_playback = select_mode(data, FSV_FREESYNC_VIDEO_MODE, 60);
		break;
        case SCENE_BASE_MODE_TO_CUSTUM_MODE:
		mode_start = select_mode(data, FSV_BASE_MODE, 0);
		prepare_custom_mode(data, &mode_custom, 72);
		mode_playback = &mode_custom;
		break;
	case SCENE_NON_FSV_MODE_TO_NON_FSV_MODE:
		mode_start = select_mode(data, FSV_NON_FREESYNC_VIDEO_MODE, 120);
		mode_playback = select_mode(data, FSV_NON_FREESYNC_VIDEO_MODE, 100);
		break;
	default:
		igt_warn("Undefined test scene: %d", scene);
		break;
	}
	igt_assert_f(mode_start && mode_playback,
			"Failure on selecting mode with given type and refresh rate.\n");
	prepare_test(data, output, pipe, mode_start);
	interval = nsec_per_frame(mode_start->vrefresh) ;
	set_vrr_on_pipe(data, pipe, 1);
	result = flip_and_measure(data, output, pipe, interval, TEST_DURATION_NS, ANIM_TYPE_SMPTE);

	igt_info("stage-2: simple animation as video playback\n");
	prepare_test(data, output, pipe, mode_playback);
	interval = nsec_per_frame(mode_playback->vrefresh) ;
	result = flip_and_measure(data, output, pipe, interval, TEST_DURATION_NS, ANIM_TYPE_CIRCLE_WAVE);
	igt_assert_f(result > 90, "Target refresh rate not meet(result=%d%%\n", result);

	finish_test(data, pipe, output);
}

static void
run_test(data_t *data, uint32_t scene)
{
	igt_output_t *output;
	bool found = false;

	for_each_connected_output(&data->display, output) {
		enum pipe pipe;

		if (!has_vrr(output))
			continue;

		for_each_pipe(&data->display, pipe)
			if (igt_pipe_connector_valid(pipe, output)) {
				mode_transition(data, pipe, output, scene);
				found = true;
				break;
			}
	}

	if (!found)
		igt_skip("No vrr capable outputs found.\n");
}

igt_main
{
	data_t data = {};
	memset(&data, 0, sizeof(data));

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_AMDGPU);
		if (data.drm_fd == -1) {
			igt_skip("Not an amdgpu driver.\n");
		}
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	/* Expectation: Modeset happens instantaneously without blanking */
        igt_describe("Test switch from base freesync mode to " \
                     "various freesync video modes");
        igt_subtest("freesync-base-to-various")
		run_test(&data, SCENE_BASE_MODE_TO_VARIOUS_FSV_MODE);

	/* Expectation: Modeset happens instantaneously without blanking */
        igt_describe("Test switching from lower refresh freesync mode to " \
                     "another freesync mode with higher refresh rate");
        igt_subtest("freesync-lower-to-higher")
		run_test(&data, SCENE_LOWER_FSV_MODE_TO_HIGHER_FSV_MODE);

	/* Expectation: Full modeset is triggered. */
        igt_describe("Test switching from non preferred video mode to " \
                     "one of freesync video mode");
        igt_subtest("freesync-non-preferred-to-freesync")
		run_test(&data, SCENE_NON_FSV_MODE_TO_FSV_MODE);

	/* Expectation: Modeset happens instantaneously without blanking */
        igt_describe("Add custom mode through xrandr based on " \
                     "base freesync mode and apply the new mode");
        igt_subtest("freesync-custom-mode")
		run_test(&data, SCENE_BASE_MODE_TO_CUSTUM_MODE);

        igt_info("end of test\n");

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
