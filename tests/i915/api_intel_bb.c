/*
 * Copyright © 2020 Intel Corporation
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
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <cairo.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <glib.h>
#include <zlib.h>
#include "intel_bufops.h"

#define PAGE_SIZE 4096

#define WIDTH 64
#define HEIGHT 64
#define COLOR_00	0x00
#define COLOR_33	0x33
#define COLOR_77	0x77
#define COLOR_CC	0xcc

IGT_TEST_DESCRIPTION("intel_bb API check.");

enum reloc_objects {
	RELOC,
	NORELOC,
};

enum obj_cache_ops {
	PURGE_CACHE,
	KEEP_CACHE,
};

static bool debug_bb = false;
static bool write_png = false;
static bool buf_info = false;
static bool print_base64 = false;

static void *alloc_aligned(uint64_t size)
{
	void *p;

	igt_assert_eq(posix_memalign(&p, 16, size), 0);

	return p;
}

static void fill_buf(struct intel_buf *buf, uint8_t color)
{
	uint8_t *ptr;
	int i915 = buf_ops_get_fd(buf->bops);
	int i;

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					buf->surface[0].size, PROT_WRITE);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);
}

static void check_buf(struct intel_buf *buf, uint8_t color)
{
	uint8_t *ptr;
	int i915 = buf_ops_get_fd(buf->bops);
	int i;

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					buf->surface[0].size, PROT_READ);

	for (i = 0; i < buf->surface[0].size; i++)
		igt_assert(ptr[i] == color);

	munmap(ptr, buf->surface[0].size);
}


static struct intel_buf *
create_buf(struct buf_ops *bops, int width, int height, uint8_t color)
{
	struct intel_buf *buf;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	intel_buf_init(bops, buf, width/4, height, 32, 0, I915_TILING_NONE, 0);
	fill_buf(buf, color);

	return buf;
}

static void print_buf(struct intel_buf *buf, const char *name)
{
	uint8_t *ptr;
	int i915 = buf_ops_get_fd(buf->bops);

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					buf->surface[0].size, PROT_READ);
	igt_debug("[%s] Buf handle: %d, size: %" PRIx64 ", v: 0x%02x, presumed_addr: %p\n",
		  name, buf->handle, buf->surface[0].size, ptr[0],
		  from_user_pointer(buf->addr.offset));
	munmap(ptr, buf->surface[0].size);
}

static void simple_bb(struct buf_ops *bops, bool use_context)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	uint32_t ctx;

	if (use_context) {
		gem_require_contexts(i915);
		ctx = gem_context_create(i915);
	}

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	/* Check we're safe with reset and no double-free will occur */
	intel_bb_reset(ibb, true);
	intel_bb_reset(ibb, false);
	intel_bb_reset(ibb, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	if (use_context) {
		intel_bb_destroy(ibb);
		ibb = intel_bb_create_with_context(i915, ctx, PAGE_SIZE);
		intel_bb_out(ibb, MI_BATCH_BUFFER_END);
		intel_bb_ptr_align(ibb, 8);
		intel_bb_exec(ibb, intel_bb_offset(ibb),
			      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC,
			      true);
	}

	intel_bb_destroy(ibb);
	if (use_context)
		gem_context_destroy(i915, ctx);
}

/*
 * Make sure we lead to realloc in the intel_bb.
 */
#define NUM_BUFS 4096
static void lot_of_buffers(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *buf[NUM_BUFS];
	int i;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	for (i = 0; i < NUM_BUFS; i++) {
		buf[i] = intel_buf_create(bops, 4096, 1, 8, 0, I915_TILING_NONE,
					  I915_COMPRESSION_NONE);
		intel_bb_add_intel_buf(ibb, buf[i], false);
	}

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);

	for (i = 0; i < NUM_BUFS; i++)
		intel_buf_destroy(buf[i]);
}

/*
 * Make sure intel-bb space allocator currently doesn't enter 47-48 bit
 * gtt sizes.
 */
