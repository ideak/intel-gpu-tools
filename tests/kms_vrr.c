/*
 * Copyright 2018 Advanced Micro Devices, Inc.
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

#define NSECS_PER_SEC (1000000000ull)

/*
 * Each test measurement step runs for ~5 seconds.
 * This gives a decent sample size + enough time for any adaptation to occur if necessary.
 */
#define TEST_DURATION_NS (5000000000ull)

#define DRM_MODE_FMT    "\"%s\": %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x"
#define DRM_MODE_ARG(m) \
	(m)->name, (m)->vrefresh, (m)->clock, \
	(m)->hdisplay, (m)->hsync_start, (m)->hsync_end, (m)->htotal, \
	(m)->vdisplay, (m)->vsync_start, (m)->vsync_end, (m)->vtotal, \
	(m)->type, (m)->flags

enum {
	TEST_NONE = 0,
	TEST_DPMS = 1 << 0,
	TEST_SUSPEND = 1 << 1,
	TEST_FLIPLINE = 1 << 2,
};

typedef struct range {
	unsigned int min;
	unsigned int max;
} range_t;

typedef struct data {
	igt_display_t display;
	int drm_fd;
	igt_plane_t *primary;
	igt_fb_t fb0;
	igt_fb_t fb1;
	range_t range;
} data_t;

typedef struct vtest_ns {
	uint64_t min;
	uint64_t mid;
	uint64_t max;
} vtest_ns_t;

typedef void (*test_t)(data_t*, enum pipe, igt_output_t*, uint32_t);

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

/* Returns the rate duration in nanoseconds for the given refresh rate. */
static uint64_t rate_from_refresh(uint64_t refresh)
{
	return NSECS_PER_SEC / refresh;
}

/* Instead of running on default mode, loop through the connector modes
 * and find the mode with max refresh rate to exercise full vrr range.
 */
static drmModeModeInfo
output_mode_with_maxrate(igt_output_t *output, unsigned int vrr_max)
{
	int i;
	drmModeConnectorPtr connector = output->config.connector;
	drmModeModeInfo mode = *igt_output_get_mode(output);

	igt_debug("Default Mode " DRM_MODE_FMT "\n", DRM_MODE_ARG(&mode));

	for (i = 0; i < connector->count_modes; i++)
		if (connector->modes[i].vrefresh > mode.vrefresh &&
		    connector->modes[i].vrefresh <= vrr_max)
			mode = connector->modes[i];

	igt_debug("Override Mode " DRM_MODE_FMT "\n", DRM_MODE_ARG(&mode));

	return mode;
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

/* Returns vrr test frequency for min, mid & max range. */
static vtest_ns_t get_test_rate_ns(range_t range)
{
	vtest_ns_t vtest_ns;

	vtest_ns.min = rate_from_refresh(range.min);
	vtest_ns.mid = rate_from_refresh(((range.max + range.min) / 2));
	vtest_ns.max = rate_from_refresh(range.max);

	return vtest_ns;
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

/* Prepare the display for testing on the given pipe. */
static void prepare_test(data_t *data, igt_output_t *output, enum pipe pipe)
{
	drmModeModeInfo mode;
	cairo_t *cr;

	/* Reset output */
	igt_display_reset(&data->display);
	igt_output_set_pipe(output, pipe);

	/* Capture VRR range */
	data->range = get_vrr_range(data, output);

	/* Override mode with max vrefresh.
	 *   - vrr_min range should be less than the override mode vrefresh.
	 *   - Limit the vrr_max range with the override mode vrefresh.
	 */
	mode = output_mode_with_maxrate(output, data->range.max);
	igt_require(mode.vrefresh > data->range.min);
	data->range.max = mode.vrefresh;
	igt_output_override_mode(output, &mode);

	/* Prepare resources */
	igt_create_color_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_NONE,
			    0.50, 0.50, 0.50, &data->fb0);

	igt_create_color_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_NONE,
			    0.50, 0.50, 0.50, &data->fb1);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb0);

	igt_paint_color(cr, 0, 0, mode.hdisplay / 10, mode.vdisplay / 10,
			1.00, 0.00, 0.00);

	igt_put_cairo_ctx(cr);

	/* Take care of any required modesetting before the test begins. */
	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(data->primary, &data->fb0);

	/* Clear vrr_enabled state before enabling it, because
	 * it might be left enabled if the previous test fails.
	 */
	igt_pipe_set_prop_value(&data->display, pipe, IGT_CRTC_VRR_ENABLED, 0);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

