// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <malloc.h>
#include <cairo.h>
#include "drm.h"
#include "igt.h"
#include "gem_create.h"
#include "i915_blt.h"

#define BITRANGE(start, end) (end - start + 1)

enum blt_special_mode {
	SM_NONE,
	SM_FULL_RESOLVE,
	SM_PARTIAL_RESOLVE,
	SM_RESERVED,
};

enum blt_aux_mode {
	AM_AUX_NONE,
	AM_AUX_CCS_E = 5,
};

enum blt_target_mem {
	TM_LOCAL_MEM,
	TM_SYSTEM_MEM,
};

struct gen12_block_copy_data {
	struct {
		uint32_t length:			BITRANGE(0, 7);
		uint32_t rsvd1:				BITRANGE(8, 8);
		uint32_t multisamples:			BITRANGE(9, 11);
		uint32_t special_mode:			BITRANGE(12, 13);
		uint32_t rsvd0:				BITRANGE(14, 18);
		uint32_t color_depth:			BITRANGE(19, 21);
		uint32_t opcode:			BITRANGE(22, 28);
		uint32_t client:			BITRANGE(29, 31);
	} dw00;

	struct {
		uint32_t dst_pitch:			BITRANGE(0, 17);
		uint32_t dst_aux_mode:			BITRANGE(18, 20);
		uint32_t dst_mocs:			BITRANGE(21, 27);
		uint32_t dst_ctrl_surface_type:		BITRANGE(28, 28);
		uint32_t dst_compression:		BITRANGE(29, 29);
		uint32_t dst_tiling:			BITRANGE(30, 31);
	} dw01;

	struct {
		int32_t dst_x1:				BITRANGE(0, 15);
		int32_t dst_y1:				BITRANGE(16, 31);
	} dw02;

	struct {
		int32_t dst_x2:				BITRANGE(0, 15);
		int32_t dst_y2:				BITRANGE(16, 31);
	} dw03;

	struct {
		uint32_t dst_address_lo;
	} dw04;

	struct {
		uint32_t dst_address_hi;
	} dw05;

	struct {
		uint32_t dst_x_offset:			BITRANGE(0, 13);
		uint32_t rsvd1:				BITRANGE(14, 15);
		uint32_t dst_y_offset:			BITRANGE(16, 29);
		uint32_t rsvd0:				BITRANGE(30, 30);
		uint32_t dst_target_memory:		BITRANGE(31, 31);
	} dw06;

	struct {
		int32_t src_x1:				BITRANGE(0, 15);
		int32_t src_y1:				BITRANGE(16, 31);
	} dw07;

	struct {
		uint32_t src_pitch:			BITRANGE(0, 17);
		uint32_t src_aux_mode:			BITRANGE(18, 20);
		uint32_t src_mocs:			BITRANGE(21, 27);
		uint32_t src_ctrl_surface_type:		BITRANGE(28, 28);
		uint32_t src_compression:		BITRANGE(29, 29);
		uint32_t src_tiling:			BITRANGE(30, 31);
	} dw08;

	struct {
		uint32_t src_address_lo;
	} dw09;

	struct {
		uint32_t src_address_hi;
	} dw10;

	struct {
		uint32_t src_x_offset:			BITRANGE(0, 13);
		uint32_t rsvd1:				BITRANGE(14, 15);
		uint32_t src_y_offset:			BITRANGE(16, 29);
		uint32_t rsvd0:				BITRANGE(30, 30);
		uint32_t src_target_memory:		BITRANGE(31, 31);
	} dw11;
};

struct gen12_block_copy_data_ext {
	struct {
		uint32_t src_compression_format:	BITRANGE(0, 4);
		uint32_t src_clear_value_enable:	BITRANGE(5, 5);
		uint32_t src_clear_address_low:		BITRANGE(6, 31);
	} dw12;

	union {
		/* DG2, XEHP */
		uint32_t src_clear_address_hi0;
		/* Others */
		uint32_t src_clear_address_hi1;
	} dw13;

	struct {
		uint32_t dst_compression_format:	BITRANGE(0, 4);
		uint32_t dst_clear_value_enable:	BITRANGE(5, 5);
		uint32_t dst_clear_address_low:		BITRANGE(6, 31);
	} dw14;

	union {
		/* DG2, XEHP */
		uint32_t dst_clear_address_hi0;
		/* Others */
		uint32_t dst_clear_address_hi1;
	} dw15;

	struct {
		uint32_t dst_surface_height:		BITRANGE(0, 13);
		uint32_t dst_surface_width:		BITRANGE(14, 27);
		uint32_t rsvd0:				BITRANGE(28, 28);
		uint32_t dst_surface_type:		BITRANGE(29, 31);
	} dw16;

	struct {
		uint32_t dst_lod:			BITRANGE(0, 3);
		uint32_t dst_surface_qpitch:		BITRANGE(4, 18);
		uint32_t rsvd0:				BITRANGE(19, 20);
		uint32_t dst_surface_depth:		BITRANGE(21, 31);
	} dw17;

	struct {
		uint32_t dst_horizontal_align:		BITRANGE(0, 1);
		uint32_t rsvd0:				BITRANGE(2, 2);
		uint32_t dst_vertical_align:		BITRANGE(3, 4);
		uint32_t rsvd1:				BITRANGE(5, 7);
		uint32_t dst_mip_tail_start_lod:	BITRANGE(8, 11);
		uint32_t rsvd2:				BITRANGE(12, 17);
		uint32_t dst_depth_stencil_resource:	BITRANGE(18, 18);
		uint32_t rsvd3:				BITRANGE(19, 20);
		uint32_t dst_array_index:		BITRANGE(21, 31);
	} dw18;

