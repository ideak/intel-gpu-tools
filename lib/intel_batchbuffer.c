/**************************************************************************
 *
 * Copyright 2006 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <search.h>
#include <glib.h>

#include "gpgpu_fill.h"
#include "huc_copy.h"
#include "i915/gem_create.h"
#include "i915/gem_mman.h"
#include "i915/i915_blt.h"
#include "igt_aux.h"
#include "igt_syncobj.h"
#include "intel_batchbuffer.h"
#include "intel_bufops.h"
#include "intel_chipset.h"
#include "media_fill.h"
#include "media_spin.h"
#include "sw_sync.h"
#include "veboxcopy.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define BCS_SWCTRL 0x22200
#define BCS_SRC_Y (1 << 0)
#define BCS_DST_Y (1 << 1)

/**
 * SECTION:intel_batchbuffer
 * @short_description: Batchbuffer and blitter support
 * @title: Batch Buffer
 * @include: igt.h
 *
 * Note that this library's header pulls in the [i-g-t core](igt-gpu-tools-i-g-t-core.html)
 * library as a dependency.
 */

static bool intel_bb_do_tracking;
static IGT_LIST_HEAD(intel_bb_list);
static pthread_mutex_t intel_bb_list_lock = PTHREAD_MUTEX_INITIALIZER;

#define CMD_POLY_STIPPLE_OFFSET       0x7906

#define CHECK_RANGE(x) do { \
	igt_assert_lte(0, (x)); \
	igt_assert_lt((x), (1 << 15)); \
} while (0)

/*
 * pitches are in bytes if the surfaces are linear, number of dwords
 * otherwise
 */
static uint32_t fast_copy_pitch(unsigned int stride, unsigned int tiling)
{
	if (tiling != I915_TILING_NONE)
		return stride / 4;
	else
		return stride;
}

uint32_t fast_copy_dword0(unsigned int src_tiling,
			  unsigned int dst_tiling)
{
	uint32_t dword0 = 0;

	dword0 |= XY_FAST_COPY_BLT;

	switch (src_tiling) {
	case I915_TILING_X:
		dword0 |= XY_FAST_COPY_SRC_TILING_X;
		break;
	case I915_TILING_Y:
	case I915_TILING_4:
	case I915_TILING_Yf:
		dword0 |= XY_FAST_COPY_SRC_TILING_Yb_Yf;
		break;
	case I915_TILING_Ys:
		dword0 |= XY_FAST_COPY_SRC_TILING_Ys;
		break;
	case I915_TILING_NONE:
	default:
		break;
	}

	switch (dst_tiling) {
	case I915_TILING_X:
		dword0 |= XY_FAST_COPY_DST_TILING_X;
		break;
	case I915_TILING_Y:
	case I915_TILING_4:
	case I915_TILING_Yf:
		dword0 |= XY_FAST_COPY_DST_TILING_Yb_Yf;
		break;
	case I915_TILING_Ys:
		dword0 |= XY_FAST_COPY_DST_TILING_Ys;
		break;
	case I915_TILING_NONE:
	default:
		break;
	}

	return dword0;
}

static bool new_tile_y_format(unsigned int tiling)
{
	return tiling == T_YFMAJOR || tiling == T_TILE4;
}

uint32_t fast_copy_dword1(int fd, unsigned int src_tiling,
			  unsigned int dst_tiling,
			  int bpp)
{
	uint32_t dword1 = 0;

	if (blt_fast_copy_supports_tiling(fd, T_YMAJOR)) {
		dword1 |= new_tile_y_format(src_tiling)
				? XY_FAST_COPY_SRC_TILING_Yf : 0;
		dword1 |= new_tile_y_format(dst_tiling)
				? XY_FAST_COPY_DST_TILING_Yf : 0;
	} else {
		/* Always set bits for platforms that don't support legacy TileY */
		dword1 |= XY_FAST_COPY_SRC_TILING_Yf | XY_FAST_COPY_DST_TILING_Yf;
	}

	switch (bpp) {
	case 8:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_8;
		break;
	case 16:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_16;
		break;
	case 32:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_32;
		break;
	case 64:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_64;
		break;
	case 128:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_128;
		break;
	default:
		igt_assert(0);
	}

	return dword1;
}

static void
fill_relocation(struct drm_i915_gem_relocation_entry *reloc,
		uint32_t gem_handle, uint64_t presumed_offset,
		uint32_t delta, /* in bytes */
		uint32_t offset, /* in dwords */
		uint32_t read_domains, uint32_t write_domains)
{
	reloc->target_handle = gem_handle;
	reloc->delta = delta;
	reloc->offset = offset * sizeof(uint32_t);
	reloc->presumed_offset = presumed_offset;
	reloc->read_domains = read_domains;
	reloc->write_domain = write_domains;
}

static void
fill_object(struct drm_i915_gem_exec_object2 *obj,
	    uint32_t gem_handle, uint64_t gem_offset,
	    struct drm_i915_gem_relocation_entry *relocs, uint32_t count)
{
	memset(obj, 0, sizeof(*obj));
	obj->handle = gem_handle;
	obj->offset = gem_offset;
	obj->relocation_count = count;
	obj->relocs_ptr = to_user_pointer(relocs);
}

static uint32_t find_engine(const intel_ctx_cfg_t *cfg, unsigned int class)
{
	unsigned int i;
	uint32_t engine_id = -1;

	for (i = 0; i < cfg->num_engines; i++) {
		if (cfg->engines[i].engine_class == class)
			engine_id = i;
	}

	igt_assert_f(engine_id != -1, "Requested engine not found!\n");

	return engine_id;
}

static void exec_blit(int fd,
		      struct drm_i915_gem_exec_object2 *objs,
		      uint32_t count, unsigned int gen,
		      uint32_t ctx, const intel_ctx_cfg_t *cfg)
{
	struct drm_i915_gem_execbuffer2 exec;
	uint32_t devid = intel_get_drm_devid(fd);
	uint32_t blt_id = HAS_BLT_RING(devid) ? I915_EXEC_BLT : I915_EXEC_DEFAULT;

	if (cfg)
		blt_id = find_engine(cfg, I915_ENGINE_CLASS_COPY);

	exec = (struct drm_i915_gem_execbuffer2) {
		.buffers_ptr = to_user_pointer(objs),
		.buffer_count = count,
		.flags = blt_id | I915_EXEC_NO_RELOC,
		.rsvd1 = ctx,
	};

	gem_execbuf(fd, &exec);
}

static uint32_t src_copy_dword0(uint32_t src_tiling, uint32_t dst_tiling,
				uint32_t bpp, uint32_t device_gen)
{
	uint32_t dword0 = 0;

	dword0 |= XY_SRC_COPY_BLT_CMD;
	if (bpp == 32)
		dword0 |= XY_SRC_COPY_BLT_WRITE_RGB |
			XY_SRC_COPY_BLT_WRITE_ALPHA;

	if (device_gen >= 4 && src_tiling)
		dword0 |= XY_SRC_COPY_BLT_SRC_TILED;
	if (device_gen >= 4 && dst_tiling)
		dword0 |= XY_SRC_COPY_BLT_DST_TILED;

	return dword0;
}

static uint32_t src_copy_dword1(uint32_t dst_pitch, uint32_t bpp)
{
	uint32_t dword1 = 0;

	switch (bpp) {
	case 8:
		break;
	case 16:
		dword1 |= 1 << 24; /* Only support 565 color */
		break;
	case 32:
		dword1 |= 3 << 24;
		break;
	default:
		igt_assert(0);
	}

	dword1 |= 0xcc << 16;
	dword1 |= dst_pitch;

	return dword1;
}

/**
 * igt_blitter_copy:
 * @fd: file descriptor of the i915 driver
 * @ahnd: handle to an allocator
 * @ctx: context within which execute copy blit
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @src_size: size of the src bo required for allocator and softpin
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 * @dst_size: size of the dst bo required for allocator and softpin
 *
 * Wrapper API to call appropriate blitter copy function.
 */

void igt_blitter_copy(int fd,
		      uint64_t ahnd,
		      uint32_t ctx,
		      const intel_ctx_cfg_t *cfg,
		      /* src */
		      uint32_t src_handle,
		      uint32_t src_delta,
		      uint32_t src_stride,
		      uint32_t src_tiling,
		      uint32_t src_x, uint32_t src_y,
		      uint64_t src_size,
		      /* size */
		      uint32_t width, uint32_t height,
		      /* bpp */
		      uint32_t bpp,
		      /* dst */
		      uint32_t dst_handle,
		      uint32_t dst_delta,
		      uint32_t dst_stride,
		      uint32_t dst_tiling,
		      uint32_t dst_x, uint32_t dst_y,
		      uint64_t dst_size)
{
	uint32_t devid;

	devid = intel_get_drm_devid(fd);

	if (intel_graphics_ver(devid) >= IP_VER(12, 60))
		igt_blitter_fast_copy__raw(fd, ahnd, ctx, NULL,
					   src_handle, src_delta,
					   src_stride, src_tiling,
					   src_x, src_y, src_size,
					   width, height, bpp,
					   dst_handle, dst_delta,
					   dst_stride, dst_tiling,
					   dst_x, dst_y, dst_size);
	else
		igt_blitter_src_copy(fd, ahnd, ctx, NULL,
				     src_handle, src_delta,
				     src_stride, src_tiling,
				     src_x, src_y, src_size,
				     width, height, bpp,
				     dst_handle, dst_delta,
				     dst_stride, dst_tiling,
				     dst_x, dst_y, dst_size);
}
/**
 * igt_blitter_src_copy:
 * @fd: file descriptor of the i915 driver
 * @ahnd: handle to an allocator
 * @ctx: context within which execute copy blit
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @src_size: size of the src bo required for allocator and softpin
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 * @dst_size: size of the dst bo required for allocator and softpin
 *
 * Copy @src into @dst using the XY_SRC blit command.
 */
