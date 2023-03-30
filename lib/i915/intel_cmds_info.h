/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_CMDS_INFO_H
#define __INTEL_CMDS_INFO_H

#include <stdint.h>

enum blt_tiling_type {
	T_LINEAR,
	T_XMAJOR,
	T_YMAJOR,
	T_TILE4,
	T_YFMAJOR,
	T_TILE64,
	__BLT_MAX_TILING
};

enum blt_cmd_type {
	SRC_COPY,
	XY_SRC_COPY,
	XY_FAST_COPY,
	XY_BLOCK_COPY,
	__BLT_MAX_CMD
};

struct blt_cmd_info {
	enum blt_cmd_type blt_cmd_type;
	uint32_t supported_tiling;

	uint32_t flags;
#define BLT_CMD_EXTENDED               (1 << 0)
#define BLT_CMD_SUPPORTS_COMPRESSION   (1 << 1)
};

struct intel_cmds_info {
	struct blt_cmd_info const *blt_cmds[__BLT_MAX_CMD];
};

extern const struct intel_cmds_info pre_gen6_cmds_info;
extern const struct intel_cmds_info gen6_cmds_info;
extern const struct intel_cmds_info gen8_cmds_info;
extern const struct intel_cmds_info gen11_cmds_info;
extern const struct intel_cmds_info gen12_cmds_info;
extern const struct intel_cmds_info gen12_dg2_cmds_info;
extern const struct intel_cmds_info gen12_mtl_cmds_info;
extern const struct intel_cmds_info gen12_pvc_cmds_info;

#define for_each_tiling(__tiling) \
	for (__tiling = T_LINEAR; __tiling < __BLT_MAX_TILING; __tiling++)

const struct blt_cmd_info *blt_get_cmd_info(const struct intel_cmds_info *cmds_info,
					    enum blt_cmd_type cmd);

#endif
