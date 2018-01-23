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
	TEST_NO_AUX_BUFFER		= 1 << 5,
	TEST_BAD_CCS_HANDLE		= 1 << 6,
	TEST_BAD_AUX_STRIDE		= 1 << 7,
};

#define TEST_FAIL_ON_ADDFB2 \
	(TEST_BAD_PIXEL_FORMAT | TEST_NO_AUX_BUFFER | TEST_BAD_CCS_HANDLE | \
	 TEST_BAD_AUX_STRIDE)

enum test_fb_flags {
	FB_COMPRESSED			= 1 << 0,
	FB_HAS_PLANE			= 1 << 1,
	FB_MISALIGN_AUX_STRIDE		= 1 << 2,
	FB_SMALL_AUX_STRIDE		= 1 << 3,
	FB_ZERO_AUX_STRIDE		= 1 << 4,
};

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb;
	struct igt_fb fb_sprite;
	igt_output_t *output;
	enum pipe pipe;
	enum test_flags flags;
	igt_plane_t *plane;
	igt_pipe_crc_t *pipe_crc;
} data_t;

#define RED			0x00ff0000
#define COMPRESSED_RED		0x0ff0000f
#define GREEN			0x0000ff00
#define COMPRESSED_GREEN	0x000ff00f

#define CCS_UNCOMPRESSED	0x0
#define CCS_COMPRESSED		0x55

struct local_drm_format_modifier {
       /* Bitmask of formats in get_plane format list this info applies to. The
	* offset allows a sliding window of which 64 formats (bits).
	*
	* Some examples:
	* In today's world with < 65 formats, and formats 0, and 2 are
	* supported
	* 0x0000000000000005
	*		  ^-offset = 0, formats = 5
	*
	* If the number formats grew to 128, and formats 98-102 are
	* supported with the modifier:
	*
	* 0x0000003c00000000 0000000000000000
	*		  ^
	*		  |__offset = 64, formats = 0x3c00000000
	*
	*/
       uint64_t formats;
       uint32_t offset;
       uint32_t pad;

       /* This modifier can be used with the format for this plane. */
       uint64_t modifier;
};

struct local_drm_format_modifier_blob {
#define LOCAL_FORMAT_BLOB_CURRENT 1
	/* Version of this blob format */
	uint32_t version;

	/* Flags */
	uint32_t flags;

	/* Number of fourcc formats supported */
	uint32_t count_formats;

	/* Where in this blob the formats exist (in bytes) */
	uint32_t formats_offset;

	/* Number of drm_format_modifiers */
	uint32_t count_modifiers;

	/* Where in this blob the modifiers exist (in bytes) */
	uint32_t modifiers_offset;

	/* u32 formats[] */
	/* struct drm_format_modifier modifiers[] */
};

static inline uint32_t *
formats_ptr(struct local_drm_format_modifier_blob *blob)
{
	return (uint32_t *)(((char *)blob) + blob->formats_offset);
}

static inline struct local_drm_format_modifier *
modifiers_ptr(struct local_drm_format_modifier_blob *blob)
{
	return (struct local_drm_format_modifier *)(((char *)blob) + blob->modifiers_offset);
}

static void plane_require_ccs(data_t *data, igt_plane_t *plane, uint32_t format)
{
	drmModePropertyBlobPtr blob;
	struct local_drm_format_modifier_blob *blob_data;
	struct local_drm_format_modifier *modifiers, *last_mod;
	uint32_t *formats, *last_fmt;
	uint64_t blob_id;
	bool ret;
	int fmt_idx = -1;

	ret = kmstest_get_property(data->drm_fd, plane->drm_plane->plane_id,
				   DRM_MODE_OBJECT_PLANE, "IN_FORMATS",
				   NULL, &blob_id, NULL);
	igt_skip_on_f(ret == false, "IN_FORMATS not supported by kernel\n");
	igt_skip_on_f(blob_id == 0, "IN_FORMATS not supported by plane\n");
	blob = drmModeGetPropertyBlob(data->drm_fd, blob_id);
	igt_assert(blob);
	igt_assert_lte(sizeof(struct local_drm_format_modifier_blob),
		       blob->length);

	blob_data = (struct local_drm_format_modifier_blob *) blob->data;
	formats = formats_ptr(blob_data);
	last_fmt = &formats[blob_data->count_formats];
	igt_assert_lte(((char *) last_fmt - (char *) blob_data), blob->length);
	for (int i = 0; i < blob_data->count_formats; i++) {
		if (formats[i] == format) {
			fmt_idx = i;
			break;
		}
	}

	igt_skip_on_f(fmt_idx == -1,
		      "Format 0x%x not supported by plane\n", format);

	modifiers = modifiers_ptr(blob_data);
	last_mod = &modifiers[blob_data->count_modifiers];
	igt_assert_lte(((char *) last_mod - (char *) blob_data), blob->length);
	for (int i = 0; i < blob_data->count_modifiers; i++) {
		if (modifiers[i].modifier != LOCAL_I915_FORMAT_MOD_Y_TILED_CCS)
			continue;

		if (modifiers[i].offset > fmt_idx ||
		    fmt_idx > modifiers[i].offset + 63)
			continue;

		if (modifiers[i].formats &
		    (1UL << (fmt_idx - modifiers[i].offset)))
			return;

		igt_skip("i915 CCS modifier not supported for format\n");
	}

	igt_skip("i915 CCS modifier not supported by kernel for plane\n");
}

