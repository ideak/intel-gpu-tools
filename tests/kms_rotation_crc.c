/*
 * Copyright © 2013,2014 Intel Corporation
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
#include <math.h>

#define MAX_FENCES 32

typedef struct {
	int gfx_fd;
	igt_display_t display;
	struct igt_fb fb;
	struct igt_fb fb_reference;
	struct igt_fb fb_modeset;
	struct igt_fb fb_flip;
	igt_crc_t ref_crc;
	igt_crc_t flip_crc;
	igt_pipe_crc_t *pipe_crc;
	igt_rotation_t rotation;
	int pos_x;
	int pos_y;
	uint32_t override_fmt;
	uint64_t override_tiling;
	bool flips;
} data_t;

static void
paint_squares(data_t *data, igt_rotation_t rotation,
	      struct igt_fb *fb, float o)
{
	cairo_t *cr;
	unsigned int w = fb->width;
	unsigned int h = fb->height;

	cr = igt_get_cairo_ctx(data->gfx_fd, fb);

	if (rotation == IGT_ROTATION_180) {
		cairo_translate(cr, w, h);
		cairo_rotate(cr, M_PI);
	}

	if (rotation == IGT_ROTATION_90) {
		/* Paint 4 squares with width == height in Green, White,
		Blue, Red Clockwise order to look like 270 degree rotated*/
		igt_paint_color(cr, 0, 0, w / 2, h / 2, 0.0, o, 0.0);
		igt_paint_color(cr, w / 2, 0, w / 2, h / 2, o, o, o);
		igt_paint_color(cr, 0, h / 2, w / 2, h / 2, o, 0.0, 0.0);
		igt_paint_color(cr, w / 2, h / 2, w / 2, h / 2, 0.0, 0.0, o);
	} else if (rotation == IGT_ROTATION_270) {
		/* Paint 4 squares with width == height in Blue, Red,
		Green, White Clockwise order to look like 90 degree rotated*/
		igt_paint_color(cr, 0, 0, w / 2, h / 2, 0.0, 0.0, o);
		igt_paint_color(cr, w / 2, 0, w / 2, h / 2, o, 0.0, 0.0);
		igt_paint_color(cr, 0, h / 2, w / 2, h / 2, o, o, o);
		igt_paint_color(cr, w / 2, h / 2, w / 2, h / 2, 0.0, o, 0.0);
	} else {
		/* Paint with 4 squares of Red, Green, White, Blue Clockwise */
		igt_paint_color(cr, 0, 0, w / 2, h / 2, o, 0.0, 0.0);
		igt_paint_color(cr, w / 2, 0, w / 2, h / 2, 0.0, o, 0.0);
		igt_paint_color(cr, 0, h / 2, w / 2, h / 2, 0.0, 0.0, o);
		igt_paint_color(cr, w / 2, h / 2, w / 2, h / 2, o, o, o);
	}

	cairo_destroy(cr);
}

