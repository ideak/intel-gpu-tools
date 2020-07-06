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

#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <search.h>

#include "drm.h"
#include "drmtest.h"
#include "intel_batchbuffer.h"
#include "intel_bufmgr.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "veboxcopy.h"
#include "rendercopy.h"
#include "media_fill.h"
#include "ioctl_wrappers.h"
#include "sw_sync.h"
#include "i915/gem_mman.h"
#include "media_spin.h"
#include "gpgpu_fill.h"
#include "igt_aux.h"
#include "igt_rand.h"
#include "i830_reg.h"

#include <i915_drm.h>

#define BCS_SWCTRL 0x22200
#define BCS_SRC_Y (1 << 0)
#define BCS_DST_Y (1 << 1)

/**
 * SECTION:intel_batchbuffer
 * @short_description: Batchbuffer and blitter support
 * @title: Batch Buffer
 * @include: igt.h
 *
 * This library provides some basic support for batchbuffers and using the
 * blitter engine based upon libdrm. A new batchbuffer is allocated with
 * intel_batchbuffer_alloc() and for simple blitter commands submitted with
 * intel_batchbuffer_flush().
 *
 * It also provides some convenient macros to easily emit commands into
 * batchbuffers. All those macros presume that a pointer to a #intel_batchbuffer
 * structure called batch is in scope. The basic macros are #BEGIN_BATCH,
 * #OUT_BATCH, #OUT_RELOC and #ADVANCE_BATCH.
 *
 * Note that this library's header pulls in the [i-g-t core](igt-gpu-tools-i-g-t-core.html)
 * library as a dependency.
 */

/**
 * intel_batchbuffer_align:
 * @batch: batchbuffer object
 * @align: value in bytes to which we want to align
 *
 * Aligns the current in-batch offset to the given value.
 *
 * Returns: Batchbuffer offset aligned to the given value.
 */
uint32_t
intel_batchbuffer_align(struct intel_batchbuffer *batch, uint32_t align)
{
	uint32_t offset = batch->ptr - batch->buffer;

	offset = ALIGN(offset, align);
	batch->ptr = batch->buffer + offset;
	return offset;
}

/**
 * intel_batchbuffer_subdata_alloc:
 * @batch: batchbuffer object
 * @size: amount of bytes need to allocate
 * @align: value in bytes to which we want to align
 *
 * Verify if sufficient @size within @batch is available to deny overflow.
 * Then allocate @size bytes within @batch.
 *
 * Returns: Offset within @batch between allocated subdata and base of @batch.
 */
void *
intel_batchbuffer_subdata_alloc(struct intel_batchbuffer *batch, uint32_t size,
				uint32_t align)
{
	uint32_t offset = intel_batchbuffer_align(batch, align);

	igt_assert(size <= intel_batchbuffer_space(batch));

	batch->ptr += size;
	return memset(batch->buffer + offset, 0, size);
}

/**
 * intel_batchbuffer_subdata_offset:
 * @batch: batchbuffer object
 * @ptr: pointer to given data
 *
 * Returns: Offset within @batch between @ptr and base of @batch.
 */
uint32_t
intel_batchbuffer_subdata_offset(struct intel_batchbuffer *batch, void *ptr)
{
	return (uint8_t *)ptr - batch->buffer;
}

/**
 * intel_batchbuffer_reset:
 * @batch: batchbuffer object
 *
 * Resets @batch by allocating a new gem buffer object as backing storage.
 */
void
intel_batchbuffer_reset(struct intel_batchbuffer *batch)
{
	if (batch->bo != NULL) {
		drm_intel_bo_unreference(batch->bo);
		batch->bo = NULL;
	}

	batch->bo = drm_intel_bo_alloc(batch->bufmgr, "batchbuffer",
				       BATCH_SZ, 4096);

	memset(batch->buffer, 0, sizeof(batch->buffer));
	batch->ctx = NULL;

	batch->ptr = batch->buffer;
	batch->end = NULL;
}

/**
 * intel_batchbuffer_alloc:
 * @bufmgr: libdrm buffer manager
 * @devid: pci device id of the drm device
 *
 * Allocates a new batchbuffer object. @devid must be supplied since libdrm
 * doesn't expose it directly.
 *
 * Returns: The allocated and initialized batchbuffer object.
 */
struct intel_batchbuffer *
intel_batchbuffer_alloc(drm_intel_bufmgr *bufmgr, uint32_t devid)
{
	struct intel_batchbuffer *batch = calloc(sizeof(*batch), 1);

	batch->bufmgr = bufmgr;
	batch->devid = devid;
	batch->gen = intel_gen(devid);
	intel_batchbuffer_reset(batch);

	return batch;
}

/**
 * intel_batchbuffer_free:
 * @batch: batchbuffer object
 *
 * Releases all resource of the batchbuffer object @batch.
 */
void
intel_batchbuffer_free(struct intel_batchbuffer *batch)
{
	drm_intel_bo_unreference(batch->bo);
	batch->bo = NULL;
	free(batch);
}

#define CMD_POLY_STIPPLE_OFFSET       0x7906

static unsigned int
flush_on_ring_common(struct intel_batchbuffer *batch, int ring)
{
	unsigned int used = batch->ptr - batch->buffer;

	if (used == 0)
		return 0;

	if (IS_GEN5(batch->devid)) {
		/* emit gen5 w/a without batch space checks - we reserve that
		 * already. */
		*(uint32_t *) (batch->ptr) = CMD_POLY_STIPPLE_OFFSET << 16;
		batch->ptr += 4;
		*(uint32_t *) (batch->ptr) = 0;
		batch->ptr += 4;
	}

	/* Round batchbuffer usage to 2 DWORDs. */
	if ((used & 4) == 0) {
		*(uint32_t *) (batch->ptr) = 0; /* noop */
		batch->ptr += 4;
	}

	/* Mark the end of the buffer. */
	*(uint32_t *)(batch->ptr) = MI_BATCH_BUFFER_END; /* noop */
	batch->ptr += 4;
	return batch->ptr - batch->buffer;
}

