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
 * Authors:
 * 	Daniel Vetter <daniel.vetter@ffwll.ch>
 * 	Damien Lespiau <damien.lespiau@intel.com>
 */

#include <stdio.h>
#include <math.h>
#include <wchar.h>
#include <inttypes.h>
#include <pixman.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_color_encoding.h"
#include "igt_fb.h"
#include "igt_kms.h"
#include "igt_matrix.h"
#include "igt_x86.h"
#include "ioctl_wrappers.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"

/**
 * SECTION:igt_fb
 * @short_description: Framebuffer handling and drawing library
 * @title: Framebuffer
 * @include: igt.h
 *
 * This library contains helper functions for handling kms framebuffer objects
 * using #igt_fb structures to track all the metadata.  igt_create_fb() creates
 * a basic framebuffer and igt_remove_fb() cleans everything up again.
 *
 * It also supports drawing using the cairo library and provides some simplified
 * helper functions to easily draw test patterns. The main function to create a
 * cairo drawing context for a framebuffer object is igt_get_cairo_ctx().
 *
 * Finally it also pulls in the drm fourcc headers and provides some helper
 * functions to work with these pixel format codes.
 */

#define PIXMAN_invalid	0

/* drm fourcc/cairo format maps */
static const struct format_desc_struct {
	const char *name;
	uint32_t drm_id;
	cairo_format_t cairo_id;
	pixman_format_code_t pixman_id;
	int depth;
	int num_planes;
	int plane_bpp[4];
} format_desc[] = {
	{ .name = "ARGB1555", .depth = -1, .drm_id = DRM_FORMAT_ARGB1555,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_a1r5g5b5,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "XRGB1555", .depth = -1, .drm_id = DRM_FORMAT_XRGB1555,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_x1r5g5b5,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "RGB565", .depth = 16, .drm_id = DRM_FORMAT_RGB565,
	  .cairo_id = CAIRO_FORMAT_RGB16_565,
	  .pixman_id = PIXMAN_r5g6b5,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "BGR565", .depth = -1, .drm_id = DRM_FORMAT_BGR565,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_b5g6r5,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "BGR888", .depth = -1, .drm_id = DRM_FORMAT_BGR888,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_b8g8r8,
	  .num_planes = 1, .plane_bpp = { 24, },
	},
	{ .name = "RGB888", .depth = -1, .drm_id = DRM_FORMAT_RGB888,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_r8g8b8,
	  .num_planes = 1, .plane_bpp = { 24, },
	},
	{ .name = "XYUV8888", .depth = -1, .drm_id = DRM_FORMAT_XYUV8888,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .num_planes = 1, .plane_bpp = { 32, },
	},
	{ .name = "XRGB8888", .depth = 24, .drm_id = DRM_FORMAT_XRGB8888,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .pixman_id = PIXMAN_x8r8g8b8,
	  .num_planes = 1, .plane_bpp = { 32, },
	},
	{ .name = "XBGR8888", .depth = -1, .drm_id = DRM_FORMAT_XBGR8888,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_x8b8g8r8,
	  .num_planes = 1, .plane_bpp = { 32, },
	},
	{ .name = "XRGB2101010", .depth = 30, .drm_id = DRM_FORMAT_XRGB2101010,
	  .cairo_id = CAIRO_FORMAT_RGB30,
	  .pixman_id = PIXMAN_x2r10g10b10,
	  .num_planes = 1, .plane_bpp = { 32, },
	},
	{ .name = "ARGB8888", .depth = 32, .drm_id = DRM_FORMAT_ARGB8888,
	  .cairo_id = CAIRO_FORMAT_ARGB32,
	  .pixman_id = PIXMAN_a8r8g8b8,
	  .num_planes = 1, .plane_bpp = { 32, },
	},
	{ .name = "ABGR8888", .depth = -1, .drm_id = DRM_FORMAT_ABGR8888,
	  .cairo_id = CAIRO_FORMAT_INVALID,
	  .pixman_id = PIXMAN_a8b8g8r8,
	  .num_planes = 1, .plane_bpp = { 32, },
	},
	{ .name = "NV12", .depth = -1, .drm_id = DRM_FORMAT_NV12,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .num_planes = 2, .plane_bpp = { 8, 16, },
	},
	{ .name = "YUYV", .depth = -1, .drm_id = DRM_FORMAT_YUYV,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "YVYU", .depth = -1, .drm_id = DRM_FORMAT_YVYU,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "UYVY", .depth = -1, .drm_id = DRM_FORMAT_UYVY,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
	{ .name = "VYUY", .depth = -1, .drm_id = DRM_FORMAT_VYUY,
	  .cairo_id = CAIRO_FORMAT_RGB24,
	  .num_planes = 1, .plane_bpp = { 16, },
	},
};
#define for_each_format(f)	\
	for (f = format_desc; f - format_desc < ARRAY_SIZE(format_desc); f++)

static const struct format_desc_struct *lookup_drm_format(uint32_t drm_format)
{
	const struct format_desc_struct *format;

	for_each_format(format) {
		if (format->drm_id != drm_format)
			continue;

		return format;
	}

	return NULL;
}

/**
 * igt_get_fb_tile_size:
 * @fd: the DRM file descriptor
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @fb_bpp: bits per pixel of the framebuffer
 * @width_ret: width of the tile in bytes
 * @height_ret: height of the tile in lines
 *
 * This function returns width and height of a tile based on the given tiling
 * format.
 */
void igt_get_fb_tile_size(int fd, uint64_t tiling, int fb_bpp,
			  unsigned *width_ret, unsigned *height_ret)
{
	switch (tiling) {
	case LOCAL_DRM_FORMAT_MOD_NONE:
		*width_ret = 64;
		*height_ret = 1;
		break;
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		igt_require_intel(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 2) {
			*width_ret = 128;
			*height_ret = 16;
		} else {
			*width_ret = 512;
			*height_ret = 8;
		}
		break;
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		igt_require_intel(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 2) {
			*width_ret = 128;
			*height_ret = 16;
		} else if (IS_915(intel_get_drm_devid(fd))) {
			*width_ret = 512;
			*height_ret = 8;
		} else {
			*width_ret = 128;
			*height_ret = 32;
		}
		break;
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		igt_require_intel(fd);
		switch (fb_bpp) {
		case 8:
			*width_ret = 64;
			*height_ret = 64;
			break;
		case 16:
		case 32:
			*width_ret = 128;
			*height_ret = 32;
			break;
		case 64:
		case 128:
			*width_ret = 256;
			*height_ret = 16;
			break;
		default:
			igt_assert(false);
		}
		break;
	default:
		igt_assert(false);
	}
}

static unsigned fb_plane_width(const struct igt_fb *fb, int plane)
{
	if (fb->drm_format == DRM_FORMAT_NV12 && plane == 1)
		return DIV_ROUND_UP(fb->width, 2);

	return fb->width;
}

static unsigned fb_plane_bpp(const struct igt_fb *fb, int plane)
{
	const struct format_desc_struct *format = lookup_drm_format(fb->drm_format);

	return format->plane_bpp[plane];
}

static unsigned fb_plane_height(const struct igt_fb *fb, int plane)
{
	if (fb->drm_format == DRM_FORMAT_NV12 && plane == 1)
		return DIV_ROUND_UP(fb->height, 2);

	return fb->height;
}

static int fb_num_planes(const struct igt_fb *fb)
{
	const struct format_desc_struct *format = lookup_drm_format(fb->drm_format);

	return format->num_planes;
}

static void fb_init(struct igt_fb *fb,
		    int fd, int width, int height,
		    uint32_t drm_format,
		    uint64_t modifier,
		    enum igt_color_encoding color_encoding,
		    enum igt_color_range color_range)
{
	const struct format_desc_struct *f = lookup_drm_format(drm_format);

	igt_assert_f(f, "DRM format %08x not found\n", drm_format);

	memset(fb, 0, sizeof(*fb));

	fb->width = width;
	fb->height = height;
	fb->tiling = modifier;
	fb->drm_format = drm_format;
	fb->fd = fd;
	fb->num_planes = fb_num_planes(fb);
	fb->color_encoding = color_encoding;
	fb->color_range = color_range;

	for (int i = 0; i < fb->num_planes; i++) {
		fb->plane_bpp[i] = fb_plane_bpp(fb, i);
		fb->plane_height[i] = fb_plane_height(fb, i);
		fb->plane_width[i] = fb_plane_width(fb, i);
	}
}

static uint32_t calc_plane_stride(struct igt_fb *fb, int plane)
{
	uint32_t min_stride = fb->plane_width[plane] *
		(fb->plane_bpp[plane] / 8);

	if (fb->tiling != LOCAL_DRM_FORMAT_MOD_NONE &&
	    intel_gen(intel_get_drm_devid(fb->fd)) <= 3) {
		uint32_t stride;

		/* Round the tiling up to the next power-of-two and the region
		 * up to the next pot fence size so that this works on all
		 * generations.
		 *
		 * This can still fail if the framebuffer is too large to be
		 * tiled. But then that failure is expected.
		 */

		stride = max(min_stride, 512);
		stride = roundup_power_of_two(stride);

		return stride;
	} else {
		unsigned int tile_width, tile_height;

		igt_get_fb_tile_size(fb->fd, fb->tiling, fb->plane_bpp[plane],
				     &tile_width, &tile_height);

		return ALIGN(min_stride, tile_width);
	}
}

static uint64_t calc_plane_size(struct igt_fb *fb, int plane)
{
	if (fb->tiling != LOCAL_DRM_FORMAT_MOD_NONE &&
	    intel_gen(intel_get_drm_devid(fb->fd)) <= 3) {
		uint64_t min_size = (uint64_t) fb->strides[plane] *
			fb->plane_height[plane];
		uint64_t size;

		/* Round the tiling up to the next power-of-two and the region
		 * up to the next pot fence size so that this works on all
		 * generations.
		 *
		 * This can still fail if the framebuffer is too large to be
		 * tiled. But then that failure is expected.
		 */

		size = max(min_size, 1024*1024);
		size = roundup_power_of_two(size);

		return size;
	} else {
		unsigned int tile_width, tile_height;

		igt_get_fb_tile_size(fb->fd, fb->tiling, fb->plane_bpp[plane],
				     &tile_width, &tile_height);

		return (uint64_t) fb->strides[plane] *
			ALIGN(fb->plane_height[plane], tile_height);
	}
}

static uint64_t calc_fb_size(struct igt_fb *fb)
{
	uint64_t size = 0;
	int plane;

	for (plane = 0; plane < fb->num_planes; plane++) {
		/* respect the stride requested by the caller */
		if (!fb->strides[plane])
			fb->strides[plane] = calc_plane_stride(fb, plane);

		fb->offsets[plane] = size;

		size += calc_plane_size(fb, plane);
	}

	return size;
}

/**
 * igt_calc_fb_size:
 * @fd: the DRM file descriptor
 * @width: width of the framebuffer in pixels
 * @height: height of the framebuffer in pixels
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @size_ret: returned size for the framebuffer
 * @stride_ret: returned stride for the framebuffer
 *
 * This function returns valid stride and size values for a framebuffer with the
 * specified parameters.
 */
void igt_calc_fb_size(int fd, int width, int height, uint32_t drm_format, uint64_t tiling,
		      uint64_t *size_ret, unsigned *stride_ret)
{
	struct igt_fb fb;

	fb_init(&fb, fd, width, height, drm_format, tiling,
		IGT_COLOR_YCBCR_BT709, IGT_COLOR_YCBCR_LIMITED_RANGE);

	fb.size = calc_fb_size(&fb);

	if (size_ret)
		*size_ret = fb.size;
	if (stride_ret)
		*stride_ret = fb.strides[0];
}

/**
 * igt_fb_mod_to_tiling:
 * @modifier: DRM framebuffer modifier
 *
 * This function converts a DRM framebuffer modifier to its corresponding
 * tiling constant.
 *
 * Returns:
 * A tiling constant
 */
uint64_t igt_fb_mod_to_tiling(uint64_t modifier)
{
	switch (modifier) {
	case LOCAL_DRM_FORMAT_MOD_NONE:
		return I915_TILING_NONE;
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		return I915_TILING_X;
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		return I915_TILING_Y;
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		return I915_TILING_Yf;
	default:
		igt_assert(0);
	}
}

/**
 * igt_fb_tiling_to_mod:
 * @tiling: DRM framebuffer tiling
 *
 * This function converts a DRM framebuffer tiling to its corresponding
 * modifier constant.
 *
 * Returns:
 * A modifier constant
 */
uint64_t igt_fb_tiling_to_mod(uint64_t tiling)
{
	switch (tiling) {
	case I915_TILING_NONE:
		return LOCAL_DRM_FORMAT_MOD_NONE;
	case I915_TILING_X:
		return LOCAL_I915_FORMAT_MOD_X_TILED;
	case I915_TILING_Y:
		return LOCAL_I915_FORMAT_MOD_Y_TILED;
	case I915_TILING_Yf:
		return LOCAL_I915_FORMAT_MOD_Yf_TILED;
	default:
		igt_assert(0);
	}
}

/* helpers to create nice-looking framebuffers */
static int create_bo_for_fb(struct igt_fb *fb)
{
	int fd = fb->fd;

	if (fb->tiling || fb->size || fb->strides[0] || igt_format_is_yuv(fb->drm_format)) {
		uint64_t size;

		size = calc_fb_size(fb);

		/* respect the size requested by the caller */
		if (fb->size == 0)
			fb->size = size;

		fb->is_dumb = false;

		if (is_i915_device(fd)) {
			void *ptr;
			bool full_range = fb->color_range == IGT_COLOR_YCBCR_FULL_RANGE;

			fb->gem_handle = gem_create(fd, fb->size);

			gem_set_tiling(fd, fb->gem_handle,
				       igt_fb_mod_to_tiling(fb->tiling),
				       fb->strides[0]);

			gem_set_domain(fd, fb->gem_handle,
				       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

			/* Ensure the framebuffer is preallocated */
			ptr = gem_mmap__gtt(fd, fb->gem_handle,
					    fb->size, PROT_READ | PROT_WRITE);
			igt_assert(*(uint32_t *)ptr == 0);

			switch (fb->drm_format) {
			case DRM_FORMAT_NV12:
				memset(ptr + fb->offsets[0],
				       full_range ? 0x00 : 0x10,
				       fb->strides[0] * fb->plane_height[0]);
				memset(ptr + fb->offsets[1],
				       0x80,
				       fb->strides[1] * fb->plane_height[1]);
				break;
			case DRM_FORMAT_XYUV8888:
				wmemset(ptr + fb->offsets[0], full_range ? 0x00008080 : 0x00108080,
					fb->strides[0] * fb->plane_height[0] / sizeof(wchar_t));
				break;
			case DRM_FORMAT_YUYV:
			case DRM_FORMAT_YVYU:
				wmemset(ptr + fb->offsets[0],
					full_range ? 0x80008000 : 0x80108010,
					fb->strides[0] * fb->plane_height[0] / sizeof(wchar_t));
				break;
			case DRM_FORMAT_UYVY:
			case DRM_FORMAT_VYUY:
				wmemset(ptr + fb->offsets[0],
					full_range ? 0x00800080 : 0x10801080,
					fb->strides[0] * fb->plane_height[0] / sizeof(wchar_t));
				break;
			}
			gem_munmap(ptr, fb->size);

			return fb->gem_handle;
		} else {
			bool driver_has_gem_api = false;

			igt_require(driver_has_gem_api);
			return -EINVAL;
		}
	} else {
		fb->is_dumb = true;

		fb->gem_handle = kmstest_dumb_create(fd, fb->width, fb->height,
						     fb->plane_bpp[0],
						     &fb->strides[0], &fb->size);

		return fb->gem_handle;
	}
}

/**
 * igt_create_bo_with_dimensions:
 * @fd: open drm file descriptor
 * @width: width of the buffer object in pixels
 * @height: height of the buffer object in pixels
 * @format: drm fourcc pixel format code
 * @modifier: modifier corresponding to the tiling layout of the buffer object
 * @stride: stride of the buffer object in bytes (0 for automatic stride)
 * @size_ret: size of the buffer object as created by the kernel
 * @stride_ret: stride of the buffer object as created by the kernel
 * @is_dumb: whether the created buffer object is a dumb buffer or not
 *
 * This function allocates a gem buffer object matching the requested
 * properties.
 *
 * Returns:
 * The kms id of the created buffer object.
 */
int igt_create_bo_with_dimensions(int fd, int width, int height,
				  uint32_t format, uint64_t modifier,
				  unsigned stride, uint64_t *size_ret,
				  unsigned *stride_ret, bool *is_dumb)
{
	struct igt_fb fb;

	fb_init(&fb, fd, width, height, format, modifier,
		IGT_COLOR_YCBCR_BT709, IGT_COLOR_YCBCR_LIMITED_RANGE);

	for (int i = 0; i < fb.num_planes; i++)
		fb.strides[i] = stride;

	create_bo_for_fb(&fb);

	if (size_ret)
		*size_ret = fb.size;
	if (stride_ret)
		*stride_ret = fb.strides[0];
	if (is_dumb)
		*is_dumb = fb.is_dumb;

	return fb.gem_handle;
}

/**
 * igt_paint_color:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 *
 * This functions draws a solid rectangle with the given color using the drawing
 * context @cr.
 */
void igt_paint_color(cairo_t *cr, int x, int y, int w, int h,
		     double r, double g, double b)
{
	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_fill(cr);
}

/**
 * igt_paint_color_alpha:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @a: alpha value to use as fill color
 *
 * This functions draws a rectangle with the given color and alpha values using
 * the drawing context @cr.
 */
void igt_paint_color_alpha(cairo_t *cr, int x, int y, int w, int h,
			   double r, double g, double b, double a)
{
	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_fill(cr);
}

/**
 * igt_paint_color_gradient:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 *
 * This functions draws a gradient into the rectangle which fades in from black
 * to the given values using the drawing context @cr.
 */
void
igt_paint_color_gradient(cairo_t *cr, int x, int y, int w, int h,
			 int r, int g, int b)
{
	cairo_pattern_t *pat;

	pat = cairo_pattern_create_linear(x, y, x + w, y + h);
	cairo_pattern_add_color_stop_rgba(pat, 1, 0, 0, 0, 1);
	cairo_pattern_add_color_stop_rgba(pat, 0, r, g, b, 1);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);
}

/**
 * igt_paint_color_gradient_range:
 * @cr: cairo drawing context
 * @x: pixel x-coordination of the fill rectangle
 * @y: pixel y-coordination of the fill rectangle
 * @w: width of the fill rectangle
 * @h: height of the fill rectangle
 * @sr: red value to use as start gradient color
 * @sg: green value to use as start gradient color
 * @sb: blue value to use as start gradient color
 * @er: red value to use as end gradient color
 * @eg: green value to use as end gradient color
 * @eb: blue value to use as end gradient color
 *
 * This functions draws a gradient into the rectangle which fades in
 * from one color to the other using the drawing context @cr.
 */
void
igt_paint_color_gradient_range(cairo_t *cr, int x, int y, int w, int h,
			       double sr, double sg, double sb,
			       double er, double eg, double eb)
{
	cairo_pattern_t *pat;

	pat = cairo_pattern_create_linear(x, y, x + w, y + h);
	cairo_pattern_add_color_stop_rgba(pat, 1, sr, sg, sb, 1);
	cairo_pattern_add_color_stop_rgba(pat, 0, er, eg, eb, 1);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);
}

