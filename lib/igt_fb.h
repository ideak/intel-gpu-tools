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

#ifndef __IGT_FB_H__
#define __IGT_FB_H__

#include <cairo.h>
#include <stddef.h>
#include <stdbool.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>

#include <i915_drm.h>

/**
 * igt_fb_t:
 * @fb_id: KMS ID of the framebuffer
 * @fd: DRM device fd this framebuffer is created on
 * @gem_handle: GEM handler of the underlying backing storage
 * @is_dumb: Whether this framebuffer was allocated using the dumb buffer API
 * @drm_format: DRM FOURCC code
 * @width: width in pixels
 * @height: height in pixels
 * @stride: line stride in bytes
 * @tiling: tiling mode as a DRM framebuffer modifier
 * @size: size in bytes of the underlying backing storage
 * @cairo_surface: optionally attached cairo drawing surface
 * @domain: current domain for cache flushing tracking on i915.ko
 * @num_planes: Amount of planes on this fb. >1 for planar formats.
 * @offsets: Offset for each plane in bytes.
 * @plane_bpp: The bpp for each plane.
 * @plane_width: The width for each plane.
 * @plane_height: The height for each plane.
 *
 * Tracking structure for KMS framebuffer objects.
 */
typedef struct igt_fb {
	uint32_t fb_id;
	int fd;
	uint32_t gem_handle;
	bool is_dumb;
	uint32_t drm_format;
	int width;
	int height;
	unsigned int stride;
	uint64_t tiling;
	unsigned int size;
	cairo_surface_t *cairo_surface;
	unsigned int domain;
	unsigned int num_planes;
	uint32_t offsets[4];
	unsigned int plane_bpp[4];
	unsigned int plane_width[4];
	unsigned int plane_height[4];
} igt_fb_t;

/**
 * igt_text_align:
 * @align_left: align left
 * @align_right: align right
 * @align_bottom: align bottom
 * @align_top: align top
 * @align_vcenter: align vcenter
 * @align_hcenter: align hcenter
 *
 * Alignment mode for text drawing using igt_cairo_printf_line().
 */
enum igt_text_align {
	align_left,
	align_bottom	= align_left,
	align_right	= 0x01,
	align_top	= 0x02,
	align_vcenter	= 0x04,
	align_hcenter	= 0x08,
};

void igt_get_fb_tile_size(int fd, uint64_t tiling, int fb_bpp,
			  unsigned *width_ret, unsigned *height_ret);
void igt_calc_fb_size(int fd, int width, int height, uint32_t format, uint64_t tiling,
		      unsigned *size_ret, unsigned *stride_ret);
unsigned int
igt_create_fb_with_bo_size(int fd, int width, int height,
			   uint32_t format, uint64_t tiling,
			   struct igt_fb *fb, unsigned bo_size,
			   unsigned bo_stride);
unsigned int igt_create_fb(int fd, int width, int height, uint32_t format,
			   uint64_t tiling, struct igt_fb *fb);
unsigned int igt_create_color_fb(int fd, int width, int height,
				 uint32_t format, uint64_t tiling,
				 double r, double g, double b,
				 struct igt_fb *fb /* out */);
unsigned int igt_create_pattern_fb(int fd, int width, int height,
				   uint32_t format, uint64_t tiling,
				   struct igt_fb *fb /* out */);
unsigned int igt_create_color_pattern_fb(int fd, int width, int height,
					 uint32_t format, uint64_t tiling,
					 double r, double g, double b,
					 struct igt_fb *fb /* out */);
unsigned int igt_create_image_fb(int drm_fd,  int width, int height,
				 uint32_t format, uint64_t tiling,
				 const char *filename,
				 struct igt_fb *fb /* out */);
unsigned int igt_create_stereo_fb(int drm_fd, drmModeModeInfo *mode,
				  uint32_t format, uint64_t tiling);
void igt_remove_fb(int fd, struct igt_fb *fb);
int igt_dirty_fb(int fd, struct igt_fb *fb);

int igt_create_bo_with_dimensions(int fd, int width, int height, uint32_t format,
				  uint64_t modifier, unsigned stride,
				  unsigned *stride_ret, unsigned *size_ret,
				  bool *is_dumb);

uint64_t igt_fb_mod_to_tiling(uint64_t modifier);
uint64_t igt_fb_tiling_to_mod(uint64_t tiling);

/* cairo-based painting */
cairo_surface_t *igt_get_cairo_surface(int fd, struct igt_fb *fb);
cairo_surface_t *igt_cairo_image_surface_create_from_png(const char *filename);
cairo_t *igt_get_cairo_ctx(int fd, struct igt_fb *fb);
void igt_put_cairo_ctx(int fd, struct igt_fb *fb, cairo_t *cr);
void igt_paint_color(cairo_t *cr, int x, int y, int w, int h,
			 double r, double g, double b);
void igt_paint_color_alpha(cairo_t *cr, int x, int y, int w, int h,
			       double r, double g, double b, double a);
void igt_paint_color_gradient(cairo_t *cr, int x, int y, int w, int h,
				  int r, int g, int b);
void igt_paint_color_gradient_range(cairo_t *cr, int x, int y, int w, int h,
				    double sr, double sg, double sb,
				    double er, double eg, double eb);
void igt_paint_test_pattern(cairo_t *cr, int width, int height);
void igt_paint_image(cairo_t *cr, const char *filename,
			 int dst_x, int dst_y, int dst_width, int dst_height);
int igt_cairo_printf_line(cairo_t *cr, enum igt_text_align align,
			       double yspacing, const char *fmt, ...)
			       __attribute__((format (printf, 4, 5)));

/* helpers to handle drm fourcc codes */
uint32_t igt_bpp_depth_to_drm_format(int bpp, int depth);
uint32_t igt_drm_format_to_bpp(uint32_t drm_format);
const char *igt_format_str(uint32_t drm_format);
bool igt_fb_supported_format(uint32_t drm_format);

#endif /* __IGT_FB_H__ */