/**
 * intel_batchbuffer_flush_on_ring:
 * @batch: batchbuffer object
 * @ring: execbuf ring flag
 *
 * Submits the batch for execution on @ring.
 */
void
intel_batchbuffer_flush_on_ring(struct intel_batchbuffer *batch, int ring)
{
	unsigned int used = flush_on_ring_common(batch, ring);
	drm_intel_context *ctx;

	if (used == 0)
		return;

	do_or_die(drm_intel_bo_subdata(batch->bo, 0, used, batch->buffer));

	batch->ptr = NULL;

	/* XXX bad kernel API */
	ctx = batch->ctx;
	if (ring != I915_EXEC_RENDER)
		ctx = NULL;
	do_or_die(drm_intel_gem_bo_context_exec(batch->bo, ctx, used, ring));

	intel_batchbuffer_reset(batch);
}

void
intel_batchbuffer_set_context(struct intel_batchbuffer *batch,
				     drm_intel_context *context)
{
	batch->ctx = context;
}

/**
 * intel_batchbuffer_flush_with_context:
 * @batch: batchbuffer object
 * @context: libdrm hardware context object
 *
 * Submits the batch for execution on the render engine with the supplied
 * hardware context.
 */
void
intel_batchbuffer_flush_with_context(struct intel_batchbuffer *batch,
				     drm_intel_context *context)
{
	int ret;
	unsigned int used = flush_on_ring_common(batch, I915_EXEC_RENDER);

	if (used == 0)
		return;

	ret = drm_intel_bo_subdata(batch->bo, 0, used, batch->buffer);
	igt_assert(ret == 0);

	batch->ptr = NULL;

	ret = drm_intel_gem_bo_context_exec(batch->bo, context, used,
					    I915_EXEC_RENDER);
	igt_assert(ret == 0);

	intel_batchbuffer_reset(batch);
}

/**
 * intel_batchbuffer_flush:
 * @batch: batchbuffer object
 *
 * Submits the batch for execution on the blitter engine, selecting the right
 * ring depending upon the hardware platform.
 */
void
intel_batchbuffer_flush(struct intel_batchbuffer *batch)
{
	int ring = 0;
	if (HAS_BLT_RING(batch->devid))
		ring = I915_EXEC_BLT;
	intel_batchbuffer_flush_on_ring(batch, ring);
}


/**
 * intel_batchbuffer_emit_reloc:
 * @batch: batchbuffer object
 * @buffer: relocation target libdrm buffer object
 * @delta: delta value to add to @buffer's gpu address
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @fenced: whether this gpu access requires fences
 *
 * Emits both a libdrm relocation entry pointing at @buffer and the pre-computed
 * DWORD of @batch's presumed gpu address plus the supplied @delta into @batch.
 *
 * Note that @fenced is only relevant if @buffer is actually tiled.
 *
 * This is the only way buffers get added to the validate list.
 */
void
intel_batchbuffer_emit_reloc(struct intel_batchbuffer *batch,
                             drm_intel_bo *buffer, uint64_t delta,
			     uint32_t read_domains, uint32_t write_domain,
			     int fenced)
{
	uint64_t offset;
	int ret;

	if (batch->ptr - batch->buffer > BATCH_SZ)
		igt_info("bad relocation ptr %p map %p offset %d size %d\n",
			 batch->ptr, batch->buffer,
			 (int)(batch->ptr - batch->buffer), BATCH_SZ);

	if (fenced)
		ret = drm_intel_bo_emit_reloc_fence(batch->bo, batch->ptr - batch->buffer,
						    buffer, delta,
						    read_domains, write_domain);
	else
		ret = drm_intel_bo_emit_reloc(batch->bo, batch->ptr - batch->buffer,
					      buffer, delta,
					      read_domains, write_domain);

	offset = buffer->offset64;
	offset += delta;
	intel_batchbuffer_emit_dword(batch, offset);
	if (batch->gen >= 8)
		intel_batchbuffer_emit_dword(batch, offset >> 32);
	igt_assert(ret == 0);
}

/**
 * intel_batchbuffer_copy_data:
 * @batch: batchbuffer object
 * @data: pointer to the data to write into the batchbuffer
 * @bytes: number of bytes to write into the batchbuffer
 * @align: value in bytes to which we want to align
 *
 * This transfers the given @data into the batchbuffer. Note that the length
 * must be DWORD aligned, i.e. multiples of 32bits. The caller must
 * confirm that there is enough space in the batch for the data to be
 * copied.
 *
 * Returns: Offset of copied data.
 */
uint32_t
intel_batchbuffer_copy_data(struct intel_batchbuffer *batch,
			    const void *data, unsigned int bytes,
			    uint32_t align)
{
	uint32_t *subdata;

	igt_assert((bytes & 3) == 0);
	subdata = intel_batchbuffer_subdata_alloc(batch, bytes, align);
	memcpy(subdata, data, bytes);

	return intel_batchbuffer_subdata_offset(batch, subdata);
}

#define CHECK_RANGE(x)	do { \
	igt_assert_lte(0, (x)); \
	igt_assert_lt((x), (1 << 15)); \
} while (0)

/**
 * intel_blt_copy:
 * @batch: batchbuffer object
 * @src_bo: source libdrm buffer object
 * @src_x1: source pixel x-coordination
 * @src_y1: source pixel y-coordination
 * @src_pitch: @src_bo's pitch in bytes
 * @dst_bo: destination libdrm buffer object
 * @dst_x1: destination pixel x-coordination
 * @dst_y1: destination pixel y-coordination
 * @dst_pitch: @dst_bo's pitch in bytes
 * @width: width of the copied rectangle
 * @height: height of the copied rectangle
 * @bpp: bits per pixel
 *
 * This emits a 2D copy operation using blitter commands into the supplied batch
 * buffer object.
 */
