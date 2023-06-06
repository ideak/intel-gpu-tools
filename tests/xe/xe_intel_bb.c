// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "igt.h"
#include "igt_crc.h"
#include "intel_bufops.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * TEST: Basic tests for intel-bb xe functionality
 * Category: Software building block
 * Sub-category: xe
 * Functionality: intel-bb
 * Test category: functionality test
 */

#define PAGE_SIZE 4096

#define WIDTH	64
#define HEIGHT	64
#define STRIDE	(WIDTH * 4)
#define SIZE	(HEIGHT * STRIDE)

#define COLOR_00	0x00
#define COLOR_33	0x33
#define COLOR_77	0x77
#define COLOR_CC	0xcc

IGT_TEST_DESCRIPTION("xe_intel_bb API check.");

static bool debug_bb;
static bool write_png;
static bool buf_info;
static bool print_base64;

static void *alloc_aligned(uint64_t size)
{
	void *p;

	igt_assert_eq(posix_memalign(&p, 16, size), 0);

	return p;
}

static void fill_buf(struct intel_buf *buf, uint8_t color)
{
	uint8_t *ptr;
	int xe = buf_ops_get_fd(buf->bops);
	int i;

	ptr = xe_bo_map(xe, buf->handle, buf->surface[0].size);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);
}

static void check_buf(struct intel_buf *buf, uint8_t color)
{
	uint8_t *ptr;
	int xe = buf_ops_get_fd(buf->bops);
	int i;

	ptr = xe_bo_map(xe, buf->handle, buf->surface[0].size);

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
	int xe = buf_ops_get_fd(buf->bops);

	ptr = xe_bo_map(xe, buf->handle, buf->surface[0].size);

	igt_debug("[%s] Buf handle: %d, size: %" PRIu64
		  ", v: 0x%02x, presumed_addr: %p\n",
		  name, buf->handle, buf->surface[0].size, ptr[0],
		  from_user_pointer(buf->addr.offset));
	munmap(ptr, buf->surface[0].size);
}

/**
 * SUBTEST: reset-bb
 * Description: check bb reset
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void reset_bb(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;

	ibb = intel_bb_create(xe, PAGE_SIZE);
	intel_bb_reset(ibb, false);
	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: purge-bb
 * Description: check bb reset == full (purge)
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void purge_bb(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_buf *buf;
	struct intel_bb *ibb;
	uint64_t offset0, offset1;

	buf = intel_buf_create(bops, 512, 512, 32, 0, I915_TILING_NONE,
			       I915_COMPRESSION_NONE);
	ibb = intel_bb_create(xe, 4096);
	intel_bb_set_debug(ibb, true);

	intel_bb_add_intel_buf(ibb, buf, false);
	offset0 = buf->addr.offset;

	intel_bb_reset(ibb, true);
	buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;

	intel_bb_add_intel_buf(ibb, buf, false);
	offset1 = buf->addr.offset;

	igt_assert(offset0 == offset1);

	intel_buf_destroy(buf);
	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: simple-%s
 * Description: Run simple bb xe %arg[1] test
 * Run type: BAT
 *
 * arg[1]:
 *
 * @bb:     bb
 * @bb-ctx: bb-ctx
 */
