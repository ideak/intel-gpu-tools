/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */
#ifndef _I915_CRC_H_
#define _I915_CRC_H_

#include <stdint.h>
#include "intel_ctx.h"

/**
 * SECTION:i915_crc
 * @short_description: i915 gpu crc
 * @title: I915 GPU CRC
 * @include: i915_crc.h
 *
 * # Introduction
 *
 * Intel gpu crc calculation implementation.
 */

uint32_t i915_crc32(int i915, uint64_t ahnd, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e,
		    uint32_t data_handle, uint32_t data_size);
bool supports_i915_crc32(int i915);

#endif /* _I915_CRC_ */