void igt_blitter_src_copy(int fd,
			  uint64_t ahnd,
			  uint32_t ctx,
			  const intel_ctx_cfg_t *cfg,
			  /* src */
			  uint32_t src_handle,
			  uint32_t src_delta,
			  uint32_t src_stride,
			  uint32_t src_tiling,
			  uint32_t src_x, uint32_t src_y,
			  uint64_t src_size,

			  /* size */
			  uint32_t width, uint32_t height,

			  /* bpp */
			  uint32_t bpp,

			  /* dst */
			  uint32_t dst_handle,
			  uint32_t dst_delta,
			  uint32_t dst_stride,
			  uint32_t dst_tiling,
			  uint32_t dst_x, uint32_t dst_y,
			  uint64_t dst_size)
{
	uint32_t batch[32];
	struct drm_i915_gem_exec_object2 objs[3];
	struct drm_i915_gem_relocation_entry relocs[2];
	uint32_t batch_handle;
	uint32_t src_pitch, dst_pitch;
	uint32_t dst_reloc_offset, src_reloc_offset;
	uint32_t gen = intel_gen(intel_get_drm_devid(fd));
	uint64_t batch_offset, src_offset, dst_offset;
	const bool has_64b_reloc = gen >= 8;
	int i = 0;

	batch_handle = gem_create(fd, 4096);
	if (ahnd) {
		src_offset = get_offset(ahnd, src_handle, src_size, 0);
		dst_offset = get_offset(ahnd, dst_handle, dst_size, 0);
		batch_offset = get_offset(ahnd, batch_handle, 4096, 0);
	} else {
		src_offset = 16 << 20;
		dst_offset = ALIGN(src_offset + src_size, 1 << 20);
		batch_offset = ALIGN(dst_offset + dst_size, 1 << 20);
	}

	memset(batch, 0, sizeof(batch));

	igt_assert((src_tiling == I915_TILING_NONE) ||
		   (src_tiling == I915_TILING_X) ||
		   (src_tiling == I915_TILING_Y));
	igt_assert((dst_tiling == I915_TILING_NONE) ||
		   (dst_tiling == I915_TILING_X) ||
		   (dst_tiling == I915_TILING_Y));

	src_pitch = (gen >= 4 && src_tiling) ? src_stride / 4 : src_stride;
	dst_pitch = (gen >= 4 && dst_tiling) ? dst_stride / 4 : dst_stride;

	if (bpp == 64) {
		bpp /= 2;
		width *= 2;
	}

	CHECK_RANGE(src_x); CHECK_RANGE(src_y);
	CHECK_RANGE(dst_x); CHECK_RANGE(dst_y);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x + width); CHECK_RANGE(src_y + height);
	CHECK_RANGE(dst_x + width); CHECK_RANGE(dst_y + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	if ((src_tiling | dst_tiling) >= I915_TILING_Y) {
		unsigned int mask;

		batch[i++] = MI_LOAD_REGISTER_IMM(1);
		batch[i++] = BCS_SWCTRL;

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src_tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst_tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		batch[i++] = mask;
	}

	batch[i] = src_copy_dword0(src_tiling, dst_tiling, bpp, gen);
	batch[i++] |= 6 + 2 * has_64b_reloc;
	batch[i++] = src_copy_dword1(dst_pitch, bpp);
	batch[i++] = (dst_y << 16) | dst_x; /* dst x1,y1 */
	batch[i++] = ((dst_y + height) << 16) | (dst_x + width); /* dst x2,y2 */
	dst_reloc_offset = i;
	batch[i++] = dst_offset + dst_delta; /* dst address lower bits */
	if (has_64b_reloc)
		batch[i++] = (dst_offset + dst_delta) >> 32; /* dst address upper bits */
	batch[i++] = (src_y << 16) | src_x; /* src x1,y1 */
	batch[i++] = src_pitch;
	src_reloc_offset = i;
	batch[i++] = src_offset + src_delta; /* src address lower bits */
	if (has_64b_reloc)
		batch[i++] = (src_offset + src_delta) >> 32; /* src address upper bits */

	if ((src_tiling | dst_tiling) >= I915_TILING_Y) {
		igt_assert(gen >= 6);
		batch[i++] = MI_FLUSH_DW_CMD | 2;
		batch[i++] = 0;
		batch[i++] = 0;
		batch[i++] = 0;

		batch[i++] = MI_LOAD_REGISTER_IMM(1);
		batch[i++] = BCS_SWCTRL;
		batch[i++] = (BCS_SRC_Y | BCS_DST_Y) << 16;
	}

	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	igt_assert(i <= ARRAY_SIZE(batch));

	gem_write(fd, batch_handle, 0, batch, sizeof(batch));

	fill_relocation(&relocs[0], dst_handle, dst_offset,
			dst_delta, dst_reloc_offset,
			I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
	fill_relocation(&relocs[1], src_handle, src_offset,
			src_delta, src_reloc_offset,
			I915_GEM_DOMAIN_RENDER, 0);

	fill_object(&objs[0], dst_handle, dst_offset, NULL, 0);
	fill_object(&objs[1], src_handle, src_offset, NULL, 0);
	fill_object(&objs[2], batch_handle, batch_offset, relocs, !ahnd ? 2 : 0);

	objs[0].flags |= EXEC_OBJECT_NEEDS_FENCE | EXEC_OBJECT_WRITE;
	objs[1].flags |= EXEC_OBJECT_NEEDS_FENCE;

	if (ahnd) {
		objs[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		objs[1].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		objs[2].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	exec_blit(fd, objs, 3, gen, ctx, cfg);

	gem_close(fd, batch_handle);
}

/**
 * igt_blitter_fast_copy__raw:
 * @fd: file descriptor of the i915 driver
 * @ahnd: handle to an allocator
 * @ctx: context within which execute copy blit
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @src_size: size of the src bo required for allocator and softpin
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 * @dst_size: size of the dst bo required for allocator and softpin
 *
 * Like igt_blitter_fast_copy(), but talking to the kernel directly.
 */
void igt_blitter_fast_copy__raw(int fd,
				uint64_t ahnd,
				uint32_t ctx,
				const intel_ctx_cfg_t *cfg,
				/* src */
				uint32_t src_handle,
				unsigned int src_delta,
				unsigned int src_stride,
				unsigned int src_tiling,
				unsigned int src_x, unsigned src_y,
				uint64_t src_size,

				/* size */
				unsigned int width, unsigned int height,

				/* bpp */
				int bpp,

				/* dst */
				uint32_t dst_handle,
				unsigned dst_delta,
				unsigned int dst_stride,
				unsigned int dst_tiling,
				unsigned int dst_x, unsigned dst_y,
				uint64_t dst_size)
{
	uint32_t batch[12];
	struct drm_i915_gem_exec_object2 objs[3];
	struct drm_i915_gem_relocation_entry relocs[2];
	uint32_t batch_handle;
	uint32_t dword0, dword1;
	uint32_t src_pitch, dst_pitch;
	uint64_t batch_offset, src_offset, dst_offset;
	int i = 0;

	batch_handle = gem_create(fd, 4096);
	if (ahnd) {
		src_offset = get_offset(ahnd, src_handle, src_size, 0);
		dst_offset = get_offset(ahnd, dst_handle, dst_size, 0);
		batch_offset = get_offset(ahnd, batch_handle, 4096, 0);
	} else {
		src_offset = 16 << 20;
		dst_offset = ALIGN(src_offset + src_size, 1 << 20);
		batch_offset = ALIGN(dst_offset + dst_size, 1 << 20);
	}

	src_pitch = fast_copy_pitch(src_stride, src_tiling);
	dst_pitch = fast_copy_pitch(dst_stride, dst_tiling);
	dword0 = fast_copy_dword0(src_tiling, dst_tiling);
	dword1 = fast_copy_dword1(fd, src_tiling, dst_tiling, bpp);

	CHECK_RANGE(src_x); CHECK_RANGE(src_y);
	CHECK_RANGE(dst_x); CHECK_RANGE(dst_y);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x + width); CHECK_RANGE(src_y + height);
	CHECK_RANGE(dst_x + width); CHECK_RANGE(dst_y + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	batch[i++] = dword0;
	batch[i++] = dword1 | dst_pitch;
	batch[i++] = (dst_y << 16) | dst_x; /* dst x1,y1 */
	batch[i++] = ((dst_y + height) << 16) | (dst_x + width); /* dst x2,y2 */
	batch[i++] = dst_offset + dst_delta; /* dst address lower bits */
	batch[i++] = (dst_offset + dst_delta) >> 32; /* dst address upper bits */
	batch[i++] = (src_y << 16) | src_x; /* src x1,y1 */
	batch[i++] = src_pitch;
	batch[i++] = src_offset + src_delta; /* src address lower bits */
	batch[i++] = (src_offset + src_delta) >> 32; /* src address upper bits */
	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	igt_assert(i == ARRAY_SIZE(batch));

	gem_write(fd, batch_handle, 0, batch, sizeof(batch));

	fill_relocation(&relocs[0], dst_handle, dst_offset, dst_delta, 4,
			I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
	fill_relocation(&relocs[1], src_handle, src_offset, src_delta, 8,
			I915_GEM_DOMAIN_RENDER, 0);

	fill_object(&objs[0], dst_handle, dst_offset, NULL, 0);
	objs[0].flags |= EXEC_OBJECT_WRITE;
	fill_object(&objs[1], src_handle, src_offset, NULL, 0);
	fill_object(&objs[2], batch_handle, batch_offset, relocs, !ahnd ? 2 : 0);

	if (ahnd) {
		objs[0].flags |= EXEC_OBJECT_PINNED;
		objs[1].flags |= EXEC_OBJECT_PINNED;
		objs[2].flags |= EXEC_OBJECT_PINNED;
	}

	exec_blit(fd, objs, 3, intel_gen(intel_get_drm_devid(fd)), ctx, cfg);

	gem_close(fd, batch_handle);
}

/**
 * igt_get_render_copyfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific render copy function pointer for the device
 * specified with @devid. Will return NULL when no render copy function is
 * implemented.
 */
igt_render_copyfunc_t igt_get_render_copyfunc(int devid)
{
	igt_render_copyfunc_t copy = NULL;

	if (IS_GEN2(devid))
		copy = gen2_render_copyfunc;
	else if (IS_GEN3(devid))
		copy = gen3_render_copyfunc;
	else if (IS_GEN4(devid) || IS_GEN5(devid))
		copy = gen4_render_copyfunc;
	else if (IS_GEN6(devid))
		copy = gen6_render_copyfunc;
	else if (IS_GEN7(devid))
		copy = gen7_render_copyfunc;
	else if (IS_GEN8(devid))
		copy = gen8_render_copyfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid))
		copy = gen9_render_copyfunc;
	else if (IS_GEN11(devid))
		copy = gen11_render_copyfunc;
	else if (HAS_4TILE(devid))
		copy = gen12p71_render_copyfunc;
	else if (IS_GEN12(devid))
		copy = gen12_render_copyfunc;

	return copy;
}

igt_vebox_copyfunc_t igt_get_vebox_copyfunc(int devid)
{
	igt_vebox_copyfunc_t copy = NULL;

	if (IS_GEN12(devid))
		copy = gen12_vebox_copyfunc;

	return copy;
}

igt_render_clearfunc_t igt_get_render_clearfunc(int devid)
{
	if (IS_DG2(devid)) {
		return gen12p71_render_clearfunc;
	} else if (IS_GEN12(devid)) {
		return gen12_render_clearfunc;
	} else {
		return NULL;
	}
}

/**
 * igt_get_media_fillfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific media fill function pointer for the device specified
 * with @devid. Will return NULL when no media fill function is implemented.
 */
igt_fillfunc_t igt_get_media_fillfunc(int devid)
{
	igt_fillfunc_t fill = NULL;

	if (intel_graphics_ver(devid) >= IP_VER(12, 50)) {
		/* current implementation defeatured PIPELINE_MEDIA */
	} else if (IS_GEN12(devid))
		fill = gen12_media_fillfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid) || IS_GEN11(devid))
		fill = gen9_media_fillfunc;
	else if (IS_GEN8(devid))
		fill = gen8_media_fillfunc;
	else if (IS_GEN7(devid))
		fill = gen7_media_fillfunc;

	return fill;
}

igt_vme_func_t igt_get_media_vme_func(int devid)
{
	igt_vme_func_t fill = NULL;
	const struct intel_device_info *devinfo = intel_get_device_info(devid);

	if (IS_GEN11(devid) && !devinfo->is_elkhartlake && !devinfo->is_jasperlake)
		fill = gen11_media_vme_func;

	return fill;
}