	struct {
		uint32_t src_surface_height:		BITRANGE(0, 13);
		uint32_t src_surface_width:		BITRANGE(14, 27);
		uint32_t rsvd0:				BITRANGE(28, 28);
		uint32_t src_surface_type:		BITRANGE(29, 31);
	} dw19;

	struct {
		uint32_t src_lod:			BITRANGE(0, 3);
		uint32_t src_surface_qpitch:		BITRANGE(4, 18);
		uint32_t rsvd0:				BITRANGE(19, 20);
		uint32_t src_surface_depth:		BITRANGE(21, 31);
	} dw20;

	struct {
		uint32_t src_horizontal_align:		BITRANGE(0, 1);
		uint32_t rsvd0:				BITRANGE(2, 2);
		uint32_t src_vertical_align:		BITRANGE(3, 4);
		uint32_t rsvd1:				BITRANGE(5, 7);
		uint32_t src_mip_tail_start_lod:	BITRANGE(8, 11);
		uint32_t rsvd2:				BITRANGE(12, 17);
		uint32_t src_depth_stencil_resource:	BITRANGE(18, 18);
		uint32_t rsvd3:				BITRANGE(19, 20);
		uint32_t src_array_index:		BITRANGE(21, 31);
	} dw21;
};

/**
 * blt_supports_compression:
 * @i915: drm fd
 *
 * Function checks if HW supports flatccs compression in blitter commands
 * on @i915 device.
 *
 * Returns:
 * true if it does, false otherwise.
 */
bool blt_supports_compression(int i915)
{
	uint32_t devid = intel_get_drm_devid(i915);

	return HAS_FLATCCS(devid);
}

/**
 * blt_supports_tiling:
 * @i915: drm fd
 * @tiling: tiling id
 *
 * Function checks if blitter supports @tiling on @i915 device.
 *
 * Returns:
 * true if it does, false otherwise.
 */
bool blt_supports_tiling(int i915, enum blt_tiling tiling)
{
	uint32_t devid = intel_get_drm_devid(i915);

	if (tiling == T_XMAJOR) {
		if (IS_TIGERLAKE(devid) || IS_DG1(devid))
			return false;
		else
			return true;
	}

	if (tiling == T_YMAJOR) {
		if (IS_TIGERLAKE(devid) || IS_DG1(devid))
			return true;
		else
			return false;
	}

	return true;
}

/**
 * blt_tiling_name:
 * @tiling: tiling id
 *
 * Returns:
 * name of @tiling passed. Useful to build test names.
 */
const char *blt_tiling_name(enum blt_tiling tiling)
{
	switch (tiling) {
	case T_LINEAR: return "linear";
	case T_XMAJOR: return "xmajor";
	case T_YMAJOR: return "ymajor";
	case T_TILE4:  return "tile4";
	case T_TILE64: return "tile64";
	}

	igt_warn("invalid tiling passed: %d\n", tiling);
	return NULL;
}

static int __block_tiling(enum blt_tiling tiling)
{
	switch (tiling) {
	case T_LINEAR: return 0;
	case T_XMAJOR: return 1;
	case T_YMAJOR: return 1;
	case T_TILE4:  return 2;
	case T_TILE64: return 3;
	}

	igt_warn("invalid tiling passed: %d\n", tiling);
	return 0;
}

static int __special_mode(const struct blt_copy_data *blt)
{
	if (blt->src.handle == blt->dst.handle &&
	    blt->src.compression && !blt->dst.compression)
		return SM_FULL_RESOLVE;


	return SM_NONE;
}

static int __memory_type(uint32_t region)
{
	igt_assert_f(IS_DEVICE_MEMORY_REGION(region) ||
		     IS_SYSTEM_MEMORY_REGION(region),
		     "Invalid region: %x\n", region);

	if (IS_DEVICE_MEMORY_REGION(region))
		return TM_LOCAL_MEM;
	return TM_SYSTEM_MEM;
}

static enum blt_aux_mode __aux_mode(const struct blt_copy_object *obj)
{
	if (obj->compression == COMPRESSION_ENABLED) {
		igt_assert_f(IS_DEVICE_MEMORY_REGION(obj->region),
			     "XY_BLOCK_COPY_BLT supports compression "
			     "on device memory only\n");
		return AM_AUX_CCS_E;
	}

	return AM_AUX_NONE;
}

