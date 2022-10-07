/*
 * Copyright © 2016 Intel Corporation
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
 */

#include "igt.h"
#include <limits.h>
#include <stdbool.h>

IGT_TEST_DESCRIPTION("Make sure all modesets are rejected when the requested mode is invalid");

typedef struct _data data_t;

struct _data {
	int drm_fd;
	enum pipe pipe;
	igt_display_t display;
	igt_output_t *output;
	drmModeResPtr res;
	int max_dotclock;
	bool (*adjust_mode)(data_t *data, drmModeModeInfoPtr mode);
};

static bool has_scaling_mode_prop(data_t *data)
{
	return kmstest_get_property(data->drm_fd,
				    data->output->id,
				    DRM_MODE_OBJECT_CONNECTOR,
				    "scaling mode",
				    NULL, NULL, NULL);
}
static bool
can_bigjoiner(data_t *data)
{
	uint32_t devid = intel_get_drm_devid(data->drm_fd);

	/*
	 * GEN11 and GEN12 require DSC to support bigjoiner.
	 * XELPD and later GEN support uncompressed bigjoiner.
	 */
	if (intel_display_ver(devid) > 12) {
		igt_debug("Platform supports uncompressed bigjoiner\n");
		return true;
	} else if (intel_display_ver(devid) >= 11) {
		return igt_is_dsc_supported(data->drm_fd, data->output->name);
	}

	return false;
}

static bool
adjust_mode_clock_too_high(data_t *data, drmModeModeInfoPtr mode)
{
	int max_dotclock = data->max_dotclock;

	igt_require(max_dotclock != 0);

	/*
	 * FIXME When we have a fixed mode, the kernel will ignore
	 * the user timings apart from hdisplay/vdisplay. Should
	 * fix the kernel to at least make sure the requested
	 * refresh rate as specified by the user timings will
	 * roughly match the user will get. For now skip the
	 * test on  any connector with a fixed mode.
	 */
	if (has_scaling_mode_prop(data))
		return false;

	/*
	 * Newer platforms can support modes higher than the maximum dot clock
	 * by using pipe joiner, so set the mode clock twice that of maximum
	 * dot clock;
	 */
	if (can_bigjoiner(data)) {
		igt_info("Platform supports bigjoiner with %s\n",
			 data->output->name);
		max_dotclock *= 2;
	}

	mode->clock = max_dotclock + 1;

	return true;
}

static bool
adjust_mode_zero_clock(data_t *data, drmModeModeInfoPtr mode)
{
	mode->clock = 0;
	return true;
}

static bool
adjust_mode_int_max_clock(data_t *data, drmModeModeInfoPtr mode)
{
	mode->clock = INT_MAX;
	return true;
}

static bool
adjust_mode_uint_max_clock(data_t *data, drmModeModeInfoPtr mode)
{
	mode->clock = UINT_MAX;
	return true;
}

static bool
adjust_mode_zero_hdisplay(data_t *data, drmModeModeInfoPtr mode)
{
	mode->hdisplay = 0;
	return true;
}

static bool
adjust_mode_zero_vdisplay(data_t *data, drmModeModeInfoPtr mode)
{
	mode->vdisplay = 0;
	return true;
}

static bool
adjust_mode_bad_hsync_start(data_t *data, drmModeModeInfoPtr mode)
{
	mode->hsync_start = mode->hdisplay - 1;
	return true;
}

static bool
adjust_mode_bad_vsync_start(data_t *data, drmModeModeInfoPtr mode)
{
	mode->vsync_start = mode->vdisplay - 1;
	return true;
}

static bool
adjust_mode_bad_hsync_end(data_t *data, drmModeModeInfoPtr mode)
{
	mode->hsync_end = mode->hsync_start - 1;
	return true;
}

static bool
adjust_mode_bad_vsync_end(data_t *data, drmModeModeInfoPtr mode)
{
	mode->vsync_end = mode->vsync_start - 1;
	return true;
}

static bool
adjust_mode_bad_htotal(data_t *data, drmModeModeInfoPtr mode)
{
	mode->htotal = mode->hsync_end - 1;
	return true;
}

static bool
adjust_mode_bad_vtotal(data_t *data, drmModeModeInfoPtr mode)
{
	mode->vtotal = mode->vsync_end - 1;
	return true;
}