/**
 * igt_get_gpgpu_fillfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific gpgpu fill function pointer for the device specified
 * with @devid. Will return NULL when no gpgpu fill function is implemented.
 */
igt_fillfunc_t igt_get_gpgpu_fillfunc(int devid)
{
	igt_fillfunc_t fill = NULL;

	if (intel_graphics_ver(devid) >= IP_VER(12, 50))
		fill = xehp_gpgpu_fillfunc;
	else if (IS_GEN12(devid))
		fill = gen12_gpgpu_fillfunc;
	else if (IS_GEN11(devid))
		fill = gen11_gpgpu_fillfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid))
		fill = gen9_gpgpu_fillfunc;
	else if (IS_GEN8(devid))
		fill = gen8_gpgpu_fillfunc;
	else if (IS_GEN7(devid))
		fill = gen7_gpgpu_fillfunc;

	return fill;
}

/**
 * igt_get_media_spinfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific media spin function pointer for the device specified
 * with @devid. Will return NULL when no media spin function is implemented.
 */
igt_media_spinfunc_t igt_get_media_spinfunc(int devid)
{
	igt_media_spinfunc_t spin = NULL;

	if (IS_GEN9(devid))
		spin = gen9_media_spinfunc;
	else if (IS_GEN8(devid))
		spin = gen8_media_spinfunc;

	return spin;
}

/* Intel batchbuffer v2 */
static bool intel_bb_debug_tree = false;

/*
 * __reallocate_objects:
 * @ibb: pointer to intel_bb
 *
 * Increases number of objects if necessary.
 */
static void __reallocate_objects(struct intel_bb *ibb)
{
	const uint32_t inc = 4096 / sizeof(*ibb->objects);

	if (ibb->num_objects == ibb->allocated_objects) {
		ibb->objects = realloc(ibb->objects,
				       sizeof(*ibb->objects) *
				       (inc + ibb->allocated_objects));

		igt_assert(ibb->objects);
		ibb->allocated_objects += inc;

		memset(&ibb->objects[ibb->num_objects],	0,
		       inc * sizeof(*ibb->objects));
	}
}

static inline uint64_t __intel_bb_get_offset(struct intel_bb *ibb,
					     uint32_t handle,
					     uint64_t size,
					     uint32_t alignment)
{
	uint64_t offset;

	if (ibb->enforce_relocs)
		return 0;

	offset = intel_allocator_alloc(ibb->allocator_handle,
				       handle, size, alignment);

	return offset;
}

/**
 * __intel_bb_create:
 * @fd: drm fd - i915 or xe
 * @ctx: context id
 * @cfg: for i915 intel_ctx configuration, NULL for default context or legacy mode,
 *       unused for xe
 * @size: size of the batchbuffer
 * @do_relocs: use relocations or allocator
 * @allocator_type: allocator type, must be INTEL_ALLOCATOR_NONE for relocations
 *
 * intel-bb assumes it will work in one of two modes - with relocations or
 * with using allocator (currently RELOC and SIMPLE are implemented).
 * Some description is required to describe how they maintain the addresses.
 *
 * Before entering into each scenarios generic rule is intel-bb keeps objects
 * and their offsets in the internal cache and reuses in subsequent execs.
 *
 * 1. intel-bb with relocations (i915 only)
 *
 * Creating new intel-bb adds handle to cache implicitly and sets its address
 * to 0. Objects added to intel-bb later also have address 0 set for first run.
 * After calling execbuf cache is altered with new addresses. As intel-bb
 * works in reloc mode addresses are only suggestion to the driver and we
 * cannot be sure they won't change at next exec.
 *
 * 2. with allocator (i915 or xe)
 *
 * This mode is valid only for ppgtt. Addresses are acquired from allocator
 * and softpinned (i915) or vm-binded (xe). intel-bb cache must be then
 * coherent with allocator (simple is coherent, reloc partially [doesn't
 * support address reservation]).
 * When we do intel-bb reset with purging cache it has to reacquire addresses
 * from allocator (allocator should return same address - what is true for
 * simple and reloc allocators).
 *
 * If we do reset without purging caches we use addresses from intel-bb cache
 * during execbuf objects construction.
 *
 * If we do reset with purging caches allocator entries are freed as well.
 *
 * __intel_bb_create checks if a context configuration for intel_ctx_t was
 * passed in. If this is the case, it copies the information over to the
 * newly created batch buffer.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
static struct intel_bb *
__intel_bb_create(int fd, uint32_t ctx, const intel_ctx_cfg_t *cfg,
		  uint32_t size, bool do_relocs,
		  uint64_t start, uint64_t end,
		  uint8_t allocator_type, enum allocator_strategy strategy)
{
	struct drm_i915_gem_exec_object2 *object;
	struct intel_bb *ibb = calloc(1, sizeof(*ibb));

	igt_assert(ibb);

	ibb->devid = intel_get_drm_devid(fd);
	ibb->gen = intel_gen(ibb->devid);
	ibb->ctx = ctx;

	ibb->fd = fd;
	ibb->driver = is_i915_device(fd) ? INTEL_DRIVER_I915 :
					   is_xe_device(fd) ? INTEL_DRIVER_XE : 0;
	igt_assert(ibb->driver);

	/*
	 * If we don't have full ppgtt driver can change our addresses
	 * so allocator is useless in this case. Just enforce relocations
	 * for such gens and don't use allocator at all.
	 */
	if (ibb->driver == INTEL_DRIVER_I915) {
		ibb->uses_full_ppgtt = gem_uses_full_ppgtt(fd);
		ibb->alignment = gem_detect_safe_alignment(fd);
		ibb->gtt_size = gem_aperture_size(fd);
		ibb->handle = gem_create(fd, size);

		if (!ibb->uses_full_ppgtt)
			do_relocs = true;

		/*
		 * For softpin mode allocator has full control over offsets allocation
		 * so we want kernel to not interfere with this.
		 */
		if (do_relocs) {
			ibb->allows_obj_alignment = gem_allows_obj_alignment(fd);
			allocator_type = INTEL_ALLOCATOR_NONE;
		} else {
			/* Use safe start offset instead assuming 0x0 is safe */
			start = max_t(uint64_t, start, gem_detect_safe_start_offset(fd));

			/* if relocs are set we won't use an allocator */
			ibb->allocator_handle =
				intel_allocator_open_full(fd, ctx, start, end,
							  allocator_type,
							  strategy, 0);
		}

		ibb->vm_id = 0;
	} else {
		igt_assert(!do_relocs);

		ibb->alignment = xe_get_default_alignment(fd);
		size = ALIGN(size, ibb->alignment);
		ibb->handle = xe_bo_create_flags(fd, 0, size, vram_if_possible(fd, 0));

		/* Limit to 48-bit due to MI_* address limitation */
		ibb->gtt_size = 1ull << min_t(uint32_t, xe_va_bits(fd), 48);
		end = ibb->gtt_size;

		if (!ctx)
			ctx = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);

		ibb->uses_full_ppgtt = true;
		ibb->allocator_handle =
			intel_allocator_open_full(fd, ctx, start, end,
						  allocator_type, strategy,
						  ibb->alignment);
		ibb->vm_id = ctx;
		ibb->last_engine = ~0U;
	}

	ibb->allocator_type = allocator_type;
	ibb->allocator_strategy = strategy;
	ibb->allocator_start = start;
	ibb->allocator_end = end;
	ibb->enforce_relocs = do_relocs;

	ibb->size = size;
	ibb->batch = calloc(1, size);
	igt_assert(ibb->batch);
	ibb->ptr = ibb->batch;
	ibb->fence = -1;

	/* Cache context configuration */
	if (cfg) {
		ibb->cfg = malloc(sizeof(*cfg));
		igt_assert(ibb->cfg);
		memcpy(ibb->cfg, cfg, sizeof(*cfg));
	}

	if ((ibb->gtt_size - 1) >> 32)
		ibb->supports_48b_address = true;

	object = intel_bb_add_object(ibb, ibb->handle, ibb->size,
				     INTEL_BUF_INVALID_ADDRESS, ibb->alignment,
				     false);
	ibb->batch_offset = object->offset;

	IGT_INIT_LIST_HEAD(&ibb->intel_bufs);

	ibb->refcount = 1;

	if (intel_bb_do_tracking && ibb->allocator_type != INTEL_ALLOCATOR_NONE) {
		pthread_mutex_lock(&intel_bb_list_lock);
		igt_list_add(&ibb->link, &intel_bb_list);
		pthread_mutex_unlock(&intel_bb_list_lock);
	}

	return ibb;
}

/**
 * intel_bb_create_full:
 * @fd: drm fd - i915 or xe
 * @ctx: context
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 * @start: allocator vm start address
 * @end: allocator vm start address
 * @allocator_type: allocator type, SIMPLE, RELOC, ...
 * @strategy: allocation strategy
 *
 * Creates bb with context passed in @ctx, size in @size and allocator type
 * in @allocator_type. Relocations are set to false because IGT allocator
 * is used in that case. VM range is passed to allocator (@start and @end)
 * and allocation @strategy (suggestion to allocator about address allocation
 * preferences).
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_full(int fd, uint32_t ctx,
				      const intel_ctx_cfg_t *cfg, uint32_t size,
				      uint64_t start, uint64_t end,
				      uint8_t allocator_type,
				      enum allocator_strategy strategy)
{
	return __intel_bb_create(fd, ctx, cfg, size, false, start, end,
				 allocator_type, strategy);
}

/**
 * intel_bb_create_with_allocator:
 * @fd: drm fd - i915 or xe
 * @ctx: context
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 * @allocator_type: allocator type, SIMPLE, RANDOM, ...
 *
 * Creates bb with context passed in @ctx, size in @size and allocator type
 * in @allocator_type. Relocations are set to false because IGT allocator
 * is used in that case.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_with_allocator(int fd, uint32_t ctx,
						const intel_ctx_cfg_t *cfg,
						uint32_t size,
						uint8_t allocator_type)
{
	return __intel_bb_create(fd, ctx, cfg, size, false, 0, 0,
				 allocator_type, ALLOC_STRATEGY_HIGH_TO_LOW);
}

static bool aux_needs_softpin(int fd)
{
	return intel_gen(intel_get_drm_devid(fd)) >= 12;
}

static bool has_ctx_cfg(struct intel_bb *ibb)
{
	return ibb->cfg && ibb->cfg->num_engines > 0;
}

/**
 * intel_bb_create:
 * @fd: drm fd - i915 or xe
 * @size: size of the batchbuffer
 *
 * Creates bb with default context.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 *
 * Notes:
 *
 * intel_bb must not be created in igt_fixture. The reason is intel_bb
 * "opens" connection to the allocator and when test completes it can
 * leave the allocator in unknown state (mostly for failed tests).
 * As igt_core was armed to reset the allocator infrastructure
 * connection to it inside intel_bb is not valid anymore.
 * Trying to use it leads to catastrofic errors.
 */
struct intel_bb *intel_bb_create(int fd, uint32_t size)
{
	bool relocs = is_i915_device(fd) && gem_has_relocations(fd);

	return __intel_bb_create(fd, 0, NULL, size,
				 relocs && !aux_needs_softpin(fd), 0, 0,
				 INTEL_ALLOCATOR_SIMPLE,
				 ALLOC_STRATEGY_HIGH_TO_LOW);
}