static void
paint_test_patterns(cairo_t *cr, int width, int height)
{
	double gr_height, gr_width;
	int x, y;

	y = height * 0.10;
	gr_width = width * 0.75;
	gr_height = height * 0.08;
	x = (width / 2) - (gr_width / 2);

	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 1, 0, 0);

	y += gr_height;
	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 0, 1, 0);

	y += gr_height;
	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 0, 0, 1);

	y += gr_height;
	igt_paint_color_gradient(cr, x, y, gr_width, gr_height, 1, 1, 1);
}

/**
 * igt_cairo_printf_line:
 * @cr: cairo drawing context
 * @align: text alignment
 * @yspacing: additional y-direction feed after this line
 * @fmt: format string
 * @...: optional arguments used in the format string
 *
 * This is a little helper to draw text onto framebuffers. All the initial setup
 * (like setting the font size and the moving to the starting position) still
 * needs to be done manually with explicit cairo calls on @cr.
 *
 * Returns:
 * The width of the drawn text.
 */
int igt_cairo_printf_line(cairo_t *cr, enum igt_text_align align,
				double yspacing, const char *fmt, ...)
{
	double x, y, xofs, yofs;
	cairo_text_extents_t extents;
	char *text;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&text, fmt, ap);
	igt_assert(ret >= 0);
	va_end(ap);

	cairo_text_extents(cr, text, &extents);

	xofs = yofs = 0;
	if (align & align_right)
		xofs = -extents.width;
	else if (align & align_hcenter)
		xofs = -extents.width / 2;

	if (align & align_top)
		yofs = extents.height;
	else if (align & align_vcenter)
		yofs = extents.height / 2;

	cairo_get_current_point(cr, &x, &y);
	if (xofs || yofs)
		cairo_rel_move_to(cr, xofs, yofs);

	cairo_text_path(cr, text);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_stroke_preserve(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);

	cairo_move_to(cr, x, y + extents.height + yspacing);

	free(text);

	return extents.width;
}