static void
test_output(data_t *data)
{
	igt_output_t *output = data->output;
	drmModeModeInfo mode;
	struct igt_fb fb;
	int ret;
	uint32_t crtc_id;

	/*
	 * FIXME test every mode we have to be more
	 * sure everything is really getting rejected?
	 */
	mode = *igt_output_get_mode(output);
	igt_require(data->adjust_mode(data, &mode));

	igt_create_fb(data->drm_fd,
		      max_t(uint16_t, mode.hdisplay, 64),
		      max_t(uint16_t, mode.vdisplay, 64),
		      DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR,
		      &fb);

	kmstest_unset_all_crtcs(data->drm_fd, data->res);

	crtc_id = data->display.pipes[data->pipe].crtc_id;

	ret = drmModeSetCrtc(data->drm_fd, crtc_id,
			     fb.fb_id, 0, 0,
			     &output->id, 1, &mode);
	igt_assert_lt(ret, 0);

	igt_remove_fb(data->drm_fd, &fb);
}

static int i915_max_dotclock(data_t *data)
{
	char buf[4096];
	char *s;
	int max_dotclock = 0;

	if (!is_i915_device(data->drm_fd))
		return 0;

	igt_debugfs_read(data->drm_fd, "i915_frequency_info", buf);
	s = strstr(buf, "Max pixel clock frequency:");
	igt_assert(s);
	igt_assert_eq(sscanf(s, "Max pixel clock frequency: %d kHz", &max_dotclock), 1);

	/* 100 Mhz to 5 GHz seem like reasonable values to expect */
	igt_assert_lt(max_dotclock, 5000000);
	igt_assert_lt(100000, max_dotclock);

	return max_dotclock;
}

static const struct {
	const char *name;
	bool (*adjust_mode)(data_t *data, drmModeModeInfoPtr mode);
} subtests[] = {
	{ .name = "clock-too-high",
	  .adjust_mode = adjust_mode_clock_too_high,
	},
	{ .name = "zero-clock",
	  .adjust_mode = adjust_mode_zero_clock,
	},
	{ .name = "int-max-clock",
	  .adjust_mode = adjust_mode_int_max_clock,
	},
	{ .name = "uint-max-clock",
	  .adjust_mode = adjust_mode_uint_max_clock,
	},
	{ .name = "zero-hdisplay",
	  .adjust_mode = adjust_mode_zero_hdisplay,
	},
	{ .name = "zero-vdisplay",
	  .adjust_mode = adjust_mode_zero_vdisplay,
	},
	{ .name = "bad-hsync-start",
	  .adjust_mode = adjust_mode_bad_hsync_start,
	},
	{ .name = "bad-vsync-start",
	  .adjust_mode = adjust_mode_bad_vsync_start,
	},
	{ .name = "bad-hsync-end",
	  .adjust_mode = adjust_mode_bad_hsync_end,
	},
	{ .name = "bad-vsync-end",
	  .adjust_mode = adjust_mode_bad_vsync_end,
	},
	{ .name = "bad-htotal",
	  .adjust_mode = adjust_mode_bad_htotal,
	},
	{ .name = "bad-vtotal",
	  .adjust_mode = adjust_mode_bad_vtotal,
	},
};

static data_t data;

igt_main
{

	enum pipe pipe;
	igt_output_t *output;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		data.res = drmModeGetResources(data.drm_fd);
		igt_assert(data.res);

		data.max_dotclock = i915_max_dotclock(&data);
		igt_info("Max dotclock: %d kHz\n", data.max_dotclock);
	}

	igt_describe("Make sure all modesets are rejected when the requested mode is invalid");
	for (int i = 0; i < ARRAY_SIZE(subtests); i++) {
		igt_subtest_with_dynamic(subtests[i].name) {
			for_each_pipe_with_valid_output(&data.display, pipe, output) {
				igt_dynamic_f("%s-pipe-%s", igt_output_name(output), kmstest_pipe_name(pipe)) {
					data.output = output;
					data.pipe = pipe;
					data.adjust_mode = subtests[i].adjust_mode;
					test_output(&data);
				}
			}
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
		igt_reset_connectors();
		drmModeFreeResources(data.res);
		close(data.drm_fd);
	}
}