static void simple_bb(struct buf_ops *bops, bool new_context)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	uint32_t ctx = 0, vm = 0;

	ibb = intel_bb_create_with_allocator(xe, 0, 0, NULL, PAGE_SIZE,
					     INTEL_ALLOCATOR_SIMPLE);
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

	if (new_context) {
		vm = xe_vm_create(xe, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
		ctx = xe_engine_create(xe, vm, xe_hw_engine(xe, 0), 0);
		intel_bb_destroy(ibb);
		ibb = intel_bb_create_with_context(xe, ctx, vm, NULL, PAGE_SIZE);
		intel_bb_out(ibb, MI_BATCH_BUFFER_END);
		intel_bb_ptr_align(ibb, 8);
		intel_bb_exec(ibb, intel_bb_offset(ibb),
			      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC,
			      true);
		xe_engine_destroy(xe, ctx);
		xe_vm_destroy(xe, vm);
	}

	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: bb-with-allocator
 * Description: check bb with passed allocator
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void bb_with_allocator(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst;

	ibb = intel_bb_create_with_allocator(xe, 0, 0, NULL, PAGE_SIZE,
					     INTEL_ALLOCATOR_SIMPLE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	src = intel_buf_create(bops, 4096/32, 32, 8, 0, I915_TILING_NONE,
			       I915_COMPRESSION_NONE);
	dst = intel_buf_create(bops, 4096/32, 32, 8, 0, I915_TILING_NONE,
			       I915_COMPRESSION_NONE);

	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_copy_intel_buf(ibb, dst, src, 4096);
	intel_bb_remove_intel_buf(ibb, src);
	intel_bb_remove_intel_buf(ibb, dst);

	intel_buf_destroy(src);
	intel_buf_destroy(dst);
	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: lot-of-buffers
 * Description: check running bb with many buffers
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
#define NUM_BUFS 500
static void lot_of_buffers(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *buf[NUM_BUFS];
	int i;

	ibb = intel_bb_create(xe, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	for (i = 0; i < NUM_BUFS; i++) {
		buf[i] = intel_buf_create(bops, 4096, 1, 8, 0, I915_TILING_NONE,
					  I915_COMPRESSION_NONE);
		if (i % 2)
			intel_bb_add_intel_buf(ibb, buf[i], false);
		else
			intel_bb_add_intel_buf_with_alignment(ibb, buf[i],
							      0x4000, false);
	}

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);

	for (i = 0; i < NUM_BUFS; i++)
		intel_buf_destroy(buf[i]);

	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: add-remove-objects
 * Description: check bb object manipulation (add + remove)
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void add_remove_objects(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *mid, *dst;
	uint32_t offset;
	const uint32_t width = 512;
	const uint32_t height = 512;

	ibb = intel_bb_create(xe, PAGE_SIZE);
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
	intel_bb_remove_intel_buf(ibb, mid);
	intel_bb_remove_intel_buf(ibb, mid);
	intel_bb_remove_intel_buf(ibb, mid);
	intel_bb_add_intel_buf(ibb, dst, true);

	offset = intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, offset,
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);

	intel_buf_destroy(src);
	intel_buf_destroy(mid);
	intel_buf_destroy(dst);
	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: destroy-bb
 * Description: check bb destroy/create
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void destroy_bb(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *mid, *dst;
	uint32_t offset;
	const uint32_t width = 512;
	const uint32_t height = 512;

	ibb = intel_bb_create(xe, PAGE_SIZE);
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
	intel_bb_add_intel_buf(ibb, dst, true);

	offset = intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, offset,
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);

	/* Check destroy will detach intel_bufs */
	intel_bb_destroy(ibb);
	igt_assert(src->addr.offset == INTEL_BUF_INVALID_ADDRESS);
	igt_assert(src->ibb == NULL);
	igt_assert(mid->addr.offset == INTEL_BUF_INVALID_ADDRESS);
	igt_assert(mid->ibb == NULL);
	igt_assert(dst->addr.offset == INTEL_BUF_INVALID_ADDRESS);
	igt_assert(dst->ibb == NULL);

	ibb = intel_bb_create(xe, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	intel_bb_add_intel_buf(ibb, src, false);
	offset = intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, offset,
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);

	intel_bb_destroy(ibb);
	intel_buf_destroy(src);
	intel_buf_destroy(mid);
	intel_buf_destroy(dst);
}

/**
 * SUBTEST: create-in-region
 * Description: check size validation on available regions
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void create_in_region(struct buf_ops *bops, uint64_t region)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf buf = {};
	uint32_t handle, offset;
	uint64_t size;
	int width = 64;
	int height = 64;

	ibb = intel_bb_create(xe, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	size = xe_min_page_size(xe, system_memory(xe));
	handle = xe_bo_create_flags(xe, 0, size, system_memory(xe));
	intel_buf_init_full(bops, handle, &buf,
			    width/4, height, 32, 0,
			    I915_TILING_NONE, 0,
			    size, 0, region);
	intel_buf_set_ownership(&buf, true);

	intel_bb_add_intel_buf(ibb, &buf, false);
	offset = intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, offset,
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);

	intel_buf_close(bops, &buf);
	intel_bb_destroy(ibb);
}

static void __emit_blit(struct intel_bb *ibb,
			 struct intel_buf *src, struct intel_buf *dst)
{
	intel_bb_emit_blt_copy(ibb,
			       src, 0, 0, src->surface[0].stride,
			       dst, 0, 0, dst->surface[0].stride,
			       intel_buf_width(dst),
			       intel_buf_height(dst),
			       dst->bpp);
}

/**
 * SUBTEST: blit-%s
 * Description: Run blit on %arg[1] allocator
 * Run type: BAT
 *
 * arg[1]:
 *
 * @simple:				simple
 * @reloc:				reloc
 */
static void blit(struct buf_ops *bops, uint8_t allocator_type)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst;
	uint64_t poff_src, poff_dst;
	uint64_t flags = 0;

	ibb = intel_bb_create_with_allocator(xe, 0, 0, NULL, PAGE_SIZE,
					     allocator_type);
	flags |= I915_EXEC_NO_RELOC;

	src = create_buf(bops, WIDTH, HEIGHT, COLOR_CC);
	dst = create_buf(bops, WIDTH, HEIGHT, COLOR_00);

	if (buf_info) {
		print_buf(src, "src");
		print_buf(dst, "dst");
	}

	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	__emit_blit(ibb, src, dst);
	intel_bb_emit_bbe(ibb);
	intel_bb_flush_blit(ibb);
	intel_bb_sync(ibb);
	intel_bb_reset(ibb, false);
	check_buf(dst, COLOR_CC);

	poff_src = intel_bb_get_object_offset(ibb, src->handle);
	poff_dst = intel_bb_get_object_offset(ibb, dst->handle);

	/* Add buffers again */
	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, dst, true);

	igt_assert_f(poff_src == src->addr.offset,
		     "prev src addr: %" PRIx64 " <> src addr %" PRIx64 "\n",
		     poff_src, src->addr.offset);
	igt_assert_f(poff_dst == dst->addr.offset,
		     "prev dst addr: %" PRIx64 " <> dst addr %" PRIx64 "\n",
		     poff_dst, dst->addr.offset);

	fill_buf(src, COLOR_77);
	fill_buf(dst, COLOR_00);

	__emit_blit(ibb, src, dst);
	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);
	check_buf(dst, COLOR_77);

	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);
	check_buf(dst, COLOR_77);

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
	int fd = buf_ops_get_fd(bops);
	int bpp = 32;

	/*
	 * We use system memory even if vram is possible because wc mapping
	 * is extremely slow.
	 */
	intel_buf_init_in_region(bops, buf, width, height, bpp, 0,
				 req_tiling, compression,
				 system_memory(fd));

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
		if (hist[i])
			igt_debug("[group %4x]: %d\n", i, hist[i]);
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

	ptr1 = xe_bo_map(fd1, buf1->handle, buf1->surface[0].size);
	ptr2 = xe_bo_map(fd2, buf2->handle, buf2->surface[0].size);
	ret = memcmp(ptr1, ptr2, buf1->surface[0].size);
	if (detail_compare)
		ret = compare_detail(ptr1, ptr2, buf1->surface[0].size);

	munmap(ptr1, buf1->surface[0].size);
	munmap(ptr2, buf2->surface[0].size);

	return ret;
}