static void prepare_crtc(data_t *data, igt_output_t *output, enum pipe pipe,
			 igt_plane_t *plane, enum igt_commit_style commit)
{
	drmModeModeInfo *mode;
	unsigned int w, h;
	uint64_t tiling = data->override_tiling ?: LOCAL_DRM_FORMAT_MOD_NONE;
	uint32_t pixel_format = data->override_fmt ?: DRM_FORMAT_XRGB8888;
	igt_display_t *display = &data->display;
	igt_plane_t *primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_pipe(output, pipe);
	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	/* create the pipe_crc object for this pipe */
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = igt_pipe_crc_new(data->gfx_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

	mode = igt_output_get_mode(output);

	w = mode->hdisplay;
	h = mode->vdisplay;

	igt_create_fb(data->gfx_fd, w, h, pixel_format, tiling, &data->fb_modeset);

	/*
	 * With igt_display_commit2 and COMMIT_UNIVERSAL, we call just the
	 * setplane without a modeset. So, to be able to call
	 * igt_display_commit and ultimately setcrtc to do the first modeset,
	 * we create an fb covering the crtc and call commit
	 *
	 * It's also a good idea to set a primary fb on the primary plane
	 * regardless, to force a underrun when watermarks are allocated
	 * incorrectly for other planes.
	 */
	igt_plane_set_fb(primary, &data->fb_modeset);

	if (commit < COMMIT_ATOMIC) {
		primary->rotation_changed = false;
		igt_display_commit(display);

		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			primary->rotation_changed = true;
	}

	igt_plane_set_fb(plane, NULL);

	igt_display_commit2(display, commit);
}

static void remove_fbs(data_t *data)
{
	if (!data->fb.fb_id)
		return;

	igt_remove_fb(data->gfx_fd, &data->fb);
	igt_remove_fb(data->gfx_fd, &data->fb_reference);

	if (data->fb_flip.fb_id)
		igt_remove_fb(data->gfx_fd, &data->fb_flip);

	data->fb_flip.fb_id = data->fb.fb_id = 0;
}

enum rectangle_type {
	rectangle,
	square,
	portrait,
	landscape,
	num_rectangle_types /* must be last */
};

static void prepare_fbs(data_t *data, igt_output_t *output,
			igt_plane_t *plane, enum rectangle_type rect)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	unsigned int w, h, ref_w, ref_h, min_w, min_h;
	uint64_t tiling = data->override_tiling ?: LOCAL_DRM_FORMAT_MOD_NONE;
	uint32_t pixel_format = data->override_fmt ?: DRM_FORMAT_XRGB8888;
	const float flip_opacity = 0.75;

	if (data->fb.fb_id) {
		igt_plane_set_fb(plane, NULL);
		igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_UNIVERSAL);

		remove_fbs(data);
	}

	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	mode = igt_output_get_mode(output);
	if (plane->type != DRM_PLANE_TYPE_CURSOR) {
		w = mode->hdisplay;
		h = mode->vdisplay;

		min_w = 256;
		min_h = 256;
	} else {
		pixel_format = data->override_fmt ?: DRM_FORMAT_ARGB8888;

		w = h = 256;
		min_w = min_h = 64;
	}

	switch (rect) {
	case rectangle:
		break;
	case square:
		w = h = min(h, w);
		break;
	case portrait:
		w = min_w;
		break;
	case landscape:
		h = min_h;
		break;
	case num_rectangle_types:
		igt_assert(0);
	}

	ref_w = w;
	ref_h = h;

	/*
	 * For 90/270, we will use create smaller fb so that the rotated
	 * frame can fit in
	 */
	if (data->rotation == IGT_ROTATION_90 ||
	    data->rotation == IGT_ROTATION_270) {
		tiling = data->override_tiling ?: LOCAL_I915_FORMAT_MOD_Y_TILED;

		igt_swap(w, h);
	}

	igt_create_fb(data->gfx_fd, w, h, pixel_format, tiling, &data->fb);

	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	/*
	 * Create a reference software rotated flip framebuffer.
	 */
	if (data->flips) {
		igt_create_fb(data->gfx_fd, ref_w, ref_h, pixel_format, tiling,
			      &data->fb_flip);
		paint_squares(data, data->rotation, &data->fb_flip,
			      flip_opacity);
		igt_plane_set_fb(plane, &data->fb_flip);
		if (plane->type != DRM_PLANE_TYPE_CURSOR)
			igt_plane_set_position(plane, data->pos_x, data->pos_y);
		igt_display_commit2(display,
				    display->is_atomic ?
				    COMMIT_ATOMIC : COMMIT_UNIVERSAL);
		igt_pipe_crc_collect_crc(data->pipe_crc, &data->flip_crc);
	}

	/*
	 * Create a reference CRC for a software-rotated fb.
	 */
	igt_create_fb(data->gfx_fd, ref_w, ref_h, pixel_format,
		      data->override_tiling ?: LOCAL_DRM_FORMAT_MOD_NONE, &data->fb_reference);
	paint_squares(data, data->rotation, &data->fb_reference, 1.0);

	igt_plane_set_fb(plane, &data->fb_reference);
	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_position(plane, data->pos_x, data->pos_y);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_UNIVERSAL);

	igt_pipe_crc_collect_crc(data->pipe_crc, &data->ref_crc);

	/*
	 * Prepare the plane with an non-rotated fb let the hw rotate it.
	 */
	paint_squares(data, IGT_ROTATION_0, &data->fb, 1.0);
	igt_plane_set_fb(plane, &data->fb);

	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_position(plane, data->pos_x, data->pos_y);

	/*
	 * Prepare the non-rotated flip fb.
	 */
	if (data->flips) {
		igt_remove_fb(data->gfx_fd, &data->fb_flip);
		igt_create_fb(data->gfx_fd, w, h, pixel_format, tiling,
			      &data->fb_flip);
		paint_squares(data, IGT_ROTATION_0, &data->fb_flip,
			      flip_opacity);
	}

}