static void check_canonical(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *buf;
	uint32_t offset;
	uint64_t address;
	bool supports_48bit;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	supports_48bit = ibb->supports_48b_address;
	if (!supports_48bit)
		intel_bb_destroy(ibb);
	igt_require_f(supports_48bit, "We need 48bit ppgtt for testing\n");

	address = 0xc00000000000;
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	offset = intel_bb_emit_bbe(ibb);

	buf = intel_buf_create(bops, 512, 512, 32, 0,
			       I915_TILING_NONE, I915_COMPRESSION_NONE);

	buf->addr.offset = address;
	intel_bb_add_intel_buf(ibb, buf, true);
	intel_bb_object_set_flag(ibb, buf->handle, EXEC_OBJECT_PINNED);

	igt_assert(buf->addr.offset == 0);

	intel_bb_exec(ibb, offset,
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_buf_destroy(buf);
	intel_bb_destroy(ibb);
}

/*
 * Check flags are cleared after intel_bb_reset(ibb, false);
 */
static void reset_flags(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *mid, *dst;
	uint64_t src_48bit, mid_48bit, dst_48bit;
	struct drm_i915_gem_exec_object2 *obj;
	const uint32_t width = 512;
	const uint32_t height = 512;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	src = intel_buf_create(bops, width, height, 32, 0,
			       I915_TILING_NONE, I915_COMPRESSION_NONE);
	mid = intel_buf_create(bops, width, height, 32, 0,
			       I915_TILING_NONE, I915_COMPRESSION_NONE);
	dst = intel_buf_create(bops, width, height, 32, 0,
			       I915_TILING_NONE, I915_COMPRESSION_NONE);

	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, mid, true);

	/* Check src has no EXEC_OBJECT_WRITE */
	obj = intel_bb_find_object(ibb, src->handle);
	igt_assert(obj);
	igt_assert((obj->flags & EXEC_OBJECT_WRITE) == 0);
	src_48bit = obj->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	/* Check mid has EXEC_OBJECT_WRITE */
	obj = intel_bb_find_object(ibb, mid->handle);
	igt_assert(obj);
	igt_assert(obj->flags & EXEC_OBJECT_WRITE);
	mid_48bit = obj->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	intel_bb_out(ibb, 0);
	intel_bb_flush_blit(ibb);

	/* Check src has zeroed flags */
	obj = intel_bb_find_object(ibb, src->handle);
	igt_assert(obj);
	igt_assert((obj->flags & EXEC_OBJECT_WRITE) == 0);

	/* Check src keep 48bit address flag */
	igt_assert((obj->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS) == src_48bit);

	/* Check mid has zeroed flags */
	obj = intel_bb_find_object(ibb, mid->handle);
	igt_assert(obj);
	igt_assert((obj->flags & EXEC_OBJECT_WRITE) == 0);

	/* Check mid keep 48bit address flag */
	igt_assert((obj->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS) == mid_48bit);

	intel_bb_emit_blt_copy(ibb,
			       mid, 0, 0, mid->surface[0].stride,
			       dst, 0, 0, dst->surface[0].stride,
			       intel_buf_width(dst),
			       intel_buf_height(dst),
			       dst->bpp);

	/* Check mid has no EXEC_OBJECT_WRITE */
	obj = intel_bb_find_object(ibb, mid->handle);
	igt_assert(obj);
	igt_assert((obj->flags & EXEC_OBJECT_WRITE) == 0);

	/* Check mid has no EXEC_OBJECT_WRITE */
	obj = intel_bb_find_object(ibb, dst->handle);
	igt_assert(obj);
	igt_assert(obj->flags & EXEC_OBJECT_WRITE);
	dst_48bit = obj->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	intel_bb_flush_blit(ibb);

	/* Check dst has no EXEC_OBJECT_WRITE */
	obj = intel_bb_find_object(ibb, dst->handle);
	igt_assert(obj);
	igt_assert((obj->flags & EXEC_OBJECT_WRITE) == 0);

	/* Check dst keep 48bit address flag */
	igt_assert((obj->flags & EXEC_OBJECT_SUPPORTS_48B_ADDRESS) == dst_48bit);

	intel_buf_destroy(src);
	intel_buf_destroy(mid);
	intel_buf_destroy(dst);
	intel_bb_destroy(ibb);
}


#define MI_FLUSH_DW (0x26<<23)
#define BCS_SWCTRL  0x22200
#define BCS_SRC_Y   (1 << 0)
#define BCS_DST_Y   (1 << 1)
static void __emit_blit(struct intel_bb *ibb,
			struct intel_buf *src, struct intel_buf *dst)
{
	uint32_t mask;
	bool has_64b_reloc;
	uint64_t address;