static void
paint_marker(cairo_t *cr, int x, int y)
{
	enum igt_text_align align;
	int xoff, yoff;

	cairo_move_to(cr, x, y - 20);
	cairo_line_to(cr, x, y + 20);
	cairo_move_to(cr, x - 20, y);
	cairo_line_to(cr, x + 20, y);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, 10, 0, M_PI * 2);
	cairo_set_line_width(cr, 4);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_stroke_preserve(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);

	xoff = x ? -20 : 20;
	align = x ? align_right : align_left;

	yoff = y ? -20 : 20;
	align |= y ? align_bottom : align_top;

	cairo_move_to(cr, x + xoff, y + yoff);
	cairo_set_font_size(cr, 18);
	igt_cairo_printf_line(cr, align, 0, "(%d, %d)", x, y);
}

/**
 * igt_paint_test_pattern:
 * @cr: cairo drawing context
 * @width: width of the visible area
 * @height: height of the visible area
 *
 * This functions draws an entire set of test patterns for the given visible
 * area using the drawing context @cr. This is useful for manual visual
 * inspection of displayed framebuffers.
 *
 * The test patterns include
 *  - corner markers to check for over/underscan and
 *  - a set of color and b/w gradients.
 */
void igt_paint_test_pattern(cairo_t *cr, int width, int height)
{
	paint_test_patterns(cr, width, height);

	cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);

	/* Paint corner markers */
	paint_marker(cr, 0, 0);
	paint_marker(cr, width, 0);
	paint_marker(cr, 0, height);
	paint_marker(cr, width, height);

	igt_assert(!cairo_status(cr));
}

static cairo_status_t
stdio_read_func(void *closure, unsigned char* data, unsigned int size)
{
	if (fread(data, 1, size, (FILE*)closure) != size)
		return CAIRO_STATUS_READ_ERROR;

	return CAIRO_STATUS_SUCCESS;
}

cairo_surface_t *igt_cairo_image_surface_create_from_png(const char *filename)
{
	cairo_surface_t *image;
	FILE *f;

	f = igt_fopen_data(filename);
	image = cairo_image_surface_create_from_png_stream(&stdio_read_func, f);
	fclose(f);

	return image;
}

/**
 * igt_paint_image:
 * @cr: cairo drawing context
 * @filename: filename of the png image to draw
 * @dst_x: pixel x-coordination of the destination rectangle
 * @dst_y: pixel y-coordination of the destination rectangle
 * @dst_width: width of the destination rectangle
 * @dst_height: height of the destination rectangle
 *
 * This function can be used to draw a scaled version of the supplied png image,
 * which is loaded from the package data directory.
 */
void igt_paint_image(cairo_t *cr, const char *filename,
		     int dst_x, int dst_y, int dst_width, int dst_height)
{
	cairo_surface_t *image;
	int img_width, img_height;
	double scale_x, scale_y;

	image = igt_cairo_image_surface_create_from_png(filename);
	igt_assert(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);

	img_width = cairo_image_surface_get_width(image);
	img_height = cairo_image_surface_get_height(image);

	scale_x = (double)dst_width / img_width;
	scale_y = (double)dst_height / img_height;

	cairo_save(cr);

	cairo_translate(cr, dst_x, dst_y);
	cairo_scale(cr, scale_x, scale_y);
	cairo_set_source_surface(cr, image, 0, 0);
	cairo_paint(cr);

	cairo_surface_destroy(image);

	cairo_restore(cr);
}

/**
 * igt_create_fb_with_bo_size:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer (as framebuffer modifier)
 * @fb: pointer to an #igt_fb structure
 * @bo_size: size of the backing bo (0 for automatic size)
 * @bo_stride: stride of the backing bo (0 for automatic stride)
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object of the requested size. All metadata is stored in @fb.
 *
 * The backing storage of the framebuffer is filled with all zeros, i.e. black
 * for rgb pixel formats.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int
igt_create_fb_with_bo_size(int fd, int width, int height,
			   uint32_t format, uint64_t tiling,
			   struct igt_fb *fb, uint64_t bo_size,
			   unsigned bo_stride)
{
	/* FIXME allow the caller to pass these in */
	enum igt_color_encoding color_encoding = IGT_COLOR_YCBCR_BT709;
	enum igt_color_range color_range = IGT_COLOR_YCBCR_LIMITED_RANGE;
	uint32_t flags = 0;

	fb_init(fb, fd, width, height, format, tiling,
		color_encoding, color_range);

	for (int i = 0; i < fb->num_planes; i++)
		fb->strides[i] = bo_stride;

	fb->size = bo_size;

	igt_debug("%s(width=%d, height=%d, format=0x%x, tiling=0x%"PRIx64", size=%"PRIu64")\n",
		  __func__, width, height, format, tiling, bo_size);

	create_bo_for_fb(fb);
	igt_assert(fb->gem_handle > 0);

	igt_debug("%s(handle=%d, pitch=%d)\n",
		  __func__, fb->gem_handle, fb->strides[0]);

	if (fb->tiling || igt_has_fb_modifiers(fd))
		flags = LOCAL_DRM_MODE_FB_MODIFIERS;

	do_or_die(__kms_addfb(fb->fd, fb->gem_handle,
			      fb->width, fb->height,
			      fb->drm_format, fb->tiling,
			      fb->strides, fb->offsets, fb->num_planes, flags,
			      &fb->fb_id));

	return fb->fb_id;
}

/**
 * igt_create_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * The backing storage of the framebuffer is filled with all zeros, i.e. black
 * for rgb pixel formats.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int igt_create_fb(int fd, int width, int height, uint32_t format,
			   uint64_t tiling, struct igt_fb *fb)
{
	return igt_create_fb_with_bo_size(fd, width, height, format, tiling, fb,
					  0, 0);
}

/**
 * igt_create_color_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also fills the entire framebuffer
 * with the given color, which is useful for some simple pipe crc based tests.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_color_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 double r, double g, double b,
				 struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_color(cr, 0, 0, width, height, r, g, b);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

/**
 * igt_create_pattern_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also draws the standard test pattern
 * into the framebuffer.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_pattern_fb(int fd, int width, int height,
				   uint32_t format, uint64_t tiling,
				   struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_test_pattern(cr, width, height);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

/**
 * igt_create_color_pattern_fb:
 * @fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel
 * @height: height of the framebuffer in pixel
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @r: red value to use as fill color
 * @g: green value to use as fill color
 * @b: blue value to use as fill color
 * @fb: pointer to an #igt_fb structure
 *
 * This function allocates a gem buffer object suitable to back a framebuffer
 * with the requested properties and then wraps it up in a drm framebuffer
 * object. All metadata is stored in @fb.
 *
 * Compared to igt_create_fb() this function also fills the entire framebuffer
 * with the given color, and then draws the standard test pattern into the
 * framebuffer.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_color_pattern_fb(int fd, int width, int height,
					 uint32_t format, uint64_t tiling,
					 double r, double g, double b,
					 struct igt_fb *fb /* out */)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_color(cr, 0, 0, width, height, r, g, b);
	igt_paint_test_pattern(cr, width, height);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