static void fill_data(struct gen12_block_copy_data *data,
		      const struct blt_copy_data *blt,
		      uint64_t src_offset, uint64_t dst_offset,
		      bool extended_command)
{
	data->dw00.client = 0x2;
	data->dw00.opcode = 0x41;
	data->dw00.color_depth = blt->color_depth;
	data->dw00.special_mode = __special_mode(blt);
	data->dw00.length = extended_command ? 20 : 10;

	if (__special_mode(blt) == SM_FULL_RESOLVE && blt->src.tiling == T_TILE64) {
		data->dw01.dst_pitch = blt->src.pitch - 1;
		data->dw01.dst_aux_mode = __aux_mode(&blt->src);
	} else {
		data->dw01.dst_pitch = blt->dst.pitch - 1;
		data->dw01.dst_aux_mode = __aux_mode(&blt->dst);
	}

	data->dw01.dst_mocs = blt->dst.mocs;
	data->dw01.dst_compression = blt->dst.compression;
	data->dw01.dst_tiling = __block_tiling(blt->dst.tiling);

	if (blt->dst.compression)
		data->dw01.dst_ctrl_surface_type = blt->dst.compression_type;

	data->dw02.dst_x1 = blt->dst.x1;
	data->dw02.dst_y1 = blt->dst.y1;

	data->dw03.dst_x2 = blt->dst.x2;
	data->dw03.dst_y2 = blt->dst.y2;

	data->dw04.dst_address_lo = dst_offset;
	data->dw05.dst_address_hi = dst_offset >> 32;

	data->dw06.dst_x_offset = blt->dst.x_offset;
	data->dw06.dst_y_offset = blt->dst.y_offset;
	data->dw06.dst_target_memory = __memory_type(blt->dst.region);

	data->dw07.src_x1 = blt->src.x1;
	data->dw07.src_y1 = blt->src.y1;

	data->dw08.src_pitch = blt->src.pitch - 1;
	data->dw08.src_aux_mode = __aux_mode(&blt->src);
	data->dw08.src_mocs = blt->src.mocs;
	data->dw08.src_compression = blt->src.compression;
	data->dw08.src_tiling = __block_tiling(blt->src.tiling);

	if (blt->src.compression)
		data->dw08.src_ctrl_surface_type = blt->src.compression_type;

	data->dw09.src_address_lo = src_offset;
	data->dw10.src_address_hi = src_offset >> 32;

	data->dw11.src_x_offset = blt->src.x_offset;
	data->dw11.src_y_offset = blt->src.y_offset;
	data->dw11.src_target_memory = __memory_type(blt->src.region);
}

static void fill_data_ext(struct gen12_block_copy_data_ext *dext,
			  const struct blt_block_copy_data_ext *ext)
{
	dext->dw12.src_compression_format = ext->src.compression_format;
	dext->dw12.src_clear_value_enable = ext->src.clear_value_enable;
	dext->dw12.src_clear_address_low = ext->src.clear_address;

	dext->dw13.src_clear_address_hi0 = ext->src.clear_address >> 32;

	dext->dw14.dst_compression_format = ext->dst.compression_format;
	dext->dw14.dst_clear_value_enable = ext->dst.clear_value_enable;
	dext->dw14.dst_clear_address_low = ext->dst.clear_address;

	dext->dw15.dst_clear_address_hi0 = ext->dst.clear_address >> 32;

	dext->dw16.dst_surface_width = ext->dst.surface_width - 1;
	dext->dw16.dst_surface_height = ext->dst.surface_height - 1;
	dext->dw16.dst_surface_type = ext->dst.surface_type;

	dext->dw17.dst_lod = ext->dst.lod;
	dext->dw17.dst_surface_depth = ext->dst.surface_depth;
	dext->dw17.dst_surface_qpitch = ext->dst.surface_qpitch;

	dext->dw18.dst_horizontal_align = ext->dst.horizontal_align;
	dext->dw18.dst_vertical_align = ext->dst.vertical_align;
	dext->dw18.dst_mip_tail_start_lod = ext->dst.mip_tail_start_lod;
	dext->dw18.dst_depth_stencil_resource = ext->dst.depth_stencil_resource;
	dext->dw18.dst_array_index = ext->dst.array_index;

	dext->dw19.src_surface_width = ext->src.surface_width - 1;
	dext->dw19.src_surface_height = ext->src.surface_height - 1;

	dext->dw19.src_surface_type = ext->src.surface_type;

	dext->dw20.src_lod = ext->src.lod;
	dext->dw20.src_surface_depth = ext->src.surface_depth;
	dext->dw20.src_surface_qpitch = ext->src.surface_qpitch;

	dext->dw21.src_horizontal_align = ext->src.horizontal_align;
	dext->dw21.src_vertical_align = ext->src.vertical_align;
	dext->dw21.src_mip_tail_start_lod = ext->src.mip_tail_start_lod;
	dext->dw21.src_depth_stencil_resource = ext->src.depth_stencil_resource;
	dext->dw21.src_array_index = ext->src.array_index;
}