	has_64b_reloc = ibb->gen >= 8;

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
		intel_bb_out(ibb, BCS_SWCTRL);

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst->tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		intel_bb_out(ibb, mask);
	}

	intel_bb_out(ibb,
		     XY_SRC_COPY_BLT_CMD |
		     XY_SRC_COPY_BLT_WRITE_ALPHA |
		     XY_SRC_COPY_BLT_WRITE_RGB |
		     (6 + 2 * has_64b_reloc));
	intel_bb_out(ibb, 3 << 24 | 0xcc << 16 | dst->surface[0].stride);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, intel_buf_height(dst) << 16 | intel_buf_width(dst));

	address = intel_bb_get_object_offset(ibb, dst->handle);
	intel_bb_emit_reloc_fenced(ibb, dst->handle,
				   I915_GEM_DOMAIN_RENDER,
				   I915_GEM_DOMAIN_RENDER,
				   0, address);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, src->surface[0].stride);

	address = intel_bb_get_object_offset(ibb, src->handle);
	intel_bb_emit_reloc_fenced(ibb, src->handle,
				   I915_GEM_DOMAIN_RENDER, 0,
				   0, address);

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		igt_assert(ibb->gen >= 6);
		intel_bb_out(ibb, MI_FLUSH_DW | 2);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);

		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
		intel_bb_out(ibb, BCS_SWCTRL);
		intel_bb_out(ibb, (BCS_SRC_Y | BCS_DST_Y) << 16);
	}
}

static void blit(struct buf_ops *bops,
		 enum reloc_objects reloc_obj,
		 enum obj_cache_ops cache_op)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst;
	uint64_t poff_bb, poff_src, poff_dst;
	uint64_t poff2_bb, poff2_src, poff2_dst;
	uint64_t flags = 0;
	bool purge_cache = cache_op == PURGE_CACHE ? true : false;
	bool do_relocs = reloc_obj == RELOC ? true : false;

	src = create_buf(bops, WIDTH, HEIGHT, COLOR_CC);
	dst = create_buf(bops, WIDTH, HEIGHT, COLOR_00);

	if (buf_info) {
		print_buf(src, "src");
		print_buf(dst, "dst");
	}

	if (do_relocs) {
		ibb = intel_bb_create_with_relocs(i915, PAGE_SIZE);
	} else {
		ibb = intel_bb_create(i915, PAGE_SIZE);
		flags |= I915_EXEC_NO_RELOC;
	}

	if (ibb->gen >= 6)
		flags |= I915_EXEC_BLT;

	if (debug_bb)
		intel_bb_set_debug(ibb, true);


	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, dst, true);

	__emit_blit(ibb, src, dst);

	/* We expect initial addresses are zeroed for relocs */
	poff_bb = intel_bb_get_object_offset(ibb, ibb->handle);
	poff_src = intel_bb_get_object_offset(ibb, src->handle);
	poff_dst = intel_bb_get_object_offset(ibb, dst->handle);
	igt_debug("bb  presumed offset: 0x%"PRIx64"\n", poff_bb);
	igt_debug("src presumed offset: 0x%"PRIx64"\n", poff_src);
	igt_debug("dst presumed offset: 0x%"PRIx64"\n", poff_dst);
	if (reloc_obj == RELOC) {
		igt_assert(poff_bb == 0);
		igt_assert(poff_src == 0);
		igt_assert(poff_dst == 0);
	}

	intel_bb_emit_bbe(ibb);
	igt_debug("exec flags: %" PRIX64 "\n", flags);
	intel_bb_exec(ibb, intel_bb_offset(ibb), flags, true);
	check_buf(dst, COLOR_CC);

	poff_bb = intel_bb_get_object_offset(ibb, ibb->handle);
	poff_src = intel_bb_get_object_offset(ibb, src->handle);
	poff_dst = intel_bb_get_object_offset(ibb, dst->handle);

	intel_bb_reset(ibb, purge_cache);

	fill_buf(src, COLOR_77);
	fill_buf(dst, COLOR_00);

	if (purge_cache && !do_relocs) {
		intel_bb_add_intel_buf(ibb, src, false);
		intel_bb_add_intel_buf(ibb, dst, true);
	}

	__emit_blit(ibb, src, dst);

	poff2_bb = intel_bb_get_object_offset(ibb, ibb->handle);
	poff2_src = intel_bb_get_object_offset(ibb, src->handle);
	poff2_dst = intel_bb_get_object_offset(ibb, dst->handle);

	igt_debug("purge: %d, relocs: %d\n", purge_cache, do_relocs);
	igt_debug("bb  presumed offset: 0x%"PRIx64"\n", poff_bb);
	igt_debug("src presumed offset: 0x%"PRIx64"\n", poff_src);
	igt_debug("dst presumed offset: 0x%"PRIx64"\n", poff_dst);
	igt_debug("bb2  presumed offset: 0x%"PRIx64"\n", poff2_bb);
	igt_debug("src2 presumed offset: 0x%"PRIx64"\n", poff2_src);
	igt_debug("dst2 presumed offset: 0x%"PRIx64"\n", poff2_dst);

	/*
	 * Since we let the objects idle, if the GTT is shared, another client
	 * is liable to reuse our offsets for themselves, causing us to have
	 * to relocate. We don't expect this to happen as LRU eviction should
	 * try to avoid reuse, but we use random eviction instead as it is
	 * much quicker! Given that the kernel is *allowed* to relocate objects,
	 * we cannot assert that the objects remain in the same location, unless
	 * we are in full control of our own GTT.
	 */
	if (gem_uses_full_ppgtt(i915)) {
		if (purge_cache) {
			if (do_relocs) {
				igt_assert_eq_u64(poff2_bb,  0);
				igt_assert_eq_u64(poff2_src, 0);
				igt_assert_eq_u64(poff2_dst, 0);
			} else {
				igt_assert_neq_u64(poff_bb, poff2_bb);
				igt_assert_eq_u64(poff_src, poff2_src);
				igt_assert_eq_u64(poff_dst, poff2_dst);
			}
		} else {
			igt_assert_eq_u64(poff_bb,  poff2_bb);
			igt_assert_eq_u64(poff_src, poff2_src);
			igt_assert_eq_u64(poff_dst, poff2_dst);
		}
	}

	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb), flags, true);
	check_buf(dst, COLOR_77);

	if (gem_uses_full_ppgtt(i915)) {
		igt_assert_eq_u64(intel_bb_get_object_offset(ibb, src->handle),
				  poff_src);
		igt_assert_eq_u64(intel_bb_get_object_offset(ibb, dst->handle),
				  poff_dst);
	}

	intel_buf_destroy(src);
	intel_buf_destroy(dst);
	intel_bb_destroy(ibb);
}

