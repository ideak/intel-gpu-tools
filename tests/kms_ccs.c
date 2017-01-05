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

IGT_TEST_DESCRIPTION("Test render compression (RC), in which the main surface "
		     "is complemented by a color control surface (CCS) that "
		     "the display uses to interpret the compressed data.");

enum test_flags {
	TEST_CRC			= 1 << 1,
	TEST_ROTATE_180			= 1 << 2,
	TEST_BAD_PIXEL_FORMAT		= 1 << 3,
	TEST_BAD_ROTATION_90		= 1 << 4,
};

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb;
	igt_output_t *output;
	enum pipe pipe;
	enum test_flags flags;
} data_t;

#define COMPRESSED_RED		0x0ff0000f
#define COMPRESSED_GREEN	0x000ff00f
#define COMPRESSED_BLUE		0x00000fff

#define RED			0x00ff0000

static void render_fb(data_t *data, bool compressed)
{
	struct igt_fb *fb = &data->fb;
	uint32_t *ptr;
	int i;

	igt_assert(fb->fb_id);

	ptr = gem_mmap__cpu(data->drm_fd, fb->gem_handle,
			    0, fb->size,
			    PROT_READ | PROT_WRITE);

	for (i = 0; i < fb->size / 4; i++) {
		/* Fill upper half as compressed */
		if (compressed && i < fb->size / 4 / 2)
			ptr[i] = COMPRESSED_RED;
		else
			ptr[i] = RED;
	}

	munmap(ptr, fb->size);
}

static uint8_t *ccs_ptr(uint8_t *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride)
{
	return ptr +
		((y & ~0x3f) * stride) +
		((x & ~0x7) * 64) +
		((y & 0x3f) * 8) +
		(x & 7);
}

static void render_ccs(data_t *data, uint32_t gem_handle,
		       uint32_t offset, uint32_t size,
		       int w, int h, unsigned int stride)
{
	uint8_t *ptr;
	int x, y;

	ptr = gem_mmap__cpu(data->drm_fd, gem_handle,
			    offset, size,
			    PROT_READ | PROT_WRITE);

	/* Mark upper half as compressed */
	for (x = 0 ; x < w; x++)
		for (y = 0 ; y <= h / 2; y++)
			*ccs_ptr(ptr, x, y, stride) = 0x55;

	munmap(ptr, size);
}

static void display_fb(data_t *data, int compressed)
{
	struct local_drm_mode_fb_cmd2 f = {};
	struct igt_fb *fb = &data->fb;
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	unsigned int width, height;
	unsigned int size[2];
	uint64_t modifier;
	enum igt_commit_style commit = COMMIT_UNIVERSAL;
	int ret;

	if (data->display.is_atomic)
		commit = COMMIT_ATOMIC;

	mode = igt_output_get_mode(data->output);

	if (compressed)
		modifier = LOCAL_I915_FORMAT_MOD_Y_TILED_CCS;
	else
		modifier = LOCAL_I915_FORMAT_MOD_Y_TILED;

	f.flags = LOCAL_DRM_MODE_FB_MODIFIERS;
	f.width = ALIGN(mode->hdisplay, 16);
	f.height = ALIGN(mode->vdisplay, 8);

	if (data->flags & TEST_BAD_PIXEL_FORMAT)
		f.pixel_format = DRM_FORMAT_RGB565;
	else
		f.pixel_format = DRM_FORMAT_XRGB8888;

	width = f.width;
	height = f.height;
	f.pitches[0] = ALIGN(width * 4, 128);
	f.modifier[0] = modifier;
	f.offsets[0] = 0;
	size[0] = f.pitches[0] * ALIGN(height, 32);

	if (compressed) {
		width = ALIGN(f.width, 16) / 16;
		height = ALIGN(f.height, 8) / 8;
		f.pitches[1] = ALIGN(width * 1, 64);
		f.modifier[1] = modifier;
		f.offsets[1] = size[0];
		size[1] = f.pitches[1] * ALIGN(height, 64);

		f.handles[0] = gem_create(data->drm_fd, size[0] + size[1]);
		f.handles[1] = f.handles[0];
	} else
		f.handles[0] = gem_create(data->drm_fd, size[0]);

	ret = drmIoctl(data->drm_fd, LOCAL_DRM_IOCTL_MODE_ADDFB2, &f);
	if (data->flags & TEST_BAD_PIXEL_FORMAT) {
		igt_assert_eq(ret, -1);
		igt_assert_eq(errno, EINVAL);
		return;
	} else
		igt_assert_eq(ret, 0);

	fb->fb_id = f.fb_id;
	fb->fd = data->drm_fd;
	fb->gem_handle = f.handles[0];
	fb->is_dumb = false;
	fb->drm_format = f.pixel_format;
	fb->width = f.width;
	fb->height = f.height;
	fb->stride = f.pitches[0];
	fb->tiling = f.modifier[0];
	fb->size = size[0];
	fb->cairo_surface = NULL;
	fb->domain = 0;

	render_fb(data, compressed);

	if (compressed)
		render_ccs(data, f.handles[0], f.offsets[1], size[1],
			   f.width/16, f.height/8, f.pitches[1]);

	primary = igt_output_get_plane(data->output, IGT_PLANE_PRIMARY);
	igt_plane_set_fb(primary, fb);

	if (data->flags & TEST_ROTATE_180)
		igt_plane_set_rotation(primary, IGT_ROTATION_180);
	if (data->flags & TEST_BAD_ROTATION_90)
		igt_plane_set_rotation(primary, IGT_ROTATION_90);

	ret = igt_display_try_commit2(display, commit);
	if (data->flags & TEST_BAD_ROTATION_90)
		igt_assert_eq(ret, -EINVAL);
	else
		igt_assert_eq(ret, 0);

	igt_debug_wait_for_keypress("ccs");
}