#define LINELEN 76ul
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
		igt_info("compressed %" PRIu64 " -> %lu\n",
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
	int xe = buf_ops_get_fd(bops), fails;

	ibb = intel_bb_create(xe, PAGE_SIZE);
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

/**
 * SUBTEST: intel-bb-blit-%s
 * Description: Run simple bb xe %arg[1] test
 * Run type: BAT
 *
 * arg[1]:
 *
 * @none:				none
 * @x:					x
 * @y:					y
 */
static void do_intel_bb_blit(struct buf_ops *bops, int loops, uint32_t tiling)
{
	int i, fails = 0, xe = buf_ops_get_fd(bops);

	/* We'll fix it for gen2/3 later. */
	igt_require(intel_gen(intel_get_drm_devid(xe)) > 3);

	for (i = 0; i < loops; i++)
		fails += __do_intel_bb_blit(bops, tiling);

	igt_assert_f(fails == 0, "intel-bb-blit (tiling: %d) fails: %d\n",
		     tiling, fails);
}

/**
 * SUBTEST: offset-control
 * Description: check offset is kept on default simple allocator
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void offset_control(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	struct intel_buf *src, *dst1, *dst2, *dst3;
	uint64_t poff_src, poff_dst1, poff_dst2;

	ibb = intel_bb_create(xe, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	src = create_buf(bops, WIDTH, HEIGHT, COLOR_CC);
	dst1 = create_buf(bops, WIDTH, HEIGHT, COLOR_00);
	dst2 = create_buf(bops, WIDTH, HEIGHT, COLOR_77);

	intel_bb_add_object(ibb, src->handle, intel_buf_bo_size(src),
			    src->addr.offset, 0, false);
	intel_bb_add_object(ibb, dst1->handle, intel_buf_bo_size(dst1),
			    dst1->addr.offset, 0, true);
	intel_bb_add_object(ibb, dst2->handle, intel_buf_bo_size(dst2),
			    dst2->addr.offset, 0, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	if (buf_info) {
		print_buf(src, "src ");
		print_buf(dst1, "dst1");
		print_buf(dst2, "dst2");
	}

	poff_src = src->addr.offset;
	poff_dst1 = dst1->addr.offset;
	poff_dst2 = dst2->addr.offset;
	intel_bb_reset(ibb, true);

	dst3 = create_buf(bops, WIDTH, HEIGHT, COLOR_33);
	intel_bb_add_object(ibb, dst3->handle, intel_buf_bo_size(dst3),
			    dst3->addr.offset, 0, true);
	intel_bb_add_object(ibb, src->handle, intel_buf_bo_size(src),
			    src->addr.offset, 0, false);
	intel_bb_add_object(ibb, dst1->handle, intel_buf_bo_size(dst1),
			    dst1->addr.offset, 0, true);
	intel_bb_add_object(ibb, dst2->handle, intel_buf_bo_size(dst2),
			    dst2->addr.offset, 0, true);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);
	intel_bb_sync(ibb);
	intel_bb_reset(ibb, true);

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

/**
 * SUBTEST: delta-check
 * Description: check delta is honoured in intel-bb pipelines
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
#define DELTA_BUFFERS 3
static void delta_check(struct buf_ops *bops)
{
	const uint32_t expected = 0x1234abcd;
	int xe = buf_ops_get_fd(bops);
	uint32_t *ptr, hi, lo, val;
	struct intel_buf *buf;
	struct intel_bb *ibb;
	uint64_t offset;
	uint64_t obj_size = xe_get_default_alignment(xe) + 0x2000;
	uint64_t obj_offset = (1ULL << 32) - xe_get_default_alignment(xe);
	uint64_t delta = xe_get_default_alignment(xe) + 0x1000;

	ibb = intel_bb_create_with_allocator(xe, 0, 0, NULL, PAGE_SIZE,
					     INTEL_ALLOCATOR_SIMPLE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	buf = create_buf(bops, obj_size, 0x1, COLOR_CC);
	buf->addr.offset = obj_offset;
	intel_bb_add_object(ibb, buf->handle, intel_buf_bo_size(buf),
			    buf->addr.offset, 0, false);

	intel_bb_out(ibb, MI_STORE_DWORD_IMM_GEN4);
	intel_bb_emit_reloc(ibb, buf->handle,
			    I915_GEM_DOMAIN_RENDER,
			    I915_GEM_DOMAIN_RENDER,
			    delta, buf->addr.offset);
	intel_bb_out(ibb, expected);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb), I915_EXEC_DEFAULT, false);
	intel_bb_sync(ibb);

	/* Buffer should be @ obj_offset */
	offset = intel_bb_get_object_offset(ibb, buf->handle);
	igt_assert_eq_u64(offset, obj_offset);

	ptr = xe_bo_map(xe, ibb->handle, ibb->size);
	lo = ptr[1];
	hi = ptr[2];
	gem_munmap(ptr, ibb->size);

	ptr = xe_bo_map(xe, buf->handle, intel_buf_size(buf));
	val = ptr[delta / sizeof(uint32_t)];
	gem_munmap(ptr, intel_buf_size(buf));

	intel_buf_destroy(buf);
	intel_bb_destroy(ibb);

	/* Assert after all resources are freed */
	igt_assert_f(lo == 0x1000 && hi == 0x1,
		     "intel-bb doesn't properly handle delta in emit relocation\n");
	igt_assert_f(val == expected,
		     "Address doesn't contain expected [%x] value [%x]\n",
		     expected, val);
}