/**
 * intel_bb_create_with_context:
 * @fd: drm fd - i915 or xe
 * @ctx: context id
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 *
 * Creates bb with context passed in @ctx and @cfg configuration (when
 * working with custom engines layout).
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *
intel_bb_create_with_context(int fd, uint32_t ctx,
			     const intel_ctx_cfg_t *cfg, uint32_t size)
{
	bool relocs = is_i915_device(fd) && gem_has_relocations(fd);

	return __intel_bb_create(fd, ctx, cfg, size,
				 relocs && !aux_needs_softpin(fd), 0, 0,
				 INTEL_ALLOCATOR_SIMPLE,
				 ALLOC_STRATEGY_HIGH_TO_LOW);
}

/**
 * intel_bb_create_with_relocs:
 * @fd: drm fd - i915
 * @size: size of the batchbuffer
 *
 * Creates bb which will disable passing addresses.
 * This will lead to relocations when objects are not previously pinned.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_with_relocs(int fd, uint32_t size)
{
	igt_require(is_i915_device(fd) && gem_has_relocations(fd));

	return __intel_bb_create(fd, 0, NULL, size, true, 0, 0,
				 INTEL_ALLOCATOR_NONE, ALLOC_STRATEGY_NONE);
}

/**
 * intel_bb_create_with_relocs_and_context:
 * @fd: drm fd - i915
 * @ctx: context
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 *
 * Creates bb with default context which will disable passing addresses.
 * This will lead to relocations when objects are not previously pinned.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *
intel_bb_create_with_relocs_and_context(int fd, uint32_t ctx,
					const intel_ctx_cfg_t *cfg,
					uint32_t size)
{
	igt_require(is_i915_device(fd) && gem_has_relocations(fd));

	return __intel_bb_create(fd, ctx, cfg, size, true, 0, 0,
				 INTEL_ALLOCATOR_NONE, ALLOC_STRATEGY_NONE);
}

/**
 * intel_bb_create_no_relocs:
 * @fd: drm fd
 * @size: size of the batchbuffer
 *
 * Creates bb with disabled relocations.
 * This enables passing addresses and requires pinning objects.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_no_relocs(int fd, uint32_t size)
{
	igt_require(gem_uses_full_ppgtt(fd));

	return __intel_bb_create(fd, 0, NULL, size, false, 0, 0,
				 INTEL_ALLOCATOR_SIMPLE,
				 ALLOC_STRATEGY_HIGH_TO_LOW);
}

static void __intel_bb_destroy_relocations(struct intel_bb *ibb)
{
	uint32_t i;

	/* Free relocations */
	for (i = 0; i < ibb->num_objects; i++) {
		free(from_user_pointer(ibb->objects[i]->relocs_ptr));
		ibb->objects[i]->relocs_ptr = to_user_pointer(NULL);
		ibb->objects[i]->relocation_count = 0;
	}

	ibb->relocs = NULL;
	ibb->num_relocs = 0;
	ibb->allocated_relocs = 0;
}

static void __intel_bb_destroy_objects(struct intel_bb *ibb)
{
	free(ibb->objects);
	ibb->objects = NULL;

	tdestroy(ibb->current, free);
	ibb->current = NULL;

	ibb->num_objects = 0;
	ibb->allocated_objects = 0;
}

static void __intel_bb_destroy_cache(struct intel_bb *ibb)
{
	tdestroy(ibb->root, free);
	ibb->root = NULL;
}

static void __intel_bb_remove_intel_bufs(struct intel_bb *ibb)
{
	struct intel_buf *entry, *tmp;

	igt_list_for_each_entry_safe(entry, tmp, &ibb->intel_bufs, link)
		intel_bb_remove_intel_buf(ibb, entry);
}

/**
 * intel_bb_destroy:
 * @ibb: pointer to intel_bb
 *
 * Frees all relocations / objects allocated during filling the batch.
 */
void intel_bb_destroy(struct intel_bb *ibb)
{
	igt_assert(ibb);

	ibb->refcount--;
	igt_assert_f(ibb->refcount == 0, "Trying to destroy referenced bb!");

	__intel_bb_remove_intel_bufs(ibb);
	__intel_bb_destroy_relocations(ibb);
	__intel_bb_destroy_objects(ibb);
	__intel_bb_destroy_cache(ibb);

	if (ibb->allocator_type != INTEL_ALLOCATOR_NONE) {
		if (intel_bb_do_tracking) {
			pthread_mutex_lock(&intel_bb_list_lock);
			igt_list_del(&ibb->link);
			pthread_mutex_unlock(&intel_bb_list_lock);
		}

		intel_allocator_free(ibb->allocator_handle, ibb->handle);
		intel_allocator_close(ibb->allocator_handle);
	}
	gem_close(ibb->fd, ibb->handle);

	if (ibb->fence >= 0)
		close(ibb->fence);
	if (ibb->engine_syncobj)
		syncobj_destroy(ibb->fd, ibb->engine_syncobj);
	if (ibb->vm_id && !ibb->ctx)
		xe_vm_destroy(ibb->fd, ibb->vm_id);

	free(ibb->batch);
	free(ibb->cfg);
	free(ibb);
}

static struct drm_xe_vm_bind_op *xe_alloc_bind_ops(struct intel_bb *ibb,
						   uint32_t op, uint32_t region)
{
	struct drm_i915_gem_exec_object2 **objects = ibb->objects;
	struct drm_xe_vm_bind_op *bind_ops, *ops;
	bool set_obj = (op & 0xffff) == XE_VM_BIND_OP_MAP;

	bind_ops = calloc(ibb->num_objects, sizeof(*bind_ops));
	igt_assert(bind_ops);

	igt_debug("bind_ops: %s\n", set_obj ? "MAP" : "UNMAP");
	for (int i = 0; i < ibb->num_objects; i++) {
		ops = &bind_ops[i];

		if (set_obj)
			ops->obj = objects[i]->handle;

		ops->op = op;
		ops->obj_offset = 0;
		ops->addr = objects[i]->offset;
		ops->range = objects[i]->rsvd1;
		ops->region = region;

		igt_debug("  [%d]: handle: %u, offset: %llx, size: %llx\n",
			  i, ops->obj, (long long)ops->addr, (long long)ops->range);
	}

	return bind_ops;
}

static void __unbind_xe_objects(struct intel_bb *ibb)
{
	struct drm_xe_sync syncs[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	int ret;

	syncs[0].handle = ibb->engine_syncobj;
	syncs[1].handle = syncobj_create(ibb->fd, 0);

	if (ibb->num_objects > 1) {
		struct drm_xe_vm_bind_op *bind_ops;
		uint32_t op = XE_VM_BIND_OP_UNMAP | XE_VM_BIND_FLAG_ASYNC;

		bind_ops = xe_alloc_bind_ops(ibb, op, 0);
		xe_vm_bind_array(ibb->fd, ibb->vm_id, 0, bind_ops,
				 ibb->num_objects, syncs, 2);
		free(bind_ops);
	} else {
		igt_debug("bind: UNMAP\n");
		igt_debug("  offset: %llx, size: %llx\n",
			  (long long)ibb->batch_offset, (long long)ibb->size);
		xe_vm_unbind_async(ibb->fd, ibb->vm_id, 0, 0,
				   ibb->batch_offset, ibb->size, syncs, 2);
	}
	ret = syncobj_wait_err(ibb->fd, &syncs[1].handle, 1, INT64_MAX, 0);
	igt_assert_eq(ret, 0);
	syncobj_destroy(ibb->fd, syncs[1].handle);

	ibb->xe_bound = false;
}

/*
 * intel_bb_reset:
 * @ibb: pointer to intel_bb
 * @purge_objects_cache: if true destroy internal execobj and relocs + cache
 *
 * Recreate batch bo when there's no additional reference.
 *
 * When purge_object_cache == true we destroy cache as well as remove intel_buf
 * from intel-bb tracking list. Removing intel_bufs releases their addresses
 * in the allocator.
*/

void intel_bb_reset(struct intel_bb *ibb, bool purge_objects_cache)
{
	uint32_t i;

	if (purge_objects_cache && ibb->refcount > 1)
		igt_warn("Cannot purge objects cache on bb, refcount > 1!");

	/* Someone keeps reference, just exit */
	if (ibb->refcount > 1)
		return;

	/*
	 * To avoid relocation objects previously pinned to high virtual
	 * addresses should keep 48bit flag. Ensure we won't clear it
	 * in the reset path.
	 */
	for (i = 0; i < ibb->num_objects; i++)
		ibb->objects[i]->flags &= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	if (ibb->driver == INTEL_DRIVER_XE && ibb->xe_bound)
		__unbind_xe_objects(ibb);

	__intel_bb_destroy_relocations(ibb);
	__intel_bb_destroy_objects(ibb);
	__reallocate_objects(ibb);

	if (purge_objects_cache) {
		__intel_bb_remove_intel_bufs(ibb);
		__intel_bb_destroy_cache(ibb);
	}

	/*
	 * When we use allocators we're in no-reloc mode so we have to free
	 * and reacquire offset (ibb->handle can change in multiprocess
	 * environment). We also have to remove and add it again to
	 * objects and cache tree.
	 */
	if (ibb->allocator_type != INTEL_ALLOCATOR_NONE && !purge_objects_cache)
		intel_bb_remove_object(ibb, ibb->handle, ibb->batch_offset,
				       ibb->size);

	gem_close(ibb->fd, ibb->handle);
	if (ibb->driver == INTEL_DRIVER_I915)
		ibb->handle = gem_create(ibb->fd, ibb->size);
	else
		ibb->handle = xe_bo_create_flags(ibb->fd, 0, ibb->size,
						 vram_if_possible(ibb->fd, 0));

	/* Reacquire offset for RELOC and SIMPLE */
	if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE ||
	    ibb->allocator_type == INTEL_ALLOCATOR_RELOC)
		ibb->batch_offset = __intel_bb_get_offset(ibb,
							  ibb->handle,
							  ibb->size,
							  ibb->alignment);

	intel_bb_add_object(ibb, ibb->handle, ibb->size,
			    ibb->batch_offset,
			    ibb->alignment, false);
	ibb->ptr = ibb->batch;
	memset(ibb->batch, 0, ibb->size);
}

/*
 * intel_bb_sync:
 * @ibb: pointer to intel_bb
 *
 * Waits for bb completion. Returns 0 on success, otherwise errno.
 */
int intel_bb_sync(struct intel_bb *ibb)
{
	int ret;

	if (ibb->fence < 0 && !ibb->engine_syncobj)
		return 0;

	if (ibb->fence >= 0) {
		ret = sync_fence_wait(ibb->fence, -1);
		if (ret == 0) {
			close(ibb->fence);
			ibb->fence = -1;
		}
	} else {
		igt_assert_neq(ibb->engine_syncobj, 0);
		ret = syncobj_wait_err(ibb->fd, &ibb->engine_syncobj,
				       1, INT64_MAX, 0);
	}

	return ret;
}

/*
 * intel_bb_print:
 * @ibb: pointer to intel_bb
 *
 * Prints batch to stdout.
 */