static void dump_bb_cmd(struct gen12_block_copy_data *data)
{
	uint32_t *cmd = (uint32_t *) data;

	igt_info("details:\n");
	igt_info(" dw00: [%08x] <client: 0x%x, opcode: 0x%x, color depth: %d, "
		 "special mode: %d, length: %d>\n",
		 cmd[0],
		 data->dw00.client, data->dw00.opcode, data->dw00.color_depth,
		 data->dw00.special_mode, data->dw00.length);
	igt_info(" dw01: [%08x] dst <pitch: %d, aux: %d, mocs: %d, compr: %d, "
		 "tiling: %d, ctrl surf type: %d>\n",
		 cmd[1], data->dw01.dst_pitch, data->dw01.dst_aux_mode,
		 data->dw01.dst_mocs, data->dw01.dst_compression,
		 data->dw01.dst_tiling, data->dw01.dst_ctrl_surface_type);
	igt_info(" dw02: [%08x] dst geom <x1: %d, y1: %d>\n",
		 cmd[2], data->dw02.dst_x1, data->dw02.dst_y1);
	igt_info(" dw03: [%08x]          <x2: %d, y2: %d>\n",
		 cmd[3], data->dw03.dst_x2, data->dw03.dst_y2);
	igt_info(" dw04: [%08x] dst offset lo (0x%x)\n",
		 cmd[4], data->dw04.dst_address_lo);
	igt_info(" dw05: [%08x] dst offset hi (0x%x)\n",
		 cmd[5], data->dw05.dst_address_hi);
	igt_info(" dw06: [%08x] dst <x offset: 0x%x, y offset: 0x%0x, target mem: %d>\n",
		 cmd[6], data->dw06.dst_x_offset, data->dw06.dst_y_offset,
		 data->dw06.dst_target_memory);
	igt_info(" dw07: [%08x] src geom <x1: %d, y1: %d>\n",
		 cmd[7], data->dw07.src_x1, data->dw07.src_y1);
	igt_info(" dw08: [%08x] src <pitch: %d, aux: %d, mocs: %d, compr: %d, "
		 "tiling: %d, ctrl surf type: %d>\n",
		 cmd[8], data->dw08.src_pitch, data->dw08.src_aux_mode,
		 data->dw08.src_mocs, data->dw08.src_compression,
		 data->dw08.src_tiling, data->dw08.src_ctrl_surface_type);
	igt_info(" dw09: [%08x] src offset lo (0x%x)\n",
		 cmd[9], data->dw09.src_address_lo);
	igt_info(" dw10: [%08x] src offset hi (0x%x)\n",
		 cmd[10], data->dw10.src_address_hi);
	igt_info(" dw11: [%08x] src <x offset: 0x%x, y offset: 0x%0x, target mem: %d>\n",
		 cmd[11], data->dw11.src_x_offset, data->dw11.src_y_offset,
		 data->dw11.src_target_memory);
}

static void dump_bb_ext(struct gen12_block_copy_data_ext *data)
{
	uint32_t *cmd = (uint32_t *) data;

	igt_info("ext details:\n");
	igt_info(" dw12: [%08x] src <compression fmt: %d, clear value enable: %d, "
		 "clear address low: 0x%x>\n",
		 cmd[0],
		 data->dw12.src_compression_format,
		 data->dw12.src_clear_value_enable,
		 data->dw12.src_clear_address_low);
	igt_info(" dw13: [%08x] src clear address hi: 0x%x\n",
		 cmd[1], data->dw13.src_clear_address_hi0);
	igt_info(" dw14: [%08x] dst <compression fmt: %d, clear value enable: %d, "
		 "clear address low: 0x%x>\n",
		 cmd[2],
		 data->dw14.dst_compression_format,
		 data->dw14.dst_clear_value_enable,
		 data->dw14.dst_clear_address_low);
	igt_info(" dw15: [%08x] dst clear address hi: 0x%x\n",
		 cmd[3], data->dw15.dst_clear_address_hi0);
	igt_info(" dw16: [%08x] dst surface <width: %d, height: %d, type: %d>\n",
		 cmd[4], data->dw16.dst_surface_width,
		 data->dw16.dst_surface_height, data->dw16.dst_surface_type);
	igt_info(" dw17: [%08x] dst surface <lod: %d, depth: %d, qpitch: %d>\n",
		 cmd[5], data->dw17.dst_lod,
		 data->dw17.dst_surface_depth, data->dw17.dst_surface_qpitch);
	igt_info(" dw18: [%08x] dst <halign: %d, valign: %d, mip tail: %d, "
		 "depth stencil: %d, array index: %d>\n",
		 cmd[6],
		 data->dw18.dst_horizontal_align,
		 data->dw18.dst_vertical_align,
		 data->dw18.dst_mip_tail_start_lod,
		 data->dw18.dst_depth_stencil_resource,
		 data->dw18.dst_array_index);

	igt_info(" dw19: [%08x] src surface <width: %d, height: %d, type: %d>\n",
		 cmd[7], data->dw19.src_surface_width,
		 data->dw19.src_surface_height, data->dw19.src_surface_type);
	igt_info(" dw20: [%08x] src surface <lod: %d, depth: %d, qpitch: %d>\n",
		 cmd[8], data->dw20.src_lod,
		 data->dw20.src_surface_depth, data->dw20.src_surface_qpitch);
	igt_info(" dw21: [%08x] src <halign: %d, valign: %d, mip tail: %d, "
		 "depth stencil: %d, array index: %d>\n",
		 cmd[9],
		 data->dw21.src_horizontal_align,
		 data->dw21.src_vertical_align,
		 data->dw21.src_mip_tail_start_lod,
		 data->dw21.src_depth_stencil_resource,
		 data->dw21.src_array_index);
}

/**
 * blt_block_copy:
 * @i915: drm fd
 * @ctx: intel_ctx_t context
 * @e: blitter engine for @ctx
 * @ahnd: allocator handle
 * @blt: basic blitter data (for TGL/DG1 which doesn't support ext version)
 * @ext: extended blitter data (for DG2+, supports flatccs compression)
 *
 * Function does blit between @src and @dst described in @blt object.
 *
 * Returns:
 * execbuffer status.
 */
