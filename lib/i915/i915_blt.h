/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

/**
 * SECTION:i915_blt
 * @short_description: i915 blitter library
 * @title: Blitter library
 * @include: i915_blt.h
 *
 * # Introduction
 *
 * Gen12+ blitter commands like XY_BLOCK_COPY_BLT are quite long
 * and if we would like to provide all arguments to function,
 * list would be long, unreadable and error prone to invalid argument placement.
 * Providing objects (structs) seems more reasonable and opens some more
 * opportunities to share some object data across different blitter commands.
 *
 * Blitter library supports no-reloc (softpin) mode only (apart of TGL
 * there's no relocations enabled) thus ahnd is mandatory. Providing NULL ctx
 * means we use default context with I915_EXEC_BLT as an execution engine.
 *
 * Library introduces tiling enum which distinguishes tiling formats regardless
 * legacy I915_TILING_... definitions. This allows to control fully what tilings
 * are handled by command and skip/assert ones which are not supported.
 *
 * # Supported commands
 *
 * - XY_BLOCK_COPY_BLT - (block-copy) TGL/DG1 + DG2+ (ext version)
 * - XY_FAST_COPY_BLT - (fast-copy)
 * - XY_CTRL_SURF_COPY_BLT - (ctrl-surf-copy) DG2+
 *
 * # Usage details
 *
 * For block-copy and fast-copy @blt_copy_object struct is used to collect
 * data about source and destination objects. It contains handle, region,
 * size, etc...  which are using for blits. Some fields are not used for
 * fast-copy copy (like compression) and command which use this exclusively
 * is annotated in the comment.
 *
 */

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <malloc.h>
#include "drm.h"
#include "igt.h"
#include "intel_cmds_info.h"

#define CCS_RATIO 256

enum blt_color_depth {
	CD_8bit,
	CD_16bit,
	CD_32bit,
	CD_64bit,
	CD_96bit,
	CD_128bit,
};

enum blt_compression {
	COMPRESSION_DISABLED,
	COMPRESSION_ENABLED,
};

enum blt_compression_type {
	COMPRESSION_TYPE_3D,
	COMPRESSION_TYPE_MEDIA,
};

/* BC - block-copy */
struct blt_copy_object {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
	uint8_t mocs;
	enum blt_tiling_type tiling;
	enum blt_compression compression;  /* BC only */
	enum blt_compression_type compression_type; /* BC only */
	uint32_t pitch;
	uint16_t x_offset, y_offset;
	int16_t x1, y1, x2, y2;

	/* mapping or null */
	uint32_t *ptr;
};

struct blt_copy_batch {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
};

/* Common for block-copy and fast-copy */
struct blt_copy_data {
	int i915;
	struct blt_copy_object src;
	struct blt_copy_object dst;
	struct blt_copy_batch bb;
	enum blt_color_depth color_depth;

	/* debug stuff */
	bool print_bb;
};

enum blt_surface_type {
	SURFACE_TYPE_1D,
	SURFACE_TYPE_2D,
	SURFACE_TYPE_3D,
	SURFACE_TYPE_CUBE,
};

struct blt_block_copy_object_ext {
	uint8_t compression_format;
	bool clear_value_enable;
	uint64_t clear_address;
	uint16_t surface_width;
	uint16_t surface_height;
	enum blt_surface_type surface_type;
	uint16_t surface_qpitch;
	uint16_t surface_depth;
	uint8_t lod;
	uint8_t horizontal_align;
	uint8_t vertical_align;
	uint8_t mip_tail_start_lod;
	bool depth_stencil_resource;
	uint16_t array_index;
};

struct blt_block_copy_data_ext {
	struct blt_block_copy_object_ext src;
	struct blt_block_copy_object_ext dst;
};

enum blt_access_type {
	INDIRECT_ACCESS,
	DIRECT_ACCESS,
};

struct blt_ctrl_surf_copy_object {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
	uint8_t mocs;
	enum blt_access_type access_type;
};

struct blt_ctrl_surf_copy_data {
	int i915;
	struct blt_ctrl_surf_copy_object src;
	struct blt_ctrl_surf_copy_object dst;
	struct blt_copy_batch bb;