static void cleanup_crtc(data_t *data, igt_output_t *output, igt_plane_t *plane)
{
	igt_display_t *display = &data->display;

	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	remove_fbs(data);

	igt_remove_fb(data->gfx_fd, &data->fb_modeset);

	/* XXX: see the note in prepare_crtc() */
	if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
		igt_plane_t *primary;

		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, NULL);
	}

	igt_plane_set_fb(plane, NULL);
	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	igt_display_commit2(display, COMMIT_UNIVERSAL);

	igt_output_set_pipe(output, PIPE_ANY);

	igt_display_commit(display);
}

static void wait_for_pageflip(int fd)
{
	drmEventContext evctx = { .version = 2 };
	struct timeval timeout = { .tv_sec = 0, .tv_usec = 50000 };
	fd_set fds;
	int ret;

	/* Wait for pageflip completion, then consume event on fd */
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	do {
		ret = select(fd + 1, &fds, NULL, NULL, &timeout);
	} while (ret < 0 && errno == EINTR);
	igt_assert_eq(ret, 1);
	igt_assert(drmHandleEvent(fd, &evctx) == 0);
}

static void test_plane_rotation(data_t *data, int plane_type)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;
	int valid_tests = 0;
	igt_crc_t crc_output;
	enum igt_commit_style commit = COMMIT_LEGACY;
	int ret;

	if (plane_type == DRM_PLANE_TYPE_PRIMARY || plane_type == DRM_PLANE_TYPE_CURSOR)
		commit = COMMIT_UNIVERSAL;

	if (plane_type == DRM_PLANE_TYPE_CURSOR)
		igt_require(display->has_cursor_plane);

	if (data->display.is_atomic)
		commit = COMMIT_ATOMIC;

	for_each_pipe_with_valid_output(display, pipe, output) {
		igt_plane_t *plane;
		int i;

		igt_output_set_pipe(output, pipe);

		plane = igt_output_get_plane_type(output, plane_type);
		igt_require(igt_plane_supports_rotation(plane));

		prepare_crtc(data, output, pipe, plane, commit);

		for (i = 0; i < num_rectangle_types; i++) {
			/* Unsupported on i915 */
			if (plane_type == DRM_PLANE_TYPE_CURSOR &&
			    i != square)
				continue;

			/* Only support partial covering primary plane on gen9+ */
			if (plane_type == DRM_PLANE_TYPE_PRIMARY &&
			    i != rectangle && intel_gen(intel_get_drm_devid(data->gfx_fd)) < 9)
				continue;

			igt_debug("Testing case %i on pipe %s\n", i, kmstest_pipe_name(pipe));
			prepare_fbs(data, output, plane, i);

			igt_display_commit2(display, commit);

			igt_plane_set_rotation(plane, data->rotation);
			if (data->rotation == IGT_ROTATION_90 || data->rotation == IGT_ROTATION_270)
				igt_plane_set_size(plane, data->fb.height, data->fb.width);

			ret = igt_display_try_commit2(display, commit);
			if (data->override_fmt || data->override_tiling) {
				igt_assert_eq(ret, -EINVAL);
				continue;
			}

			/* Verify commit was ok. */
			igt_assert_eq(ret, 0);

			/* Check CRC */
			igt_pipe_crc_collect_crc(data->pipe_crc, &crc_output);
			igt_assert_crc_equal(&data->ref_crc, &crc_output);

			/*
			 * If flips are requested flip to a different fb and
			 * check CRC against that one as well.
			 */
			if (data->flips) {
				ret = drmModePageFlip(data->gfx_fd,
						      output->config.crtc->crtc_id,
						      data->fb_flip.fb_id,
						      DRM_MODE_PAGE_FLIP_EVENT,
						      NULL);
				igt_assert_eq(ret, 0);
				wait_for_pageflip(data->gfx_fd);
				igt_pipe_crc_collect_crc(data->pipe_crc,
							 &crc_output);
				igt_assert_crc_equal(&data->flip_crc,
						     &crc_output);
			}
		}

		valid_tests++;
		cleanup_crtc(data, output, plane);
	}
	igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
}