static void scratch_buf_init(struct buf_ops *bops,
			     struct intel_buf *buf,
			     int width, int height,
			     uint32_t req_tiling,
			     enum i915_compression compression)
{
	int bpp = 32;

	intel_buf_init(bops, buf, width, height, bpp, 0,
		       req_tiling, compression);

	igt_assert(intel_buf_width(buf) == width);
	igt_assert(intel_buf_height(buf) == height);
}

static void scratch_buf_draw_pattern(struct buf_ops *bops,
				     struct intel_buf *buf,
				     int x, int y, int w, int h,
				     int cx, int cy, int cw, int ch,
				     bool use_alternate_colors)
{
	cairo_surface_t *surface;
	cairo_pattern_t *pat;
	cairo_t *cr;
	void *linear;

	linear = alloc_aligned(buf->surface[0].size);

	surface = cairo_image_surface_create_for_data(linear,
						      CAIRO_FORMAT_RGB24,
						      intel_buf_width(buf),
						      intel_buf_height(buf),
						      buf->surface[0].stride);

	cr = cairo_create(surface);

	cairo_rectangle(cr, cx, cy, cw, ch);
	cairo_clip(cr);

	pat = cairo_pattern_create_mesh();
	cairo_mesh_pattern_begin_patch(pat);
	cairo_mesh_pattern_move_to(pat, x,   y);
	cairo_mesh_pattern_line_to(pat, x+w, y);
	cairo_mesh_pattern_line_to(pat, x+w, y+h);
	cairo_mesh_pattern_line_to(pat, x,   y+h);
	if (use_alternate_colors) {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 0.0, 1.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 1.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 1.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 0.0, 0.0, 0.0);
	} else {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 1.0, 0.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 0.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 0.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 1.0, 1.0, 1.0);
	}
	cairo_mesh_pattern_end_patch(pat);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);

	linear_to_intel_buf(bops, buf, linear);

	free(linear);
}

#define GROUP_SIZE 4096
static int compare_detail(const uint32_t *ptr1, uint32_t *ptr2,
			  uint32_t size)
{
	int i, ok = 0, fail = 0;
	int groups = size / GROUP_SIZE;
	int *hist = calloc(GROUP_SIZE, groups);

	igt_debug("size: %d, group_size: %d, groups: %d\n",
		  size, GROUP_SIZE, groups);

	for (i = 0; i < size / sizeof(uint32_t); i++) {
		if (ptr1[i] == ptr2[i]) {
			ok++;
		} else {
			fail++;
			hist[i * sizeof(uint32_t) / GROUP_SIZE]++;
		}
	}

	for (i = 0; i < groups; i++) {
		if (hist[i]) {
			igt_debug("[group %4x]: %d\n", i, hist[i]);
		}
	}
	free(hist);

	igt_debug("ok: %d, fail: %d\n", ok, fail);

	return fail;
}