static void render_fb(data_t *data, uint32_t gem_handle, unsigned int size,
		      enum test_fb_flags fb_flags,
		      int height, unsigned int stride)
{
	uint32_t *ptr;
	unsigned int half_height, half_size;
	uint32_t uncompressed_color = data->plane ? GREEN : RED;
	uint32_t compressed_color =
		data->plane ? COMPRESSED_GREEN : COMPRESSED_RED;
	uint32_t bad_color = RED;
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
				ptr[i] = uncompressed_color;
			else
				ptr[i] = compressed_color;
		}
	} else {
		/* When we're displaying the primary plane underneath a
		 * sprite plane, cut out a 128 x 128 area (less than the sprite)
		 * plane size which we paint red, so we know easily if it's
		 * bad.
		 */
		for (i = 0; i < size / 4; i++) {
			if ((fb_flags & FB_HAS_PLANE) &&
			    (i / (stride / 4)) < 128 &&
			    (i % (stride / 4)) < 128) {
				ptr[i] = bad_color;
			} else {
				ptr[i] = uncompressed_color;
			}
		}
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
	uint32_t ccs_handle;

	/* Use either compressed or Y-tiled to test. However, given the lack of
	 * available bandwidth, we use linear for the primary plane when
	 * testing sprites, since we cannot fit two CCS planes into the
	 * available FIFO configurations.
	 */
	if (fb_flags & FB_COMPRESSED)
		modifier = LOCAL_I915_FORMAT_MOD_Y_TILED_CCS;
	else if (!(fb_flags & FB_HAS_PLANE))
		modifier = LOCAL_I915_FORMAT_MOD_Y_TILED;
	else
		modifier = 0;

	f.flags = LOCAL_DRM_MODE_FB_MODIFIERS;
	f.width = width;
	f.height = height;

	if (data->flags & TEST_BAD_PIXEL_FORMAT)
		f.pixel_format = DRM_FORMAT_RGB565;
	else
		f.pixel_format = DRM_FORMAT_XRGB8888;

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
		int ccs_width = ALIGN(width * 4, 32) / 32;
		int ccs_height = ALIGN(height, 16) / 16;
		int ccs_pitches = ALIGN(ccs_width * 1, 128);
		int ccs_offsets = size[0];

		if (fb_flags & FB_MISALIGN_AUX_STRIDE) {
			igt_skip_on_f(width <= 1024,
				      "FB already has the smallest possible stride\n");
			ccs_pitches -= 64;
		}
		else if (fb_flags & FB_SMALL_AUX_STRIDE) {
			igt_skip_on_f(width <= 1024,
				      "FB already has the smallest possible stride\n");
			ccs_pitches = ALIGN(ccs_width/2, 128);
		}

		size[1] = ccs_pitches * ALIGN(ccs_height, 32);

		f.handles[0] = gem_create(data->drm_fd, size[0] + size[1]);
		if (data->flags & TEST_BAD_CCS_HANDLE) {
			/* Put the CCS buffer on a different BO. */
			ccs_handle = gem_create(data->drm_fd, size[0] + size[1]);
		} else
			ccs_handle = f.handles[0];

		if (!(data->flags & TEST_NO_AUX_BUFFER)) {
			f.modifier[1] = modifier;
			f.handles[1] = ccs_handle;
			f.offsets[1] = ccs_offsets;
			f.pitches[1] = (fb_flags & FB_ZERO_AUX_STRIDE)? 0:ccs_pitches;

			render_ccs(data, f.handles[1], f.offsets[1], size[1],
				   height, ccs_pitches);
		}
	} else {
		f.handles[0] = gem_create(data->drm_fd, size[0]);
	}

	render_fb(data, f.handles[0], size[0], fb_flags, height, f.pitches[0]);

	ret = drmIoctl(data->drm_fd, LOCAL_DRM_IOCTL_MODE_ADDFB2, &f);
	if (data->flags & TEST_FAIL_ON_ADDFB2) {
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

static void try_config(data_t *data, enum test_fb_flags fb_flags,
		       igt_crc_t *crc)
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

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	plane_require_ccs(data, primary, DRM_FORMAT_XRGB8888);

	if (data->plane && fb_flags & FB_COMPRESSED) {
		plane_require_ccs(data, data->plane, DRM_FORMAT_XRGB8888);
		generate_fb(data, &data->fb, drm_mode->hdisplay,
			    drm_mode->vdisplay,
			    (fb_flags & ~FB_COMPRESSED) | FB_HAS_PLANE);
		generate_fb(data, &data->fb_sprite, 256, 256, fb_flags);
	} else {
		generate_fb(data, &data->fb, drm_mode->hdisplay,
			    drm_mode->vdisplay, fb_flags);
	}

	if (data->flags & TEST_FAIL_ON_ADDFB2)
		return;

	igt_plane_set_position(primary, 0, 0);
	igt_plane_set_size(primary, drm_mode->hdisplay, drm_mode->vdisplay);
	igt_plane_set_fb(primary, &data->fb);

	if (data->plane && fb_flags & FB_COMPRESSED) {
		igt_plane_set_position(data->plane, 0, 0);
		igt_plane_set_size(data->plane, 256, 256);
		igt_plane_set_fb(data->plane, &data->fb_sprite);
	}

	if (data->flags & TEST_ROTATE_180)
		igt_plane_set_rotation(primary, IGT_ROTATION_180);
	if (data->flags & TEST_BAD_ROTATION_90)
		igt_plane_set_rotation(primary, IGT_ROTATION_90);

	ret = igt_display_try_commit2(display, commit);
	if (data->flags & TEST_BAD_ROTATION_90) {
		igt_assert_eq(ret, -EINVAL);
	} else {
		igt_assert_eq(ret, 0);

		if (crc)
			igt_pipe_crc_collect_crc(data->pipe_crc, crc);
	}

	igt_debug_wait_for_keypress("ccs");

	if (data->plane && fb_flags & FB_COMPRESSED) {
		igt_plane_set_position(data->plane, 0, 0);
		igt_plane_set_size(data->plane, 0, 0);
		igt_plane_set_fb(data->plane, NULL);
		igt_remove_fb(display->drm_fd, &data->fb_sprite);
	}
}