int blt_block_copy(int i915,
		   const intel_ctx_t *ctx,
		   const struct intel_execution_engine2 *e,
		   uint64_t ahnd,
		   const struct blt_copy_data *blt,
		   const struct blt_block_copy_data_ext *ext)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj[3] = {};
	struct gen12_block_copy_data data = {};
	struct gen12_block_copy_data_ext dext = {};
	uint64_t dst_offset, src_offset, bb_offset, alignment;
	uint32_t *bb;
	int i, ret;

	igt_assert_f(ahnd, "block-copy supports softpin only\n");
	igt_assert_f(blt, "block-copy requires data to do blit\n");

	alignment = gem_detect_safe_alignment(i915);
	src_offset = get_offset(ahnd, blt->src.handle, blt->src.size, alignment);
	if (__special_mode(blt) == SM_FULL_RESOLVE)
		dst_offset = src_offset;
	else
		dst_offset = get_offset(ahnd, blt->dst.handle, blt->dst.size, alignment);
	bb_offset = get_offset(ahnd, blt->bb.handle, blt->bb.size, alignment);

	fill_data(&data, blt, src_offset, dst_offset, ext);

	i = sizeof(data) / sizeof(uint32_t);
	bb = gem_mmap__device_coherent(i915, blt->bb.handle, 0, blt->bb.size,
				       PROT_READ | PROT_WRITE);
	memcpy(bb, &data, sizeof(data));

	if (ext) {
		fill_data_ext(&dext, ext);
		memcpy(bb + i, &dext, sizeof(dext));
		i += sizeof(dext) / sizeof(uint32_t);
	}
	bb[i++] = MI_BATCH_BUFFER_END;

	if (blt->print_bb) {
		igt_info("[BLOCK COPY]\n");
		igt_info("src offset: %llx, dst offset: %llx, bb offset: %llx\n",
			 (long long) src_offset, (long long) dst_offset,
			 (long long) bb_offset);

		dump_bb_cmd(&data);
		if (ext)
			dump_bb_ext(&dext);
	}

	munmap(bb, blt->bb.size);

	obj[0].offset = CANONICAL(dst_offset);
	obj[1].offset = CANONICAL(src_offset);
	obj[2].offset = CANONICAL(bb_offset);
	obj[0].handle = blt->dst.handle;
	obj[1].handle = blt->src.handle;
	obj[2].handle = blt->bb.handle;
	obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
		       EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[2].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	execbuf.buffer_count = 3;
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.rsvd1 = ctx ? ctx->id : 0;
	execbuf.flags = e ? e->flags : I915_EXEC_BLT;
	ret = __gem_execbuf(i915, &execbuf);

	return ret;
}

static uint16_t __ccs_size(const struct blt_ctrl_surf_copy_data *surf)
{
	uint32_t src_size, dst_size;

	src_size = surf->src.access_type == DIRECT_ACCESS ?
				surf->src.size : surf->src.size / CCS_RATIO;

	dst_size = surf->dst.access_type == DIRECT_ACCESS ?
				surf->dst.size : surf->dst.size / CCS_RATIO;

	igt_assert_f(src_size <= dst_size, "dst size must be >= src size for CCS copy\n");

	return src_size;
}

struct gen12_ctrl_surf_copy_data {
	struct {
		uint32_t length:			BITRANGE(0, 7);
		uint32_t size_of_ctrl_copy:		BITRANGE(8, 17);
		uint32_t rsvd0:				BITRANGE(18, 19);
		uint32_t dst_access_type:		BITRANGE(20, 20);
		uint32_t src_access_type:		BITRANGE(21, 21);
		uint32_t opcode:			BITRANGE(22, 28);
		uint32_t client:			BITRANGE(29, 31);
	} dw00;

	struct {
		uint32_t src_address_lo;
	} dw01;

	struct {
		uint32_t src_address_hi:		BITRANGE(0, 24);
		uint32_t src_mocs:			BITRANGE(25, 31);
	} dw02;

	struct {
		uint32_t dst_address_lo;
	} dw03;

	struct {
		uint32_t dst_address_hi:		BITRANGE(0, 24);
		uint32_t dst_mocs:			BITRANGE(25, 31);
	} dw04;
};

static void dump_bb_surf_ctrl_cmd(const struct gen12_ctrl_surf_copy_data *data)
{
	uint32_t *cmd = (uint32_t *) data;

	igt_info("details:\n");
	igt_info(" dw00: [%08x] <client: 0x%x, opcode: 0x%x, "
		 "src/dst access type: <%d, %d>, size of ctrl copy: %u, length: %d>\n",
		 cmd[0],
		 data->dw00.client, data->dw00.opcode,
		 data->dw00.src_access_type, data->dw00.dst_access_type,
		 data->dw00.size_of_ctrl_copy, data->dw00.length);
	igt_info(" dw01: [%08x] src offset lo (0x%x)\n",
		 cmd[1], data->dw01.src_address_lo);
	igt_info(" dw02: [%08x] src offset hi (0x%x), src mocs: %u\n",
		 cmd[2], data->dw02.src_address_hi, data->dw02.src_mocs);
	igt_info(" dw03: [%08x] dst offset lo (0x%x)\n",
		 cmd[3], data->dw03.dst_address_lo);
	igt_info(" dw04: [%08x] dst offset hi (0x%x), src mocs: %u\n",
		 cmd[4], data->dw04.dst_address_hi, data->dw04.dst_mocs);
}

/**
 * blt_ctrl_surf_copy:
 * @i915: drm fd
 * @ctx: intel_ctx_t context
 * @e: blitter engine for @ctx
 * @ahnd: allocator handle
 * @surf: blitter data for ctrl-surf-copy
 *
 * Function does ctrl-surf-copy blit between @src and @dst described in
 * @blt object.
 *
 * Returns:
 * execbuffer status.
 */