void intel_bb_print(struct intel_bb *ibb)
{
	igt_info("drm fd: %d, gen: %d, devid: %u, debug: %d\n",
		 ibb->fd, ibb->gen, ibb->devid, ibb->debug);
	igt_info("handle: %u, size: %u, batch: %p, ptr: %p\n",
		 ibb->handle, ibb->size, ibb->batch, ibb->ptr);
	igt_info("gtt_size: %" PRIu64 ", supports 48bit: %d\n",
		 ibb->gtt_size, ibb->supports_48b_address);
	igt_info("ctx: %u\n", ibb->ctx);
	igt_info("root: %p\n", ibb->root);
	igt_info("objects: %p, num_objects: %u, allocated obj: %u\n",
		 ibb->objects, ibb->num_objects, ibb->allocated_objects);
	igt_info("relocs: %p, num_relocs: %u, allocated_relocs: %u\n----\n",
		 ibb->relocs, ibb->num_relocs, ibb->allocated_relocs);
}

/*
 * intel_bb_dump:
 * @ibb: pointer to intel_bb
 * @filename: name to which write bb
 *
 * Dump batch bo to file.
 */
void intel_bb_dump(struct intel_bb *ibb, const char *filename)
{
	FILE *out;
	void *ptr;

	ptr = gem_mmap__device_coherent(ibb->fd, ibb->handle, 0, ibb->size,
					PROT_READ);
	out = fopen(filename, "wb");
	igt_assert(out);
	fwrite(ptr, ibb->size, 1, out);
	fclose(out);
	munmap(ptr, ibb->size);
}

/**
 * intel_bb_set_debug:
 * @ibb: pointer to intel_bb
 * @debug: true / false
 *
 * Sets debug to true / false. Execbuf is then called synchronously and
 * object/reloc arrays are printed after execution.
 */
void intel_bb_set_debug(struct intel_bb *ibb, bool debug)
{
	ibb->debug = debug;
}

/**
 * intel_bb_set_dump_base64:
 * @ibb: pointer to intel_bb
 * @dump: true / false
 *
 * Do bb dump as base64 string before execbuf call.
 */
void intel_bb_set_dump_base64(struct intel_bb *ibb, bool dump)
{
	ibb->dump_base64 = dump;
}

static int __compare_objects(const void *p1, const void *p2)
{
	const struct drm_i915_gem_exec_object2 *o1 = p1, *o2 = p2;

	return (int) ((int64_t) o1->handle - (int64_t) o2->handle);
}

static struct drm_i915_gem_exec_object2 *
__add_to_cache(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 **found, *object;

	object = malloc(sizeof(*object));
	igt_assert(object);

	object->handle = handle;
	object->alignment = 0;
	found = tsearch((void *) object, &ibb->root, __compare_objects);

	if (*found == object) {
		memset(object, 0, sizeof(*object));
		object->handle = handle;
		object->offset = INTEL_BUF_INVALID_ADDRESS;
	} else {
		free(object);
		object = *found;
	}

	return object;
}

static bool __remove_from_cache(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 **found, *object;

	object = intel_bb_find_object(ibb, handle);
	if (!object) {
		igt_warn("Object: handle: %u not found\n", handle);
		return false;
	}

	found = tdelete((void *) object, &ibb->root, __compare_objects);
	if (!found)
		return false;

	free(object);

	return true;
}

static int __compare_handles(const void *p1, const void *p2)
{
	return (int) (*(int32_t *) p1 - *(int32_t *) p2);
}

static void __add_to_objects(struct intel_bb *ibb,
			     struct drm_i915_gem_exec_object2 *object)
{
	uint32_t **found, *handle;

	handle = malloc(sizeof(*handle));
	igt_assert(handle);

	*handle = object->handle;
	found = tsearch((void *) handle, &ibb->current, __compare_handles);

	if (*found == handle) {
		__reallocate_objects(ibb);
		igt_assert(ibb->num_objects < ibb->allocated_objects);
		ibb->objects[ibb->num_objects++] = object;
	} else {
		free(handle);
	}
}

static void __remove_from_objects(struct intel_bb *ibb,
				  struct drm_i915_gem_exec_object2 *object)
{
	uint32_t i, **handle, *to_free;
	bool found = false;

	for (i = 0; i < ibb->num_objects; i++) {
		if (ibb->objects[i] == object) {
			found = true;
			break;
		}
	}

	/*
	 * When we reset bb (without purging) we have:
	 * 1. cache which contains all cached objects
	 * 2. objects array which contains only bb object (cleared in reset
	 *    path with bb object added at the end)
	 * So !found is normal situation and no warning is added here.
	 */
	if (!found)
		return;

	ibb->num_objects--;
	if (i < ibb->num_objects)
		memmove(&ibb->objects[i], &ibb->objects[i + 1],
			sizeof(object) * (ibb->num_objects - i));

	handle = tfind((void *) &object->handle,
		       &ibb->current, __compare_handles);
	if (!handle) {
		igt_warn("Object %u doesn't exist in the tree, can't remove",
			 object->handle);
		return;
	}

	to_free = *handle;
	tdelete((void *) &object->handle, &ibb->current, __compare_handles);
	free(to_free);
}

/**
 * __intel_bb_add_object:
 * @ibb: pointer to intel_bb
 * @handle: which handle to add to objects array
 * @size: object size
 * @offset: presumed offset of the object when no relocation is enforced
 * @alignment: alignment of the object, if 0 it will be set to page size
 * @write: does a handle is a render target
 *
 * Function adds or updates execobj slot in bb objects array and
 * in the object tree. When object is a render target it has to
 * be marked with EXEC_OBJECT_WRITE flag.
 */
static struct drm_i915_gem_exec_object2 *
__intel_bb_add_object(struct intel_bb *ibb, uint32_t handle, uint64_t size,
		      uint64_t offset, uint64_t alignment, bool write)
{
	struct drm_i915_gem_exec_object2 *object;

	igt_assert(INVALID_ADDR(offset) || alignment == 0
		   || ALIGN(offset, alignment) == offset);
	igt_assert(is_power_of_two(alignment));

	if (ibb->driver == INTEL_DRIVER_I915)
		alignment = max_t(uint64_t, alignment, gem_detect_safe_alignment(ibb->fd));
	else
		alignment = max_t(uint64_t, ibb->alignment, alignment);

	object = __add_to_cache(ibb, handle);
	__add_to_objects(ibb, object);

	/*
	 * If object->offset == INVALID_ADDRESS we added freshly object to the
	 * cache. In that case we have two choices:
	 * a) get new offset (passed offset was invalid)
	 * b) use offset passed in the call (valid)
	 */
	if (INVALID_ADDR(object->offset)) {
		if (INVALID_ADDR(offset)) {
			offset = __intel_bb_get_offset(ibb, handle, size,
						       alignment);
		} else {
			offset = offset & (ibb->gtt_size - 1);

			/*
			 * For simple allocator check entry consistency
			 * - reserve if it is not already allocated.
			 */
			if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE) {
				bool allocated, reserved;

				reserved = intel_allocator_reserve_if_not_allocated(ibb->allocator_handle,
										    handle, size, offset,
										    &allocated);
				igt_assert_f(allocated || reserved,
					     "Can't get offset, allocated: %d, reserved: %d\n",
					     allocated, reserved);
			}
		}
	} else {
		/*
		 * This assertion makes sense only when we have to be consistent
		 * with underlying allocator. For relocations and when !ppgtt
		 * we can expect addresses passed by the user can be moved
		 * within the driver.
		 */
		if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE)
			igt_assert_f(object->offset == offset,
				     "(pid: %ld) handle: %u, offset not match: %" PRIx64 " <> %" PRIx64 "\n",
				     (long) getpid(), handle,
				     (uint64_t) object->offset,
				     offset);
	}

	object->offset = offset;

	if (write)
		object->flags |= EXEC_OBJECT_WRITE;

	if (ibb->supports_48b_address)
		object->flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	if (ibb->uses_full_ppgtt && !ibb->enforce_relocs)
		object->flags |= EXEC_OBJECT_PINNED;

	if (ibb->allows_obj_alignment)
		object->alignment = alignment;

	if (ibb->driver == INTEL_DRIVER_XE) {
		object->alignment = alignment;
		object->rsvd1 = size;
	}

	return object;
}

struct drm_i915_gem_exec_object2 *
intel_bb_add_object(struct intel_bb *ibb, uint32_t handle, uint64_t size,
		    uint64_t offset, uint64_t alignment, bool write)
{
	struct drm_i915_gem_exec_object2 *obj = NULL;

	obj = __intel_bb_add_object(ibb, handle, size, offset,
				    alignment, write);
	igt_assert(obj);

	return obj;
}

bool intel_bb_remove_object(struct intel_bb *ibb, uint32_t handle,
			    uint64_t offset, uint64_t size)
{
	struct drm_i915_gem_exec_object2 *object;
	bool is_reserved;

	object = intel_bb_find_object(ibb, handle);
	if (!object)
		return false;

	if (ibb->allocator_type != INTEL_ALLOCATOR_NONE) {
		intel_allocator_free(ibb->allocator_handle, handle);
		is_reserved = intel_allocator_is_reserved(ibb->allocator_handle,
							  size, offset);
		if (is_reserved)
			intel_allocator_unreserve(ibb->allocator_handle, handle,
						  size, offset);
	}

	__remove_from_objects(ibb, object);
	__remove_from_cache(ibb, handle);

	return true;
}

static struct drm_i915_gem_exec_object2 *
__intel_bb_add_intel_buf(struct intel_bb *ibb, struct intel_buf *buf,
			 uint64_t alignment, bool write)
{
	struct drm_i915_gem_exec_object2 *obj;

	igt_assert(ibb);
	igt_assert(buf);
	igt_assert(!buf->ibb || buf->ibb == ibb);
	igt_assert(ALIGN(alignment, 4096) == alignment);

	if (!alignment) {
		alignment = 0x1000;

		if (ibb->gen >= 12 && buf->compression)
			alignment = 0x10000;

		/* For gen3 ensure tiled buffers are aligned to power of two size */
		if (ibb->gen == 3 && buf->tiling) {
			alignment = 1024 * 1024;

			while (alignment < buf->surface[0].size)
				alignment <<= 1;
		}
	}

	obj = intel_bb_add_object(ibb, buf->handle, intel_buf_bo_size(buf),
				  buf->addr.offset, alignment, write);
	buf->addr.offset = obj->offset;

	if (igt_list_empty(&buf->link)) {
		igt_list_add_tail(&buf->link, &ibb->intel_bufs);
		buf->ibb = ibb;
	} else {
		igt_assert(buf->ibb == ibb);
	}

	return obj;
}

struct drm_i915_gem_exec_object2 *
intel_bb_add_intel_buf(struct intel_bb *ibb, struct intel_buf *buf, bool write)
{
	return __intel_bb_add_intel_buf(ibb, buf, 0, write);
}

struct drm_i915_gem_exec_object2 *
intel_bb_add_intel_buf_with_alignment(struct intel_bb *ibb, struct intel_buf *buf,
				      uint64_t alignment, bool write)
{
	return __intel_bb_add_intel_buf(ibb, buf, alignment, write);
}

bool intel_bb_remove_intel_buf(struct intel_bb *ibb, struct intel_buf *buf)
{
	bool removed;

	igt_assert(ibb);
	igt_assert(buf);
	igt_assert(!buf->ibb || buf->ibb == ibb);

	if (igt_list_empty(&buf->link))
		return false;

	removed = intel_bb_remove_object(ibb, buf->handle,
					 buf->addr.offset,
					 intel_buf_bo_size(buf));
	if (removed) {
		buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;
		buf->ibb = NULL;
		igt_list_del_init(&buf->link);
	}

	return removed;
}

