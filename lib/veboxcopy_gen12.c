/*
 * Copyright © 2019 Intel Corporation
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
#include <drm.h>

#include "igt.h"
#include "intel_aux_pgtable.h"
#include "veboxcopy.h"

#define YCRCB_NORMAL	0
#define PLANAR_420_8	4
#define R8G8B8A8_UNORM	8
#define PLANAR_420_16	12

struct vebox_surface_state {
	struct {
		uint32_t dw_count:12;
		uint32_t pad:4;
		uint32_t sub_opcode_b:5;
		uint32_t sub_opcode_a:3;
		uint32_t media_cmd_opcode:3;
		uint32_t media_cmd_pipeline:2;
		uint32_t cmd_type:3;
	} ss0;
	struct {
#define VEBOX_SURFACE_INPUT	0
#define VEBOX_SURFACE_OUTPUT	1
		uint32_t surface_id:1;
		uint32_t pad:31;
	} ss1;
	struct {
		uint32_t pad:4;
		uint32_t width:14;
		uint32_t height:14;
	} ss2;
	struct {
#define VEBOX_TILE_WALK_XMAJOR 0
#define VEBOX_TILE_WALK_YMAJOR 1
		uint32_t tile_walk:1;
		uint32_t tiled_surface:1;
		uint32_t chroma_half_pitch:1;
		uint32_t surface_pitch:17;
		uint32_t chroma_interleave:1;
		uint32_t lsb_packed_enable:1;
		uint32_t bayer_input_alignment:2;
		uint32_t bayer_pattern_format:1;
		uint32_t bayer_pattern_offset:2;
		uint32_t surface_format:5;
	} ss3;
	struct {
		uint32_t u_y_offset:15;
		uint32_t u_x_offset:13;
		uint32_t pad:4;
	} ss4;
	struct {
		uint32_t v_y_offset:15;
		uint32_t v_x_offset:13;
		uint32_t pad:4;
	} ss5;
	struct {
		uint32_t frame_y_offset:15;
		uint32_t frame_x_offset:15;
		uint32_t pad:2;
	} ss6;
	struct {
		uint32_t derived_surface_pitch:17;
		uint32_t pad:15;
	} ss7;
	struct {
		uint32_t skin_score_output_surface_pitch:17;
		uint32_t pad:15;
	} ss8;
} __attribute__((packed));

struct vebox_tiling_convert {
	struct {
		uint32_t dw_count:12;
		uint32_t pad:4;
		uint32_t sub_opcode_b:5;
		uint32_t sub_opcode_a:3;
		uint32_t cmd_opcode:3;
		uint32_t pipeline:2;
		uint32_t cmd_type:3;
	} tc0;
	union {
		struct {
			uint64_t input_encrypted_data:1;
			uint64_t input_mocs_idx:6;
			uint64_t input_memory_compression_enable:1;
#define COMPRESSION_TYPE_MEDIA 0
#define COMPRESSION_TYPE_RENDER	1
			uint64_t input_compression_type:1;
#define TRMODE_NONE	0
#define TRMODE_TILE_YF	1
#define TRMODE_TILE_YS	2
			uint64_t input_tiled_resource_mode:2;
			uint64_t pad:1;
			uint64_t input_address:52;
		} tc1_2;
		uint64_t tc1_2_l;
	};
	union {
		struct {
			uint64_t output_encrypted_data:1;
			uint64_t output_mocs_idx:6;
			uint64_t output_memory_compression_enable:1;
			uint64_t output_compression_type:1;
			uint64_t output_tiled_resource_mode:2;
			uint64_t pad:1;
			uint64_t output_address:52;
		} tc3_4;
		uint64_t tc3_4_l;
	};
} __attribute__((packed));

static bool format_is_interleaved_yuv(int format)
{
	switch (format) {
	case YCRCB_NORMAL:
	case PLANAR_420_8:
	case PLANAR_420_16:
		return true;
	}

	return false;
}

static void emit_surface_state_cmd(struct intel_batchbuffer *batch,
				   int surface_id,
				   int width, int height, int bpp,
				   int pitch, uint32_t tiling, int format,
				   uint32_t uv_offset)
{
	struct vebox_surface_state *ss;

	ss = intel_batchbuffer_subdata_alloc(batch, sizeof(*ss), 4);

	ss->ss0.cmd_type = 3;
	ss->ss0.media_cmd_pipeline = 2;
	ss->ss0.media_cmd_opcode = 4;
	ss->ss0.dw_count = 7;

	ss->ss1.surface_id = surface_id;

	ss->ss2.height = height - 1;
	ss->ss2.width = width - 1;

	ss->ss3.surface_format = format;
	if (format_is_interleaved_yuv(format))
		ss->ss3.chroma_interleave = 1;
	ss->ss3.surface_pitch = pitch - 1;
	ss->ss3.tile_walk = (tiling == I915_TILING_Y) ||
			    (tiling == I915_TILING_Yf);
	ss->ss3.tiled_surface = tiling != I915_TILING_NONE;

	ss->ss4.u_y_offset = uv_offset / pitch;

	ss->ss7.derived_surface_pitch = pitch - 1;
}

static void emit_tiling_convert_cmd(struct intel_batchbuffer *batch,
				    drm_intel_bo *input_bo,
				    uint32_t input_tiling,
				    uint32_t input_compression,
				    drm_intel_bo *output_bo,
				    uint32_t output_tiling,
				    uint32_t output_compression)
{
	uint32_t reloc_delta;
	struct vebox_tiling_convert *tc;
	int ret;

	tc = intel_batchbuffer_subdata_alloc(batch, sizeof(*tc), 8);

	tc->tc0.cmd_type = 3;
	tc->tc0.pipeline = 2;
	tc->tc0.cmd_opcode = 4;
	tc->tc0.sub_opcode_b = 1;

	tc->tc0.dw_count = 3;

	if (input_compression != I915_COMPRESSION_NONE) {
		tc->tc1_2.input_memory_compression_enable = 1;
		tc->tc1_2.input_compression_type =
			input_compression == I915_COMPRESSION_RENDER;
	}
	tc->tc1_2.input_tiled_resource_mode = input_tiling == I915_TILING_Yf;
	reloc_delta = tc->tc1_2_l;

	igt_assert(input_bo->offset64 == ALIGN(input_bo->offset64, 0x1000));
	tc->tc1_2.input_address = input_bo->offset64 >> 12;
	igt_assert(reloc_delta <= INT32_MAX);
	ret = drm_intel_bo_emit_reloc(batch->bo,
				      intel_batchbuffer_subdata_offset(batch, tc) +
					offsetof(typeof(*tc), tc1_2),
				      input_bo, reloc_delta,
				      0, 0);
	igt_assert(ret == 0);

	if (output_compression != I915_COMPRESSION_NONE) {
		tc->tc3_4.output_memory_compression_enable = 1;
		tc->tc3_4.output_compression_type =
			output_compression == I915_COMPRESSION_RENDER;
	}
	tc->tc3_4.output_tiled_resource_mode = output_tiling == I915_TILING_Yf;
	reloc_delta = tc->tc3_4_l;

	igt_assert(output_bo->offset64 == ALIGN(output_bo->offset64, 0x1000));
	tc->tc3_4.output_address = output_bo->offset64 >> 12;
	igt_assert(reloc_delta <= INT32_MAX);
	ret = drm_intel_bo_emit_reloc(batch->bo,
				      intel_batchbuffer_subdata_offset(batch, tc) +
					offsetof(typeof(*tc), tc3_4),
				      output_bo, reloc_delta,
				      0, I915_GEM_DOMAIN_RENDER);
	igt_assert(ret == 0);

}

/* Borrowing the idea from the rendercopy state setup. */
#define BATCH_STATE_SPLIT 2048

