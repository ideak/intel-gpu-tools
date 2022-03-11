// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "igt.h"
#include "i915/gem.h"
#include "intel_mocs.h"

#define DG1_MOCS_UC_IDX				1
#define DG1_MOCS_WB_IDX				5
#define DG2_MOCS_UC_IDX				1
#define DG2_MOCS_WB_IDX				3
#define GEN12_MOCS_UC_IDX			3
#define GEN12_MOCS_WB_IDX			2
#define XY_BLOCK_COPY_BLT_MOCS_SHIFT		21
#define XY_CTRL_SURF_COPY_BLT_MOCS_SHIFT	25

struct drm_i915_mocs_index {
	uint8_t uc_index;
	uint8_t wb_index;
};

static void get_mocs_index(int fd, struct drm_i915_mocs_index *mocs)
{
	uint16_t devid = intel_get_drm_devid(fd);

	/*
	 * Gen >= 12 onwards don't have a setting for PTE,
	 * so using I915_MOCS_PTE as mocs index may leads to
	 * some undefined MOCS behavior.
	 * This helper function is providing current UC as well
	 * as WB MOCS index based on platform.
	 */
	if (IS_DG1(devid)) {
		mocs->uc_index = DG1_MOCS_UC_IDX;
		mocs->wb_index = DG1_MOCS_WB_IDX;
	} else if (IS_DG2(devid)) {
		mocs->uc_index = DG2_MOCS_UC_IDX;
		mocs->wb_index = DG2_MOCS_WB_IDX;

	} else if (IS_GEN12(devid)) {
		mocs->uc_index = GEN12_MOCS_UC_IDX;
		mocs->wb_index = GEN12_MOCS_WB_IDX;
	} else {
		mocs->uc_index = I915_MOCS_PTE;
		mocs->wb_index = I915_MOCS_CACHED;
	}
}

/* BitField [6:1] represents index to MOCS Tables
 * BitField [0] represents Encryption/Decryption
 */

uint8_t intel_get_wb_mocs(int fd)
{
	struct drm_i915_mocs_index mocs;

	get_mocs_index(fd, &mocs);
	return mocs.wb_index << 1;
}

uint8_t intel_get_uc_mocs(int fd)
{
	struct drm_i915_mocs_index mocs;

	get_mocs_index(fd, &mocs);
	return mocs.uc_index << 1;
}