static void test_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	igt_crc_t crc, ref_crc;
	enum test_fb_flags fb_flags = 0;

	igt_display_require_output_on_pipe(display, data->pipe);

	/* Sets data->output with a valid output. */
	for_each_valid_output_on_pipe(display, data->pipe, data->output) {
		break;
	}

	igt_output_set_pipe(data->output, data->pipe);

	if (data->flags & TEST_CRC) {
		data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

		try_config(data, fb_flags | FB_COMPRESSED, &ref_crc);
		try_config(data, fb_flags, &crc);

		igt_assert_crc_equal(&crc, &ref_crc);

		igt_pipe_crc_free(data->pipe_crc);
		data->pipe_crc = NULL;
	}

	if (data->flags & TEST_BAD_PIXEL_FORMAT ||
	    data->flags & TEST_BAD_ROTATION_90 ||
	    data->flags & TEST_NO_AUX_BUFFER ||
	    data->flags & TEST_BAD_CCS_HANDLE) {
		try_config(data, fb_flags | FB_COMPRESSED, NULL);
	}

	if (data->flags & TEST_BAD_AUX_STRIDE) {
		try_config(data, fb_flags | FB_COMPRESSED | FB_MISALIGN_AUX_STRIDE , NULL);
		try_config(data, fb_flags | FB_COMPRESSED | FB_SMALL_AUX_STRIDE , NULL);
		try_config(data, fb_flags | FB_COMPRESSED | FB_ZERO_AUX_STRIDE , NULL);
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

static data_t data;

igt_main
{
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		igt_require(intel_gen(intel_get_drm_devid(data.drm_fd)) >= 9);
		kmstest_set_vt_graphics_mode();
		igt_require_pipe_crc(data.drm_fd);

		igt_display_init(&data.display, data.drm_fd);
	}

	for_each_pipe_static(pipe) {
		const char *pipe_name = kmstest_pipe_name(pipe);
		int sprite_idx = 0;

		data.pipe = pipe;

		data.flags = TEST_BAD_PIXEL_FORMAT;
		igt_subtest_f("pipe-%s-bad-pixel-format", pipe_name)
			test_output(&data);

		data.flags = TEST_BAD_ROTATION_90;
		igt_subtest_f("pipe-%s-bad-rotation-90", pipe_name)
			test_output(&data);

		data.flags = TEST_CRC;
		igt_subtest_f("pipe-%s-crc-primary-basic", pipe_name)
			test_output(&data);

		data.flags = TEST_CRC | TEST_ROTATE_180;
		igt_subtest_f("pipe-%s-crc-primary-rotation-180", pipe_name)
			test_output(&data);

		data.flags = TEST_CRC;
		igt_subtest_f("pipe-%s-crc-sprite-planes-basic", pipe_name) {

			igt_display_require_output_on_pipe(&data.display, data.pipe);

			for_each_plane_on_pipe(&data.display, data.pipe, data.plane) {
				if (data.plane->type == DRM_PLANE_TYPE_PRIMARY)
					continue;
				sprite_idx++;
					test_output(&data);
			}
		}

		data.plane = NULL;

		data.flags = TEST_NO_AUX_BUFFER;
		igt_subtest_f("pipe-%s-missing-ccs-buffer", pipe_name)
			test_output(&data);

		data.flags = TEST_BAD_CCS_HANDLE;
		igt_subtest_f("pipe-%s-ccs-on-another-bo", pipe_name)
			test_output(&data);

		data.flags = TEST_BAD_AUX_STRIDE;
		igt_subtest_f("pipe-%s-bad-aux-stride", pipe_name)
			test_output(&data);
	}

	igt_fixture
		igt_display_fini(&data.display);
}