void gen12_vebox_copyfunc(struct intel_batchbuffer *batch,
			  const struct igt_buf *src,
			  unsigned width, unsigned height,
			  const struct igt_buf *dst)
{
	struct aux_pgtable_info aux_pgtable_info = { };
	uint32_t aux_pgtable_state;
	int format;

	intel_batchbuffer_flush_on_ring(batch, I915_EXEC_VEBOX);

	intel_batchbuffer_align(batch, 8);

	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	gen12_aux_pgtable_init(&aux_pgtable_info, batch->bufmgr, src, dst);

	aux_pgtable_state = gen12_create_aux_pgtable_state(batch,
							   aux_pgtable_info.pgtable_bo);

	assert(batch->ptr < &batch->buffer[4095]);
	batch->ptr = batch->buffer;

	gen12_emit_aux_pgtable_state(batch, aux_pgtable_state, false);

	/* The tiling convert command can't convert formats. */
	igt_assert_eq(src->format_is_yuv, dst->format_is_yuv);
	igt_assert_eq(src->format_is_yuv_semiplanar,
		      dst->format_is_yuv_semiplanar);
	igt_assert_eq(src->bpp, dst->bpp);

	/* TODO: add support for more formats */
	switch (src->bpp) {
	case 8:
		igt_assert(src->format_is_yuv_semiplanar);
		format = PLANAR_420_8;
		break;
	case 16:
		igt_assert(src->format_is_yuv);
		format = src->format_is_yuv_semiplanar ? PLANAR_420_16 :
							 YCRCB_NORMAL;
		break;
	case 32:
		igt_assert(!src->format_is_yuv &&
			   !src->format_is_yuv_semiplanar);
		format = R8G8B8A8_UNORM;
		break;
	default:
		igt_assert_f(0, "Unsupported bpp: %u\n", src->bpp);
	}

	igt_assert(!src->format_is_yuv_semiplanar ||
		   (src->surface[1].offset && dst->surface[1].offset));
	emit_surface_state_cmd(batch, VEBOX_SURFACE_INPUT,
			       width, height, src->bpp,
			       src->surface[0].stride,
			       src->tiling, format, src->surface[1].offset);

	emit_surface_state_cmd(batch, VEBOX_SURFACE_OUTPUT,
			       width, height, dst->bpp,
			       dst->surface[0].stride,
			       dst->tiling, format, dst->surface[1].offset);

	emit_tiling_convert_cmd(batch,
				src->bo, src->tiling, src->compression,
				dst->bo, dst->tiling, dst->compression);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	intel_batchbuffer_flush_on_ring(batch, I915_EXEC_VEBOX);

	gen12_aux_pgtable_cleanup(&aux_pgtable_info);
	intel_batchbuffer_reset(batch);
}