	/* debug stuff */
	bool print_bb;
};

bool blt_supports_compression(int i915);
bool blt_supports_command(const struct intel_cmds_info *cmds_info,
			  enum blt_cmd_type cmd);
bool blt_cmd_supports_tiling(const struct intel_cmds_info *cmds_info,
			     enum blt_cmd_type cmd,
			     enum blt_tiling_type tiling);
bool blt_cmd_has_property(const struct intel_cmds_info *cmds_info,
			  enum blt_cmd_type cmd,
			  uint32_t prop);

bool blt_has_block_copy(int i915);
bool blt_has_fast_copy(int i915);

bool blt_fast_copy_supports_tiling(int i915, enum blt_tiling_type tiling);
bool blt_block_copy_supports_tiling(int i915, enum blt_tiling_type tiling);
bool blt_block_copy_supports_compression(int i915);
bool blt_uses_extended_block_copy(int i915);

const char *blt_tiling_name(enum blt_tiling_type tiling);

uint64_t emit_blt_block_copy(int i915,
			     uint64_t ahnd,
			     const struct blt_copy_data *blt,
			     const struct blt_block_copy_data_ext *ext,
			     uint64_t bb_pos,
			     bool emit_bbe);

int blt_block_copy(int i915,
		   const intel_ctx_t *ctx,
		   const struct intel_execution_engine2 *e,
		   uint64_t ahnd,
		   const struct blt_copy_data *blt,
		   const struct blt_block_copy_data_ext *ext);

uint64_t emit_blt_ctrl_surf_copy(int i915,
				 uint64_t ahnd,
				 const struct blt_ctrl_surf_copy_data *surf,
				 uint64_t bb_pos,
				 bool emit_bbe);

int blt_ctrl_surf_copy(int i915,
		       const intel_ctx_t *ctx,
		       const struct intel_execution_engine2 *e,
		       uint64_t ahnd,
		       const struct blt_ctrl_surf_copy_data *surf);

uint64_t emit_blt_fast_copy(int i915,
			    uint64_t ahnd,
			    const struct blt_copy_data *blt,
			    uint64_t bb_pos,
			    bool emit_bbe);

int blt_fast_copy(int i915,
		  const intel_ctx_t *ctx,
		  const struct intel_execution_engine2 *e,
		  uint64_t ahnd,
		  const struct blt_copy_data *blt);

void blt_set_geom(struct blt_copy_object *obj, uint32_t pitch,
		  int16_t x1, int16_t y1, int16_t x2, int16_t y2,
		  uint16_t x_offset, uint16_t y_offset);
void blt_set_batch(struct blt_copy_batch *batch,
		   uint32_t handle, uint64_t size, uint32_t region);

struct blt_copy_object *
blt_create_object(int i915, uint32_t region,
		  uint32_t width, uint32_t height, uint32_t bpp, uint8_t mocs,
		  enum blt_tiling_type tiling,
		  enum blt_compression compression,
		  enum blt_compression_type compression_type,
		  bool create_mapping);
void blt_destroy_object(int i915, struct blt_copy_object *obj);
void blt_set_object(struct blt_copy_object *obj,
		    uint32_t handle, uint64_t size, uint32_t region,
		    uint8_t mocs, enum blt_tiling_type tiling,
		    enum blt_compression compression,
		    enum blt_compression_type compression_type);
void blt_set_object_ext(struct blt_block_copy_object_ext *obj,
			uint8_t compression_format,
			uint16_t surface_width, uint16_t surface_height,
			enum blt_surface_type surface_type);
void blt_set_copy_object(struct blt_copy_object *obj,
			 const struct blt_copy_object *orig);

void blt_surface_info(const char *info,
		      const struct blt_copy_object *obj);
void blt_surface_fill_rect(int i915, const struct blt_copy_object *obj,
			   uint32_t width, uint32_t height);
void blt_surface_to_png(int i915, uint32_t run_id, const char *fileid,
			const struct blt_copy_object *obj,
			uint32_t width, uint32_t height);
