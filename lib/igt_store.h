/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#include "igt_gt.h"

void igt_store_word(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e,
		    int fence, uint32_t target_handle,
		    uint64_t target_gpu_addr,
		    uint64_t store_offset, uint32_t store_value);