void intel_bb_print_intel_bufs(struct intel_bb *ibb)
{
	struct intel_buf *entry;

	igt_list_for_each_entry(entry, &ibb->intel_bufs, link) {
		igt_info("handle: %u, ibb: %p, offset: %lx\n",
			 entry->handle, entry->ibb,
			 (long) entry->addr.offset);
	}
}

struct drm_i915_gem_exec_object2 *
intel_bb_find_object(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found)
		return NULL;

	return *found;
}

bool
intel_bb_object_set_flag(struct intel_bb *ibb, uint32_t handle, uint64_t flag)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	igt_assert_f(ibb->root, "Trying to search in null tree\n");

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found) {
		igt_warn("Trying to set fence on not found handle: %u\n",
			 handle);
		return false;
	}

	(*found)->flags |= flag;

	return true;
}

bool
intel_bb_object_clear_flag(struct intel_bb *ibb, uint32_t handle, uint64_t flag)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found) {
		igt_warn("Trying to set fence on not found handle: %u\n",
			 handle);
		return false;
	}

	(*found)->flags &= ~flag;

	return true;
}

/*
 * intel_bb_add_reloc:
 * @ibb: pointer to intel_bb
 * @to_handle: object handle in which do relocation
 * @handle: object handle which address will be taken to patch the @to_handle
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 * @offset: offset within bb to be patched
 *
 * When relocations are requested function allocates additional relocation slot
 * in reloc array for a handle.
 * Object must be previously added to bb.
 */
static uint64_t intel_bb_add_reloc(struct intel_bb *ibb,
				   uint32_t to_handle,
				   uint32_t handle,
				   uint32_t read_domains,
				   uint32_t write_domain,
				   uint64_t delta,
				   uint64_t offset,
				   uint64_t presumed_offset)
{
	struct drm_i915_gem_relocation_entry *relocs;
	struct drm_i915_gem_exec_object2 *object, *to_object;
	uint32_t i;

	object = intel_bb_find_object(ibb, handle);
	igt_assert(object);

	/* In no-reloc mode we just return the previously assigned address */
	if (!ibb->enforce_relocs)
		goto out;

	/* For ibb we have relocs allocated in chunks */
	if (to_handle == ibb->handle) {
		relocs = ibb->relocs;
		if (ibb->num_relocs == ibb->allocated_relocs) {
			ibb->allocated_relocs += 4096 / sizeof(*relocs);
			relocs = realloc(relocs, sizeof(*relocs) * ibb->allocated_relocs);
			igt_assert(relocs);
			ibb->relocs = relocs;
		}
		i = ibb->num_relocs++;
	} else {
		to_object = intel_bb_find_object(ibb, to_handle);
		igt_assert_f(to_object, "object has to be added to ibb first!\n");

		i = to_object->relocation_count++;
		relocs = from_user_pointer(to_object->relocs_ptr);
		relocs = realloc(relocs, sizeof(*relocs) * to_object->relocation_count);
		to_object->relocs_ptr = to_user_pointer(relocs);
		igt_assert(relocs);
	}

	memset(&relocs[i], 0, sizeof(*relocs));
	relocs[i].target_handle = handle;
	relocs[i].read_domains = read_domains;
	relocs[i].write_domain = write_domain;
	relocs[i].delta = delta;
	relocs[i].offset = offset;
	if (ibb->enforce_relocs)
		relocs[i].presumed_offset = -1;
	else
		relocs[i].presumed_offset = object->offset;

	igt_debug("add reloc: to_handle: %u, handle: %u, r/w: 0x%x/0x%x, "
		  "delta: 0x%" PRIx64 ", "
		  "offset: 0x%" PRIx64 ", "
		  "poffset: %p\n",
		  to_handle, handle, read_domains, write_domain,
		  delta, offset,
		  from_user_pointer(relocs[i].presumed_offset));

out:
	return object->offset;
}

static uint64_t __intel_bb_emit_reloc(struct intel_bb *ibb,
				      uint32_t to_handle,
				      uint32_t to_offset,
				      uint32_t handle,
				      uint32_t read_domains,
				      uint32_t write_domain,
				      uint64_t delta,
				      uint64_t presumed_offset)
{
	uint64_t address;

	igt_assert(ibb);

	address = intel_bb_add_reloc(ibb, to_handle, handle,
				     read_domains, write_domain,
				     delta, to_offset,
				     presumed_offset);

	intel_bb_out(ibb, delta + address);
	if (ibb->gen >= 8)
		intel_bb_out(ibb, (delta + address) >> 32);

	return address;
}

/**
 * intel_bb_emit_reloc:
 * @ibb: pointer to intel_bb
 * @handle: object handle which address will be taken to patch the bb
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 * @presumed_offset: address of the object in address space. If -1 is passed
 * then final offset of the object will be randomized (for no-reloc bb) or
 * 0 (for reloc bb, in that case reloc.presumed_offset will be -1). In
 * case address is known it should passed in @presumed_offset (for no-reloc).
 * @write: does a handle is a render target
 *
 * Function prepares relocation (execobj if required + reloc) and emits
 * offset in bb. For I915_EXEC_NO_RELOC presumed_offset is a hint we already
 * have object in valid place and relocation step can be skipped in this case.
 *
 * Note: delta is value added to address, mostly used when some instructions
 * require modify-bit set to apply change. Which delta is valid depends
 * on instruction (see instruction specification).
 */
uint64_t intel_bb_emit_reloc(struct intel_bb *ibb,
			     uint32_t handle,
			     uint32_t read_domains,
			     uint32_t write_domain,
			     uint64_t delta,
			     uint64_t presumed_offset)
{
	igt_assert(ibb);

	return __intel_bb_emit_reloc(ibb, ibb->handle, intel_bb_offset(ibb),
				     handle, read_domains, write_domain,
				     delta, presumed_offset);
}

uint64_t intel_bb_emit_reloc_fenced(struct intel_bb *ibb,
				    uint32_t handle,
				    uint32_t read_domains,
				    uint32_t write_domain,
				    uint64_t delta,
				    uint64_t presumed_offset)
{
	uint64_t address;

	address = intel_bb_emit_reloc(ibb, handle, read_domains, write_domain,
				      delta, presumed_offset);

	intel_bb_object_set_flag(ibb, handle, EXEC_OBJECT_NEEDS_FENCE);

	return address;
}

/**
 * intel_bb_offset_reloc:
 * @ibb: pointer to intel_bb
 * @handle: object handle which address will be taken to patch the bb
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @offset: offset within bb to be patched
 * @presumed_offset: address of the object in address space. If -1 is passed
 * then final offset of the object will be randomized (for no-reloc bb) or
 * 0 (for reloc bb, in that case reloc.presumed_offset will be -1). In
 * case address is known it should passed in @presumed_offset (for no-reloc).
 *
 * Function prepares relocation (execobj if required + reloc). It it used
 * for editing batchbuffer via modifying structures. It means when we're
 * preparing batchbuffer it is more descriptive to edit the structure
 * than emitting dwords. But it require for some fields to point the
 * relocation. For that case @offset is passed by the user and it points
 * to the offset in bb where the relocation will be applied.
 */
uint64_t intel_bb_offset_reloc(struct intel_bb *ibb,
			       uint32_t handle,
			       uint32_t read_domains,
			       uint32_t write_domain,
			       uint32_t offset,
			       uint64_t presumed_offset)
{
	igt_assert(ibb);

	return intel_bb_add_reloc(ibb, ibb->handle, handle,
				  read_domains, write_domain,
				  0, offset, presumed_offset);
}

uint64_t intel_bb_offset_reloc_with_delta(struct intel_bb *ibb,
					  uint32_t handle,
					  uint32_t read_domains,
					  uint32_t write_domain,
					  uint32_t delta,
					  uint32_t offset,
					  uint64_t presumed_offset)
{
	igt_assert(ibb);

	return intel_bb_add_reloc(ibb, ibb->handle, handle,
				  read_domains, write_domain,
				  delta, offset, presumed_offset);
}

uint64_t intel_bb_offset_reloc_to_object(struct intel_bb *ibb,
					 uint32_t to_handle,
					 uint32_t handle,
					 uint32_t read_domains,
					 uint32_t write_domain,
					 uint32_t delta,
					 uint32_t offset,
					 uint64_t presumed_offset)
{
	igt_assert(ibb);

	return intel_bb_add_reloc(ibb, to_handle, handle,
				  read_domains, write_domain,
				  delta, offset, presumed_offset);
}

/*
 * @intel_bb_set_pxp:
 * @ibb: pointer to intel_bb
 * @new_state: enable or disable pxp session
 * @apptype: pxp session input identifies what type of session to enable
 * @appid: pxp session input provides which appid to use
 *
 * This function merely stores the pxp state and session information to
 * be retrieved and programmed later by supporting libraries such as
 * gen12_render_copy that must program the HW within the same dispatch
 */
void intel_bb_set_pxp(struct intel_bb *ibb, bool new_state,
		      uint32_t apptype, uint32_t appid)
{
	igt_assert(ibb);

	ibb->pxp.enabled = new_state;
	ibb->pxp.apptype = new_state ? apptype : 0;
	ibb->pxp.appid   = new_state ? appid : 0;
}

static void intel_bb_dump_execbuf(struct intel_bb *ibb,
				  struct drm_i915_gem_execbuffer2 *execbuf)
{
	struct drm_i915_gem_exec_object2 *objects;
	struct drm_i915_gem_relocation_entry *relocs, *reloc;
	int i, j;
	uint64_t address;

	igt_debug("execbuf [pid: %ld, fd: %d, ctx: %u]\n",
		  (long) getpid(), ibb->fd, ibb->ctx);
	igt_debug("execbuf batch len: %u, start offset: 0x%x, "
		  "DR1: 0x%x, DR4: 0x%x, "
		  "num clip: %u, clipptr: 0x%llx, "
		  "flags: 0x%llx, rsvd1: 0x%llx, rsvd2: 0x%llx\n",
		  execbuf->batch_len, execbuf->batch_start_offset,
		  execbuf->DR1, execbuf->DR4,
		  execbuf->num_cliprects, execbuf->cliprects_ptr,
		  execbuf->flags, execbuf->rsvd1, execbuf->rsvd2);

	igt_debug("execbuf buffer_count: %d\n", execbuf->buffer_count);
	for (i = 0; i < execbuf->buffer_count; i++) {
		objects = &((struct drm_i915_gem_exec_object2 *)
			    from_user_pointer(execbuf->buffers_ptr))[i];
		relocs = from_user_pointer(objects->relocs_ptr);
		address = objects->offset;
		igt_debug(" [%d] handle: %u, reloc_count: %d, reloc_ptr: %p, "
			  "align: 0x%llx, offset: 0x%" PRIx64 ", flags: 0x%llx, "
			  "rsvd1: 0x%llx, rsvd2: 0x%llx\n",
			  i, objects->handle, objects->relocation_count,
			  relocs,
			  objects->alignment,
			  address,
			  objects->flags,
			  objects->rsvd1, objects->rsvd2);
		if (objects->relocation_count) {
			igt_debug("\texecbuf relocs:\n");
			for (j = 0; j < objects->relocation_count; j++) {
				reloc = &relocs[j];
				address = reloc->presumed_offset;
				igt_debug("\t [%d] target handle: %u, "
					  "offset: 0x%llx, delta: 0x%x, "
					  "presumed_offset: 0x%" PRIx64 ", "
					  "read_domains: 0x%x, "
					  "write_domain: 0x%x\n",
					  j, reloc->target_handle,
					  reloc->offset, reloc->delta,
					  address,
					  reloc->read_domains,
					  reloc->write_domain);
			}
		}
	}
}