/**
 * SUBTEST: full-batch
 * Description: check bb totally filled is executing correct
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static void full_batch(struct buf_ops *bops)
{
	int xe = buf_ops_get_fd(bops);
	struct intel_bb *ibb;
	int i;

	ibb = intel_bb_create(xe, PAGE_SIZE);
	if (debug_bb)
		intel_bb_set_debug(ibb, true);

	for (i = 0; i < PAGE_SIZE / sizeof(uint32_t) - 1; i++)
		intel_bb_out(ibb, 0);
	intel_bb_emit_bbe(ibb);

	igt_assert(intel_bb_offset(ibb) == PAGE_SIZE);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);
	intel_bb_reset(ibb, false);

	intel_bb_destroy(ibb);
}

/**
 * SUBTEST: render
 * Description: check intel-bb render pipeline
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */
static int render(struct buf_ops *bops, uint32_t tiling,
		  uint32_t width, uint32_t height)
{
	struct intel_bb *ibb;
	struct intel_buf src, dst, final;
	int xe = buf_ops_get_fd(bops);
	uint32_t fails = 0;
	char name[128];
	uint32_t devid = intel_get_drm_devid(xe);
	igt_render_copyfunc_t render_copy = NULL;

	igt_debug("%s() gen: %d\n", __func__, intel_gen(devid));

	ibb = intel_bb_create(xe, PAGE_SIZE);

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
	int xe, i;
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
		xe = drm_open_driver(DRIVER_XE);
		bops = buf_ops_create(xe);
		xe_device_get(xe);
	}

	igt_describe("Ensure reset is possible on fresh bb");
	igt_subtest("reset-bb")
		reset_bb(bops);

	igt_subtest_f("purge-bb")
		purge_bb(bops);

	igt_subtest("simple-bb")
		simple_bb(bops, false);

	igt_subtest("simple-bb-ctx")
		simple_bb(bops, true);

	igt_subtest("bb-with-allocator")
		bb_with_allocator(bops);

	igt_subtest("lot-of-buffers")
		lot_of_buffers(bops);

	igt_subtest("add-remove-objects")
		add_remove_objects(bops);

	igt_subtest("destroy-bb")
		destroy_bb(bops);

	igt_subtest_with_dynamic("create-in-region") {
		uint64_t memreg = all_memory_regions(xe), region;

		xe_for_each_mem_region(fd, memreg, region)
			igt_dynamic_f("region-%s", xe_region_name(region))
				create_in_region(bops, region);
	}

	igt_subtest("blit-simple")
		blit(bops, INTEL_ALLOCATOR_SIMPLE);

	igt_subtest("blit-reloc")
		blit(bops, INTEL_ALLOCATOR_RELOC);

	igt_subtest("intel-bb-blit-none")
		do_intel_bb_blit(bops, 3, I915_TILING_NONE);

	igt_subtest("intel-bb-blit-x")
		do_intel_bb_blit(bops, 3, I915_TILING_X);

	igt_subtest("intel-bb-blit-y") {
		igt_require(intel_gen(intel_get_drm_devid(xe)) >= 6);
		do_intel_bb_blit(bops, 3, I915_TILING_Y);
	}

	igt_subtest("offset-control")
		offset_control(bops);

	igt_subtest("delta-check")
		delta_check(bops);

	igt_subtest("full-batch")
		full_batch(bops);

	igt_subtest_with_dynamic("render") {
		igt_require(xe_has_engine_class(xe, DRM_XE_ENGINE_CLASS_RENDER));

		for (i = 0; i < ARRAY_SIZE(tests); i++) {
			const struct test *t = &tests[i];

			for (width = 512; width <= 1024; width += 512)
				igt_dynamic_f("render-%s-%u", t->tiling_name, width)
					render(bops, t->tiling, width, width);
		}
	}

	igt_fixture {
		xe_device_put(xe);
		buf_ops_destroy(bops);
		close(xe);
	}
}