static void test_plane_rotation_ytiled_obj(data_t *data,
					   igt_output_t *output,
					   int plane_type)
{
	igt_display_t *display = &data->display;
	uint64_t tiling = LOCAL_I915_FORMAT_MOD_Y_TILED;
	uint32_t format = DRM_FORMAT_XRGB8888;
	int bpp = igt_drm_format_to_bpp(format);
	enum igt_commit_style commit = COMMIT_LEGACY;
	int fd = data->gfx_fd;
	igt_plane_t *plane;
	drmModeModeInfo *mode;
	unsigned int stride, size, w, h;
	uint32_t gem_handle;
	int ret;

	plane = igt_output_get_plane_type(output, plane_type);
	igt_require(igt_plane_supports_rotation(plane));

	if (plane_type == DRM_PLANE_TYPE_PRIMARY || plane_type == DRM_PLANE_TYPE_CURSOR)
		commit = COMMIT_UNIVERSAL;

	if (plane_type == DRM_PLANE_TYPE_CURSOR)
		igt_require(display->has_cursor_plane);

	if (data->display.is_atomic)
		commit = COMMIT_ATOMIC;

	mode = igt_output_get_mode(output);
	w = mode->hdisplay;
	h = mode->vdisplay;

	for (stride = 512; stride < (w * bpp / 8); stride *= 2)
		;
	for (size = 1024*1024; size < stride * h; size *= 2)
		;

	gem_handle = gem_create(fd, size);
	ret = __gem_set_tiling(fd, gem_handle, I915_TILING_Y, stride);
	igt_assert_eq(ret, 0);

	do_or_die(__kms_addfb(fd, gem_handle, w, h, stride,
		  format, tiling, LOCAL_DRM_MODE_FB_MODIFIERS,
		  &data->fb.fb_id));
	data->fb.width = w;
	data->fb.height = h;
	data->fb.gem_handle = gem_handle;

	igt_plane_set_fb(plane, NULL);
	igt_display_commit(display);

	igt_plane_set_rotation(plane, data->rotation);
	igt_plane_set_fb(plane, &data->fb);
	igt_plane_set_size(plane, h, w);

	if (commit < COMMIT_ATOMIC)
		drmModeObjectSetProperty(fd, plane->drm_plane->plane_id,
					DRM_MODE_OBJECT_PLANE,
					plane->rotation_property,
					plane->rotation);

	ret = igt_display_try_commit2(display, commit);

	igt_output_set_pipe(output, PIPE_NONE);

	kmstest_restore_vt_mode();
	igt_remove_fb(fd, &data->fb);
	igt_assert_eq(ret, 0);
}

