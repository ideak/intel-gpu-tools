/*
 * Copyright © 2013 Intel Corporation
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
#include "igt_sysfs.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>


typedef struct {
	int drm_fd;
	int debugfs;
	igt_display_t display;
	struct igt_fb fb;
} data_t;

static struct {
	double r, g, b;
	igt_crc_t crc;
} colors[2] = {
	{ .r = 0.0, .g = 1.0, .b = 0.0 },
	{ .r = 0.0, .g = 1.0, .b = 1.0 },
};

static void test_bad_source(data_t *data)
{
	errno = 0;
	if (igt_sysfs_set(data->debugfs, "crtc-0/crc/control", "foo")) {
		igt_assert(openat(data->debugfs, "crtc-0/crc/data", O_WRONLY) == -1);
		igt_skip_on(errno == EIO);
	}

	igt_assert_eq(errno, EINVAL);
}

#define N_CRCS	3

#define TEST_SEQUENCE (1<<0)
#define TEST_NONBLOCK (1<<1)

static void test_read_crc(data_t *data, enum pipe pipe, unsigned flags)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	igt_crc_t *crcs = NULL;
	int c, j;

	igt_display_require_output_on_pipe(display, pipe);
	output = igt_get_single_output_for_pipe(display, pipe);

	igt_display_reset(display);
	igt_output_set_pipe(output, pipe);

	for (c = 0; c < ARRAY_SIZE(colors); c++) {
		char *crc_str;
		int n_crcs;

		igt_debug("Clearing the fb with color (%.02lf,%.02lf,%.02lf)\n",
			  colors[c].r, colors[c].g, colors[c].b);

		mode = igt_output_get_mode(output);
		igt_create_color_fb(data->drm_fd,
					mode->hdisplay, mode->vdisplay,
					DRM_FORMAT_XRGB8888,
					LOCAL_DRM_FORMAT_MOD_NONE,
					colors[c].r,
					colors[c].g,
					colors[c].b,
					&data->fb);

		primary = igt_output_get_plane(output, 0);
		igt_plane_set_fb(primary, &data->fb);

		igt_display_commit(display);

		/* wait for N_CRCS vblanks and the corresponding N_CRCS CRCs */
		if (flags & TEST_NONBLOCK) {
			igt_pipe_crc_t *pipe_crc;

			pipe_crc = igt_pipe_crc_new_nonblock(data->drm_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
			igt_wait_for_vblank(data->drm_fd, display->pipes[pipe].crtc_offset);
			igt_pipe_crc_start(pipe_crc);

			igt_wait_for_vblank_count(data->drm_fd,
					display->pipes[pipe].crtc_offset, N_CRCS);
			n_crcs = igt_pipe_crc_get_crcs(pipe_crc, N_CRCS+1, &crcs);
			igt_pipe_crc_stop(pipe_crc);
			igt_pipe_crc_free(pipe_crc);

			/* allow a one frame difference */
			igt_assert_lte(N_CRCS, n_crcs);
		} else {
			igt_pipe_crc_t *pipe_crc;

			pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
			igt_pipe_crc_start(pipe_crc);

			n_crcs = igt_pipe_crc_get_crcs(pipe_crc, N_CRCS, &crcs);

			igt_pipe_crc_stop(pipe_crc);
			igt_pipe_crc_free(pipe_crc);

			igt_assert_eq(n_crcs, N_CRCS);
		}


		/*
		 * save the CRC in colors so it can be compared to the CRC of
		 * other fbs
		 */
		colors[c].crc = crcs[0];

		crc_str = igt_crc_to_string(&crcs[0]);
		igt_debug("CRC for this fb: %s\n", crc_str);
		free(crc_str);

		/* and ensure that they'are all equal, we haven't changed the fb */
		for (j = 0; j < (n_crcs - 1); j++)
			igt_assert_crc_equal(&crcs[j], &crcs[j + 1]);

		if (flags & TEST_SEQUENCE)
			for (j = 0; j < (n_crcs - 1); j++)
				igt_assert_eq(crcs[j].frame + 1, crcs[j + 1].frame);

		free(crcs);
		igt_remove_fb(data->drm_fd, &data->fb);
	}
}