static int compare_bufs(struct intel_buf *buf1, struct intel_buf *buf2,
			 bool detail_compare)
{
	void *ptr1, *ptr2;
	int fd1, fd2, ret;

	igt_assert(buf1->surface[0].size == buf2->surface[0].size);

	fd1 = buf_ops_get_fd(buf1->bops);
	fd2 = buf_ops_get_fd(buf2->bops);

	ptr1 = gem_mmap__device_coherent(fd1, buf1->handle, 0,
					 buf1->surface[0].size, PROT_READ);
	ptr2 = gem_mmap__device_coherent(fd2, buf2->handle, 0,
					 buf2->surface[0].size, PROT_READ);
	ret = memcmp(ptr1, ptr2, buf1->surface[0].size);
	if (detail_compare)
		ret = compare_detail(ptr1, ptr2, buf1->surface[0].size);

	munmap(ptr1, buf1->surface[0].size);
	munmap(ptr2, buf2->surface[0].size);

	return ret;
}

#define LINELEN 76
static int dump_base64(const char *name, struct intel_buf *buf)
{
	void *ptr;
	int fd, ret;
	uLongf outsize = buf->surface[0].size * 3 / 2;
	Bytef *destbuf = malloc(outsize);
	gchar *str, *pos;

	fd = buf_ops_get_fd(buf->bops);

	ptr = gem_mmap__device_coherent(fd, buf->handle, 0,
					buf->surface[0].size, PROT_READ);

	ret = compress2(destbuf, &outsize, ptr, buf->surface[0].size,
			Z_BEST_COMPRESSION);
	if (ret != Z_OK) {
		igt_warn("error compressing, ret: %d\n", ret);
	} else {
		igt_info("compressed %" PRIx64 " -> %lu\n",
			 buf->surface[0].size, outsize);

		igt_info("--- %s ---\n", name);
		pos = str = g_base64_encode(destbuf, outsize);
		outsize = strlen(str);
		while (pos) {
			char line[LINELEN + 1];
			int to_copy = min(LINELEN, outsize);

			memcpy(line, pos, to_copy);
			line[to_copy] = 0;
			igt_info("%s\n", line);
			pos += LINELEN;
			outsize -= to_copy;

			if (outsize == 0)
				break;
		}
		free(str);
	}

	munmap(ptr, buf->surface[0].size);
	free(destbuf);

	return ret;
}


static int __do_intel_bb_blit(struct buf_ops *bops, uint32_t tiling)
{
	struct intel_bb *ibb;
	const int width = 1024;
	const int height = 1024;
	struct intel_buf src, dst, final;
	char name[128];
	int i915 = buf_ops_get_fd(bops), fails;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	scratch_buf_init(bops, &src, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &dst, width, height, tiling,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &final, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);

	if (buf_info) {
		intel_buf_print(&src);
		intel_buf_print(&dst);
	}

	scratch_buf_draw_pattern(bops, &src,
				 0, 0, width, height,
				 0, 0, width, height, 0);

	intel_bb_blt_copy(ibb,
			  &src, 0, 0, src.surface[0].stride,
			  &dst, 0, 0, dst.surface[0].stride,
			  intel_buf_width(&dst),
			  intel_buf_height(&dst),
			  dst.bpp);

	intel_bb_blt_copy(ibb,
			  &dst, 0, 0, dst.surface[0].stride,
			  &final, 0, 0, final.surface[0].stride,
			  intel_buf_width(&dst),
			  intel_buf_height(&dst),
			  dst.bpp);

	igt_assert(intel_bb_sync(ibb) == 0);
	intel_bb_destroy(ibb);

	if (write_png) {
		snprintf(name, sizeof(name) - 1,
			 "bb_blit_dst_tiling_%d.png", tiling);
		intel_buf_write_to_png(&src, "bb_blit_src_tiling_none.png");
		intel_buf_write_to_png(&dst, name);
		intel_buf_write_to_png(&final, "bb_blit_final_tiling_none.png");
	}

	/* We'll fail on src <-> final compare so just warn */
	if (tiling == I915_TILING_NONE) {
		if (compare_bufs(&src, &dst, false) > 0)
			igt_warn("none->none blit failed!");
	} else {
		if (compare_bufs(&src, &dst, false) == 0)
			igt_warn("none->tiled blit failed!");
	}

	fails = compare_bufs(&src, &final, true);

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_buf_close(bops, &final);

	return fails;
}

