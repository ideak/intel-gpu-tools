/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Francois Dugast <francois.dugast@intel.com>
 */

#ifndef XE_COMPUTE_H
#define XE_COMPUTE_H

#include <stdint.h>

void tgllp_create_indirect_data(uint32_t *addr_bo_buffer_batch,
				uint64_t addr_input, uint64_t addr_output);
void tgllp_create_surface_state(uint32_t *addr_bo_buffer_batch,
				uint64_t addr_input, uint64_t addr_output);
void tgllp_create_dynamic_state(uint32_t *addr_bo_buffer_batch,
				uint64_t offset_kernel);
void tgllp_create_batch_compute(uint32_t *addr_bo_buffer_batch,
				uint64_t addr_surface_state_base,
				uint64_t addr_dynamic_state_base,
				uint64_t addr_indirect_object_base,
				uint64_t offset_indirect_data_start);

extern unsigned char tgllp_kernel_square_bin[];
extern unsigned int tgllp_kernel_square_length;

#endif	/* XE_COMPUTE_H */
