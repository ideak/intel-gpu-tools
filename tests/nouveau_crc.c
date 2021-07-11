/*
 * Copyright © 2020 Red Hat Inc.
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

#include <fcntl.h>
#include "igt.h"
#include "igt_sysfs.h"

IGT_TEST_DESCRIPTION(
"Tests certain aspects of CRC capture that are exclusive to nvidia hardware, "
"such as context flipping.");

typedef struct {
	int pipe;
	int drm_fd;
	int nv_crc_dir;
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	igt_fb_t default_fb;
} data_t;

struct color_fb {
	double r, g, b;
	igt_crc_t crc;
	igt_fb_t fb;
};

#define HEX_COLOR(r_, g_, b_) \
	{ .r = (r_ / 255.0), .g = (g_ / 255.0), .b = (b_ / 255.0) }

static void set_crc_flip_threshold(data_t *data, unsigned int threshold)
{
	igt_debug("Setting CRC notifier flip threshold to %d\n", threshold);
	igt_assert_lt(0, igt_sysfs_printf(data->nv_crc_dir, "flip_threshold", "%d", threshold));
}

/* Initialize each color_fb along with its respective CRC */
static void create_crc_colors(data_t *data,
			      struct color_fb *colors,
			      size_t len,
			      igt_pipe_crc_t *pipe_crc)
{
	char *crc_str;

	igt_pipe_crc_start(pipe_crc);

	for (int i = 0; i < len; i++) {
		igt_create_color_fb(data->drm_fd,
				    data->mode->hdisplay,
				    data->mode->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_NONE,
				    colors[i].r, colors[i].g, colors[i].b,
				    &colors[i].fb);

		igt_plane_set_fb(data->primary, &colors[i].fb);
		igt_display_commit(&data->display);
		igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &colors[i].crc);

		crc_str = igt_crc_to_string(&colors[i].crc);
		igt_debug("CRC for frame %d of pattern: %s\n",
			  i, crc_str);
		free(crc_str);
	}

	igt_pipe_crc_stop(pipe_crc);
}

static void destroy_crc_colors(data_t *data, struct color_fb *colors, size_t len)
{
	/* So we don't turn off the pipe if we remove it's current fb */
	igt_plane_set_fb(data->primary, &data->default_fb);

	for (int i = 0; i < len; i++)
		igt_remove_fb(data->drm_fd, &colors[i].fb);
}

/*
 * Nvidia GPUs store CRCs in a limited memory region called the CRC notifier context. When this
 * region fills, new CRCs are not reported. Nouveau works around this by allocating two notifier
 * contents, and then flips between them whenever we pass a specific threshold. Note that even with
 * this approach, a single frame is lost during the context flip.
 */
