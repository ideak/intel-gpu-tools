// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <stdint.h>
#include "intel_chipset.h"
#include "i915/intel_tiling_info.h"

#define BLT_INFO(_cmd, _tiling)  { \
		.blt_cmd_type = _cmd, \
		.supported_tiling = _tiling \
	}

static const struct blt_tiling_info src_copy = BLT_INFO(SRC_COPY, BIT(T_LINEAR));
static const struct blt_tiling_info
		pre_gen8_xy_src_copy = BLT_INFO(XY_SRC_COPY,
						BIT(T_LINEAR) |
						BIT(T_XMAJOR));
static const struct blt_tiling_info
		gen8_xy_src_copy = BLT_INFO(XY_SRC_COPY,
					    BIT(T_LINEAR) |
					    BIT(T_XMAJOR) |
					    BIT(T_YMAJOR));
static const struct blt_tiling_info
		gen11_xy_fast_copy = BLT_INFO(XY_FAST_COPY,
					      BIT(T_LINEAR)  |
					      BIT(T_YMAJOR)  |
					      BIT(T_YFMAJOR) |
					      BIT(T_TILE64));
static const struct blt_tiling_info
		gen12_xy_fast_copy = BLT_INFO(XY_FAST_COPY,
					      BIT(T_LINEAR) |
					      BIT(T_YMAJOR) |
					      BIT(T_TILE4)  |
					      BIT(T_TILE64));
static const struct blt_tiling_info
		dg2_xy_fast_copy = BLT_INFO(XY_FAST_COPY,
					    BIT(T_LINEAR) |
					    BIT(T_XMAJOR) |
					    BIT(T_TILE4)  |
					    BIT(T_TILE64));
static const struct blt_tiling_info
		gen12_xy_block_copy = BLT_INFO(XY_BLOCK_COPY,
					       BIT(T_LINEAR) |
					       BIT(T_YMAJOR));
static const struct blt_tiling_info
		dg2_xy_block_copy = BLT_INFO(XY_BLOCK_COPY,
					     BIT(T_LINEAR) |
					     BIT(T_XMAJOR) |
					     BIT(T_TILE4)  |
					     BIT(T_TILE64));

const struct intel_cmds_info pre_gen8_cmds_info = {
	.blt_cmds = {
		[SRC_COPY] = &src_copy,
		[XY_SRC_COPY] = &pre_gen8_xy_src_copy
	}
};

const struct intel_cmds_info gen8_cmds_info = {
	.blt_cmds = {
		[XY_SRC_COPY] = &gen8_xy_src_copy,
	}
};

const struct intel_cmds_info gen11_cmds_info = {
	.blt_cmds = {
		[XY_SRC_COPY] = &gen8_xy_src_copy,
		[XY_FAST_COPY] = &gen11_xy_fast_copy,
	}
};

const struct intel_cmds_info gen12_cmds_info = {
	.blt_cmds = {
		[XY_SRC_COPY] = &gen8_xy_src_copy,
		[XY_FAST_COPY] = &gen12_xy_fast_copy,
		[XY_BLOCK_COPY] = &gen12_xy_block_copy,
	}
};

const struct intel_cmds_info gen12_dg2_cmds_info = {
	.blt_cmds = {
		[XY_SRC_COPY] = &gen8_xy_src_copy,
		[XY_FAST_COPY] = &dg2_xy_fast_copy,
		[XY_BLOCK_COPY] = &dg2_xy_block_copy,
	}
};

const struct intel_cmds_info gen12_mtl_cmds_info = {
	.blt_cmds = {
		[XY_FAST_COPY] = &dg2_xy_fast_copy,
		[XY_BLOCK_COPY] = &dg2_xy_block_copy
	}
};
