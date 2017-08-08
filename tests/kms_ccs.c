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

enum test_fb_flags {
	FB_COMPRESSED			= 1 << 0,
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

#define CCS_UNCOMPRESSED	0x0
#define CCS_COMPRESSED		0x55

#define RED			0x00ff0000

static void render_fb(data_t *data, uint32_t gem_handle, unsigned int size,
		      enum test_fb_flags fb_flags,
		      int height, unsigned int stride)
{
	uint32_t *ptr;
	unsigned int half_height, half_size;
	int i;

	ptr = gem_mmap__cpu(data->drm_fd, gem_handle, 0, size,
			    PROT_READ | PROT_WRITE);

	if (fb_flags & FB_COMPRESSED) {
		/* In the compressed case, we want the top half of the
		 * surface to be uncompressed and the bottom half to be
		 * compressed.
		 *
		 * We need to cut the surface on a CCS cache-line boundary,
		 * otherwise, we're going to be in trouble when we try to
		 * generate CCS data for the surface.  A cache line in the
		 * CCS is 16x16 cache-line-pairs in the main surface.  16
		 * cache lines is 64 rows high.
		 */
		half_height = ALIGN(height, 128) / 2;
		half_size = half_height * stride;
		for (i = 0; i < size / 4; i++) {
			if (i < half_size / 4)
				ptr[i] = RED;
			else
				ptr[i] = COMPRESSED_RED;
		}
	} else {
		for (i = 0; i < size / 4; i++)
			ptr[i] = RED;
	}

	munmap(ptr, size);
}

static unsigned int
y_tile_y_pos(unsigned int offset, unsigned int stride)
{
	unsigned int y_tiles, y;
	y_tiles = (offset / 4096) / (stride / 128);
	y = y_tiles * 32 + ((offset & 0x1f0) >> 4);
	return y;
}

static void render_ccs(data_t *data, uint32_t gem_handle,
		       uint32_t offset, uint32_t size,
		       int height, unsigned int ccs_stride)
{
	unsigned int half_height, ccs_half_height;
	uint8_t *ptr;
	int i;

	half_height = ALIGN(height, 128) / 2;
	ccs_half_height = half_height / 16;

	ptr = gem_mmap__cpu(data->drm_fd, gem_handle, offset, size,
			    PROT_READ | PROT_WRITE);

	for (i = 0; i < size; i++) {
		if (y_tile_y_pos(i, ccs_stride) < ccs_half_height)
			ptr[i] = CCS_UNCOMPRESSED;
		else
			ptr[i] = CCS_COMPRESSED;
	}

	munmap(ptr, size);
}

static void generate_fb(data_t *data, struct igt_fb *fb,
			int width, int height,
			enum test_fb_flags fb_flags)
{
	struct local_drm_mode_fb_cmd2 f = {};
	unsigned int size[2];
	uint64_t modifier;
	int ret;

	if (fb_flags & FB_COMPRESSED)
		modifier = LOCAL_I915_FORMAT_MOD_Y_TILED_CCS;
	else
		modifier = LOCAL_I915_FORMAT_MOD_Y_TILED;

	f.flags = LOCAL_DRM_MODE_FB_MODIFIERS;
	f.width = ALIGN(width, 16);
	f.height = ALIGN(height, 8);

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

	if (fb_flags & FB_COMPRESSED) {
		/* From the Sky Lake PRM, Vol 12, "Color Control Surface":
		 *
		 *    "The compression state of the cache-line pair is
		 *    specified by 2 bits in the CCS.  Each CCS cache-line
		 *    represents an area on the main surface of 16x16 sets
		 *    of 128 byte Y-tiled cache-line-pairs. CCS is always Y
		 *    tiled."
		 *
		 * A "cache-line-pair" for a Y-tiled surface is two
		 * horizontally adjacent cache lines.  When operating in
		 * bytes and rows, this gives us a scale-down factor of
		 * 32x16.  Since the main surface has a 32-bit format, we
		 * need to multiply width by 4 to get bytes.
		 */
		width = ALIGN(f.width * 4, 32) / 32;
		height = ALIGN(f.height, 16) / 16;
		f.pitches[1] = ALIGN(width * 1, 128);
		f.modifier[1] = modifier;
		f.offsets[1] = size[0];
		size[1] = f.pitches[1] * ALIGN(height, 32);

		f.handles[0] = gem_create(data->drm_fd, size[0] + size[1]);
		f.handles[1] = f.handles[0];
		render_ccs(data, f.handles[1], f.offsets[1], size[1],
			   height, f.pitches[1]);
	} else
		f.handles[0] = gem_create(data->drm_fd, size[0]);

	render_fb(data, f.handles[0], size[0], fb_flags, height, f.pitches[0]);

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
}

static void try_config(data_t *data, enum test_fb_flags fb_flags)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	drmModeModeInfo *drm_mode = igt_output_get_mode(data->output);
	enum igt_commit_style commit;
	int ret;

	if (data->display.is_atomic)
		commit = COMMIT_ATOMIC;
	else
		commit = COMMIT_UNIVERSAL;

	generate_fb(data, &data->fb, drm_mode->hdisplay, drm_mode->vdisplay,
		    fb_flags);
	if (data->flags & TEST_BAD_PIXEL_FORMAT)
		return;

	primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_position(primary, 0, 0);
	igt_plane_set_size(primary, drm_mode->hdisplay, drm_mode->vdisplay);
	igt_plane_set_fb(primary, &data->fb);

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

static void test_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	igt_crc_t crc, ref_crc;
	igt_pipe_crc_t *pipe_crc;
	enum test_fb_flags fb_flags = 0;

	igt_output_set_pipe(data->output, data->pipe);

	if (data->flags & TEST_CRC) {
		pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

		try_config(data, fb_flags | FB_COMPRESSED);
		igt_pipe_crc_collect_crc(pipe_crc, &ref_crc);

		try_config(data, fb_flags);
		igt_pipe_crc_collect_crc(pipe_crc, &crc);

		igt_assert_crc_equal(&crc, &ref_crc);

		igt_pipe_crc_free(pipe_crc);
		pipe_crc = NULL;
	}

	if (data->flags & TEST_BAD_PIXEL_FORMAT ||
	    data->flags & TEST_BAD_ROTATION_90) {
		try_config(data, fb_flags | FB_COMPRESSED);
	}

	primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
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

	igt_skip_on(wanted_pipe >= display->n_pipes);

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
		igt_require_pipe_crc(data.drm_fd);

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

	for (data.pipe = PIPE_A; data.pipe < IGT_MAX_PIPES; data.pipe++) {
		data.flags = TEST_CRC;
		igt_subtest_f("pipe-%s-crc-basic", kmstest_pipe_name(data.pipe))
			test(&data);
	}

	for (data.pipe = PIPE_A; data.pipe < IGT_MAX_PIPES; data.pipe++) {
		data.flags = TEST_CRC | TEST_ROTATE_180;
		igt_subtest_f("pipe-%s-crc-rotation-180", kmstest_pipe_name(data.pipe))
			test(&data);
	}

	igt_fixture
		igt_display_fini(&data.display);
}