static void test_ctx_flip_detection(data_t *data)
{
	struct color_fb colors[] = {
		HEX_COLOR(0xFF, 0x00, 0x18),
		HEX_COLOR(0xFF, 0xA5, 0x2C),
		HEX_COLOR(0xFF, 0xFF, 0x41),
		HEX_COLOR(0x00, 0x80, 0x18),
		HEX_COLOR(0x00, 0x00, 0xF9),
		HEX_COLOR(0x86, 0x00, 0x7D),
	};
	igt_output_t *output = data->output;
	igt_plane_t *primary = data->primary;
	igt_pipe_crc_t *pipe_crc;
	const int n_colors = ARRAY_SIZE(colors);
	const int n_crcs = 20;
	igt_crc_t *crcs = NULL;
	int start = -1, frame, start_color = -1, i;
	bool found_skip = false;

	pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe, "auto");

	create_crc_colors(data, colors, n_colors, pipe_crc);

	set_crc_flip_threshold(data, n_crcs / 2);
	igt_pipe_crc_start(pipe_crc);

	for (i = 0; i < n_crcs; i++) {
		const int color_idx = i % n_colors;

		igt_plane_set_fb(primary, &colors[color_idx].fb);
		do_or_die(drmModePageFlip(data->drm_fd,
					  output->config.crtc->crtc_id,
					  colors[color_idx].fb.fb_id,
					  DRM_MODE_PAGE_FLIP_EVENT,
					  NULL));
		kmstest_wait_for_pageflip(data->drm_fd);
	}

	igt_pipe_crc_get_crcs(pipe_crc, n_crcs, &crcs);
	igt_pipe_crc_stop(pipe_crc);

	/*
	 * Guard against CRC collisions in the color framebuffers by finding the first color in our
	 * pattern with a CRC that differs from the last CRC. That CRC can then be used to find the
	 * start of the pattern
	 */
	for (i = 0; i < n_colors - 1; i++) {
		if (igt_check_crc_equal(&colors[i].crc, &colors[n_colors - 1].crc))
			continue;

		igt_debug("Using frame %d of pattern for finding start\n", i);
		start_color = i;
		break;
	}
	igt_assert_lte(0, start_color);

	/* Now, figure out where the pattern starts */
	for (i = 0; i < n_crcs; i++) {
		if (!igt_check_crc_equal(&colors[start_color].crc, &crcs[i]))
			continue;

		start = i - start_color;
		frame = crcs[i].frame;
		igt_debug("Pattern started on frame %d\n", frame);
		break;
	}
	igt_assert_lte(0, start);

	/* And finally, assert that according to the CRCs exactly all but one
	 * frame was displayed in order. The missing frame comes from
	 * (inevitably) losing a single CRC event when nouveau switches notifier
	 * contexts
	 */
	for (i = start; i < n_crcs; i++, frame++) {
		igt_crc_t *crc = &crcs[i];
		char *crc_str;
		int color_idx;

		crc_str = igt_crc_to_string(crc);
		igt_debug("CRC %d: vbl=%d val=%s\n", i, crc->frame, crc_str);
		free(crc_str);

		if (!found_skip && crc->frame != frame) {
			igt_debug("^^^ Found expected skipped CRC %d ^^^\n",
				  crc->frame - 1);
			found_skip = true;
			frame++;
		}

		/* We should never skip more then one frame, as with nouveau's current CRC
		 * implementation this would mean that we've lost track of which CRC corresponds to
		 * which frame, making our frame index unreliable. So, we also check each frame that
		 * comes after the skip, and ensure that it matches the colors that we expect.
		 */
		if (found_skip) {
			igt_assert_eq(crc->frame, frame);
			color_idx = (i - start + 1) % n_colors;
		} else {
			color_idx = (i - start) % n_colors;
		}

		igt_assert_crc_equal(crc, &colors[color_idx].crc);
	}
	/* Also, if we never found a skip in the first place then something's broken and the CRC
	 * threshold we set was ignored by the driver, or the driver failed to flip contexts */
	igt_assert(found_skip);

	free(crcs);
	igt_pipe_crc_free(pipe_crc);
	destroy_crc_colors(data, colors, ARRAY_SIZE(colors));
}

/* Test whether or not IGT is able to handle frame skips when requesting the
 * CRC for the current frame
 */
static void test_ctx_flip_skip_current_frame(data_t *data)
{
	struct color_fb colors[] = {
		{ .r = 1.0, .g = 0.0, .b = 0.0 },
		{ .r = 0.0, .g = 1.0, .b = 0.0 },
		{ .r = 0.0, .g = 0.0, .b = 1.0 },
	};
	igt_output_t *output = data->output;
	igt_pipe_crc_t *pipe_crc;
	igt_plane_t *primary = data->primary;
	const int fd = data->drm_fd;
	const int n_colors = ARRAY_SIZE(colors);
	const int n_crcs = 30;

	pipe_crc = igt_pipe_crc_new(fd, data->pipe, "auto");
	create_crc_colors(data, colors, n_colors, pipe_crc);

	set_crc_flip_threshold(data, 5);
	igt_pipe_crc_start(pipe_crc);

	for (int i = 0; i < n_crcs; i++) {
		igt_crc_t crc;
		const int color_idx = i % n_colors;

		igt_plane_set_fb(primary, &colors[color_idx].fb);
		do_or_die(drmModePageFlip(fd,
					  output->config.crtc->crtc_id,
					  colors[color_idx].fb.fb_id,
					  DRM_MODE_PAGE_FLIP_EVENT,
					  NULL));
		kmstest_wait_for_pageflip(fd);

		igt_pipe_crc_get_current(fd, pipe_crc, &crc);
		igt_assert_crc_equal(&colors[color_idx].crc, &crc);
	}

	igt_pipe_crc_stop(pipe_crc);
	igt_pipe_crc_free(pipe_crc);
	destroy_crc_colors(data, colors, n_colors);
}

static void test_ctx_flip_threshold_reset_after_capture(data_t *data)
{
	igt_pipe_crc_t *pipe_crc;
	const int fd = data->drm_fd;

	pipe_crc = igt_pipe_crc_new(fd, data->pipe, "auto");

	set_crc_flip_threshold(data, 5);
	igt_pipe_crc_start(pipe_crc);
	igt_assert_eq(igt_sysfs_get_u32(data->nv_crc_dir, "flip_threshold"), 5);
	igt_pipe_crc_stop(pipe_crc);

	igt_assert_neq(igt_sysfs_get_u32(data->nv_crc_dir, "flip_threshold"), 5);
	igt_pipe_crc_free(pipe_crc);
}