int blt_ctrl_surf_copy(int i915,
		       const intel_ctx_t *ctx,
		       const struct intel_execution_engine2 *e,
		       uint64_t ahnd,
		       const struct blt_ctrl_surf_copy_data *surf)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj[3] = {};
	struct gen12_ctrl_surf_copy_data data = {};
	uint64_t dst_offset, src_offset, bb_offset, alignment;
	uint32_t *bb;
	int i;

	igt_assert_f(ahnd, "ctrl-surf-copy supports softpin only\n");
	igt_assert_f(surf, "ctrl-surf-copy requires data to do ctrl-surf-copy blit\n");

	alignment = max_t(uint64_t, gem_detect_safe_alignment(i915), 1ull << 16);

	data.dw00.client = 0x2;
	data.dw00.opcode = 0x48;
	data.dw00.src_access_type = surf->src.access_type;
	data.dw00.dst_access_type = surf->dst.access_type;

	/* Ensure dst has size capable to keep src ccs aux */
	data.dw00.size_of_ctrl_copy = __ccs_size(surf) / CCS_RATIO - 1;
	data.dw00.length = 0x3;

	src_offset = get_offset(ahnd, surf->src.handle, surf->src.size,
				alignment);
	dst_offset = get_offset(ahnd, surf->dst.handle, surf->dst.size,
				alignment);
	bb_offset = get_offset(ahnd, surf->bb.handle, surf->bb.size,
			       alignment);

	data.dw01.src_address_lo = src_offset;
	data.dw02.src_address_hi = src_offset >> 32;
	data.dw02.src_mocs = surf->src.mocs;

	data.dw03.dst_address_lo = dst_offset;
	data.dw04.dst_address_hi = dst_offset >> 32;
	data.dw04.dst_mocs = surf->dst.mocs;

	i = sizeof(data) / sizeof(uint32_t);
	bb = gem_mmap__device_coherent(i915, surf->bb.handle, 0, surf->bb.size,
				       PROT_READ | PROT_WRITE);
	memcpy(bb, &data, sizeof(data));
	bb[i++] = MI_BATCH_BUFFER_END;

	if (surf->print_bb) {
		igt_info("BB [CTRL SURF]:\n");
		igt_info("src offset: %llx, dst offset: %llx, bb offset: %llx\n",
			 (long long) src_offset, (long long) dst_offset,
			 (long long) bb_offset);

		dump_bb_surf_ctrl_cmd(&data);
	}
	munmap(bb, surf->bb.size);

	obj[0].offset = CANONICAL(dst_offset);
	obj[1].offset = CANONICAL(src_offset);
	obj[2].offset = CANONICAL(bb_offset);
	obj[0].handle = surf->dst.handle;
	obj[1].handle = surf->src.handle;
	obj[2].handle = surf->bb.handle;
	obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
		       EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[2].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	execbuf.buffer_count = 3;
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.flags = e ? e->flags : I915_EXEC_BLT;
	execbuf.rsvd1 = ctx ? ctx->id : 0;
	gem_execbuf(i915, &execbuf);
	put_offset(ahnd, surf->dst.handle);
	put_offset(ahnd, surf->src.handle);
	put_offset(ahnd, surf->bb.handle);

	return 0;
}

struct gen12_fast_copy_data {
	struct {
		uint32_t length:			BITRANGE(0, 7);
		uint32_t rsvd1:				BITRANGE(8, 12);
		uint32_t dst_tiling:			BITRANGE(13, 14);
		uint32_t rsvd0:				BITRANGE(15, 19);
		uint32_t src_tiling:			BITRANGE(20, 21);
		uint32_t opcode:			BITRANGE(22, 28);
		uint32_t client:			BITRANGE(29, 31);
	} dw00;

	struct {
		uint32_t dst_pitch:			BITRANGE(0, 15);
		uint32_t rsvd1:				BITRANGE(16, 23);
		uint32_t color_depth:			BITRANGE(24, 26);
		uint32_t rsvd0:				BITRANGE(27, 27);
		uint32_t dst_memory:			BITRANGE(28, 28);
		uint32_t src_memory:			BITRANGE(29, 29);
		uint32_t dst_type_y:			BITRANGE(30, 30);
		uint32_t src_type_y:			BITRANGE(31, 31);
	} dw01;

	struct {
		int32_t dst_x1:				BITRANGE(0, 15);
		int32_t dst_y1:				BITRANGE(16, 31);
	} dw02;

	struct {
		int32_t dst_x2:				BITRANGE(0, 15);
		int32_t dst_y2:				BITRANGE(16, 31);
	} dw03;

	struct {
		uint32_t dst_address_lo;
	} dw04;

	struct {
		uint32_t dst_address_hi;
	} dw05;

	struct {
		int32_t src_x1:				BITRANGE(0, 15);
		int32_t src_y1:				BITRANGE(16, 31);
	} dw06;

	struct {
		uint32_t src_pitch:			BITRANGE(0, 15);
		uint32_t rsvd0:				BITRANGE(16, 31);
	} dw07;

	struct {
		uint32_t src_address_lo;
	} dw08;

	struct {
		uint32_t src_address_hi;
	} dw09;
};

static int __fast_tiling(enum blt_tiling tiling)
{
	switch (tiling) {
	case T_LINEAR: return 0;
	case T_XMAJOR: return 1;
	case T_YMAJOR: return 2;
	case T_TILE4:  return 2;
	case T_TILE64: return 3;
	}
	return 0;
}