void
intel_blt_copy(struct intel_batchbuffer *batch,
	       drm_intel_bo *src_bo, int src_x1, int src_y1, int src_pitch,
	       drm_intel_bo *dst_bo, int dst_x1, int dst_y1, int dst_pitch,
	       int width, int height, int bpp)
{
	const int gen = batch->gen;
	uint32_t src_tiling, dst_tiling, swizzle;
	uint32_t cmd_bits = 0;
	uint32_t br13_bits;

	igt_assert(bpp*(src_x1 + width) <= 8*src_pitch);
	igt_assert(bpp*(dst_x1 + width) <= 8*dst_pitch);
	igt_assert(src_pitch * (src_y1 + height) <= src_bo->size);
	igt_assert(dst_pitch * (dst_y1 + height) <= dst_bo->size);

	drm_intel_bo_get_tiling(src_bo, &src_tiling, &swizzle);
	drm_intel_bo_get_tiling(dst_bo, &dst_tiling, &swizzle);

	if (gen >= 4 && src_tiling != I915_TILING_NONE) {
		src_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
	}

	if (gen >= 4 && dst_tiling != I915_TILING_NONE) {
		dst_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_DST_TILED;
	}

	CHECK_RANGE(src_x1); CHECK_RANGE(src_y1);
	CHECK_RANGE(dst_x1); CHECK_RANGE(dst_y1);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x1 + width); CHECK_RANGE(src_y1 + height);
	CHECK_RANGE(dst_x1 + width); CHECK_RANGE(dst_y1 + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	br13_bits = 0;
	switch (bpp) {
	case 8:
		break;
	case 16:		/* supporting only RGB565, not ARGB1555 */
		br13_bits |= 1 << 24;
		break;
	case 32:
		br13_bits |= 3 << 24;
		cmd_bits |= XY_SRC_COPY_BLT_WRITE_ALPHA |
			    XY_SRC_COPY_BLT_WRITE_RGB;
		break;
	default:
		igt_fail(IGT_EXIT_FAILURE);
	}

	BLIT_COPY_BATCH_START(cmd_bits);
	OUT_BATCH((br13_bits) |
		  (0xcc << 16) | /* copy ROP */
		  dst_pitch);
	OUT_BATCH((dst_y1 << 16) | dst_x1); /* dst x1,y1 */
	OUT_BATCH(((dst_y1 + height) << 16) | (dst_x1 + width)); /* dst x2,y2 */
	OUT_RELOC_FENCED(dst_bo, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, 0);
	OUT_BATCH((src_y1 << 16) | src_x1); /* src x1,y1 */
	OUT_BATCH(src_pitch);
	OUT_RELOC_FENCED(src_bo, I915_GEM_DOMAIN_RENDER, 0, 0);
	ADVANCE_BATCH();

#define CMD_POLY_STIPPLE_OFFSET       0x7906
	if (gen == 5) {
		BEGIN_BATCH(2, 0);
		OUT_BATCH(CMD_POLY_STIPPLE_OFFSET << 16);
		OUT_BATCH(0);
		ADVANCE_BATCH();
	}

	if (gen >= 6 && src_bo == dst_bo) {
		BEGIN_BATCH(3, 0);
		OUT_BATCH(XY_SETUP_CLIP_BLT_CMD);
		OUT_BATCH(0);
		OUT_BATCH(0);
		ADVANCE_BATCH();
	}

	intel_batchbuffer_flush(batch);
}

/**
 * intel_copy_bo:
 * @batch: batchbuffer object
 * @src_bo: source libdrm buffer object
 * @dst_bo: destination libdrm buffer object
 * @size: size of the copy range in bytes
 *
 * This emits a copy operation using blitter commands into the supplied batch
 * buffer object. A total of @size bytes from the start of @src_bo is copied
 * over to @dst_bo. Note that @size must be page-aligned.
 */
void
intel_copy_bo(struct intel_batchbuffer *batch,
	      drm_intel_bo *dst_bo, drm_intel_bo *src_bo,
	      long int size)
{
	igt_assert(size % 4096 == 0);

	intel_blt_copy(batch,
		       src_bo, 0, 0, 4096,
		       dst_bo, 0, 0, 4096,
		       4096/4, size/4096, 32);
}

/**
 * igt_buf_width:
 * @buf: the i-g-t buffer object
 *
 * Computes the width in 32-bit pixels of the given buffer.
 *
 * Returns:
 * The width of the buffer.
 */
unsigned igt_buf_width(const struct igt_buf *buf)
{
	return buf->surface[0].stride/(buf->bpp / 8);
}

/**
 * igt_buf_height:
 * @buf: the i-g-t buffer object
 *
 * Computes the height in 32-bit pixels of the given buffer.
 *
 * Returns:
 * The height of the buffer.
 */
unsigned igt_buf_height(const struct igt_buf *buf)
{
	return buf->surface[0].size/buf->surface[0].stride;
}

/**
 * igt_buf_intel_ccs_width:
 * @buf: the Intel i-g-t buffer object
 * @gen: device generation
 *
 * Computes the width of ccs buffer when considered as Intel surface data.
 *
 * Returns:
 * The width of the ccs buffer data.
 */