static void intel_bb_dump_base64(struct intel_bb *ibb, int linelen)
{
	int outsize;
	gchar *str, *pos;

	igt_info("--- bb ---\n");
	pos = str = g_base64_encode((const guchar *) ibb->batch, ibb->size);
	outsize = strlen(str);

	while (outsize > 0) {
		igt_info("%.*s\n", min(outsize, linelen), pos);
		pos += linelen;
		outsize -= linelen;
	}

	free(str);
}

static void print_node(const void *node, VISIT which, int depth)
{
	const struct drm_i915_gem_exec_object2 *object =
		*(const struct drm_i915_gem_exec_object2 **) node;
	(void) depth;

	switch (which) {
	case preorder:
	case endorder:
		break;

	case postorder:
	case leaf:
		igt_info("\t handle: %u, offset: 0x%" PRIx64 "\n",
			 object->handle, (uint64_t) object->offset);
		break;
	}
}

void intel_bb_dump_cache(struct intel_bb *ibb)
{
	igt_info("[pid: %ld] dump cache\n", (long) getpid());
	twalk(ibb->root, print_node);
}

static struct drm_i915_gem_exec_object2 *
create_objects_array(struct intel_bb *ibb)
{
	struct drm_i915_gem_exec_object2 *objects;
	uint32_t i;

	objects = malloc(sizeof(*objects) * ibb->num_objects);
	igt_assert(objects);

	for (i = 0; i < ibb->num_objects; i++) {
		objects[i] = *(ibb->objects[i]);
		objects[i].offset = CANONICAL(objects[i].offset);
	}

	return objects;
}

static void update_offsets(struct intel_bb *ibb,
			   struct drm_i915_gem_exec_object2 *objects)
{
	struct drm_i915_gem_exec_object2 *object;
	struct intel_buf *entry;
	uint32_t i;

	for (i = 0; i < ibb->num_objects; i++) {
		object = intel_bb_find_object(ibb, objects[i].handle);
		igt_assert(object);

		object->offset = DECANONICAL(objects[i].offset);

		if (i == 0)
			ibb->batch_offset = object->offset;
	}

	igt_list_for_each_entry(entry, &ibb->intel_bufs, link) {
		object = intel_bb_find_object(ibb, entry->handle);
		igt_assert(object);

		if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE)
			igt_assert(object->offset == entry->addr.offset);
		else
			entry->addr.offset = object->offset;

		entry->addr.ctx = ibb->ctx;
	}
}

#define LINELEN 76

static int
__xe_bb_exec(struct intel_bb *ibb, uint64_t flags, bool sync)
{
	uint32_t engine = flags & (I915_EXEC_BSD_MASK | I915_EXEC_RING_MASK);
	uint32_t engine_id;
	struct drm_xe_sync syncs[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_vm_bind_op *bind_ops;
	void *map;

	igt_assert_eq(ibb->num_relocs, 0);
	igt_assert_eq(ibb->xe_bound, false);

	if (ibb->last_engine != engine) {
		struct drm_xe_engine_class_instance inst = { };

		inst.engine_instance =
			(flags & I915_EXEC_BSD_MASK) >> I915_EXEC_BSD_SHIFT;

		switch (flags & I915_EXEC_RING_MASK) {
		case I915_EXEC_DEFAULT:
		case I915_EXEC_BLT:
			inst.engine_class = DRM_XE_ENGINE_CLASS_COPY;
			break;
		case I915_EXEC_BSD:
			inst.engine_class = DRM_XE_ENGINE_CLASS_VIDEO_DECODE;
			break;
		case I915_EXEC_RENDER:
			inst.engine_class = DRM_XE_ENGINE_CLASS_RENDER;
			break;
		case I915_EXEC_VEBOX:
			inst.engine_class = DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE;
			break;
		default:
			igt_assert_f(false, "Unknown engine: %x", (uint32_t) flags);
		}
		igt_debug("Run on %s\n", xe_engine_class_string(inst.engine_class));

		ibb->engine_id = engine_id =
			xe_engine_create(ibb->fd, ibb->vm_id, &inst, 0);
	} else {
		engine_id = ibb->engine_id;
	}
	ibb->last_engine = engine;

	map = xe_bo_map(ibb->fd, ibb->handle, ibb->size);
	memcpy(map, ibb->batch, ibb->size);
	gem_munmap(map, ibb->size);

	syncs[0].handle = syncobj_create(ibb->fd, 0);
	if (ibb->num_objects > 1) {
		bind_ops = xe_alloc_bind_ops(ibb, XE_VM_BIND_OP_MAP | XE_VM_BIND_FLAG_ASYNC, 0);
		xe_vm_bind_array(ibb->fd, ibb->vm_id, 0, bind_ops,
				 ibb->num_objects, syncs, 1);
		free(bind_ops);
	} else {
		igt_debug("bind: MAP\n");
		igt_debug("  handle: %u, offset: %llx, size: %llx\n",
			  ibb->handle, (long long)ibb->batch_offset,
			  (long long)ibb->size);
		xe_vm_bind_async(ibb->fd, ibb->vm_id, 0, ibb->handle, 0,
				 ibb->batch_offset, ibb->size, syncs, 1);
	}
	ibb->xe_bound = true;

	syncs[0].flags &= ~DRM_XE_SYNC_SIGNAL;
	ibb->engine_syncobj = syncobj_create(ibb->fd, 0);
	syncs[1].handle = ibb->engine_syncobj;

	xe_exec_sync(ibb->fd, engine_id, ibb->batch_offset, syncs, 2);

	if (sync)
		intel_bb_sync(ibb);

	return 0;
}

/*
 * __intel_bb_exec:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb
 * @flags: flags passed directly to execbuf
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Returns: 0 on success, otherwise errno.
 *
 * Note: In this step execobj for bb is allocated and inserted to the objects
 * array.
*/
int __intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
			   uint64_t flags, bool sync)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 *objects;
	int ret, fence, new_fence;

	ibb->objects[0]->relocs_ptr = to_user_pointer(ibb->relocs);
	ibb->objects[0]->relocation_count = ibb->num_relocs;
	ibb->objects[0]->handle = ibb->handle;
	ibb->objects[0]->offset = ibb->batch_offset;

	gem_write(ibb->fd, ibb->handle, 0, ibb->batch, ibb->size);

	memset(&execbuf, 0, sizeof(execbuf));
	objects = create_objects_array(ibb);
	execbuf.buffers_ptr = to_user_pointer(objects);
	execbuf.buffer_count = ibb->num_objects;
	execbuf.batch_len = end_offset;
	execbuf.rsvd1 = ibb->ctx;
	execbuf.flags = flags | I915_EXEC_BATCH_FIRST | I915_EXEC_FENCE_OUT;
	if (ibb->enforce_relocs)
		execbuf.flags &= ~I915_EXEC_NO_RELOC;
	execbuf.rsvd2 = 0;

	if (ibb->dump_base64)
		intel_bb_dump_base64(ibb, LINELEN);

	/* For debugging on CI, remove in final series */
	intel_bb_dump_execbuf(ibb, &execbuf);

	ret = __gem_execbuf_wr(ibb->fd, &execbuf);
	if (ret) {
		intel_bb_dump_execbuf(ibb, &execbuf);
		free(objects);
		return ret;
	}

	/* Update addresses in the cache */
	update_offsets(ibb, objects);

	/* Save/merge fences */
	fence = execbuf.rsvd2 >> 32;

	if (ibb->fence < 0) {
		ibb->fence = fence;
	} else {
		new_fence = sync_fence_merge(ibb->fence, fence);
		close(ibb->fence);
		close(fence);
		ibb->fence = new_fence;
	}

	if (sync || ibb->debug)
		igt_assert(intel_bb_sync(ibb) == 0);

	if (ibb->debug) {
		intel_bb_dump_execbuf(ibb, &execbuf);
		if (intel_bb_debug_tree) {
			igt_info("\nTree:\n");
			twalk(ibb->root, print_node);
		}
	}

	free(objects);

	return 0;
}

/**
 * intel_bb_exec:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb (for i915)
 * @flags: flags passed directly to execbuf
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Do execbuf on context selected during bb creation. Asserts on failure.
*/
void intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
		   uint64_t flags, bool sync)
{
	if (ibb->dump_base64)
		intel_bb_dump_base64(ibb, LINELEN);

	if (ibb->driver == INTEL_DRIVER_I915)
		igt_assert_eq(__intel_bb_exec(ibb, end_offset, flags, sync), 0);
	else
		igt_assert_eq(__xe_bb_exec(ibb, flags, sync), 0);
}

/**
 * intel_bb_get_object_address:
 * @ibb: pointer to intel_bb
 * @handle: object handle
 *
 * When objects addresses are previously pinned and we don't want to relocate
 * we need to acquire them from previous execbuf. Function returns previous
 * object offset for @handle or 0 if object is not found.
 */
uint64_t intel_bb_get_object_offset(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	igt_assert(ibb);

	found = tfind((void *)&object, &ibb->root, __compare_objects);
	if (!found)
		return INTEL_BUF_INVALID_ADDRESS;

	return (*found)->offset;
}

/*
 * intel_bb_emit_bbe:
 * @ibb: batchbuffer
 *
 * Outputs MI_BATCH_BUFFER_END and ensures batch is properly aligned.
 */
uint32_t intel_bb_emit_bbe(struct intel_bb *ibb)
{
	/* Mark the end of the buffer. */
	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	return intel_bb_offset(ibb);
}

/*
 * intel_bb_emit_flush_common:
 * @ibb: batchbuffer
 *
 * Emits instructions which completes batch buffer.
 *
 * Returns: offset in batch buffer where there's end of instructions.
 */
uint32_t intel_bb_emit_flush_common(struct intel_bb *ibb)
{
	if (intel_bb_offset(ibb) == 0)
		return 0;

	if (ibb->gen == 5) {
		/*
		 * emit gen5 w/a without batch space checks - we reserve that
		 * already.
		 */
		intel_bb_out(ibb, CMD_POLY_STIPPLE_OFFSET << 16);
		intel_bb_out(ibb, 0);
	}

	/* Round batchbuffer usage to 2 DWORDs. */
	if ((intel_bb_offset(ibb) & 4) == 0)
		intel_bb_out(ibb, 0);

	intel_bb_emit_bbe(ibb);

	return intel_bb_offset(ibb);
}

static void intel_bb_exec_with_ring(struct intel_bb *ibb,uint32_t ring)
{
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      ring | I915_EXEC_NO_RELOC, false);
	intel_bb_reset(ibb, false);
}