/**
 * igt_create_image_fb:
 * @drm_fd: open i915 drm file descriptor
 * @width: width of the framebuffer in pixel or 0
 * @height: height of the framebuffer in pixel or 0
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 * @filename: filename of the png image to draw
 * @fb: pointer to an #igt_fb structure
 *
 * Create a framebuffer with the specified image. If @width is zero the
 * image width will be used. If @height is zero the image height will be used.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_image_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 const char *filename,
				 struct igt_fb *fb /* out */)
{
	cairo_surface_t *image;
	uint32_t fb_id;
	cairo_t *cr;

	image = igt_cairo_image_surface_create_from_png(filename);
	igt_assert(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS);
	if (width == 0)
		width = cairo_image_surface_get_width(image);
	if (height == 0)
		height = cairo_image_surface_get_height(image);
	cairo_surface_destroy(image);

	fb_id = igt_create_fb(fd, width, height, format, tiling, fb);

	cr = igt_get_cairo_ctx(fd, fb);
	igt_paint_image(cr, filename, 0, 0, width, height);
	igt_put_cairo_ctx(fd, fb, cr);

	return fb_id;
}

struct box {
	int x, y, width, height;
};

struct stereo_fb_layout {
	int fb_width, fb_height;
	struct box left, right;
};

static void box_init(struct box *box, int x, int y, int bwidth, int bheight)
{
	box->x = x;
	box->y = y;
	box->width = bwidth;
	box->height = bheight;
}


static void stereo_fb_layout_from_mode(struct stereo_fb_layout *layout,
				       drmModeModeInfo *mode)
{
	unsigned int format = mode->flags & DRM_MODE_FLAG_3D_MASK;
	const int hdisplay = mode->hdisplay, vdisplay = mode->vdisplay;
	int middle;

	switch (format) {
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
		layout->fb_width = hdisplay;
		layout->fb_height = vdisplay;

		middle = vdisplay / 2;
		box_init(&layout->left, 0, 0, hdisplay, middle);
		box_init(&layout->right,
			 0, middle, hdisplay, vdisplay - middle);
		break;
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
		layout->fb_width = hdisplay;
		layout->fb_height = vdisplay;

		middle = hdisplay / 2;
		box_init(&layout->left, 0, 0, middle, vdisplay);
		box_init(&layout->right,
			 middle, 0, hdisplay - middle, vdisplay);
		break;
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
	{
		int vactive_space = mode->vtotal - vdisplay;

		layout->fb_width = hdisplay;
		layout->fb_height = 2 * vdisplay + vactive_space;

		box_init(&layout->left,
			 0, 0, hdisplay, vdisplay);
		box_init(&layout->right,
			 0, vdisplay + vactive_space, hdisplay, vdisplay);
		break;
	}
	default:
		igt_assert(0);
	}
}

/**
 * igt_create_stereo_fb:
 * @drm_fd: open i915 drm file descriptor
 * @mode: A stereo 3D mode.
 * @format: drm fourcc pixel format code
 * @tiling: tiling layout of the framebuffer
 *
 * Create a framebuffer for use with the stereo 3D mode specified by @mode.
 *
 * Returns:
 * The kms id of the created framebuffer on success or a negative error code on
 * failure.
 */
unsigned int igt_create_stereo_fb(int drm_fd, drmModeModeInfo *mode,
				  uint32_t format, uint64_t tiling)
{
	struct stereo_fb_layout layout;
	cairo_t *cr;
	uint32_t fb_id;
	struct igt_fb fb;

	stereo_fb_layout_from_mode(&layout, mode);
	fb_id = igt_create_fb(drm_fd, layout.fb_width, layout.fb_height, format,
			      tiling, &fb);
	cr = igt_get_cairo_ctx(drm_fd, &fb);

	igt_paint_image(cr, "1080p-left.png",
			layout.left.x, layout.left.y,
			layout.left.width, layout.left.height);
	igt_paint_image(cr, "1080p-right.png",
			layout.right.x, layout.right.y,
			layout.right.width, layout.right.height);

	igt_put_cairo_ctx(drm_fd, &fb, cr);

	return fb_id;
}