static void test_plane_rotation_exhaust_fences(data_t *data,
					       igt_output_t *output,
					       int plane_type)
{
	igt_display_t *display = &data->display;
	uint64_t tiling = LOCAL_I915_FORMAT_MOD_Y_TILED;
	uint32_t format = DRM_FORMAT_XRGB8888;
	int bpp = igt_drm_format_to_bpp(format);
	enum igt_commit_style commit = COMMIT_LEGACY;
	int fd = data->gfx_fd;
	igt_plane_t *plane;
	drmModeModeInfo *mode;
	data_t data2[MAX_FENCES+1] = {};
	unsigned int stride, size, w, h;
	uint32_t gem_handle;
	uint64_t total_aperture_size, total_fbs_size;
	int i, ret;

	plane = igt_output_get_plane_type(output, plane_type);
	igt_require(igt_plane_supports_rotation(plane));

	if (plane_type == DRM_PLANE_TYPE_PRIMARY || plane_type == DRM_PLANE_TYPE_CURSOR)
		commit = COMMIT_UNIVERSAL;

	if (plane_type == DRM_PLANE_TYPE_CURSOR)
		igt_require(display->has_cursor_plane);

	if (data->display.is_atomic)
		commit = COMMIT_ATOMIC;

	mode = igt_output_get_mode(output);
	w = mode->hdisplay;
	h = mode->vdisplay;

	for (stride = 512; stride < (w * bpp / 8); stride *= 2)
		;
	for (size = 1024*1024; size < stride * h; size *= 2)
		;

	/*
	 * Make sure there is atleast 90% of the available GTT space left
	 * for creating (MAX_FENCES+1) framebuffers.
	 */
	total_fbs_size = size * (MAX_FENCES + 1);
	total_aperture_size = gem_available_aperture_size(fd);
	igt_require(total_fbs_size < total_aperture_size * 0.9);

	igt_plane_set_fb(plane, NULL);
	igt_display_commit(display);

	for (i = 0; i < MAX_FENCES + 1; i++) {
		gem_handle = gem_create(fd, size);
		ret = __gem_set_tiling(fd, gem_handle, I915_TILING_Y, stride);
		if (ret) {
			igt_warn("failed to set tiling\n");
			goto err_alloc;
		}

		ret = (__kms_addfb(fd, gem_handle, w, h, stride,
		       format, tiling, LOCAL_DRM_MODE_FB_MODIFIERS,
		       &data2[i].fb.fb_id));
		if (ret) {
			igt_warn("failed to create framebuffer\n");
			goto err_alloc;
		}

		data2[i].fb.width = w;
		data2[i].fb.height = h;
		data2[i].fb.gem_handle = gem_handle;

		igt_plane_set_fb(plane, &data2[i].fb);
		igt_plane_set_rotation(plane, IGT_ROTATION_0);

		ret = igt_display_try_commit2(display, commit);
		if (ret) {
			igt_warn("failed to commit unrotated fb\n");
			goto err_commit;
		}

		igt_plane_set_rotation(plane, IGT_ROTATION_90);
		igt_plane_set_size(plane, h, w);

		drmModeObjectSetProperty(fd, plane->drm_plane->plane_id,
					 DRM_MODE_OBJECT_PLANE,
					 plane->rotation_property,
					 plane->rotation);
		igt_display_commit2(display, commit);
		if (ret) {
			igt_warn("failed to commit hardware rotated fb: %i\n", ret);
			goto err_commit;
		}
	}

err_alloc:
	if (ret)
		gem_close(fd, gem_handle);

	i--;
err_commit:
	igt_plane_set_fb(plane, NULL);
	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	if (commit < COMMIT_ATOMIC)
		igt_display_commit2(display, commit);

	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	for (; i >= 0; i--)
		igt_remove_fb(fd, &data2[i].fb);

	kmstest_restore_vt_mode();
	igt_assert_eq(ret, 0);
}