/* Performs an atomic non-blocking page-flip on a pipe. */
static void
do_flip(data_t *data, igt_fb_t *fb)
{
	int ret;

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
flip_and_measure(data_t *data, igt_output_t *output, enum pipe pipe,
		 uint64_t rate_ns, uint64_t duration_ns)
{
	uint64_t start_ns, last_event_ns, target_ns;
	uint32_t total_flip = 0, total_pass = 0;
	bool front = false;
	vtest_ns_t vtest_ns = get_test_rate_ns(data->range);

	/* Align with the flip completion event to speed up convergence. */
	do_flip(data, &data->fb0);
	start_ns = last_event_ns = target_ns = get_kernel_event_ns(data,
							DRM_EVENT_FLIP_COMPLETE);

	for (;;) {
		uint64_t event_ns, wait_ns;
		int64_t diff_ns;

		front = !front;
		do_flip(data, front ? &data->fb1 : &data->fb0);

		/* We need to cpture flip event instead of vblank event,
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
		if ((rate_ns < vtest_ns.min) && (rate_ns >= vtest_ns.max))
			diff_ns = rate_ns;
		else
			diff_ns = vtest_ns.max;
		diff_ns -= event_ns - last_event_ns;

		if (llabs(diff_ns) < 50000ll)
			total_pass += 1;

		last_event_ns = event_ns;
		total_flip += 1;

		if (event_ns - start_ns > duration_ns)
			break;

		/*
		 * Burn CPU until next timestamp, sleeping isn't accurate enough.
		 * The target timestamp is based on the delta b/w event timestamps
		 * and whatever the time left to reach the expected refresh rate.
		 */
		diff_ns = event_ns - target_ns;
		wait_ns = ((diff_ns + rate_ns - 1) / rate_ns) * rate_ns;
		wait_ns -= diff_ns;
		target_ns = event_ns + wait_ns;

		while (get_time_ns() < target_ns - 10);
	}

	igt_info("Completed %u flips, %u were in threshold for (%llu Hz) %"PRIu64"ns.\n",
		 total_flip, total_pass, (NSECS_PER_SEC/rate_ns), rate_ns);

	return total_flip ? ((total_pass * 100) / total_flip) : 0;
}

/* Basic VRR flip functionality test - enable, measure, disable, measure */
static void
test_basic(data_t *data, enum pipe pipe, igt_output_t *output, uint32_t flags)
{
	uint32_t result;
	vtest_ns_t vtest_ns;
	range_t range;
	uint64_t rate;

	prepare_test(data, output, pipe);
	range = data->range;
	vtest_ns = get_test_rate_ns(range);
	rate = vtest_ns.mid;

	igt_info("VRR Test execution on %s, PIPE_%s with VRR range: (%u-%u) Hz\n",
		 output->name, kmstest_pipe_name(pipe), range.min, range.max);

	set_vrr_on_pipe(data, pipe, 1);

	/*
	 * Do a short run with VRR, but don't check the result.
	 * This is to make sure we were actually in the middle of
	 * active flipping before doing the DPMS/suspend steps.
	 */
	flip_and_measure(data, output, pipe, rate, 250000000ull);

	if (flags & TEST_DPMS) {
		kmstest_set_connector_dpms(output->display->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(output->display->drm_fd,
					   output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	if (flags & TEST_SUSPEND)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);

	/*
	 * Check flipline mode by making sure that flips happen at flipline
	 * decision boundary.
	 *
	 * Example: if range is 40 - 60Hz and
	 * if refresh_rate > 60Hz:
	 *      Flip should happen at the flipline boundary & returned refresh rate
	 *      would be 60Hz.
	 * if refresh_rate is 50Hz:
	 *      Flip will happen right away so returned refresh rate is 50Hz.
	 * if refresh_rate < 40Hz:
	 *      h/w will terminate the vblank at Vmax which is obvious.
	 *      So, for now we can safely ignore the lower refresh rates
	 */
	if (flags & TEST_FLIPLINE) {
		rate = rate_from_refresh(range.max + 5);
		result = flip_and_measure(data, output, pipe, rate, TEST_DURATION_NS);
		igt_assert_f(result > 75,
			     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR on threshold not reached, result was %u%%\n",
			     (range.max + 5), rate, result);
	}

	rate = vtest_ns.mid;
	result = flip_and_measure(data, output, pipe, rate, TEST_DURATION_NS);
	igt_assert_f(result > 75,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR on threshold not reached, result was %u%%\n",
		     ((range.max + range.min) / 2), rate, result);

	set_vrr_on_pipe(data, pipe, 0);
	result = flip_and_measure(data, output, pipe, rate, TEST_DURATION_NS);
	igt_assert_f(result < 10,
		     "Refresh rate (%u Hz) %"PRIu64"ns: Target VRR off threshold exceeded, result was %u%%\n",
		     ((range.max + range.min) / 2), rate, result);

	/* Clean-up */
	igt_plane_set_fb(data->primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_output_override_mode(output, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_remove_fb(data->drm_fd, &data->fb1);
	igt_remove_fb(data->drm_fd, &data->fb0);
}

/* Runs tests on outputs that are VRR capable. */
static void
run_vrr_test(data_t *data, test_t test, uint32_t flags)
{
	igt_output_t *output;
	bool found = false;

	for_each_connected_output(&data->display, output) {
		enum pipe pipe;

		if (!has_vrr(output))
			continue;

		for_each_pipe(&data->display, pipe)
			if (igt_pipe_connector_valid(pipe, output)) {
				test(data, pipe, output, flags);
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

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Tests that VRR is enabled and that the difference between flip "
		     "timestamps converges to the requested rate");
	igt_subtest("flip-basic")
		run_vrr_test(&data, test_basic, 0);

	igt_describe("Tests with DPMS that VRR is enabled and that the difference between flip "
		     "timestamps converges to the requested rate.");
	igt_subtest("flip-dpms")
		run_vrr_test(&data, test_basic, TEST_DPMS);

	igt_describe("Tests that VRR is enabled and that the difference between flip "
		     "timestamps converges to the requested rate in a suspend test");
	igt_subtest("flip-suspend")
		run_vrr_test(&data, test_basic, TEST_SUSPEND);

	igt_describe("Make sure that flips happen at flipline decision boundary.");
	igt_subtest("flipline")
		run_vrr_test(&data, test_basic, TEST_FLIPLINE);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