static int __fast_color_depth(enum blt_color_depth depth)
{
	switch (depth) {
	case CD_8bit:   return 0;
	case CD_16bit:  return 1;
	case CD_32bit:  return 3;
	case CD_64bit:  return 4;
	case CD_96bit:
		igt_assert_f(0, "Unsupported depth\n");
		break;
	case CD_128bit: return 5;
	};
	return 0;
}

static void dump_bb_fast_cmd(struct gen12_fast_copy_data *data)
{
	uint32_t *cmd = (uint32_t *) data;

	igt_info("BB details:\n");
	igt_info(" dw00: [%08x] <client: 0x%x, opcode: 0x%x, src tiling: %d, "
		 "dst tiling: %d, length: %d>\n",
		 cmd[0], data->dw00.client, data->dw00.opcode,
		 data->dw00.src_tiling, data->dw00.dst_tiling, data->dw00.length);
	igt_info(" dw01: [%08x] dst <pitch: %d, color depth: %d, dst memory: %d, "
		 "src memory: %d,\n"
		 "\t\t\tdst type tile: %d (0-legacy, 1-tile4),\n"
		 "\t\t\tsrc type tile: %d (0-legacy, 1-tile4)>\n",
		 cmd[1], data->dw01.dst_pitch, data->dw01.color_depth,
		 data->dw01.dst_memory, data->dw01.src_memory,
		 data->dw01.dst_type_y, data->dw01.src_type_y);
	igt_info(" dw02: [%08x] dst geom <x1: %d, y1: %d>\n",
		 cmd[2], data->dw02.dst_x1, data->dw02.dst_y1);
	igt_info(" dw03: [%08x]          <x2: %d, y2: %d>\n",
		 cmd[3], data->dw03.dst_x2, data->dw03.dst_y2);
	igt_info(" dw04: [%08x] dst offset lo (0x%x)\n",
		 cmd[4], data->dw04.dst_address_lo);
	igt_info(" dw05: [%08x] dst offset hi (0x%x)\n",
		 cmd[5], data->dw05.dst_address_hi);
	igt_info(" dw06: [%08x] src geom <x1: %d, y1: %d>\n",
		 cmd[6], data->dw06.src_x1, data->dw06.src_y1);
	igt_info(" dw07: [%08x] src <pitch: %d>\n",
		 cmd[7], data->dw07.src_pitch);
	igt_info(" dw08: [%08x] src offset lo (0x%x)\n",
		 cmd[8], data->dw08.src_address_lo);
	igt_info(" dw09: [%08x] src offset hi (0x%x)\n",
		 cmd[9], data->dw09.src_address_hi);
}

/**
 * blt_fast_copy:
 * @i915: drm fd
 * @ctx: intel_ctx_t context
 * @e: blitter engine for @ctx
 * @ahnd: allocator handle
 * @blt: blitter data for fast-copy (same as for block-copy but doesn't use
 * compression fields).
 *
 * Function does fast blit between @src and @dst described in @blt object.
 *
 * Returns:
 * execbuffer status.
 */
int blt_fast_copy(int i915,
		  const intel_ctx_t *ctx,
		  const struct intel_execution_engine2 *e,
		  uint64_t ahnd,
		  const struct blt_copy_data *blt)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj[3] = {};
	struct gen12_fast_copy_data data = {};
	uint64_t dst_offset, src_offset, bb_offset, alignment;
	uint32_t *bb;
	int i, ret;

	alignment = gem_detect_safe_alignment(i915);

	data.dw00.client = 0x2;
	data.dw00.opcode = 0x42;
	data.dw00.dst_tiling = __fast_tiling(blt->dst.tiling);
	data.dw00.src_tiling = __fast_tiling(blt->src.tiling);
	data.dw00.length = 8;

	data.dw01.dst_pitch = blt->dst.pitch;
	data.dw01.color_depth = __fast_color_depth(blt->color_depth);
	data.dw01.dst_memory = __memory_type(blt->dst.region);
	data.dw01.src_memory = __memory_type(blt->src.region);
	data.dw01.dst_type_y = blt->dst.tiling == T_TILE4 ? 1 : 0;
	data.dw01.src_type_y = blt->src.tiling == T_TILE4 ? 1 : 0;

	data.dw02.dst_x1 = blt->dst.x1;
	data.dw02.dst_y1 = blt->dst.y1;

	data.dw03.dst_x2 = blt->dst.x2;
	data.dw03.dst_y2 = blt->dst.y2;

	src_offset = get_offset(ahnd, blt->src.handle, blt->src.size, alignment);
	dst_offset = get_offset(ahnd, blt->dst.handle, blt->dst.size, alignment);
	bb_offset = get_offset(ahnd, blt->bb.handle, blt->bb.size, alignment);

	data.dw04.dst_address_lo = dst_offset;
	data.dw05.dst_address_hi = dst_offset >> 32;

	data.dw06.src_x1 = blt->src.x1;
	data.dw06.src_y1 = blt->src.y1;

	data.dw07.src_pitch = blt->src.pitch;

	data.dw08.src_address_lo = src_offset;
	data.dw09.src_address_hi = src_offset >> 32;

	i = sizeof(data) / sizeof(uint32_t);
	bb = gem_mmap__device_coherent(i915, blt->bb.handle, 0, blt->bb.size,
				       PROT_READ | PROT_WRITE);

	memcpy(bb, &data, sizeof(data));
	bb[i++] = MI_BATCH_BUFFER_END;

	if (blt->print_bb) {
		igt_info("BB [FAST COPY]\n");
		igt_info("blit [src offset: %llx, dst offset: %llx\n",
			 (long long) src_offset, (long long) dst_offset);
		dump_bb_fast_cmd(&data);
	}

	munmap(bb, blt->bb.size);

	obj[0].offset = CANONICAL(dst_offset);
	obj[1].offset = CANONICAL(src_offset);
	obj[2].offset = CANONICAL(bb_offset);
	obj[0].handle = blt->dst.handle;
	obj[1].handle = blt->src.handle;
	obj[2].handle = blt->bb.handle;
	obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
		       EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[2].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	execbuf.buffer_count = 3;
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.rsvd1 = ctx ? ctx->id : 0;
	execbuf.flags = e ? e->flags : I915_EXEC_BLT;
	ret = __gem_execbuf(i915, &execbuf);
	put_offset(ahnd, blt->dst.handle);
	put_offset(ahnd, blt->src.handle);
	put_offset(ahnd, blt->bb.handle);

	return ret;
}