static const char *plane_test_str(unsigned plane)
{
	switch (plane) {
	case DRM_PLANE_TYPE_PRIMARY:
		return "primary";
	case DRM_PLANE_TYPE_OVERLAY:
		return "sprite";
	case DRM_PLANE_TYPE_CURSOR:
		return "cursor";
	default:
		igt_assert(0);
	}
}

static const char *rot_test_str(igt_rotation_t rot)
{
	switch (rot) {
	case IGT_ROTATION_90:
		return "90";
	case IGT_ROTATION_180:
		return "180";
	case IGT_ROTATION_270:
		return "270";
	default:
		igt_assert(0);
	}
}

static const char *flip_test_str(unsigned flips)
{
	if (flips)
		return "-flip";
	else
		return "";
}

igt_main
{
	struct rot_subtest {
		unsigned plane;
		igt_rotation_t rot;
		unsigned flips;
	} *subtest, subtests[] = {
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_90, 0 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_180, 0 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_270, 0 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_90, 1 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_180, 1 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_270, 1 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_90, 0 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_180, 0 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_270, 0 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_90, 1 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_180, 1 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_270, 1 },
		{ DRM_PLANE_TYPE_CURSOR, IGT_ROTATION_180, 0 },
		{ 0, 0, 0}
	};
	data_t data = {};
	int gen = 0;

	igt_skip_on_simulation();

	igt_fixture {
		data.gfx_fd = drm_open_driver_master(DRIVER_INTEL);
		gen = intel_gen(intel_get_drm_devid(data.gfx_fd));

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.gfx_fd);

		igt_display_init(&data.display, data.gfx_fd);
	}

	for (subtest = subtests; subtest->rot; subtest++) {
		igt_subtest_f("%s-rotation-%s%s",
			      plane_test_str(subtest->plane),
			      rot_test_str(subtest->rot),
			      flip_test_str(subtest->flips)) {
			igt_require(!(subtest->rot &
				    (IGT_ROTATION_90 | IGT_ROTATION_270)) ||
				    gen >= 9);
			data.rotation = subtest->rot;
			data.flips = subtest->flips;
			test_plane_rotation(&data, subtest->plane);
		}
	}

	igt_subtest_f("sprite-rotation-90-pos-100-0") {
		igt_require(gen >= 9);
		data.rotation = IGT_ROTATION_90;
		data.pos_x = 100,
		data.pos_y = 0;
		test_plane_rotation(&data, DRM_PLANE_TYPE_OVERLAY);
	}

	igt_subtest_f("bad-pixel-format") {
		igt_require(gen >= 9);
		data.pos_x = 0,
		data.pos_y = 0;
		data.rotation = IGT_ROTATION_90;
		data.override_fmt = DRM_FORMAT_RGB565;
		test_plane_rotation(&data, DRM_PLANE_TYPE_PRIMARY);
	}

	igt_subtest_f("bad-tiling") {
		igt_require(gen >= 9);
		data.override_fmt = 0;
		data.rotation = IGT_ROTATION_90;
		data.override_tiling = LOCAL_DRM_FORMAT_MOD_NONE;
		test_plane_rotation(&data, DRM_PLANE_TYPE_PRIMARY);
	}

	igt_subtest_f("primary-rotation-90-Y-tiled") {
		enum pipe pipe;
		igt_output_t *output;
		int valid_tests = 0;

		igt_require(gen >= 9);
		data.rotation = IGT_ROTATION_90;

		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			igt_output_set_pipe(output, pipe);

			test_plane_rotation_ytiled_obj(&data, output, DRM_PLANE_TYPE_PRIMARY);

			valid_tests++;
			break;
		}

		igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
	}

	igt_subtest_f("exhaust-fences") {
		enum pipe pipe;
		igt_output_t *output;
		int valid_tests = 0;

		igt_require(gen >= 9);

		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			igt_output_set_pipe(output, pipe);

			test_plane_rotation_exhaust_fences(&data, output, DRM_PLANE_TYPE_PRIMARY);

			valid_tests++;
			break;
		}

		igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