#define TEST_UNCOMPRESSED 0
#define TEST_COMPRESSED 1

static void test_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	igt_crc_t crc, ref_crc;
	igt_pipe_crc_t *pipe_crc;

	igt_output_set_pipe(data->output, data->pipe);

	if (data->flags & TEST_CRC) {
		pipe_crc = igt_pipe_crc_new(data->pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

		display_fb(data, TEST_COMPRESSED);
		igt_pipe_crc_collect_crc(pipe_crc, &ref_crc);

		display_fb(data, TEST_UNCOMPRESSED);
		igt_pipe_crc_collect_crc(pipe_crc, &crc);

		igt_assert_crc_equal(&crc, &ref_crc);

		igt_pipe_crc_free(pipe_crc);
		pipe_crc = NULL;
	}

	if (data->flags & TEST_BAD_PIXEL_FORMAT ||
	    data->flags & TEST_BAD_ROTATION_90) {
		display_fb(data, TEST_COMPRESSED);
	}

	primary = igt_output_get_plane(data->output, IGT_PLANE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_plane_set_rotation(primary, IGT_ROTATION_0);
	if (!display->is_atomic)
		igt_display_commit2(display, COMMIT_UNIVERSAL);

	igt_output_set_pipe(data->output, PIPE_ANY);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	if (data->flags & TEST_CRC)
		igt_remove_fb(data->drm_fd, &data->fb);
}

static void test(data_t *data)
{
	igt_display_t *display = &data->display;
	int valid_tests = 0;
	enum pipe wanted_pipe = data->pipe;

	for_each_pipe_with_valid_output(display, data->pipe, data->output) {
		if (wanted_pipe != PIPE_NONE && data->pipe != wanted_pipe)
			continue;

		test_output(data);

		valid_tests++;

		igt_info("\n%s on pipe %s, connector %s: PASSED\n\n",
			 igt_subtest_name(),
			 kmstest_pipe_name(data->pipe),
			 igt_output_name(data->output));
	}

	igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
}

static data_t data;

igt_main
{
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		igt_require(intel_gen(intel_get_drm_devid(data.drm_fd)) >= 9);
		kmstest_set_vt_graphics_mode();
		igt_require_pipe_crc();

		igt_display_init(&data.display, data.drm_fd);
	}

	igt_subtest_f("bad-pixel-format") {
		data.flags = TEST_BAD_PIXEL_FORMAT;
		data.pipe = PIPE_NONE;
		test(&data);
	}

	igt_subtest_f("bad-rotation-90") {
		data.flags = TEST_BAD_ROTATION_90;
		data.pipe = PIPE_NONE;
		test(&data);
	}

	for (data.pipe = PIPE_A; data.pipe < I915_MAX_PIPES; data.pipe++) {
		data.flags = TEST_CRC;
		igt_subtest_f("pipe-%s-crc-basic", kmstest_pipe_name(data.pipe))
			test(&data);
	}

	for (data.pipe = PIPE_A; data.pipe < I915_MAX_PIPES; data.pipe++) {
		data.flags = TEST_CRC | TEST_ROTATE_180;
		igt_subtest_f("pipe-%s-crc-rotation-180", kmstest_pipe_name(data.pipe))
			test(&data);
	}

	igt_fixture
		igt_display_fini(&data.display);
}