/**
 * blt_surface_fill_rect:
 * @i915: drm fd
 * @obj: blitter copy object (@blt_copy_object) to fill with gradient pattern
 * @width: width
 * @height: height
 *
 * Function fills surface @width x @height * 24bpp with color gradient
 * (internally uses ARGB where A == 0xff, see Cairo docs).
 */
void blt_surface_fill_rect(int i915, const struct blt_copy_object *obj,
			   uint32_t width, uint32_t height)
{
	cairo_surface_t *surface;
	cairo_pattern_t *pat;
	cairo_t *cr;
	void *map = obj->ptr;

	if (!map)
		map = gem_mmap__device_coherent(i915, obj->handle, 0,
						obj->size, PROT_READ | PROT_WRITE);

	surface = cairo_image_surface_create_for_data(map,
						      CAIRO_FORMAT_RGB24,
						      width, height,
						      obj->pitch);

	cr = cairo_create(surface);

	cairo_rectangle(cr, 0, 0, width, height);
	cairo_clip(cr);

	pat = cairo_pattern_create_mesh();
	cairo_mesh_pattern_begin_patch(pat);
	cairo_mesh_pattern_move_to(pat, 0, 0);
	cairo_mesh_pattern_line_to(pat, width, 0);
	cairo_mesh_pattern_line_to(pat, width, height);
	cairo_mesh_pattern_line_to(pat, 0, height);
	cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 1.0, 0.0, 0.0);
	cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 0.0, 1.0, 0.0);
	cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 0.0, 0.0, 1.0);
	cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 1.0, 1.0, 1.0);
	cairo_mesh_pattern_end_patch(pat);

	cairo_rectangle(cr, 0, 0, width, height);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);
	if (!obj->ptr)
		munmap(map, obj->size);
}

/**
 * blt_surface_info:
 * @info: information header
 * @obj: blitter copy object (@blt_copy_object) to print surface info
 */
void blt_surface_info(const char *info, const struct blt_copy_object *obj)
{
	igt_info("[%s]\n", info);
	igt_info("surface <handle: %u, size: %llx, region: %x, mocs: %x>\n",
		 obj->handle, (long long) obj->size, obj->region, obj->mocs);
	igt_info("        <tiling: %s, compression: %u, compression type: %d>\n",
		 blt_tiling_name(obj->tiling), obj->compression, obj->compression_type);
	igt_info("        <pitch: %u, offset [x: %u, y: %u] geom [<%d,%d> <%d,%d>]>\n",
		 obj->pitch, obj->x_offset, obj->y_offset,
		 obj->x1, obj->y1, obj->x2, obj->y2);
}

/**
 * blt_surface_to_png:
 * @i915: drm fd
 * @run_id: prefix id to allow grouping files stored from single run
 * @fileid: file identifier
 * @obj: blitter copy object (@blt_copy_object) to save to png
 * @width: width
 * @height: height
 *
 * Function save surface to png file. Assumes ARGB format where A == 0xff.
 */
void blt_surface_to_png(int i915, uint32_t run_id, const char *fileid,
			const struct blt_copy_object *obj,
			uint32_t width, uint32_t height)
{
	cairo_surface_t *surface;
	cairo_status_t ret;
	uint8_t *map = (uint8_t *) obj->ptr;
	int format;
	int stride = obj->tiling ? obj->pitch * 4 : obj->pitch;
	char filename[FILENAME_MAX];

	snprintf(filename, FILENAME_MAX-1, "%d-%s-%s-%ux%u-%s.png",
		 run_id, fileid, blt_tiling_name(obj->tiling), width, height,
		 obj->compression ? "compressed" : "uncompressed");

	if (!map)
		map = gem_mmap__device_coherent(i915, obj->handle, 0,
						obj->size, PROT_READ);
	format = CAIRO_FORMAT_RGB24;
	surface = cairo_image_surface_create_for_data(map,
						      format, width, height,
						      stride);
	ret = cairo_surface_write_to_png(surface, filename);
	if (ret)
		igt_info("Cairo ret: %d (%s)\n", ret, cairo_status_to_string(ret));
	igt_assert(ret == CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);

	if (!obj->ptr)
		munmap(map, obj->size);
}