static void test_source(data_t *data, const char *source)
{
	igt_pipe_crc_t *pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe, source);
	igt_crc_t *crcs;

	igt_pipe_crc_start(pipe_crc);
	igt_pipe_crc_get_crcs(pipe_crc, 2, &crcs);
	igt_pipe_crc_stop(pipe_crc);

	/* The CRC shouldn't change if the source content hasn't changed */
	igt_assert_crc_equal(&crcs[0], &crcs[1]);

	igt_pipe_crc_free(pipe_crc);
	free(crcs);
}

static void test_source_outp_inactive(data_t *data)
{
	struct color_fb colors[] = {
		{ .r = 1.0, .g = 0.0, .b = 0.0 },
		{ .r = 0.0, .g = 1.0, .b = 0.0 },
	};
	igt_pipe_crc_t *pipe_crc;
	const int fd = data->drm_fd;
	const int n_colors = ARRAY_SIZE(colors);

	pipe_crc = igt_pipe_crc_new(fd, data->pipe, "outp-inactive");
	create_crc_colors(data, colors, n_colors, pipe_crc);

	/* Changing the color should not change what's outside the active raster */
	igt_assert_crc_equal(&colors[0].crc, &colors[1].crc);

	igt_pipe_crc_free(pipe_crc);
	destroy_crc_colors(data, colors, n_colors);
}

data_t data = {0};

#define pipe_test(name) igt_subtest_f("pipe-%s-" name, kmstest_pipe_name(pipe))
igt_main
{
	int pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_require_nouveau(data.drm_fd);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_reset(&data.display);
	}

	for_each_pipe_static(pipe) {
		igt_fixture {
			int dir;

			data.pipe = pipe;
			igt_display_require_output_on_pipe(&data.display, pipe);

			/* Disable the output from the previous iteration of pipe tests, if there is
			 * one
			 */
			if (data.output) {
				igt_output_set_pipe(data.output, PIPE_NONE);
				igt_display_commit(&data.display);
			}

			data.output = igt_get_single_output_for_pipe(&data.display, pipe);
			data.mode = igt_output_get_mode(data.output);

			/* None of these tests need to perform modesets, just page flips. So running
			 * display setup here is fine
			 */
			igt_output_set_pipe(data.output, pipe);
			data.primary = igt_output_get_plane(data.output, 0);
			igt_create_color_fb(data.drm_fd,
					    data.mode->hdisplay,
					    data.mode->vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_NONE,
					    0.0, 0.0, 0.0,
					    &data.default_fb);
			igt_plane_set_fb(data.primary, &data.default_fb);
			igt_display_commit(&data.display);

			dir = igt_debugfs_pipe_dir(data.drm_fd, pipe, O_DIRECTORY);
			igt_require_fd(dir);
			data.nv_crc_dir = openat(dir, "nv_crc", O_DIRECTORY);
			close(dir);
			igt_require_fd(data.nv_crc_dir);
		}

		/* We don't need to test this on every pipe, but the setup is the same */
		if (pipe == PIPE_A) {
			igt_describe("Make sure that the CRC notifier context flip threshold "
				     "is reset to its default value after a single capture.");
			igt_subtest("ctx-flip-threshold-reset-after-capture")
				test_ctx_flip_threshold_reset_after_capture(&data);
		}

		igt_describe("Make sure the association between each CRC and its "
			     "respective frame index is not broken when the driver "
			     "performs a notifier context flip.");
		pipe_test("ctx-flip-detection")
			test_ctx_flip_detection(&data);

		igt_describe("Make sure that igt_pipe_crc_get_current() works even "
			     "when the CRC for the current frame index is lost.");
		pipe_test("ctx-flip-skip-current-frame")
			test_ctx_flip_skip_current_frame(&data);

		igt_describe("Check that basic CRC readback using the outp-complete "
			     "source works.");
		pipe_test("source-outp-complete")
			test_source(&data, "outp-complete");

		igt_describe("Check that basic CRC readback using the rg source "
			     "works.");
		pipe_test("source-rg")
			test_source(&data, "rg");

		igt_describe("Make sure that the outp-inactive source is actually "
			     "capturing the inactive raster.");
		pipe_test("source-outp-inactive")
			test_source_outp_inactive(&data);

		igt_fixture {
			igt_remove_fb(data.drm_fd, &data.default_fb);
			close(data.nv_crc_dir);
		}
	}
	igt_fixture
		igt_display_fini(&data.display);

}