/*
 * CRC-sanity test, to make sure there would be no CRC mismatches
 *
 * - Create two framebuffers (FB0 & FB1) with same color info
 * - Flip FB0 with the Primary plane & collect the CRC as ref CRC.
 * - Flip FB1 with the Primary plane, collect the CRC & compare with
 *   the ref CRC.
 *
 *   No CRC mismatch should happen
 */
static void test_compare_crc(data_t *data, enum pipe pipe)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	igt_crc_t ref_crc, crc;
	igt_pipe_crc_t *pipe_crc = NULL;
	struct igt_fb fb0, fb1;
	igt_output_t *output = igt_get_single_output_for_pipe(display, pipe);

	igt_require_f(output, "No connector found for pipe %s\n",
			kmstest_pipe_name(pipe));

	igt_display_reset(display);
	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);

	/* Create two framebuffers with the same color info. */
	igt_create_color_fb(data->drm_fd,
			mode->hdisplay, mode->vdisplay,
			DRM_FORMAT_XRGB8888,
			LOCAL_DRM_FORMAT_MOD_NONE,
			1.0, 1.0, 1.0,
			&fb0);
	igt_create_color_fb(data->drm_fd,
			mode->hdisplay, mode->vdisplay,
			DRM_FORMAT_XRGB8888,
			LOCAL_DRM_FORMAT_MOD_NONE,
			1.0, 1.0, 1.0,
			&fb1);

	/* Flip FB0 with the Primary plane & collect the CRC as ref CRC. */
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &fb0);
	igt_display_commit(display);

	pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe,
				    INTEL_PIPE_CRC_SOURCE_AUTO);
	igt_pipe_crc_collect_crc(pipe_crc, &ref_crc);

	/* Flip FB1 with the Primary plane & compare the CRC with ref CRC. */
	igt_plane_set_fb(primary, &fb1);
	igt_display_commit(display);

	igt_pipe_crc_collect_crc(pipe_crc, &crc);
	igt_assert_crc_equal(&crc, &ref_crc);

	/* Clean-up */
	igt_pipe_crc_free(pipe_crc);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(display);

	igt_remove_fb(data->drm_fd, &fb0);
	igt_remove_fb(data->drm_fd, &fb1);
}

data_t data = {0, };

igt_main
{
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);

		igt_display_require(&data.display, data.drm_fd);
		data.debugfs = igt_debugfs_dir(data.drm_fd);
	}

	igt_subtest("bad-source")
		test_bad_source(&data);

	for_each_pipe_static(pipe) {
		igt_subtest_f("read-crc-pipe-%s", kmstest_pipe_name(pipe))
			test_read_crc(&data, pipe, 0);

		igt_subtest_f("read-crc-pipe-%s-frame-sequence", kmstest_pipe_name(pipe))
			test_read_crc(&data, pipe, TEST_SEQUENCE);

		igt_subtest_f("nonblocking-crc-pipe-%s", kmstest_pipe_name(pipe))
			test_read_crc(&data, pipe, TEST_NONBLOCK);

		igt_subtest_f("nonblocking-crc-pipe-%s-frame-sequence", kmstest_pipe_name(pipe))
			test_read_crc(&data, pipe, TEST_SEQUENCE | TEST_NONBLOCK);

		igt_subtest_f("suspend-read-crc-pipe-%s", kmstest_pipe_name(pipe)) {
			igt_require_pipe(&data.display, pipe);

			test_read_crc(&data, pipe, 0);

			igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE);

			test_read_crc(&data, pipe, 0);
		}

		igt_subtest_f("hang-read-crc-pipe-%s", kmstest_pipe_name(pipe)) {
			igt_hang_t hang = igt_allow_hang(data.drm_fd, 0, 0);

			test_read_crc(&data, pipe, 0);

			igt_force_gpu_reset(data.drm_fd);

			test_read_crc(&data, pipe, 0);

			igt_disallow_hang(data.drm_fd, hang);
		}

		igt_describe("Basic sanity check for CRC mismatches");
		igt_subtest_f("compare-crc-sanitycheck-pipe-%s", kmstest_pipe_name(pipe))
			test_compare_crc(&data, pipe);
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