unsigned int igt_buf_intel_ccs_width(int gen, const struct igt_buf *buf)
{
	/*
	 * GEN12+: The CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the width of the CCS unit is 4*32=128 pixels on the
	 * main surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(igt_buf_width(buf), 128) * 64;

	return DIV_ROUND_UP(igt_buf_width(buf), 1024) * 128;
}

/**
 * igt_buf_intel_ccs_height:
 * @buf: the i-g-t buffer object
 * @gen: device generation
 *
 * Computes the height of ccs buffer when considered as Intel surface data.
 *
 * Returns:
 * The height of the ccs buffer data.
 */
unsigned int igt_buf_intel_ccs_height(int gen, const struct igt_buf *buf)
{
	/*
	 * GEN12+: The CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the height of the CCS unit is 32 pixel rows on the main
	 * surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(igt_buf_height(buf), 32);

	return DIV_ROUND_UP(igt_buf_height(buf), 512) * 32;
}

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

static uint32_t fast_copy_dword0(unsigned int src_tiling,
				 unsigned int dst_tiling)
{
	uint32_t dword0 = 0;

	dword0 |= XY_FAST_COPY_BLT;

	switch (src_tiling) {
	case I915_TILING_X:
		dword0 |= XY_FAST_COPY_SRC_TILING_X;
		break;
	case I915_TILING_Y:
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

static uint32_t fast_copy_dword1(unsigned int src_tiling,
				 unsigned int dst_tiling,
				 int bpp)
{
	uint32_t dword1 = 0;

	if (src_tiling == I915_TILING_Yf)
		dword1 |= XY_FAST_COPY_SRC_TILING_Yf;
	if (dst_tiling == I915_TILING_Yf)
		dword1 |= XY_FAST_COPY_DST_TILING_Yf;

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
		uint32_t gem_handle, uint32_t delta, /* in bytes */
		uint32_t offset, /* in dwords */
		uint32_t read_domains, uint32_t write_domains)
{
	reloc->target_handle = gem_handle;
	reloc->delta = delta;
	reloc->offset = offset * sizeof(uint32_t);
	reloc->presumed_offset = 0;
	reloc->read_domains = read_domains;
	reloc->write_domain = write_domains;
}

static void
fill_object(struct drm_i915_gem_exec_object2 *obj, uint32_t gem_handle,
	    struct drm_i915_gem_relocation_entry *relocs, uint32_t count)
{
	memset(obj, 0, sizeof(*obj));
	obj->handle = gem_handle;
	obj->relocation_count = count;
	obj->relocs_ptr = to_user_pointer(relocs);
}

static void exec_blit(int fd,
		      struct drm_i915_gem_exec_object2 *objs, uint32_t count,
		      int gen)
{
	struct drm_i915_gem_execbuffer2 exec = {
		.buffers_ptr = to_user_pointer(objs),
		.buffer_count = count,
		.flags = gen >= 6 ? I915_EXEC_BLT : 0,
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
 * igt_blitter_src_copy:
 * @fd: file descriptor of the i915 driver
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 *
 * Copy @src into @dst using the XY_SRC blit command.
 */
void igt_blitter_src_copy(int fd,
			  /* src */
			  uint32_t src_handle,
			  uint32_t src_delta,
			  uint32_t src_stride,
			  uint32_t src_tiling,
			  uint32_t src_x, uint32_t src_y,

			  /* size */
			  uint32_t width, uint32_t height,

			  /* bpp */
			  uint32_t bpp,

			  /* dst */
			  uint32_t dst_handle,
			  uint32_t dst_delta,
			  uint32_t dst_stride,
			  uint32_t dst_tiling,
			  uint32_t dst_x, uint32_t dst_y)
{
	uint32_t batch[32];
	struct drm_i915_gem_exec_object2 objs[3];
	struct drm_i915_gem_relocation_entry relocs[2];
	uint32_t batch_handle;
	uint32_t src_pitch, dst_pitch;
	uint32_t dst_reloc_offset, src_reloc_offset;
	uint32_t gen = intel_gen(intel_get_drm_devid(fd));
	const bool has_64b_reloc = gen >= 8;
	int i = 0;

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

		batch[i++] = MI_LOAD_REGISTER_IMM;
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
	batch[i++] = dst_delta; /* dst address lower bits */
	if (has_64b_reloc)
		batch[i++] = 0;	/* dst address upper bits */
	batch[i++] = (src_y << 16) | src_x; /* src x1,y1 */
	batch[i++] = src_pitch;
	src_reloc_offset = i;
	batch[i++] = src_delta; /* src address lower bits */
	if (has_64b_reloc)
		batch[i++] = 0;	/* src address upper bits */

	if ((src_tiling | dst_tiling) >= I915_TILING_Y) {
		igt_assert(gen >= 6);
		batch[i++] = MI_FLUSH_DW | 2;
		batch[i++] = 0;
		batch[i++] = 0;
		batch[i++] = 0;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;
		batch[i++] = (BCS_SRC_Y | BCS_DST_Y) << 16;
	}

	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	igt_assert(i <= ARRAY_SIZE(batch));

	batch_handle = gem_create(fd, 4096);
	gem_write(fd, batch_handle, 0, batch, sizeof(batch));

	fill_relocation(&relocs[0], dst_handle, dst_delta, dst_reloc_offset,
			I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
	fill_relocation(&relocs[1], src_handle, src_delta, src_reloc_offset,
			I915_GEM_DOMAIN_RENDER, 0);

	fill_object(&objs[0], dst_handle, NULL, 0);
	fill_object(&objs[1], src_handle, NULL, 0);
	fill_object(&objs[2], batch_handle, relocs, 2);

	objs[0].flags |= EXEC_OBJECT_NEEDS_FENCE;
	objs[1].flags |= EXEC_OBJECT_NEEDS_FENCE;

	exec_blit(fd, objs, 3, gen);

	gem_close(fd, batch_handle);
}

/**
 * igt_blitter_fast_copy__raw:
 * @fd: file descriptor of the i915 driver
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 *
 * Like igt_blitter_fast_copy(), but talking to the kernel directly.
 */
void igt_blitter_fast_copy__raw(int fd,
				/* src */
				uint32_t src_handle,
				unsigned int src_delta,
				unsigned int src_stride,
				unsigned int src_tiling,
				unsigned int src_x, unsigned src_y,

				/* size */
				unsigned int width, unsigned int height,

				/* bpp */
				int bpp,

				/* dst */
				uint32_t dst_handle,
				unsigned dst_delta,
				unsigned int dst_stride,
				unsigned int dst_tiling,
				unsigned int dst_x, unsigned dst_y)
{
	uint32_t batch[12];
	struct drm_i915_gem_exec_object2 objs[3];
	struct drm_i915_gem_relocation_entry relocs[2];
	uint32_t batch_handle;
	uint32_t dword0, dword1;
	uint32_t src_pitch, dst_pitch;
	int i = 0;

	src_pitch = fast_copy_pitch(src_stride, src_tiling);
	dst_pitch = fast_copy_pitch(dst_stride, dst_tiling);
	dword0 = fast_copy_dword0(src_tiling, dst_tiling);
	dword1 = fast_copy_dword1(src_tiling, dst_tiling, bpp);

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
	batch[i++] = dst_delta; /* dst address lower bits */
	batch[i++] = 0;	/* dst address upper bits */
	batch[i++] = (src_y << 16) | src_x; /* src x1,y1 */
	batch[i++] = src_pitch;
	batch[i++] = src_delta; /* src address lower bits */
	batch[i++] = 0;	/* src address upper bits */
	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	igt_assert(i == ARRAY_SIZE(batch));

	batch_handle = gem_create(fd, 4096);
	gem_write(fd, batch_handle, 0, batch, sizeof(batch));

	fill_relocation(&relocs[0], dst_handle, dst_delta, 4,
			I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
	fill_relocation(&relocs[1], src_handle, src_delta, 8, I915_GEM_DOMAIN_RENDER, 0);

	fill_object(&objs[0], dst_handle, NULL, 0);
	fill_object(&objs[1], src_handle, NULL, 0);
	fill_object(&objs[2], batch_handle, relocs, 2);

	exec_blit(fd, objs, 3, intel_gen(intel_get_drm_devid(fd)));

	gem_close(fd, batch_handle);
}

/**
 * igt_blitter_fast_copy:
 * @batch: batchbuffer object
 * @src: source i-g-t buffer object
 * @src_delta: offset into the source i-g-t bo
 * @src_x: source pixel x-coordination
 * @src_y: source pixel y-coordination
 * @width: width of the copied rectangle
 * @height: height of the copied rectangle
 * @dst: destination i-g-t buffer object
 * @dst_delta: offset into the destination i-g-t bo
 * @dst_x: destination pixel x-coordination
 * @dst_y: destination pixel y-coordination
 *
 * Copy @src into @dst using the gen9 fast copy blitter command.
 *
 * The source and destination surfaces cannot overlap.
 */
void igt_blitter_fast_copy(struct intel_batchbuffer *batch,
			   const struct igt_buf *src, unsigned src_delta,
			   unsigned src_x, unsigned src_y,
			   unsigned width, unsigned height,
			   int bpp,
			   const struct igt_buf *dst, unsigned dst_delta,
			   unsigned dst_x, unsigned dst_y)
{
	uint32_t src_pitch, dst_pitch;
	uint32_t dword0, dword1;

	igt_assert(src->bpp == dst->bpp);

	src_pitch = fast_copy_pitch(src->surface[0].stride, src->tiling);
	dst_pitch = fast_copy_pitch(dst->surface[0].stride, src->tiling);
	dword0 = fast_copy_dword0(src->tiling, dst->tiling);
	dword1 = fast_copy_dword1(src->tiling, dst->tiling, dst->bpp);

	CHECK_RANGE(src_x); CHECK_RANGE(src_y);
	CHECK_RANGE(dst_x); CHECK_RANGE(dst_y);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x + width); CHECK_RANGE(src_y + height);
	CHECK_RANGE(dst_x + width); CHECK_RANGE(dst_y + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	BEGIN_BATCH(10, 2);
	OUT_BATCH(dword0);
	OUT_BATCH(dword1 | dst_pitch);
	OUT_BATCH((dst_y << 16) | dst_x); /* dst x1,y1 */
	OUT_BATCH(((dst_y + height) << 16) | (dst_x + width)); /* dst x2,y2 */
	OUT_RELOC(dst->bo, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, dst_delta);
	OUT_BATCH(0);	/* dst address upper bits */
	OUT_BATCH((src_y << 16) | src_x); /* src x1,y1 */
	OUT_BATCH(src_pitch);
	OUT_RELOC(src->bo, I915_GEM_DOMAIN_RENDER, 0, src_delta);
	OUT_BATCH(0);	/* src address upper bits */
	ADVANCE_BATCH();

	intel_batchbuffer_flush(batch);
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

	if (IS_GEN12(devid))
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

	if (IS_GEN11(devid))
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

	if (IS_GEN7(devid))
		fill = gen7_gpgpu_fillfunc;
	else if (IS_GEN8(devid))
		fill = gen8_gpgpu_fillfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid))
		fill = gen9_gpgpu_fillfunc;
	else if (IS_GEN11(devid))
		fill = gen11_gpgpu_fillfunc;
	else if (IS_GEN12(devid))
		fill = gen12_gpgpu_fillfunc;

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
	uint32_t num;

	if (ibb->num_objects == ibb->allocated_objects) {
		num = 4096 / sizeof(*ibb->objects);
		ibb->objects = realloc(ibb->objects,
				       sizeof(*ibb->objects) *
				       (num + ibb->allocated_objects));
		igt_assert(ibb->objects);
		ibb->allocated_objects += num;

		memset(&ibb->objects[ibb->num_objects],	0,
		       num * sizeof(*ibb->objects));
	}
}

static inline uint64_t __intel_bb_propose_offset(struct intel_bb *ibb)
{
	uint64_t offset;

	if (ibb->enforce_relocs)
		return 0;

	/* randomize the address, we try to avoid relocations */
	offset = hars_petruska_f54_1_random64(&ibb->prng);
	offset += 256 << 10; /* Keep the low 256k clear, for negative deltas */
	offset &= ibb->gtt_size - 1;

	return offset;
}

/**
 * intel_bb_create:
 * @i915: drm fd
 * @size: size of the batchbuffer
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
static struct intel_bb *
__intel_bb_create(int i915, uint32_t size, bool do_relocs)
{
	struct intel_bb *ibb = calloc(1, sizeof(*ibb));
	uint64_t gtt_size;
	uint64_t bb_address;

	igt_assert(ibb);

	ibb->i915 = i915;
	ibb->devid = intel_get_drm_devid(i915);
	ibb->gen = intel_gen(ibb->devid);
	ibb->enforce_relocs = do_relocs;
	ibb->handle = gem_create(i915, size);
	ibb->size = size;
	ibb->batch = calloc(1, size);
	igt_assert(ibb->batch);
	ibb->ptr = ibb->batch;
	ibb->prng = (uint32_t) to_user_pointer(ibb);
	ibb->fence = -1;

	gtt_size = gem_aperture_size(i915);
	if (!gem_uses_full_ppgtt(i915))
		gtt_size /= 2;
	if ((gtt_size - 1) >> 32)
		ibb->supports_48b_address = true;
	ibb->gtt_size = gtt_size;

	__reallocate_objects(ibb);
	bb_address = __intel_bb_propose_offset(ibb);
	intel_bb_add_object(ibb, ibb->handle, bb_address, false);

	ibb->refcount = 1;

	return ibb;
}

/**
 * intel_bb_create:
 * @i915: drm fd
 * @size: size of the batchbuffer
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create(int i915, uint32_t size)
{
	return __intel_bb_create(i915, size, false);
}

/**
 * intel_bb_create_with_relocs:
 * @i915: drm fd
 * @size: size of the batchbuffer
 *
 * Disable passing or randomizing addresses. This will lead to relocations
 * when objects are not previously pinned.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_with_relocs(int i915, uint32_t size)
{
	return __intel_bb_create(i915, size, true);
}

/*
 * tdestroy() calls free function for each node, but we spread tree
 * on objects array, so do nothing.
 */
static void __do_nothing(void *node)
{
	(void) node;
}

static void __intel_bb_destroy_relocations(struct intel_bb *ibb)
{
	uint32_t i;

	/* Free relocations */
	for (i = 0; i < ibb->num_objects; i++) {
		free(from_user_pointer(ibb->objects[i].relocs_ptr));
		ibb->objects[i].relocs_ptr = to_user_pointer(NULL);
		ibb->objects[i].relocation_count = 0;
	}

	ibb->relocs = NULL;
	ibb->num_relocs = 0;
	ibb->allocated_relocs = 0;
}

static void __intel_bb_destroy_objects(struct intel_bb *ibb)
{
	free(ibb->objects);
	ibb->objects = NULL;

	tdestroy(ibb->root, __do_nothing);
	ibb->root = NULL;

	ibb->num_objects = 0;
	ibb->allocated_objects = 0;
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

	__intel_bb_destroy_relocations(ibb);
	__intel_bb_destroy_objects(ibb);
	gem_close(ibb->i915, ibb->handle);

	if (ibb->fence >= 0)
		close(ibb->fence);

	free(ibb);
}

/*
 * intel_bb_reset:
 * @ibb: pointer to intel_bb
 * @purge_objects_cache: if true destroy internal execobj and relocs + cache
 *
 * Recreate batch bo when there's no additional reference.
*/
void intel_bb_reset(struct intel_bb *ibb, bool purge_objects_cache)
{
	uint64_t bb_address;

	if (purge_objects_cache && ibb->refcount > 1)
		igt_warn("Cannot purge objects cache on bb, refcount > 1!");

	/* Someone keeps reference, just exit */
	if (ibb->refcount > 1)
		return;

	__intel_bb_destroy_relocations(ibb);

	if (purge_objects_cache) {
		__intel_bb_destroy_objects(ibb);
		__reallocate_objects(ibb);
	}

	gem_close(ibb->i915, ibb->handle);
	ibb->handle = gem_create(ibb->i915, ibb->size);

	bb_address = __intel_bb_propose_offset(ibb);
	intel_bb_add_object(ibb, ibb->handle, bb_address, false);
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

	if (ibb->fence < 0)
		return 0;

	ret = sync_fence_wait(ibb->fence, -1);
	if (ret == 0) {
		close(ibb->fence);
		ibb->fence = -1;
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
		 ibb->i915, ibb->gen, ibb->devid, ibb->debug);
	igt_info("handle: %u, size: %u, batch: %p, ptr: %p\n",
		 ibb->handle, ibb->size, ibb->batch, ibb->ptr);
	igt_info("prng: %u, gtt_size: %" PRIu64 ", supports 48bit: %d\n",
		 ibb->prng, ibb->gtt_size, ibb->supports_48b_address);
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

	ptr = gem_mmap__device_coherent(ibb->i915, ibb->handle, 0, ibb->size,
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

static int __compare_objects(const void *p1, const void *p2)
{
	const struct drm_i915_gem_exec_object2 *o1 = p1, *o2 = p2;

	return (int) ((int64_t) o1->handle - (int64_t) o2->handle);
}

/**
 * intel_bb_add_object:
 * @ibb: pointer to intel_bb
 * @handle: which handle to add to objects array
 * @offset: presumed offset of the object when no relocation is enforced
 * @write: does a handle is a render target
 *
 * Function adds or updates execobj slot in bb objects array and
 * in the object tree. When object is a render target it has to
 * be marked with EXEC_OBJECT_WRITE flag.
 */
struct drm_i915_gem_exec_object2 *
intel_bb_add_object(struct intel_bb *ibb, uint32_t handle,
		    uint64_t offset, bool write)
{
	struct drm_i915_gem_exec_object2 *object;
	struct drm_i915_gem_exec_object2 **found;
	uint32_t i;

	__reallocate_objects(ibb);

	i = ibb->num_objects;
	object = &ibb->objects[i];
	object->handle = handle;
	object->offset = offset;

	found = tsearch((void *) object, &ibb->root, __compare_objects);

	if (*found == object)
		ibb->num_objects++;
	else
		object = *found;

	if (object->offset == INTEL_BUF_INVALID_ADDRESS)
		object->offset = __intel_bb_propose_offset(ibb);

	if (write)
		object->flags |= EXEC_OBJECT_WRITE;

	if (ibb->supports_48b_address)
		object->flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	return object;
}

static bool intel_bb_object_set_fence(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found) {
		igt_warn("Trying to set fence on not found handle: %u\n",
			 handle);
		return false;
	}

	(*found)->flags |= EXEC_OBJECT_NEEDS_FENCE;

	return true;
}

/*
 * intel_bb_add_reloc:
 * @ibb: pointer to intel_bb
 * @handle: object handle which address will be taken to patch the bb
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 * @offset: offset within bb to be patched
 * @presumed_offset: address of the object in address space. If -1 is passed
 * then final offset of the object will be randomized (for no-reloc bb) or
 * 0 (for reloc bb, in that case reloc.presumed_offset will be -1). In
 * case address is known it should passed in @presumed_offset (for no-reloc).
 *
 * Function allocates additional relocation slot in reloc array for a handle.
 * It also implicitly adds handle in the objects array if object doesn't
 * exists but doesn't mark it as a render target.
 */
static uint64_t intel_bb_add_reloc(struct intel_bb *ibb,
				   uint32_t handle,
				   uint32_t read_domains,
				   uint32_t write_domain,
				   uint64_t delta,
				   uint64_t offset,
				   uint64_t presumed_offset)
{
	struct drm_i915_gem_relocation_entry *relocs;
	struct drm_i915_gem_exec_object2 *object;
	uint32_t i;

	object = intel_bb_add_object(ibb, handle, presumed_offset, false);

	relocs = ibb->relocs;
	if (ibb->num_relocs == ibb->allocated_relocs) {
		ibb->allocated_relocs += 4096 / sizeof(*relocs);
		relocs = realloc(relocs, sizeof(*relocs) * ibb->allocated_relocs);
		igt_assert(relocs);
		ibb->relocs = relocs;
	}

	i = ibb->num_relocs++;
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

	igt_debug("add reloc: handle: %u, r/w: 0x%x/0x%x, "
		  "delta: 0x%" PRIx64 ", "
		  "offset: 0x%" PRIx64 ", "
		  "poffset: %p\n",
		  handle, read_domains, write_domain,
		  delta, offset,
		  from_user_pointer(relocs[i].presumed_offset));

	return object->offset;
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
	uint64_t address;

	igt_assert(ibb);

	address = intel_bb_add_reloc(ibb, handle, read_domains, write_domain,
				     delta, intel_bb_offset(ibb),
				     presumed_offset);

	intel_bb_out(ibb, delta + address);
	if (ibb->gen >= 8)
		intel_bb_out(ibb, address >> 32);

	return address;
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

	intel_bb_object_set_fence(ibb, handle);

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

	return intel_bb_add_reloc(ibb, handle, read_domains, write_domain,
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

	return intel_bb_add_reloc(ibb, handle, read_domains, write_domain,
				  delta, offset, presumed_offset);
}

static void intel_bb_dump_execbuf(struct intel_bb *ibb,
				  struct drm_i915_gem_execbuffer2 *execbuf)
{
	struct drm_i915_gem_exec_object2 *objects;
	struct drm_i915_gem_relocation_entry *relocs, *reloc;
	int i, j;
	uint64_t address;

	igt_info("execbuf batch len: %u, start offset: 0x%x, "
		 "DR1: 0x%x, DR4: 0x%x, "
		 "num clip: %u, clipptr: 0x%llx, "
		 "flags: 0x%llx, rsvd1: 0x%llx, rsvd2: 0x%llx\n",
		 execbuf->batch_len, execbuf->batch_start_offset,
		 execbuf->DR1, execbuf->DR4,
		 execbuf->num_cliprects, execbuf->cliprects_ptr,
		 execbuf->flags, execbuf->rsvd1, execbuf->rsvd2);

	igt_info("execbuf buffer_count: %d\n", execbuf->buffer_count);
	for (i = 0; i < execbuf->buffer_count; i++) {
		objects = &((struct drm_i915_gem_exec_object2 *)
			    from_user_pointer(execbuf->buffers_ptr))[i];
		relocs = from_user_pointer(objects->relocs_ptr);
		address = objects->offset;
		if (address != INTEL_BUF_INVALID_ADDRESS)
			address = address & (ibb->gtt_size - 1);
		igt_info(" [%d] handle: %u, reloc_count: %d, reloc_ptr: %p, "
			 "align: 0x%llx, offset: 0x%" PRIx64 ", flags: 0x%llx, "
			 "rsvd1: 0x%llx, rsvd2: 0x%llx\n",
			 i, objects->handle, objects->relocation_count,
			 relocs,
			 objects->alignment,
			 address,
			 objects->flags,
			 objects->rsvd1, objects->rsvd2);
		if (objects->relocation_count) {
			igt_info("\texecbuf relocs:\n");
			for (j = 0; j < objects->relocation_count; j++) {
				reloc = &relocs[j];
				address = reloc->presumed_offset;
				if (address != INTEL_BUF_INVALID_ADDRESS)
					address = address & (ibb->gtt_size - 1);
				igt_info("\t [%d] target handle: %u, "
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

/*
 * @__intel_bb_exec:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb
 * @flags: flags passed directly to execbuf
 * @ctx: context
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Returns: 0 on success, otherwise errno.
 *
 * Note: In this step execobj for bb is allocated and inserted to the objects
 * array.
*/
int __intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
		    uint32_t ctx, uint64_t flags, bool sync)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	int ret, fence, new_fence;

	ibb->objects[0].relocs_ptr = to_user_pointer(ibb->relocs);
	ibb->objects[0].relocation_count = ibb->num_relocs;
	ibb->objects[0].handle = ibb->handle;

	gem_write(ibb->i915, ibb->handle, 0, ibb->batch, ibb->size);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t) ibb->objects;
	execbuf.buffer_count = ibb->num_objects;
	execbuf.batch_len = end_offset;
	execbuf.rsvd1 = ibb->ctx = ctx;
	execbuf.flags = flags | I915_EXEC_BATCH_FIRST | I915_EXEC_FENCE_OUT;
	if (ibb->enforce_relocs)
		execbuf.flags &= ~I915_EXEC_NO_RELOC;
	execbuf.rsvd2 = 0;

	ret = __gem_execbuf_wr(ibb->i915, &execbuf);
	if (ret) {
		intel_bb_dump_execbuf(ibb, &execbuf);
		return ret;
	}

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

	return 0;
}

/**
 * intel_bb_exec:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb
 * @flags: flags passed directly to execbuf
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Do execbuf with default context. Asserts on failure.
*/
void intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
		   uint64_t flags, bool sync)
{
	igt_assert_eq(__intel_bb_exec(ibb, end_offset, 0, flags, sync), 0);
}

/*
 * intel_bb_exec_with_context:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb
 * @flags: flags passed directly to execbuf
 * @ctx: context
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Do execbuf with context @context.
*/
void intel_bb_exec_with_context(struct intel_bb *ibb, uint32_t end_offset,
				uint32_t ctx, uint64_t flags, bool sync)
{
	igt_assert_eq(__intel_bb_exec(ibb, end_offset, ctx, flags, sync), 0);
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
	uint64_t address;

	igt_assert(ibb);

	found = tfind((void *)&object, &ibb->root, __compare_objects);
	if (!found)
		return INTEL_BUF_INVALID_ADDRESS;

	address = (*found)->offset;

	if (address == INTEL_BUF_INVALID_ADDRESS)
		return address;

	return address & (ibb->gtt_size - 1);
}

/**
 * intel_bb_object_offset_to_buf:
 * @ibb: pointer to intel_bb
 * @buf: buffer we want to store last exec offset and context id
 *
 * Copy object offset used in the batch to intel_buf to allow caller prepare
 * other batch likely without relocations.
 */
bool intel_bb_object_offset_to_buf(struct intel_bb *ibb, struct intel_buf *buf)
{
	struct drm_i915_gem_exec_object2 object = { .handle = buf->handle };
	struct drm_i915_gem_exec_object2 **found;

	igt_assert(ibb);
	igt_assert(buf);

	found = tfind((void *)&object, &ibb->root, __compare_objects);
	if (!found) {
		buf->addr.offset = 0;
		buf->addr.ctx = 0;

		return false;
	}

	buf->addr.offset = (*found)->offset & (ibb->gtt_size - 1);
	buf->addr.ctx = ibb->ctx;

	return true;
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
 * intel_bb_flush_with_context_ring:
 * @ibb: batchbuffer
 * @ctx: context id
 * @ring: ring
 *
 * Submits the batch for execution on the @ring engine with the supplied
 * hardware context @ctx.
 */
static void intel_bb_flush_with_context_ring(struct intel_bb *ibb,
					     uint32_t ctx, uint32_t ring)
{
	intel_bb_exec_with_context(ibb, intel_bb_offset(ibb), ctx,
				   ring | I915_EXEC_NO_RELOC,
				   false);
	intel_bb_reset(ibb, false);
}

void intel_bb_flush_render(struct intel_bb *ibb)
{
	uint32_t ring = I915_EXEC_RENDER;

	intel_bb_flush_with_context_ring(ibb, ibb->ctx, ring);
}

void intel_bb_flush_blit(struct intel_bb *ibb)
{
	uint32_t ring = I915_EXEC_DEFAULT;

	if (HAS_BLT_RING(ibb->devid))
		ring = I915_EXEC_BLT;

	intel_bb_flush_with_context_ring(ibb, ibb->ctx, ring);
}

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

void intel_bb_blit_start(struct intel_bb *ibb, uint32_t flags)
{
	intel_bb_out(ibb, XY_SRC_COPY_BLT_CMD |
		     XY_SRC_COPY_BLT_WRITE_ALPHA |
		     XY_SRC_COPY_BLT_WRITE_RGB |
		     flags |
		     (6 + 2 * (ibb->gen >= 8)));
}

void intel_bb_emit_blt_copy(struct intel_bb *ibb,
			    struct intel_buf *src,
			    int src_x1, int src_y1, int src_pitch,
			    struct intel_buf *dst,
			    int dst_x1, int dst_y1, int dst_pitch,
			    int width, int height, int bpp)
{
	const int gen = ibb->gen;
	uint32_t cmd_bits = 0;
	uint32_t br13_bits;
	uint32_t mask;

	igt_assert(bpp*(src_x1 + width) <= 8*src_pitch);
	igt_assert(bpp*(dst_x1 + width) <= 8*dst_pitch);
	igt_assert(src_pitch * (src_y1 + height) <= src->surface[0].size);
	igt_assert(dst_pitch * (dst_y1 + height) <= dst->surface[0].size);

	if (gen >= 4 && src->tiling != I915_TILING_NONE) {
		src_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
	}

	if (gen >= 4 && dst->tiling != I915_TILING_NONE) {
		dst_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_DST_TILED;
	}

	CHECK_RANGE(src_x1); CHECK_RANGE(src_y1);
	CHECK_RANGE(dst_x1); CHECK_RANGE(dst_y1);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x1 + width); CHECK_RANGE(src_y1 + height);
	CHECK_RANGE(dst_x1 + width); CHECK_RANGE(dst_y1 + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	br13_bits = 0;
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
		intel_bb_out(ibb, MI_FLUSH_DW | 2);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);

		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
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
	intel_bb_emit_bbe(ibb);
	intel_bb_flush_blit(ibb);
}