/*
 * intel_bb_flush:
 * @ibb: batchbuffer
 * @ring: ring
 *
 * If batch is not empty emit batch buffer end, execute on ring,
 * then reset the batch.
 */
void intel_bb_flush(struct intel_bb *ibb, uint32_t ring)
{
	if (intel_bb_emit_flush_common(ibb) == 0)
		return;

	intel_bb_exec_with_ring(ibb, ring);
}

/*
 * intel_bb_flush_render:
 * @ibb: batchbuffer
 *
 * If batch is not empty emit batch buffer end, find the render engine id,
 * execute on the ring and reset the batch. Context used to execute
 * is batch context.
 */
void intel_bb_flush_render(struct intel_bb *ibb)
{
	uint32_t ring;

	if (intel_bb_emit_flush_common(ibb) == 0)
		return;

	if (has_ctx_cfg(ibb))
		ring = find_engine(ibb->cfg, I915_ENGINE_CLASS_RENDER);
	else
		ring = I915_EXEC_RENDER;

	intel_bb_exec_with_ring(ibb, ring);
}

/*
 * intel_bb_flush_blit:
 * @ibb: batchbuffer
 *
 * If batch is not empty emit batch buffer end, find a suitable ring
 * (depending on gen and context configuration) and reset the batch.
 * Context used to execute is batch context.
 */
void intel_bb_flush_blit(struct intel_bb *ibb)
{
	uint32_t ring;

	if (intel_bb_emit_flush_common(ibb) == 0)
		return;

	if (has_ctx_cfg(ibb))
		ring = find_engine(ibb->cfg, I915_ENGINE_CLASS_COPY);
	else
		ring = HAS_BLT_RING(ibb->devid) ? I915_EXEC_BLT : I915_EXEC_DEFAULT;

	intel_bb_exec_with_ring(ibb, ring);
}

/*
 * intel_bb_copy_data:
 * @ibb: batchbuffer
 * @data: pointer of data which should be copied into batch
 * @bytes: number of bytes to copy, must be dword multiplied
 * @align: alignment in the batch
 *
 * Function copies @bytes of data pointed by @data into batch buffer.
 */
uint32_t intel_bb_copy_data(struct intel_bb *ibb,
			    const void *data, unsigned int bytes,
			    uint32_t align)
{
	uint32_t *subdata, offset;

	igt_assert((bytes & 3) == 0);

	intel_bb_ptr_align(ibb, align);
	offset = intel_bb_offset(ibb);
	igt_assert(offset + bytes < ibb->size);

	subdata = intel_bb_ptr(ibb);
	memcpy(subdata, data, bytes);
	intel_bb_ptr_add(ibb, bytes);

	return offset;
}

/*
 * intel_bb_blit_start:
 * @ibb: batchbuffer
 * @flags: flags to blit command
 *
 * Function emits XY_SRC_COPY_BLT instruction with size appropriate size
 * which depend on gen.
 */
void intel_bb_blit_start(struct intel_bb *ibb, uint32_t flags)
{
	if (blt_has_xy_src_copy(ibb->fd))
		intel_bb_out(ibb, XY_SRC_COPY_BLT_CMD |
			     XY_SRC_COPY_BLT_WRITE_ALPHA |
			     XY_SRC_COPY_BLT_WRITE_RGB |
			     flags |
			     (6 + 2 * (ibb->gen >= 8)));
	else if (blt_has_fast_copy(ibb->fd))
		intel_bb_out(ibb, XY_FAST_COPY_BLT | flags);
	else
		igt_assert_f(0, "No supported blit command found\n");
}

/*
 * intel_bb_emit_blt_copy:
 * @ibb: batchbuffer
 * @src: source buffer (intel_buf)
 * @src_x1: source x1 position
 * @src_y1: source y1 position
 * @src_pitch: source pitch
 * @dst: destination buffer (intel_buf)
 * @dst_x1: destination x1 position
 * @dst_y1: destination y1 position
 * @dst_pitch: destination pitch
 * @width: width of data to copy
 * @height: height of data to copy
 *
 * Function emits complete blit command.
 */
void intel_bb_emit_blt_copy(struct intel_bb *ibb,
			    struct intel_buf *src,
			    int src_x1, int src_y1, int src_pitch,
			    struct intel_buf *dst,
			    int dst_x1, int dst_y1, int dst_pitch,
			    int width, int height, int bpp)
{
	const unsigned int gen = ibb->gen;
	uint32_t cmd_bits = 0;
	uint32_t br13_bits;
	uint32_t mask;

	igt_assert(bpp*(src_x1 + width) <= 8*src_pitch);
	igt_assert(bpp*(dst_x1 + width) <= 8*dst_pitch);
	igt_assert(src_pitch * (src_y1 + height) <= src->surface[0].size);
	igt_assert(dst_pitch * (dst_y1 + height) <= dst->surface[0].size);

	if (gen >= 4 && src->tiling != I915_TILING_NONE) {
		src_pitch /= 4;
		if (blt_has_xy_src_copy(ibb->fd))
			cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
		else if (blt_has_fast_copy(ibb->fd))
			cmd_bits |= fast_copy_dword0(src->tiling, dst->tiling);
		else
			igt_assert_f(0, "No supported blit command found\n");
	}

	if (gen >= 4 && dst->tiling != I915_TILING_NONE) {
		dst_pitch /= 4;
		if (blt_has_xy_src_copy(ibb->fd))
			cmd_bits |= XY_SRC_COPY_BLT_DST_TILED;
		else
			cmd_bits |= fast_copy_dword0(src->tiling, dst->tiling);
	}

	CHECK_RANGE(src_x1); CHECK_RANGE(src_y1);
	CHECK_RANGE(dst_x1); CHECK_RANGE(dst_y1);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x1 + width); CHECK_RANGE(src_y1 + height);
	CHECK_RANGE(dst_x1 + width); CHECK_RANGE(dst_y1 + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	br13_bits = 0;
	if (blt_has_xy_src_copy(ibb->fd)) {
		switch (bpp) {
		case 8:
			break;
		case 16:		/* supporting only RGB565, not ARGB1555 */
			br13_bits |= 1 << 24;
			break;
		case 32:
			br13_bits |= 3 << 24;
			cmd_bits |= (XY_SRC_COPY_BLT_WRITE_ALPHA |
				     XY_SRC_COPY_BLT_WRITE_RGB);
			break;
		default:
			igt_fail(IGT_EXIT_FAILURE);
		}
	} else {
		br13_bits = fast_copy_dword1(ibb->fd, src->tiling, dst->tiling, bpp);
	}

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM(1));
		intel_bb_out(ibb, BCS_SWCTRL);

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst->tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		intel_bb_out(ibb, mask);
	}

	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, dst, true);

	intel_bb_blit_start(ibb, cmd_bits);
	intel_bb_out(ibb, (br13_bits) |
		     (0xcc << 16) | /* copy ROP */
		     dst_pitch);
	intel_bb_out(ibb, (dst_y1 << 16) | dst_x1); /* dst x1,y1 */
	intel_bb_out(ibb, ((dst_y1 + height) << 16) | (dst_x1 + width)); /* dst x2,y2 */
	intel_bb_emit_reloc_fenced(ibb, dst->handle,
				   I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
				   0, dst->addr.offset);
	intel_bb_out(ibb, (src_y1 << 16) | src_x1); /* src x1,y1 */
	intel_bb_out(ibb, src_pitch);
	intel_bb_emit_reloc_fenced(ibb, src->handle,
				   I915_GEM_DOMAIN_RENDER, 0,
				   0, src->addr.offset);

	if (gen >= 6 && src->handle == dst->handle) {
		intel_bb_out(ibb, XY_SETUP_CLIP_BLT_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
	}

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		igt_assert(ibb->gen >= 6);
		intel_bb_out(ibb, MI_FLUSH_DW_CMD | 2);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);

		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM(1));
		intel_bb_out(ibb, BCS_SWCTRL);
		intel_bb_out(ibb, (BCS_SRC_Y | BCS_DST_Y) << 16);
	}
}

void intel_bb_blt_copy(struct intel_bb *ibb,
		       struct intel_buf *src,
		       int src_x1, int src_y1, int src_pitch,
		       struct intel_buf *dst,
		       int dst_x1, int dst_y1, int dst_pitch,
		       int width, int height, int bpp)
{
	intel_bb_emit_blt_copy(ibb, src, src_x1, src_y1, src_pitch,
			       dst, dst_x1, dst_y1, dst_pitch,
			       width, height, bpp);
	intel_bb_flush_blit(ibb);
}

/**
 * intel_bb_copy_intel_buf:
 * @batch: batchbuffer object
 * @src: source buffer (intel_buf)
 * @dst: destination libdrm buffer object
 * @size: size of the copy range in bytes
 *
 * Emits a copy operation using blitter commands into the supplied batch.
 * A total of @size bytes from the start of @src is copied
 * over to @dst. Note that @size must be page-aligned.
 */
void intel_bb_copy_intel_buf(struct intel_bb *ibb,
			     struct intel_buf *src, struct intel_buf *dst,
			     long int size)
{
	igt_assert(size % 4096 == 0);

	intel_bb_blt_copy(ibb,
			  src, 0, 0, 4096,
			  dst, 0, 0, 4096,
			  4096/4, size/4096, 32);
}

/**
 * igt_get_huc_copyfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific huc copy function pointer for the device specified
 * with @devid. Will return NULL when no media spin function is implemented.
 */
igt_huc_copyfunc_t igt_get_huc_copyfunc(int devid)
{
	igt_huc_copyfunc_t copy = NULL;

	if (IS_GEN12(devid) || IS_GEN11(devid) || IS_GEN9(devid))
		copy = gen9_huc_copyfunc;

	return copy;
}

/**
 * intel_bb_track:
 * @do_tracking: bool
 *
 * Turn on (true) or off (false) tracking for intel_batchbuffers.
 */
void intel_bb_track(bool do_tracking)
{
	if (intel_bb_do_tracking == do_tracking)
		return;

	if (intel_bb_do_tracking) {
		struct intel_bb *entry, *tmp;

		pthread_mutex_lock(&intel_bb_list_lock);
		igt_list_for_each_entry_safe(entry, tmp, &intel_bb_list, link)
			igt_list_del(&entry->link);
		pthread_mutex_unlock(&intel_bb_list_lock);
	}

	intel_bb_do_tracking = do_tracking;
}

static void __intel_bb_reinit_alloc(struct intel_bb *ibb)
{
	if (ibb->allocator_type == INTEL_ALLOCATOR_NONE)
		return;

	ibb->allocator_handle = intel_allocator_open_full(ibb->fd, ibb->ctx,
							  ibb->allocator_start, ibb->allocator_end,
							  ibb->allocator_type,
							  ibb->allocator_strategy,
							  ibb->alignment);

	intel_bb_reset(ibb, true);
}

/**
 * intel_bb_reinit_allocator:
 *
 * Reinit allocator and get offsets in tracked intel_batchbuffers.
 */
void intel_bb_reinit_allocator(void)
{
	struct intel_bb *iter;

	if (!intel_bb_do_tracking)
		return;

	pthread_mutex_lock(&intel_bb_list_lock);
	igt_list_for_each_entry(iter, &intel_bb_list, link)
		__intel_bb_reinit_alloc(iter);
	pthread_mutex_unlock(&intel_bb_list_lock);
}