static pixman_format_code_t drm_format_to_pixman(uint32_t drm_format)
{
	const struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return f->pixman_id;

	igt_assert_f(0, "can't find a pixman format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));
}

static cairo_format_t drm_format_to_cairo(uint32_t drm_format)
{
	const struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return f->cairo_id;

	igt_assert_f(0, "can't find a cairo format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));
}

struct fb_blit_linear {
	struct igt_fb fb;
	uint8_t *map;
};

struct fb_blit_upload {
	int fd;
	struct igt_fb *fb;
	struct fb_blit_linear linear;
};

static void blitcopy(const struct igt_fb *dst_fb,
		     const struct igt_fb *src_fb)
{
	igt_assert_eq(dst_fb->fd, src_fb->fd);
	igt_assert_eq(dst_fb->num_planes, src_fb->num_planes);

	for (int i = 0; i < dst_fb->num_planes; i++) {
		igt_assert_eq(dst_fb->plane_bpp[i], src_fb->plane_bpp[i]);
		igt_assert_eq(dst_fb->plane_width[i], src_fb->plane_width[i]);
		igt_assert_eq(dst_fb->plane_height[i], src_fb->plane_height[i]);

		igt_blitter_fast_copy__raw(dst_fb->fd,
					   src_fb->gem_handle,
					   src_fb->offsets[i],
					   src_fb->strides[i],
					   igt_fb_mod_to_tiling(src_fb->tiling),
					   0, 0, /* src_x, src_y */
					   dst_fb->plane_width[i], dst_fb->plane_height[i],
					   dst_fb->plane_bpp[i],
					   dst_fb->gem_handle,
					   dst_fb->offsets[i],
					   dst_fb->strides[i],
					   igt_fb_mod_to_tiling(dst_fb->tiling),
					   0, 0 /* dst_x, dst_y */);
	}
}

static void free_linear_mapping(struct fb_blit_upload *blit)
{
	int fd = blit->fd;
	struct igt_fb *fb = blit->fb;
	struct fb_blit_linear *linear = &blit->linear;

	gem_munmap(linear->map, linear->fb.size);
	gem_set_domain(fd, linear->fb.gem_handle,
		       I915_GEM_DOMAIN_GTT, 0);

	blitcopy(fb, &linear->fb);

	gem_sync(fd, linear->fb.gem_handle);
	gem_close(fd, linear->fb.gem_handle);
}

static void destroy_cairo_surface__blit(void *arg)
{
	struct fb_blit_upload *blit = arg;

	blit->fb->cairo_surface = NULL;

	free_linear_mapping(blit);

	free(blit);
}

static void setup_linear_mapping(int fd, struct igt_fb *fb, struct fb_blit_linear *linear)
{
	/*
	 * We create a linear BO that we'll map for the CPU to write to (using
	 * cairo). This linear bo will be then blitted to its final
	 * destination, tiling it at the same time.
	 */

	fb_init(&linear->fb, fb->fd, fb->width, fb->height,
		fb->drm_format, LOCAL_DRM_FORMAT_MOD_NONE,
		fb->color_encoding, fb->color_range);

	create_bo_for_fb(&linear->fb);

	igt_assert(linear->fb.gem_handle > 0);

	/* Copy fb content to linear BO */
	gem_set_domain(fd, linear->fb.gem_handle,
			I915_GEM_DOMAIN_GTT, 0);

	blitcopy(&linear->fb, fb);

	gem_sync(fd, linear->fb.gem_handle);

	gem_set_domain(fd, linear->fb.gem_handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	/* Setup cairo context */
	linear->map = gem_mmap__cpu(fd, linear->fb.gem_handle,
				    0, linear->fb.size, PROT_READ | PROT_WRITE);
}

static void create_cairo_surface__blit(int fd, struct igt_fb *fb)
{
	struct fb_blit_upload *blit;
	cairo_format_t cairo_format;

	blit = malloc(sizeof(*blit));
	igt_assert(blit);

	blit->fd = fd;
	blit->fb = fb;
	setup_linear_mapping(fd, fb, &blit->linear);

	cairo_format = drm_format_to_cairo(fb->drm_format);
	fb->cairo_surface =
		cairo_image_surface_create_for_data(blit->linear.map,
						    cairo_format,
						    fb->width, fb->height,
						    blit->linear.fb.strides[0]);
	fb->domain = I915_GEM_DOMAIN_GTT;

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__blit,
				    blit, destroy_cairo_surface__blit);
}

/**
 * igt_dirty_fb:
 * @fd: open drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * Flushes out the whole framebuffer.
 *
 * Returns: 0 upon success.
 */
int igt_dirty_fb(int fd, struct igt_fb *fb)
{
	return drmModeDirtyFB(fb->fd, fb->fb_id, NULL, 0);
}

static void unmap_bo(struct igt_fb *fb, void *ptr)
{
	gem_munmap(ptr, fb->size);

	if (fb->is_dumb)
		igt_dirty_fb(fb->fd, fb);
}

static void destroy_cairo_surface__gtt(void *arg)
{
	struct igt_fb *fb = arg;

	unmap_bo(fb, cairo_image_surface_get_data(fb->cairo_surface));
	fb->cairo_surface = NULL;
}

static void *map_bo(int fd, struct igt_fb *fb)
{
	void *ptr;

	if (is_i915_device(fd))
		gem_set_domain(fd, fb->gem_handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	if (fb->is_dumb)
		ptr = kmstest_dumb_map_buffer(fd, fb->gem_handle, fb->size,
					      PROT_READ | PROT_WRITE);
	else
		ptr = gem_mmap__gtt(fd, fb->gem_handle, fb->size,
				    PROT_READ | PROT_WRITE);

	return ptr;
}

static void create_cairo_surface__gtt(int fd, struct igt_fb *fb)
{
	void *ptr = map_bo(fd, fb);

	fb->cairo_surface =
		cairo_image_surface_create_for_data(ptr,
						    drm_format_to_cairo(fb->drm_format),
						    fb->width, fb->height, fb->strides[0]);
	igt_require_f(cairo_surface_status(fb->cairo_surface) == CAIRO_STATUS_SUCCESS,
		      "Unable to create a cairo surface: %s\n",
		      cairo_status_to_string(cairo_surface_status(fb->cairo_surface)));

	fb->domain = I915_GEM_DOMAIN_GTT;

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__gtt,
				    fb, destroy_cairo_surface__gtt);
}

struct fb_convert_blit_upload {
	struct fb_blit_upload base;

	struct igt_fb shadow_fb;
	uint8_t *shadow_ptr;
};

static void *igt_fb_create_cairo_shadow_buffer(int fd,
					       unsigned int width,
					       unsigned int height,
					       struct igt_fb *shadow)
{
	void *ptr;

	igt_assert(shadow);

	fb_init(shadow, fd, width, height,
		DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
		IGT_COLOR_YCBCR_BT709, IGT_COLOR_YCBCR_LIMITED_RANGE);

	shadow->strides[0] = ALIGN(width * 4, 16);
	shadow->size = ALIGN(shadow->strides[0] * height,
			     sysconf(_SC_PAGESIZE));
	ptr = mmap(NULL, shadow->size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	igt_assert(ptr != MAP_FAILED);

	return ptr;
}

static void igt_fb_destroy_cairo_shadow_buffer(struct igt_fb *shadow,
					       void *ptr)
{
	munmap(ptr, shadow->size);
}

static uint8_t clamprgb(float val)
{
	return clamp((int)(val + 0.5f), 0, 255);
}

static void read_rgb(struct igt_vec4 *rgb, const uint8_t *rgb24)
{
	rgb->d[0] = rgb24[2];
	rgb->d[1] = rgb24[1];
	rgb->d[2] = rgb24[0];
	rgb->d[3] = 1.0f;
}

static void write_rgb(uint8_t *rgb24, const struct igt_vec4 *rgb)
{
	rgb24[2] = clamprgb(rgb->d[0]);
	rgb24[1] = clamprgb(rgb->d[1]);
	rgb24[0] = clamprgb(rgb->d[2]);
}

struct fb_convert_buf {
	void			*ptr;
	struct igt_fb		*fb;
	bool                     slow_reads;
};

struct fb_convert {
	struct fb_convert_buf	dst;
	struct fb_convert_buf	src;
};

static void *convert_src_get(const struct fb_convert *cvt)
{
	void *buf;

	if (!cvt->src.slow_reads)
		return cvt->src.ptr;

	/*
	 * Reading from the BO is awfully slow because of lack of read caching,
	 * it's faster to copy the whole BO to a temporary buffer and convert
	 * from there.
	 */
	buf = malloc(cvt->src.fb->size);
	if (!buf)
		return cvt->src.ptr;

	igt_memcpy_from_wc(buf, cvt->src.ptr, cvt->src.fb->size);

	return buf;
}

static void convert_src_put(const struct fb_convert *cvt,
			    void *src_buf)
{
	if (src_buf != cvt->src.ptr)
		free(src_buf);
}

static void convert_nv12_to_rgb24(struct fb_convert *cvt)
{
	int i, j;
	const uint8_t *y, *uv;
	uint8_t *rgb24 = cvt->dst.ptr;
	unsigned int rgb24_stride = cvt->dst.fb->strides[0];
	unsigned int planar_stride = cvt->src.fb->strides[0];
	struct igt_mat4 m = igt_ycbcr_to_rgb_matrix(cvt->src.fb->color_encoding,
						    cvt->src.fb->color_range);
	uint8_t *buf;

	igt_assert(cvt->src.fb->drm_format == DRM_FORMAT_NV12 &&
		   cvt->dst.fb->drm_format == DRM_FORMAT_XRGB8888);

	buf = convert_src_get(cvt);
	y = buf + cvt->src.fb->offsets[0];
	uv = buf + cvt->src.fb->offsets[1];

	for (i = 0; i < cvt->dst.fb->height / 2; i++) {
		for (j = 0; j < cvt->dst.fb->width / 2; j++) {
			/* Convert 2x2 pixel blocks */
			struct igt_vec4 yuv[4];
			struct igt_vec4 rgb[4];

			yuv[0].d[0] = y[j * 2 + 0];
			yuv[1].d[0] = y[j * 2 + 1];
			yuv[2].d[0] = y[j * 2 + 0 + planar_stride];
			yuv[3].d[0] = y[j * 2 + 1 + planar_stride];

			yuv[0].d[1] = yuv[1].d[1] = yuv[2].d[1] = yuv[3].d[1] = uv[j * 2 + 0];
			yuv[0].d[2] = yuv[1].d[2] = yuv[2].d[2] = yuv[3].d[2] = uv[j * 2 + 1];
			yuv[0].d[3] = yuv[1].d[3] = yuv[2].d[3] = yuv[3].d[3] = 1.0f;

			rgb[0] = igt_matrix_transform(&m, &yuv[0]);
			rgb[1] = igt_matrix_transform(&m, &yuv[1]);
			rgb[2] = igt_matrix_transform(&m, &yuv[2]);
			rgb[3] = igt_matrix_transform(&m, &yuv[3]);

			write_rgb(&rgb24[j * 8 + 0], &rgb[0]);
			write_rgb(&rgb24[j * 8 + 4], &rgb[1]);
			write_rgb(&rgb24[j * 8 + 0 + rgb24_stride], &rgb[2]);
			write_rgb(&rgb24[j * 8 + 4 + rgb24_stride], &rgb[3]);
		}

		if (cvt->dst.fb->width & 1) {
			/* Convert 1x2 pixel block */
			struct igt_vec4 yuv[2];
			struct igt_vec4 rgb[2];

			yuv[0].d[0] = y[j * 2 + 0];
			yuv[1].d[0] = y[j * 2 + 0 + planar_stride];

			yuv[0].d[1] = yuv[1].d[1] = uv[j * 2 + 0];
			yuv[0].d[2] = yuv[1].d[2] = uv[j * 2 + 1];
			yuv[0].d[3] = yuv[1].d[3] = 1.0f;

			rgb[0] = igt_matrix_transform(&m, &yuv[0]);
			rgb[1] = igt_matrix_transform(&m, &yuv[1]);

			write_rgb(&rgb24[j * 8 + 0], &rgb[0]);
			write_rgb(&rgb24[j * 8 + 0 + rgb24_stride], &rgb[1]);
		}

		rgb24 += 2 * rgb24_stride;
		y += 2 * planar_stride;
		uv += planar_stride;
	}

	if (cvt->dst.fb->height & 1) {
		/* Convert last row */
		for (j = 0; j < cvt->dst.fb->width / 2; j++) {
			/* Convert 2x1 pixel blocks */
			struct igt_vec4 yuv[2];
			struct igt_vec4 rgb[2];

			yuv[0].d[0] = y[j * 2 + 0];
			yuv[1].d[0] = y[j * 2 + 1];
			yuv[0].d[1] = yuv[1].d[1] = uv[j * 2 + 0];
			yuv[0].d[2] = yuv[1].d[2] = uv[j * 2 + 1];
			yuv[0].d[3] = yuv[1].d[3] = 1.0f;

			rgb[0] = igt_matrix_transform(&m, &yuv[0]);
			rgb[1] = igt_matrix_transform(&m, &yuv[1]);

			write_rgb(&rgb24[j * 8 + 0], &rgb[0]);
			write_rgb(&rgb24[j * 8 + 4], &rgb[0]);
		}

		if (cvt->dst.fb->width & 1) {
			/* Convert single pixel */
			struct igt_vec4 yuv;
			struct igt_vec4 rgb;

			yuv.d[0] = y[j * 2 + 0];
			yuv.d[1] = uv[j * 2 + 0];
			yuv.d[2] = uv[j * 2 + 1];
			yuv.d[3] = 1.0f;

			rgb = igt_matrix_transform(&m, &yuv);

			write_rgb(&rgb24[j * 8 + 0], &rgb);
		}
	}

	convert_src_put(cvt, buf);
}

static void convert_yuv444_to_rgb24(struct fb_convert *cvt)
{
	int i, j;
	uint8_t *yuv24;
	uint8_t *rgb24 = cvt->dst.ptr;
	unsigned rgb24_stride = cvt->dst.fb->strides[0], xyuv_stride = cvt->src.fb->strides[0];
	uint8_t *buf = malloc(cvt->src.fb->size);
	struct igt_mat4 m = igt_ycbcr_to_rgb_matrix(cvt->src.fb->color_encoding,
						    cvt->src.fb->color_range);

	/*
	 * Reading from the BO is awfully slow because of lack of read caching,
	 * it's faster to copy the whole BO to a temporary buffer and convert
	 * from there.
	 */
	igt_memcpy_from_wc(buf, cvt->src.ptr + cvt->src.fb->offsets[0], cvt->src.fb->size);
	yuv24 = buf;

	for (i = 0; i < cvt->dst.fb->height; i++) {
		for (j = 0; j < cvt->dst.fb->width; j++) {
			float y, u, v;
			struct igt_vec4 yuv;
			struct igt_vec4 rgb;

			v = yuv24[i * xyuv_stride + j * 4];
			u = yuv24[i * xyuv_stride + j * 4 + 1];
			y = yuv24[i * xyuv_stride + j * 4 + 2];
			yuv.d[0] = y;
			yuv.d[1] = u;
			yuv.d[2] = v;
			yuv.d[3] = 1.0f;

			rgb = igt_matrix_transform(&m, &yuv);

			write_rgb(&rgb24[i * rgb24_stride + j * 4], &rgb);
		}
	}

	free(buf);
}


static void convert_rgb24_to_yuv444(struct fb_convert *cvt)
{
	int i, j;
	uint8_t *rgb24;
	uint8_t *yuv444 = cvt->dst.ptr + cvt->dst.fb->offsets[0];
	unsigned int rgb24_stride = cvt->src.fb->strides[0], xyuv_stride = cvt->dst.fb->strides[0];
	struct igt_mat4 m = igt_rgb_to_ycbcr_matrix(cvt->dst.fb->color_encoding,
						    cvt->dst.fb->color_range);

	rgb24 = cvt->src.ptr;

	igt_assert_f(cvt->dst.fb->drm_format == DRM_FORMAT_XYUV8888,
		     "Conversion not implemented for !XYUV packed formats\n");

	for (i = 0; i < cvt->dst.fb->height; i++) {
		for (j = 0; j < cvt->dst.fb->width; j++) {
			struct igt_vec4 rgb;
			struct igt_vec4 yuv;
			float y, u, v;

			read_rgb(&rgb, &rgb24[i * rgb24_stride + j * 4]);

			yuv = igt_matrix_transform(&m, &rgb);

			yuv444[i * xyuv_stride + j * 4] = yuv.d[2];
			yuv444[i * xyuv_stride + j * 4 + 1] = yuv.d[1];
			yuv444[i * xyuv_stride + j * 4 + 2] = yuv.d[0];
		}
	}
}

static void convert_rgb24_to_nv12(struct fb_convert *cvt)
{
	int i, j;
	uint8_t *y = cvt->dst.ptr + cvt->dst.fb->offsets[0];
	uint8_t *uv = cvt->dst.ptr + cvt->dst.fb->offsets[1];
	const uint8_t *rgb24 = cvt->src.ptr;
	unsigned rgb24_stride = cvt->src.fb->strides[0];
	unsigned planar_stride = cvt->dst.fb->strides[0];
	struct igt_mat4 m = igt_rgb_to_ycbcr_matrix(cvt->dst.fb->color_encoding,
						    cvt->dst.fb->color_range);

	igt_assert(cvt->src.fb->drm_format == DRM_FORMAT_XRGB8888 &&
		   cvt->dst.fb->drm_format == DRM_FORMAT_NV12);

	for (i = 0; i < cvt->dst.fb->height / 2; i++) {
		for (j = 0; j < cvt->dst.fb->width / 2; j++) {
			/* Convert 2x2 pixel blocks */
			struct igt_vec4 rgb[4];
			struct igt_vec4 yuv[4];

			read_rgb(&rgb[0], &rgb24[j * 8 + 0]);
			read_rgb(&rgb[1], &rgb24[j * 8 + 4]);
			read_rgb(&rgb[2], &rgb24[j * 8 + 0 + rgb24_stride]);
			read_rgb(&rgb[3], &rgb24[j * 8 + 4 + rgb24_stride]);

			yuv[0] = igt_matrix_transform(&m, &rgb[0]);
			yuv[1] = igt_matrix_transform(&m, &rgb[1]);
			yuv[2] = igt_matrix_transform(&m, &rgb[2]);
			yuv[3] = igt_matrix_transform(&m, &rgb[3]);

			y[j * 2 + 0] = yuv[0].d[0];
			y[j * 2 + 1] = yuv[1].d[0];
			y[j * 2 + 0 + planar_stride] = yuv[2].d[0];
			y[j * 2 + 1 + planar_stride] = yuv[3].d[0];

			/*
			 * We assume the MPEG2 chroma siting convention, where
			 * pixel center for Cb'Cr' is between the left top and
			 * bottom pixel in a 2x2 block, so take the average.
			 */
			uv[j * 2 + 0] = (yuv[0].d[1] + yuv[2].d[1]) / 2.0f;
			uv[j * 2 + 1] = (yuv[0].d[2] + yuv[2].d[2]) / 2.0f;
		}

		if (cvt->dst.fb->width & 1) {
			/* Convert 1x2 pixel block */
			struct igt_vec4 rgb[2];
			struct igt_vec4 yuv[2];

			read_rgb(&rgb[0], &rgb24[j * 8 + 0]);
			read_rgb(&rgb[2], &rgb24[j * 8 + 0 + rgb24_stride]);

			yuv[0] = igt_matrix_transform(&m, &rgb[0]);
			yuv[1] = igt_matrix_transform(&m, &rgb[1]);

			y[j * 2 + 0] = yuv[0].d[0];
			y[j * 2 + 0 + planar_stride] = yuv[1].d[0];

			/*
			 * We assume the MPEG2 chroma siting convention, where
			 * pixel center for Cb'Cr' is between the left top and
			 * bottom pixel in a 2x2 block, so take the average.
			 */
			uv[j * 2 + 0] = (yuv[0].d[1] + yuv[1].d[1]) / 2.0f;
			uv[j * 2 + 1] = (yuv[0].d[2] + yuv[1].d[2]) / 2.0f;
		}

		rgb24 += 2 * rgb24_stride;
		y += 2 * planar_stride;
		uv += planar_stride;
	}

	/* Last row cannot be interpolated between 2 pixels, take the single value */
	if (cvt->dst.fb->height & 1) {
		for (j = 0; j < cvt->dst.fb->width / 2; j++) {
			/* Convert 2x1 pixel blocks */
			struct igt_vec4 rgb[2];
			struct igt_vec4 yuv[2];

			read_rgb(&rgb[0], &rgb24[j * 8 + 0]);
			read_rgb(&rgb[1], &rgb24[j * 8 + 4]);

			yuv[0] = igt_matrix_transform(&m, &rgb[0]);
			yuv[1] = igt_matrix_transform(&m, &rgb[1]);

			y[j * 2 + 0] = yuv[0].d[0];
			y[j * 2 + 1] = yuv[1].d[0];
			uv[j * 2 + 0] = yuv[0].d[1];
			uv[j * 2 + 1] = yuv[0].d[2];
		}

		if (cvt->dst.fb->width & 1) {
			/* Convert single pixel */
			struct igt_vec4 rgb;
			struct igt_vec4 yuv;

			read_rgb(&rgb, &rgb24[j * 8 + 0]);

			yuv = igt_matrix_transform(&m, &rgb);

			y[j * 2 + 0] = yuv.d[0];
			uv[j * 2 + 0] = yuv.d[1];
			uv[j * 2 + 1] = yuv.d[2];
		}
	}
}

/* { Y0, U, Y1, V } */
static const unsigned char swizzle_yuyv[] = { 0, 1, 2, 3 };
static const unsigned char swizzle_yvyu[] = { 0, 3, 2, 1 };
static const unsigned char swizzle_uyvy[] = { 1, 0, 3, 2 };
static const unsigned char swizzle_vyuy[] = { 1, 2, 3, 0 };

static const unsigned char *yuyv_swizzle(uint32_t format)
{
	switch (format) {
	default:
	case DRM_FORMAT_YUYV:
		return swizzle_yuyv;
	case DRM_FORMAT_YVYU:
		return swizzle_yvyu;
	case DRM_FORMAT_UYVY:
		return swizzle_uyvy;
	case DRM_FORMAT_VYUY:
		return swizzle_vyuy;
	}
}

static void convert_yuyv_to_rgb24(struct fb_convert *cvt)
{
	int i, j;
	const uint8_t *yuyv;
	uint8_t *rgb24 = cvt->dst.ptr;
	unsigned int rgb24_stride = cvt->dst.fb->strides[0];
	unsigned int yuyv_stride = cvt->src.fb->strides[0];
	struct igt_mat4 m = igt_ycbcr_to_rgb_matrix(cvt->src.fb->color_encoding,
						    cvt->src.fb->color_range);
	const unsigned char *swz = yuyv_swizzle(cvt->src.fb->drm_format);
	uint8_t *buf;

	igt_assert((cvt->src.fb->drm_format == DRM_FORMAT_YUYV ||
		    cvt->src.fb->drm_format == DRM_FORMAT_UYVY ||
		    cvt->src.fb->drm_format == DRM_FORMAT_YVYU ||
		    cvt->src.fb->drm_format == DRM_FORMAT_VYUY) &&
		   cvt->dst.fb->drm_format == DRM_FORMAT_XRGB8888);

	buf = convert_src_get(cvt);
	yuyv = buf;

	for (i = 0; i < cvt->dst.fb->height; i++) {
		for (j = 0; j < cvt->dst.fb->width / 2; j++) {
			/* Convert 2x1 pixel blocks */
			struct igt_vec4 yuv[2];
			struct igt_vec4 rgb[2];

			yuv[0].d[0] = yuyv[j * 4 + swz[0]];
			yuv[1].d[0] = yuyv[j * 4 + swz[2]];
			yuv[0].d[1] = yuv[1].d[1] = yuyv[j * 4 + swz[1]];
			yuv[0].d[2] = yuv[1].d[2] = yuyv[j * 4 + swz[3]];
			yuv[0].d[3] = yuv[1].d[3] = 1.0f;

			rgb[0] = igt_matrix_transform(&m, &yuv[0]);
			rgb[1] = igt_matrix_transform(&m, &yuv[1]);

			write_rgb(&rgb24[j * 8 + 0], &rgb[0]);
			write_rgb(&rgb24[j * 8 + 4], &rgb[1]);
		}

		if (cvt->dst.fb->width & 1) {
			struct igt_vec4 yuv;
			struct igt_vec4 rgb;

			yuv.d[0] = yuyv[j * 4 + swz[0]];
			yuv.d[1] = yuyv[j * 4 + swz[1]];
			yuv.d[2] = yuyv[j * 4 + swz[3]];
			yuv.d[3] = 1.0f;

			rgb = igt_matrix_transform(&m, &yuv);

			write_rgb(&rgb24[j * 8 + 0], &rgb);
		}

		rgb24 += rgb24_stride;
		yuyv += yuyv_stride;
	}

	convert_src_put(cvt, buf);
}

static void convert_rgb24_to_yuyv(struct fb_convert *cvt)
{
	int i, j;
	uint8_t *yuyv = cvt->dst.ptr;
	const uint8_t *rgb24 = cvt->src.ptr;
	unsigned rgb24_stride = cvt->src.fb->strides[0];
	unsigned yuyv_stride = cvt->dst.fb->strides[0];
	struct igt_mat4 m = igt_rgb_to_ycbcr_matrix(cvt->dst.fb->color_encoding,
						    cvt->dst.fb->color_range);
	const unsigned char *swz = yuyv_swizzle(cvt->dst.fb->drm_format);

	igt_assert(cvt->src.fb->drm_format == DRM_FORMAT_XRGB8888 &&
		   (cvt->dst.fb->drm_format == DRM_FORMAT_YUYV ||
		    cvt->dst.fb->drm_format == DRM_FORMAT_UYVY ||
		    cvt->dst.fb->drm_format == DRM_FORMAT_YVYU ||
		    cvt->dst.fb->drm_format == DRM_FORMAT_VYUY));

	for (i = 0; i < cvt->dst.fb->height; i++) {
		for (j = 0; j < cvt->dst.fb->width / 2; j++) {
			/* Convert 2x1 pixel blocks */
			struct igt_vec4 rgb[2];
			struct igt_vec4 yuv[2];

			read_rgb(&rgb[0], &rgb24[j * 8 + 0]);
			read_rgb(&rgb[1], &rgb24[j * 8 + 4]);

			yuv[0] = igt_matrix_transform(&m, &rgb[0]);
			yuv[1] = igt_matrix_transform(&m, &rgb[1]);

			yuyv[j * 4 + swz[0]] = yuv[0].d[0];
			yuyv[j * 4 + swz[2]] = yuv[1].d[0];
			yuyv[j * 4 + swz[1]] = (yuv[0].d[1] + yuv[1].d[1]) / 2.0f;
			yuyv[j * 4 + swz[3]] = (yuv[0].d[2] + yuv[1].d[2]) / 2.0f;
		}

		if (cvt->dst.fb->width & 1) {
			struct igt_vec4 rgb;
			struct igt_vec4 yuv;

			read_rgb(&rgb, &rgb24[j * 8 + 0]);

			yuv = igt_matrix_transform(&m, &rgb);

			yuyv[j * 4 + swz[0]] = yuv.d[0];
			yuyv[j * 4 + swz[1]] = yuv.d[1];
			yuyv[j * 4 + swz[3]] = yuv.d[2];
		}

		rgb24 += rgb24_stride;
		yuyv += yuyv_stride;
	}
}

static void convert_pixman(struct fb_convert *cvt)
{
	pixman_format_code_t src_pixman = drm_format_to_pixman(cvt->src.fb->drm_format);
	pixman_format_code_t dst_pixman = drm_format_to_pixman(cvt->dst.fb->drm_format);
	pixman_image_t *dst_image, *src_image;
	void *src_ptr;

	igt_assert((src_pixman != PIXMAN_invalid) &&
		   (dst_pixman != PIXMAN_invalid));

	src_ptr = convert_src_get(cvt);

	src_image = pixman_image_create_bits(src_pixman,
					     cvt->src.fb->width,
					     cvt->src.fb->height,
					     src_ptr,
					     cvt->src.fb->strides[0]);
	igt_assert(src_image);

	dst_image = pixman_image_create_bits(dst_pixman,
					     cvt->dst.fb->width,
					     cvt->dst.fb->height,
					     cvt->dst.ptr,
					     cvt->dst.fb->strides[0]);
	igt_assert(dst_image);

	pixman_image_composite(PIXMAN_OP_SRC, src_image, NULL, dst_image,
			       0, 0, 0, 0, 0, 0,
			       cvt->dst.fb->width, cvt->dst.fb->height);
	pixman_image_unref(dst_image);
	pixman_image_unref(src_image);

	convert_src_put(cvt, src_ptr);
}

static void fb_convert(struct fb_convert *cvt)
{
	if ((drm_format_to_pixman(cvt->src.fb->drm_format) != PIXMAN_invalid) &&
	    (drm_format_to_pixman(cvt->dst.fb->drm_format) != PIXMAN_invalid)) {
		convert_pixman(cvt);
		return;
	} else if (cvt->dst.fb->drm_format == DRM_FORMAT_XRGB8888) {
		switch (cvt->src.fb->drm_format) {
		case DRM_FORMAT_XYUV8888:
			convert_yuv444_to_rgb24(cvt);
			return;
		case DRM_FORMAT_NV12:
			convert_nv12_to_rgb24(cvt);
			return;
		case DRM_FORMAT_YUYV:
		case DRM_FORMAT_YVYU:
		case DRM_FORMAT_UYVY:
		case DRM_FORMAT_VYUY:
			convert_yuyv_to_rgb24(cvt);
			return;
		}
	} else if (cvt->src.fb->drm_format == DRM_FORMAT_XRGB8888) {
		switch (cvt->dst.fb->drm_format) {
		case DRM_FORMAT_XYUV8888:
			convert_rgb24_to_yuv444(cvt);
			return;
		case DRM_FORMAT_NV12:
			convert_rgb24_to_nv12(cvt);
			return;
		case DRM_FORMAT_YUYV:
		case DRM_FORMAT_YVYU:
		case DRM_FORMAT_UYVY:
		case DRM_FORMAT_VYUY:
			convert_rgb24_to_yuyv(cvt);
			return;
		}
	}

	igt_assert_f(false,
		     "Conversion not implemented (from format 0x%x to 0x%x)\n",
		     cvt->src.fb->drm_format, cvt->dst.fb->drm_format);
}

static void destroy_cairo_surface__convert(void *arg)
{
	struct fb_convert_blit_upload *blit = arg;
	struct igt_fb *fb = blit->base.fb;
	struct fb_convert cvt = {
		.dst	= {
			.ptr	= blit->base.linear.map,
			.fb	= &blit->base.linear.fb,
		},

		.src	= {
			.ptr	= blit->shadow_ptr,
			.fb	= &blit->shadow_fb,
		},
	};

	fb_convert(&cvt);
	igt_fb_destroy_cairo_shadow_buffer(&blit->shadow_fb, blit->shadow_ptr);

	if (blit->base.linear.fb.gem_handle)
		free_linear_mapping(&blit->base);
	else
		unmap_bo(fb, blit->base.linear.map);

	free(blit);

	fb->cairo_surface = NULL;
}

static void create_cairo_surface__convert(int fd, struct igt_fb *fb)
{
	struct fb_convert_blit_upload *blit = malloc(sizeof(*blit));
	struct fb_convert cvt = { };

	igt_assert(blit);

	blit->base.fd = fd;
	blit->base.fb = fb;
	blit->shadow_ptr = igt_fb_create_cairo_shadow_buffer(fd,
							     fb->width,
							     fb->height,
							     &blit->shadow_fb);
	igt_assert(blit->shadow_ptr);

	if (fb->tiling == LOCAL_I915_FORMAT_MOD_Y_TILED ||
	    fb->tiling == LOCAL_I915_FORMAT_MOD_Yf_TILED) {
		setup_linear_mapping(fd, fb, &blit->base.linear);
	} else {
		blit->base.linear.fb = *fb;
		blit->base.linear.fb.gem_handle = 0;
		blit->base.linear.map = map_bo(fd, fb);
		igt_assert(blit->base.linear.map);

		/* reading via gtt mmap is slow */
		cvt.src.slow_reads = is_i915_device(fd);
	}

	cvt.dst.ptr = blit->shadow_ptr;
	cvt.dst.fb = &blit->shadow_fb;
	cvt.src.ptr = blit->base.linear.map;
	cvt.src.fb = &blit->base.linear.fb;
	fb_convert(&cvt);

	fb->cairo_surface =
		cairo_image_surface_create_for_data(blit->shadow_ptr,
						    CAIRO_FORMAT_RGB24,
						    fb->width, fb->height,
						    blit->shadow_fb.strides[0]);

	cairo_surface_set_user_data(fb->cairo_surface,
				    (cairo_user_data_key_t *)create_cairo_surface__convert,
				    blit, destroy_cairo_surface__convert);
}


/**
 * igt_fb_map_buffer:
 * @fd: open drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This function will creating a new mapping of the buffer and return a pointer
 * to the content of the supplied framebuffer's plane. This mapping needs to be
 * deleted using igt_fb_unmap_buffer().
 *
 * Returns:
 * A pointer to a buffer with the contents of the framebuffer
 */
void *igt_fb_map_buffer(int fd, struct igt_fb *fb)
{
	return map_bo(fd, fb);
}

/**
 * igt_fb_unmap_buffer:
 * @fb: pointer to the backing igt_fb structure
 * @buffer: pointer to the buffer previously mappped
 *
 * This function will unmap a buffer mapped previously with
 * igt_fb_map_buffer().
 */
void igt_fb_unmap_buffer(struct igt_fb *fb, void *buffer)
{
	return unmap_bo(fb, buffer);
}

/**
 * igt_get_cairo_surface:
 * @fd: open drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This function stores the contents of the supplied framebuffer's plane
 * into a cairo surface and returns it.
 *
 * Returns:
 * A pointer to a cairo surface with the contents of the framebuffer.
 */
cairo_surface_t *igt_get_cairo_surface(int fd, struct igt_fb *fb)
{
	const struct format_desc_struct *f = lookup_drm_format(fb->drm_format);

	if (fb->cairo_surface == NULL) {
		if (igt_format_is_yuv(fb->drm_format) ||
		    ((f->cairo_id == CAIRO_FORMAT_INVALID) &&
		     (f->pixman_id != PIXMAN_invalid)))
			create_cairo_surface__convert(fd, fb);
		else if (fb->tiling == LOCAL_I915_FORMAT_MOD_Y_TILED ||
		    fb->tiling == LOCAL_I915_FORMAT_MOD_Yf_TILED)
			create_cairo_surface__blit(fd, fb);
		else
			create_cairo_surface__gtt(fd, fb);
	}

	igt_assert(cairo_surface_status(fb->cairo_surface) == CAIRO_STATUS_SUCCESS);
	return fb->cairo_surface;
}

/**
 * igt_get_cairo_ctx:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This initializes a cairo surface for @fb and then allocates a drawing context
 * for it. The return cairo drawing context should be released by calling
 * igt_put_cairo_ctx(). This also sets a default font for drawing text on
 * framebuffers.
 *
 * Returns:
 * The created cairo drawing context.
 */
cairo_t *igt_get_cairo_ctx(int fd, struct igt_fb *fb)
{
	cairo_surface_t *surface;
	cairo_t *cr;

	surface = igt_get_cairo_surface(fd, fb);
	cr = cairo_create(surface);
	cairo_surface_destroy(surface);
	igt_assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	cairo_select_font_face(cr, "Helvetica", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	igt_assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

	return cr;
}

/**
 * igt_put_cairo_ctx:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 * @cr: the cairo context returned by igt_get_cairo_ctx.
 *
 * This releases the cairo surface @cr returned by igt_get_cairo_ctx()
 * for @fb, and writes the changes out to the framebuffer if cairo doesn't
 * have native support for the format.
 */
void igt_put_cairo_ctx(int fd, struct igt_fb *fb, cairo_t *cr)
{
	cairo_status_t ret = cairo_status(cr);
	igt_assert_f(ret == CAIRO_STATUS_SUCCESS, "Cairo failed to draw with %s\n", cairo_status_to_string(ret));

	cairo_destroy(cr);
}

/**
 * igt_remove_fb:
 * @fd: open i915 drm file descriptor
 * @fb: pointer to an #igt_fb structure
 *
 * This function releases all resources allocated in igt_create_fb() for @fb.
 * Note that if this framebuffer is still in use on a primary plane the kernel
 * will disable the corresponding crtc.
 */
void igt_remove_fb(int fd, struct igt_fb *fb)
{
	if (!fb || !fb->fb_id)
		return;

	cairo_surface_destroy(fb->cairo_surface);
	do_or_die(drmModeRmFB(fd, fb->fb_id));
	if (fb->is_dumb)
		kmstest_dumb_destroy(fd, fb->gem_handle);
	else
		gem_close(fd, fb->gem_handle);
	fb->fb_id = 0;
}

/**
 * igt_fb_convert:
 * @dst: pointer to the #igt_fb structure that will store the conversion result
 * @src: pointer to the #igt_fb structure that stores the frame we convert
 * @dst_fourcc: DRM format specifier to convert to
 *
 * This will convert a given @src content to the @dst_fourcc format,
 * storing the result in the @dst fb, allocating the @dst fb
 * underlying buffer.
 *
 * Once done with @dst, the caller will have to call igt_remove_fb()
 * on it to free the associated resources.
 *
 * Returns:
 * The kms id of the created framebuffer.
 */
unsigned int igt_fb_convert(struct igt_fb *dst, struct igt_fb *src,
			    uint32_t dst_fourcc)
{
	struct fb_convert cvt = { };
	void *dst_ptr, *src_ptr;
	int fb_id;

	fb_id = igt_create_fb(src->fd, src->width, src->height,
			      dst_fourcc, LOCAL_DRM_FORMAT_MOD_NONE, dst);
	igt_assert(fb_id > 0);

	src_ptr = igt_fb_map_buffer(src->fd, src);
	igt_assert(src_ptr);

	dst_ptr = igt_fb_map_buffer(dst->fd, dst);
	igt_assert(dst_ptr);

	cvt.dst.ptr = dst_ptr;
	cvt.dst.fb = dst;
	cvt.src.ptr = src_ptr;
	cvt.src.fb = src;
	fb_convert(&cvt);

	igt_fb_unmap_buffer(dst, dst_ptr);
	igt_fb_unmap_buffer(src, src_ptr);

	return fb_id;
}

/**
 * igt_bpp_depth_to_drm_format:
 * @bpp: desired bits per pixel
 * @depth: desired depth
 *
 * Returns:
 * The rgb drm fourcc pixel format code corresponding to the given @bpp and
 * @depth values.  Fails hard if no match was found.
 */
uint32_t igt_bpp_depth_to_drm_format(int bpp, int depth)
{
	const struct format_desc_struct *f;

	for_each_format(f)
		if (f->plane_bpp[0] == bpp && f->depth == depth)
			return f->drm_id;


	igt_assert_f(0, "can't find drm format with bpp=%d, depth=%d\n", bpp,
		     depth);
}

/**
 * igt_drm_format_to_bpp:
 * @drm_format: drm fourcc pixel format code
 *
 * Returns:
 * The bits per pixel for the given drm fourcc pixel format code. Fails hard if
 * no match was found.
 */
uint32_t igt_drm_format_to_bpp(uint32_t drm_format)
{
	const struct format_desc_struct *f = lookup_drm_format(drm_format);

	igt_assert_f(f, "can't find a bpp format for %08x (%s)\n",
		     drm_format, igt_format_str(drm_format));

	return f->plane_bpp[0];
}

/**
 * igt_format_str:
 * @drm_format: drm fourcc pixel format code
 *
 * Returns:
 * Human-readable fourcc pixel format code for @drm_format or "invalid" no match
 * was found.
 */
const char *igt_format_str(uint32_t drm_format)
{
	const struct format_desc_struct *f = lookup_drm_format(drm_format);

	return f ? f->name : "invalid";
}

/**
 * igt_fb_supported_format:
 * @drm_format: drm fourcc to test.
 *
 * This functions returns whether @drm_format can be succesfully created by
 * igt_create_fb() and drawn to by igt_get_cairo_ctx().
 */
bool igt_fb_supported_format(uint32_t drm_format)
{
	const struct format_desc_struct *f;

	for_each_format(f)
		if (f->drm_id == drm_format)
			return (f->cairo_id != CAIRO_FORMAT_INVALID) ||
				(f->pixman_id != PIXMAN_invalid);

	return false;
}

/**
 * igt_format_is_yuv:
 * @drm_format: drm fourcc
 *
 * This functions returns whether @drm_format is YUV (as opposed to RGB).
 */
bool igt_format_is_yuv(uint32_t drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_XYUV8888:
		return true;
	default:
		return false;
	}
}