static void do_intel_bb_blit(struct buf_ops *bops, int loops, uint32_t tiling)
{
	int i, fails = 0, i915 = buf_ops_get_fd(bops);

	gem_require_blitter(i915);

	/* We'll fix it for gen2/3 later. */
	igt_require(intel_gen(intel_get_drm_devid(i915)) > 3);

	for (i = 0; i < loops; i++) {
		fails += __do_intel_bb_blit(bops, tiling);
	}
	igt_assert_f(fails == 0, "intel-bb-blit (tiling: %d) fails: %d\n",
		     tiling, fails);
}

static void offset_control(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst1, *dst2, *dst3;
	uint64_t poff_src, poff_dst1, poff_dst2;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	src = create_buf(bops, WIDTH, HEIGHT, COLOR_CC);
	dst1 = create_buf(bops, WIDTH, HEIGHT, COLOR_00);
	dst2 = create_buf(bops, WIDTH, HEIGHT, COLOR_77);

	intel_bb_add_object(ibb, src->handle, intel_buf_bo_size(src),
			    src->addr.offset, false);
	intel_bb_add_object(ibb, dst1->handle, intel_buf_bo_size(dst1),
			    dst1->addr.offset, true);
	intel_bb_add_object(ibb, dst2->handle, intel_buf_bo_size(dst2),
			    dst2->addr.offset, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	if (buf_info) {
		print_buf(src, "src ");
		print_buf(dst1, "dst1");
		print_buf(dst2, "dst2");
	}

	igt_assert(intel_bb_object_offset_to_buf(ibb, src) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst1) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst2) == true);
	poff_src = src->addr.offset;
	poff_dst1 = dst1->addr.offset;
	poff_dst2 = dst2->addr.offset;
	intel_bb_reset(ibb, true);

	dst3 = create_buf(bops, WIDTH, HEIGHT, COLOR_33);
	intel_bb_add_object(ibb, dst3->handle, intel_buf_bo_size(dst3),
			    dst3->addr.offset, true);
	intel_bb_add_object(ibb, src->handle, intel_buf_bo_size(src),
			    src->addr.offset, false);
	intel_bb_add_object(ibb, dst1->handle, intel_buf_bo_size(dst1),
			    dst1->addr.offset, true);
	intel_bb_add_object(ibb, dst2->handle, intel_buf_bo_size(dst2),
			    dst2->addr.offset, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);
	intel_bb_sync(ibb);

	igt_assert(intel_bb_object_offset_to_buf(ibb, src) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst1) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst2) == true);
	igt_assert(intel_bb_object_offset_to_buf(ibb, dst3) == true);
	igt_assert(poff_src == src->addr.offset);
	igt_assert(poff_dst1 == dst1->addr.offset);
	igt_assert(poff_dst2 == dst2->addr.offset);

	if (buf_info) {
		print_buf(src, "src ");
		print_buf(dst1, "dst1");
		print_buf(dst2, "dst2");
	}

	intel_buf_destroy(src);
	intel_buf_destroy(dst1);
	intel_buf_destroy(dst2);
	intel_buf_destroy(dst3);
	intel_bb_destroy(ibb);
}

/*
 * Idea of the test is to verify delta is properly added to address
 * when emit_reloc() is called.
 */
#define DELTA_BUFFERS 3
static void delta_check(struct buf_ops *bops)
{
	const uint32_t expected = 0x1234abcd;
	int i915 = buf_ops_get_fd(bops);
	uint32_t *ptr, hi, lo, val;
	struct intel_buf *buf;
	struct intel_bb *ibb;
	uint64_t offset;
	bool supports_48bit;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	supports_48bit = ibb->supports_48b_address;
	if (!supports_48bit)
		intel_bb_destroy(ibb);
	igt_require_f(supports_48bit, "We need 48bit ppgtt for testing\n");

	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	buf = create_buf(bops, 0x1000, 0x10, COLOR_CC);
	buf->addr.offset = 0xfffff000;
	intel_bb_add_object(ibb, buf->handle, intel_buf_bo_size(buf),
			    buf->addr.offset, false);

	intel_bb_out(ibb, MI_STORE_DWORD_IMM);
	intel_bb_emit_reloc(ibb, buf->handle,
			    I915_GEM_DOMAIN_RENDER,
			    I915_GEM_DOMAIN_RENDER,
			    0x2000, buf->addr.offset);
	intel_bb_out(ibb, expected);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb), I915_EXEC_DEFAULT, false);
	intel_bb_sync(ibb);

	/* Buffer should be @ 0xc000_0000 */
	offset = intel_bb_get_object_offset(ibb, buf->handle);
	igt_assert_eq_u64(offset, 0xfffff000);

	ptr = gem_mmap__device_coherent(i915, ibb->handle, 0, ibb->size, PROT_READ);
	lo = ptr[1];
	hi = ptr[2];
	gem_munmap(ptr, ibb->size);

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					intel_buf_bo_size(buf), PROT_READ);
	val = ptr[0x2000 / sizeof(uint32_t)];
	gem_munmap(ptr, ibb->size);

	intel_buf_destroy(buf);
	intel_bb_destroy(ibb);

	/* Assert after all resources are freed */
	igt_assert_f(lo == 0x1000 && hi == 0x1,
		     "intel-bb doesn't properly handle delta in emit relocation\n");
	igt_assert_f(val == expected,
		     "Address doesn't contain expected [%x] value [%x]\n",
		     expected, val);
}

