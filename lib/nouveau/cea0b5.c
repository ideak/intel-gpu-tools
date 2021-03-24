/*
 * Copyright 2021 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>

#include <nouveau_drm.h>

#include "igt.h"
#include "igt_nouveau.h"

#include "nouveau/nvif/push906f.h"
#include "nouveau/nvhw/class/cla0b5.h"

#include "priv.h"

void igt_nouveau_ce_zfilla0b5(struct igt_nouveau_dev *dev, struct igt_fb *fb, struct nouveau_bo *bo,
			      unsigned int plane)
{
	struct nouveau_pushbuf *push = dev->pushbuf;
	const int width = fb->strides[plane];
	const int height = fb->plane_height[plane];
	const int line_length = fb->plane_width[plane] * (fb->plane_bpp[plane] / 8);
	uint32_t dma_args = NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
			    NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
			    NVDEF(NVA0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) |
			    NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE);
	int push_space = 15;

	if (bo->config.nvc0.memtype) {
		dma_args |= NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, BLOCKLINEAR) |
			    NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, BLOCKLINEAR);
		push_space += 14;
	} else {
		dma_args |= NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
			    NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
	}

	PUSH_SPACE(push, push_space);
	PUSH_REFN(push, bo, NOUVEAU_BO_WR | (bo->flags & (NOUVEAU_BO_GART | NOUVEAU_BO_VRAM)));

	PUSH_MTHD(push, NVA0B5, SET_REMAP_CONST_A, 0);

	PUSH_MTHD(push, NVA0B5, SET_REMAP_COMPONENTS,
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_Y, NO_WRITE) |
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_Z, NO_WRITE) |
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_W, NO_WRITE) |
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, ONE) |
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, ONE));

	if (bo->config.nvc0.memtype) {
		PUSH_MTHD(push, NVA0B5, SET_SRC_BLOCK_SIZE,
			  NVDEF(NVA0B5, SET_SRC_BLOCK_SIZE, DEPTH, ONE_GOB) |
			  NVDEF(NVA0B5, SET_SRC_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8) |
			  bo->config.nvc0.tile_mode,

					SET_SRC_WIDTH,
			  NVVAL(NVA0B5, SET_SRC_WIDTH, V, width),

					SET_SRC_HEIGHT,
			  NVVAL(NVA0B5, SET_SRC_HEIGHT, V, height),

					SET_SRC_DEPTH,
			  NVVAL(NVA0B5, SET_SRC_DEPTH, V, fb->num_planes),

					SET_SRC_LAYER,
			  NVVAL(NVA0B5, SET_SRC_LAYER, V, plane),

					SET_SRC_ORIGIN,
			  NVVAL(NVA0B5, SET_SRC_ORIGIN, X, 0) |
			  NVVAL(NVA0B5, SET_SRC_ORIGIN, Y, 0));

		PUSH_MTHD(push,	NVA0B5, SET_DST_BLOCK_SIZE,
			  NVDEF(NVA0B5, SET_DST_BLOCK_SIZE, DEPTH, ONE_GOB) |
			  NVDEF(NVA0B5, SET_DST_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8) |
			  bo->config.nvc0.tile_mode,

					SET_DST_WIDTH,
			  NVVAL(NVA0B5, SET_DST_WIDTH, V, width),

					SET_DST_HEIGHT,
			  NVVAL(NVA0B5, SET_DST_HEIGHT, V, height),

					SET_DST_DEPTH,
			  NVVAL(NVA0B5, SET_DST_DEPTH, V, fb->num_planes),

					SET_DST_LAYER,
			  NVVAL(NVA0B5, SET_DST_LAYER, V, plane),

					SET_DST_ORIGIN,
			  NVVAL(NVA0B5, SET_DST_ORIGIN, X, 0) |
			  NVVAL(NVA0B5, SET_DST_ORIGIN, Y, 0));
	}

	PUSH_MTHD(push, NVA0B5, OFFSET_IN_UPPER,
		  NVVAL(NVA0B5, OFFSET_IN_UPPER, UPPER, bo->offset >> 32),

				OFFSET_IN_LOWER,
		  NVVAL(NVA0B5, OFFSET_IN_LOWER, VALUE, bo->offset),

				OFFSET_OUT_UPPER,
		  NVVAL(NVA0B5, OFFSET_OUT_UPPER, UPPER, bo->offset >> 32),

				OFFSET_OUT_LOWER,
		  NVVAL(NVA0B5, OFFSET_OUT_LOWER, VALUE, bo->offset),

				PITCH_IN,
		  NVVAL(NVA0B5, PITCH_IN, VALUE, fb->strides[plane]),

				PITCH_OUT,
		  NVVAL(NVA0B5, PITCH_OUT, VALUE, fb->strides[plane]),

				LINE_LENGTH_IN,
		  NVVAL(NVA0B5, LINE_LENGTH_IN, VALUE, line_length),

				LINE_COUNT,
		  NVVAL(NVA0B5, LINE_COUNT, VALUE, height));

	PUSH_MTHD(push, NVA0B5, LAUNCH_DMA, dma_args);

	PUSH_KICK(push);
}

void igt_nouveau_ce_copya0b5(struct igt_nouveau_dev *dev,
			     struct igt_fb *dst_fb, struct nouveau_bo *dst_bo,
			     struct igt_fb *src_fb, struct nouveau_bo *src_bo,
			     unsigned int plane)
{
	struct nouveau_pushbuf *push = dev->pushbuf;
	const int src_width = src_fb->strides[plane];
	const int src_height = src_fb->plane_height[plane];
	const int dst_width = dst_fb->strides[plane];
	const int dst_height = dst_fb->plane_height[plane];
	const int line_length = src_fb->plane_width[plane] * (src_fb->plane_bpp[plane] / 8);
	uint32_t dma_args = NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
			    NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
			    NVDEF(NVA0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE);
	int push_space = 11;

	if (src_bo->config.nvc0.memtype) {
		dma_args |= NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, BLOCKLINEAR);
		push_space += 7;
	} else {
		dma_args |= NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH);
	}

	if (dst_bo->config.nvc0.memtype) {
		dma_args |= NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, BLOCKLINEAR);
		push_space += 7;
	} else {
		dma_args |= NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH);
	}

	PUSH_SPACE(push, push_space);
	PUSH_REFN(push, src_bo,
		  NOUVEAU_BO_RD | (src_bo->flags & (NOUVEAU_BO_GART | NOUVEAU_BO_VRAM)));
	PUSH_REFN(push, dst_bo,
		  NOUVEAU_BO_WR | (dst_bo->flags & (NOUVEAU_BO_GART | NOUVEAU_BO_VRAM)));

	if (src_bo->config.nvc0.memtype) {
		PUSH_MTHD(push, NVA0B5, SET_SRC_BLOCK_SIZE,
			  NVDEF(NVA0B5, SET_SRC_BLOCK_SIZE, DEPTH, ONE_GOB) |
			  NVDEF(NVA0B5, SET_SRC_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8) |
			  src_bo->config.nvc0.tile_mode,

					SET_SRC_WIDTH,
			  NVVAL(NVA0B5, SET_SRC_WIDTH, V, src_width),

					SET_SRC_HEIGHT,
			  NVVAL(NVA0B5, SET_SRC_HEIGHT, V, src_height),

					SET_SRC_DEPTH,
			  NVVAL(NVA0B5, SET_SRC_DEPTH, V, src_fb->num_planes),

					SET_SRC_LAYER,
			  NVVAL(NVA0B5, SET_SRC_LAYER, V, plane),

					SET_SRC_ORIGIN,
			  NVVAL(NVA0B5, SET_SRC_ORIGIN, X, 0) |
			  NVVAL(NVA0B5, SET_SRC_ORIGIN, Y, 0));
	}

	if (dst_bo->config.nvc0.memtype) {
		PUSH_MTHD(push, NVA0B5, SET_DST_BLOCK_SIZE,
			  NVDEF(NVA0B5, SET_DST_BLOCK_SIZE, DEPTH, ONE_GOB) |
			  NVDEF(NVA0B5, SET_DST_BLOCK_SIZE, GOB_HEIGHT, GOB_HEIGHT_FERMI_8) |
			  dst_bo->config.nvc0.tile_mode,

					SET_DST_WIDTH,
			  NVVAL(NVA0B5, SET_DST_WIDTH, V, dst_width),

					SET_DST_HEIGHT,
			  NVVAL(NVA0B5, SET_DST_HEIGHT, V, dst_height),

					SET_DST_DEPTH,
			  NVVAL(NVA0B5, SET_DST_DEPTH, V, dst_fb->num_planes),

					SET_DST_LAYER,
			  NVVAL(NVA0B5, SET_DST_LAYER, V, plane),

					SET_DST_ORIGIN,
			  NVVAL(NVA0B5, SET_DST_ORIGIN, X, 0) |
			  NVVAL(NVA0B5, SET_DST_ORIGIN, Y, 0));
	}

	PUSH_MTHD(push, NVA0B5, OFFSET_IN_UPPER,
		  NVVAL(NVA0B5, OFFSET_IN_UPPER, UPPER, src_bo->offset >> 32),

				OFFSET_IN_LOWER,
		  NVVAL(NVA0B5, OFFSET_IN_LOWER, VALUE, src_bo->offset),

				OFFSET_OUT_UPPER,
		  NVVAL(NVA0B5, OFFSET_OUT_UPPER, UPPER, dst_bo->offset >> 32),

				OFFSET_OUT_LOWER,
		  NVVAL(NVA0B5, OFFSET_OUT_LOWER, VALUE, dst_bo->offset),

				PITCH_IN,
		  NVVAL(NVA0B5, PITCH_IN, VALUE, src_fb->strides[plane]),

				PITCH_OUT,
		  NVVAL(NVA0B5, PITCH_OUT, VALUE, dst_fb->strides[plane]),

				LINE_LENGTH_IN,
		  NVVAL(NVA0B5, LINE_LENGTH_IN, VALUE, line_length),

				LINE_COUNT,
		  NVVAL(NVA0B5, LINE_COUNT, VALUE, src_height));

	PUSH_MTHD(push, NVA0B5, LAUNCH_DMA, dma_args);

	PUSH_KICK(push);
};