static void full_batch(struct buf_ops *bops)
{
	int i915 = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	int i;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	for (i = 0; i < PAGE_SIZE / sizeof(uint32_t) - 1; i++)
		intel_bb_out(ibb, 0);
	intel_bb_emit_bbe(ibb);

	igt_assert(intel_bb_offset(ibb) == PAGE_SIZE);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	intel_bb_destroy(ibb);
}

static int render(struct buf_ops *bops, uint32_t tiling, bool do_reloc,
		  uint32_t width, uint32_t height)
{
	struct intel_bb *ibb;
	struct intel_buf src, dst, final;
	int i915 = buf_ops_get_fd(bops);
	uint32_t fails = 0;
	char name[128];
	uint32_t devid = intel_get_drm_devid(i915);
	igt_render_copyfunc_t render_copy = NULL;

	igt_debug("%s() gen: %d\n", __func__, intel_gen(devid));

	/* Don't use relocations on gen12+ */
	igt_require((do_reloc && intel_gen(devid) < 12) ||
		    !do_reloc);

	if (do_reloc)
		ibb = intel_bb_create_with_relocs(i915, PAGE_SIZE);
	else
		ibb = intel_bb_create(i915, PAGE_SIZE);

	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	if (print_base64)
		intel_bb_set_dump_base64(ibb, true);

	scratch_buf_init(bops, &src, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &dst, width, height, tiling,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &final, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);

	scratch_buf_draw_pattern(bops, &src,
				 0, 0, width, height,
				 0, 0, width, height, 0);

	render_copy = igt_get_render_copyfunc(devid);
	igt_assert(render_copy);

	render_copy(ibb,
		    &src,
		    0, 0, width, height,
		    &dst,
		    0, 0);

	render_copy(ibb,
		    &dst,
		    0, 0, width, height,
		    &final,
		    0, 0);

	intel_bb_sync(ibb);
	intel_bb_destroy(ibb);

	if (write_png) {
		snprintf(name, sizeof(name) - 1,
			 "render_dst_tiling_%d.png", tiling);
		intel_buf_write_to_png(&src, "render_src_tiling_none.png");
		intel_buf_write_to_png(&dst, name);
		intel_buf_write_to_png(&final, "render_final_tiling_none.png");
	}

	/* We'll fail on src <-> final compare so just warn */
	if (tiling == I915_TILING_NONE) {
		if (compare_bufs(&src, &dst, false) > 0)
			igt_warn("%s: none->none failed!\n", __func__);
	} else {
		if (compare_bufs(&src, &dst, false) == 0)
			igt_warn("%s: none->tiled failed!\n", __func__);
	}

	fails = compare_bufs(&src, &final, true);

	if (fails && print_base64) {
		dump_base64("src", &src);
		dump_base64("dst", &dst);
		dump_base64("final", &final);
	}

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_buf_close(bops, &final);

	igt_assert_f(fails == 0, "%s: (tiling: %d) fails: %d\n",
		     __func__, tiling, fails);

	return fails;
}

static uint32_t count_compressed(int gen, struct intel_buf *buf)
{
	int i915 = buf_ops_get_fd(buf->bops);
	int ccs_size = intel_buf_ccs_width(gen, buf) * intel_buf_ccs_height(gen, buf);
	uint8_t *ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
						 intel_buf_bo_size(buf),
						 PROT_READ);
	uint32_t compressed = 0;
	int i;

	for (i = 0; i < ccs_size; i++)
		if (ptr[buf->ccs[0].offset + i])
			compressed++;

	munmap(ptr, intel_buf_bo_size(buf));

	return compressed;
}

#define IMGSIZE (512 * 512 * 4)
#define CCSIMGSIZE (IMGSIZE + 4096)
static void render_ccs(struct buf_ops *bops)
{
	struct intel_bb *ibb;
	const int width = 1024;
	const int height = 1024;
	struct intel_buf src, dst, final;
	int i915 = buf_ops_get_fd(bops);
	uint32_t fails = 0;
	uint32_t compressed = 0;
	uint32_t devid = intel_get_drm_devid(i915);
	igt_render_copyfunc_t render_copy = NULL;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	scratch_buf_init(bops, &src, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &dst, width, height, I915_TILING_Y,
			 I915_COMPRESSION_RENDER);
	scratch_buf_init(bops, &final, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);

	render_copy = igt_get_render_copyfunc(devid);
	igt_assert(render_copy);

	scratch_buf_draw_pattern(bops, &src,
				 0, 0, width, height,
				 0, 0, width, height, 0);

	render_copy(ibb,
		    &src,
		    0, 0, width, height,
		    &dst,
		    0, 0);

	render_copy(ibb,
		    &dst,
		    0, 0, width, height,
		    &final,
		    0, 0);

	intel_bb_sync(ibb);

	fails = compare_bufs(&src, &final, true);
	compressed = count_compressed(ibb->gen, &dst);

	intel_bb_destroy(ibb);

	igt_debug("fails: %u, compressed: %u\n", fails, compressed);

	if (write_png) {
		intel_buf_write_to_png(&src, "render-ccs-src.png");
		intel_buf_write_to_png(&dst, "render-ccs-dst.png");
		intel_buf_write_aux_to_png(&dst, "render-ccs-dst-aux.png");
		intel_buf_write_to_png(&final, "render-ccs-final.png");
	}

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);
	intel_buf_close(bops, &final);

	igt_assert_f(fails == 0, "render-ccs fails: %d\n", fails);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'd':
		debug_bb = true;
		break;
	case 'p':
		write_png = true;
		break;
	case 'i':
		buf_info = true;
		break;
	case 'b':
		print_base64 = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -d\tDebug bb\n"
	"  -p\tWrite surfaces to png\n"
	"  -i\tPrint buffer info\n"
	"  -b\tDump to base64 (bb and images)\n"
	;

igt_main_args("dpib", NULL, help_str, opt_handler, NULL)
{
	int i915, i, gen;
	struct buf_ops *bops;
	uint32_t width;

	struct test {
		uint32_t tiling;
		const char *tiling_name;
	} tests[] = {
		{ I915_TILING_NONE, "none" },
		{ I915_TILING_X, "x" },
		{ I915_TILING_Y, "y" },
	};

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		bops = buf_ops_create(i915);
		gen = intel_gen(intel_get_drm_devid(i915));
	}

	igt_subtest("simple-bb")
		simple_bb(bops, false);

	igt_subtest("simple-bb-ctx")
		simple_bb(bops, true);

	igt_subtest("lot-of-buffers")
		lot_of_buffers(bops);

	igt_subtest("check-canonical")
		check_canonical(bops);

	igt_subtest("reset-flags")
		reset_flags(bops);

	igt_subtest("blit-noreloc-keep-cache")
		blit(bops, NORELOC, KEEP_CACHE);

	igt_subtest("blit-reloc-purge-cache")
		blit(bops, RELOC, PURGE_CACHE);

	igt_subtest("blit-noreloc-purge-cache")
		blit(bops, NORELOC, PURGE_CACHE);

	igt_subtest("blit-reloc-keep-cache")
		blit(bops, RELOC, KEEP_CACHE);

	igt_subtest("intel-bb-blit-none")
		do_intel_bb_blit(bops, 10, I915_TILING_NONE);

	igt_subtest("intel-bb-blit-x")
		do_intel_bb_blit(bops, 10, I915_TILING_X);

	igt_subtest("intel-bb-blit-y") {
		igt_require(intel_gen(intel_get_drm_devid(i915)) >= 6);
		do_intel_bb_blit(bops, 10, I915_TILING_Y);
	}

	igt_subtest("offset-control")
		offset_control(bops);

	igt_subtest("delta-check")
		delta_check(bops);

	igt_subtest("full-batch")
		full_batch(bops);

	igt_subtest_with_dynamic("render") {
		for (i = 0; i < ARRAY_SIZE(tests); i++) {
			const struct test *t = &tests[i];

			for (width = 512; width <= 1024; width += 512) {
				igt_dynamic_f("render-%s-%u", t->tiling_name, width) {
					render(bops, t->tiling, false, width, width);
				}

				/* No relocs for gen12+ */
				if (gen >= 12)
					continue;

				igt_dynamic_f("render-%s-reloc-%u", t->tiling_name, width) {
					render(bops, t->tiling, true, width, width);
				}
			}
		}
	}

	igt_subtest("render-ccs")
		render_ccs(bops);

	igt_fixture {
		buf_ops_destroy(bops);
		close(i915);
	}
}
