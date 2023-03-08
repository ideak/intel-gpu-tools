/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Francois Dugast <francois.dugast@intel.com>
 */

#include "xe_compute.h"

#define PIPE_CONTROL			0x7a000004
#define MI_LOAD_REGISTER_IMM		0x11000001
#define PIPELINE_SELECT			0x69040302
#define MEDIA_VFE_STATE			0x70000007
#define STATE_BASE_ADDRESS		0x61010014
#define MEDIA_STATE_FLUSH		0x0
#define MEDIA_INTERFACE_DESCRIPTOR_LOAD	0x70020002
#define GPGPU_WALKER			0x7105000d
#define MI_BATCH_BUFFER_END		(0xA << 23)

// generated with: ocloc -file kernel.cl -device tgllp && xxd -i kernel_Gen12LPlp.gen
unsigned char tgllp_kernel_square_bin[] = {
	0x61, 0x00, 0x03, 0x80, 0x20, 0x02, 0x05, 0x03, 0x04, 0x00, 0x10, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x66, 0x01, 0x00, 0x80, 0x20, 0x82, 0x01, 0x80,
	0x00, 0x80, 0x00, 0x01, 0xc0, 0x04, 0xc0, 0x04, 0x41, 0x01, 0x20, 0x22,
	0x16, 0x09, 0x11, 0x03, 0x49, 0x00, 0x04, 0xa2, 0x12, 0x09, 0x11, 0x03,
	0x40, 0x01, 0x04, 0x00, 0x60, 0x06, 0x05, 0x05, 0x04, 0x04, 0x00, 0x01,
	0x05, 0x01, 0x58, 0x00, 0x40, 0x00, 0x24, 0x00, 0x60, 0x06, 0x05, 0x0a,
	0x04, 0x04, 0x00, 0x01, 0x05, 0x02, 0x58, 0x00, 0x40, 0x02, 0x0c, 0xa0,
	0x02, 0x05, 0x10, 0x07, 0x40, 0x02, 0x0e, 0xa6, 0x02, 0x0a, 0x10, 0x07,
	0x70, 0x02, 0x04, 0x00, 0x60, 0x02, 0x01, 0x00, 0x05, 0x0c, 0x46, 0x52,
	0x84, 0x08, 0x00, 0x00, 0x70, 0x02, 0x24, 0x00, 0x60, 0x02, 0x01, 0x00,
	0x05, 0x0e, 0x46, 0x52, 0x84, 0x08, 0x00, 0x00, 0x72, 0x00, 0x02, 0x80,
	0x50, 0x0d, 0x04, 0x00, 0x05, 0x00, 0x05, 0x1d, 0x05, 0x00, 0x05, 0x00,
	0x22, 0x00, 0x05, 0x01, 0x00, 0xc0, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00,
	0x90, 0x00, 0x00, 0x00, 0x69, 0x00, 0x10, 0x60, 0x02, 0x0c, 0x20, 0x00,
	0x69, 0x00, 0x12, 0x66, 0x02, 0x0e, 0x20, 0x00, 0x40, 0x02, 0x14, 0xa0,
	0x32, 0x10, 0x10, 0x08, 0x40, 0x02, 0x16, 0xa6, 0x32, 0x12, 0x10, 0x08,
	0x31, 0xa0, 0x04, 0x00, 0x00, 0x00, 0x14, 0x18, 0x14, 0x14, 0x00, 0xcc,
	0x00, 0x00, 0x16, 0x00, 0x31, 0x91, 0x24, 0x00, 0x00, 0x00, 0x14, 0x1a,
	0x14, 0x16, 0x00, 0xcc, 0x00, 0x00, 0x16, 0x00, 0x40, 0x00, 0x10, 0xa0,
	0x4a, 0x10, 0x10, 0x08, 0x40, 0x00, 0x12, 0xa6, 0x4a, 0x12, 0x10, 0x08,
	0x41, 0x20, 0x18, 0x20, 0x00, 0x18, 0x00, 0x18, 0x41, 0x21, 0x1a, 0x26,
	0x00, 0x1a, 0x00, 0x1a, 0x31, 0xa2, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x14, 0x10, 0x02, 0xcc, 0x14, 0x18, 0x96, 0x00, 0x31, 0x93, 0x24, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x14, 0x12, 0x02, 0xcc, 0x14, 0x1a, 0x96, 0x00,
	0x25, 0x00, 0x05, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x61, 0x00, 0x7f, 0x64, 0x00, 0x03, 0x10, 0x00,
	0x31, 0x44, 0x03, 0x80, 0x00, 0x00, 0x0c, 0x1c, 0x0c, 0x03, 0x00, 0xa0,
	0x00, 0x00, 0x78, 0x02, 0x61, 0x24, 0x03, 0x80, 0x20, 0x02, 0x01, 0x00,
	0x05, 0x1c, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x00, 0x04, 0x80,
	0xa0, 0x4a, 0x01, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x31, 0x01, 0x03, 0x80, 0x04, 0x00, 0x00, 0x00, 0x0c, 0x7f, 0x20, 0x70,
	0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
unsigned int tgllp_kernel_square_length = sizeof(tgllp_kernel_square_bin);

/**
 * tgllp_create_indirect_data:
 * @addr_bo_buffer_batch: pointer to batch buffer
 * @addr_input: input buffer gpu offset
 * @addr_output: output buffer gpu offset
 *
 * Prepares indirect data for compute pipeline.
 */
void tgllp_create_indirect_data(uint32_t *addr_bo_buffer_batch,
				uint64_t addr_input, uint64_t addr_output)
{
	int b = 0;

	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000200;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = addr_input & 0xffffffff;
	addr_bo_buffer_batch[b++] = addr_input >> 32;
	addr_bo_buffer_batch[b++] = addr_output & 0xffffffff;
	addr_bo_buffer_batch[b++] = addr_output >> 32;
	addr_bo_buffer_batch[b++] = 0x00000400;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000200;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00010000;
	addr_bo_buffer_batch[b++] = 0x00030002;
	addr_bo_buffer_batch[b++] = 0x00050004;
	addr_bo_buffer_batch[b++] = 0x00070006;
	addr_bo_buffer_batch[b++] = 0x00090008;
	addr_bo_buffer_batch[b++] = 0x000B000A;
	addr_bo_buffer_batch[b++] = 0x000D000C;
	addr_bo_buffer_batch[b++] = 0x000F000E;
	addr_bo_buffer_batch[b++] = 0x00110010;
	addr_bo_buffer_batch[b++] = 0x00130012;
	addr_bo_buffer_batch[b++] = 0x00150014;
	addr_bo_buffer_batch[b++] = 0x00170016;
	addr_bo_buffer_batch[b++] = 0x00190018;
	addr_bo_buffer_batch[b++] = 0x001B001A;
	addr_bo_buffer_batch[b++] = 0x001D001C;
	addr_bo_buffer_batch[b++] = 0x001F001E;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00210020;
	addr_bo_buffer_batch[b++] = 0x00230022;
	addr_bo_buffer_batch[b++] = 0x00250024;
	addr_bo_buffer_batch[b++] = 0x00270026;
	addr_bo_buffer_batch[b++] = 0x00290028;
	addr_bo_buffer_batch[b++] = 0x002B002A;
	addr_bo_buffer_batch[b++] = 0x002D002C;
	addr_bo_buffer_batch[b++] = 0x002F002E;
	addr_bo_buffer_batch[b++] = 0x00310030;
	addr_bo_buffer_batch[b++] = 0x00330032;
	addr_bo_buffer_batch[b++] = 0x00350034;
	addr_bo_buffer_batch[b++] = 0x00370036;
	addr_bo_buffer_batch[b++] = 0x00390038;
	addr_bo_buffer_batch[b++] = 0x003B003A;
	addr_bo_buffer_batch[b++] = 0x003D003C;
	addr_bo_buffer_batch[b++] = 0x003F003E;
}

/**
 * tgllp_create_surface_state:
 * @addr_bo_buffer_batch: pointer to batch buffer
 * @addr_input: input buffer gpu offset
 * @addr_output: output buffer gpu offset
 *
 * Prepares surface state for compute pipeline.
 */
void tgllp_create_surface_state(uint32_t *addr_bo_buffer_batch,
				uint64_t addr_input, uint64_t addr_output)
{
	int b = 0;

	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x87FD4000;
	addr_bo_buffer_batch[b++] = 0x04000000;
	addr_bo_buffer_batch[b++] = 0x001F007F;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00004000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = addr_input & 0xffffffff;
	addr_bo_buffer_batch[b++] = addr_input >> 32;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x87FD4000;
	addr_bo_buffer_batch[b++] = 0x04000000;
	addr_bo_buffer_batch[b++] = 0x001F007F;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00004000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = addr_output & 0xffffffff;
	addr_bo_buffer_batch[b++] = addr_output >> 32;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000040;
	addr_bo_buffer_batch[b++] = 0x00000080;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
}

/**
 * tgllp_create_dynamic_state:
 * @addr_bo_buffer_batch: pointer to batch buffer
 * @offset_kernel: gpu offset of the shader
 *
 * Prepares dynamic state for compute pipeline.
 */
void tgllp_create_dynamic_state(uint32_t *addr_bo_buffer_batch,
				uint64_t offset_kernel)
{
	int b = 0;

	addr_bo_buffer_batch[b++] = offset_kernel;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00180000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x000000C0;
	addr_bo_buffer_batch[b++] = 0x00060000;
	addr_bo_buffer_batch[b++] = 0x00000010;
	addr_bo_buffer_batch[b++] = 0x00000003;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
}

/**
 * tgllp_create_batch_compute:
 * @addr_bo_buffer_batch: pointer to batch buffer
 * @addr_surface_state_base: gpu offset of surface state data
 * @addr_dynamic_state_base: gpu offset of dynamic state data
 * @addr_indirect_object_base: gpu offset of indirect object data
 * @offset_indirect_data_start: gpu offset of indirect data start
 *
 * Prepares compute pipeline.
 */
void tgllp_create_batch_compute(uint32_t *addr_bo_buffer_batch,
				uint64_t addr_surface_state_base,
				uint64_t addr_dynamic_state_base,
				uint64_t addr_indirect_object_base,
				uint64_t offset_indirect_data_start)
{
	int b = 0;

	addr_bo_buffer_batch[b++] = MI_LOAD_REGISTER_IMM;
	addr_bo_buffer_batch[b++] = 0x00002580;
	addr_bo_buffer_batch[b++] = 0x00060002;
	addr_bo_buffer_batch[b++] = PIPELINE_SELECT;
	addr_bo_buffer_batch[b++] = MI_LOAD_REGISTER_IMM;
	addr_bo_buffer_batch[b++] = 0x00007034;
	addr_bo_buffer_batch[b++] = 0x60000321;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00100000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = MI_LOAD_REGISTER_IMM;
	addr_bo_buffer_batch[b++] = 0x0000E404;
	addr_bo_buffer_batch[b++] = 0x00000100;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00101021;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = MEDIA_VFE_STATE;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00A70100;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x07820000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00100420;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = STATE_BASE_ADDRESS;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00040000;
	addr_bo_buffer_batch[b++] = (addr_surface_state_base & 0xffffffff) | 0x1;
	addr_bo_buffer_batch[b++] = addr_surface_state_base >> 32;
	addr_bo_buffer_batch[b++] = (addr_dynamic_state_base & 0xffffffff) | 0x1;
	addr_bo_buffer_batch[b++] = addr_dynamic_state_base >> 32;
	addr_bo_buffer_batch[b++] = (addr_indirect_object_base & 0xffffffff) | 0x1;
	addr_bo_buffer_batch[b++] = (addr_indirect_object_base >> 32) | 0xffff0000;
	addr_bo_buffer_batch[b++] = (addr_indirect_object_base & 0xffffffff) | 0x41;
	addr_bo_buffer_batch[b++] = addr_indirect_object_base >> 32;
	addr_bo_buffer_batch[b++] = 0xFFFFF001;
	addr_bo_buffer_batch[b++] = 0x00010001;
	addr_bo_buffer_batch[b++] = 0xFFFFF001;
	addr_bo_buffer_batch[b++] = 0xFFFFF001;
	addr_bo_buffer_batch[b++] = (addr_surface_state_base & 0xffffffff) | 0x1;
	addr_bo_buffer_batch[b++] = addr_surface_state_base >> 32;
	addr_bo_buffer_batch[b++] = 0x003BF000;
	addr_bo_buffer_batch[b++] = 0x00000041;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00100000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00100000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = MEDIA_STATE_FLUSH;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = MEDIA_INTERFACE_DESCRIPTOR_LOAD;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000020;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = GPGPU_WALKER;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000c80;
	addr_bo_buffer_batch[b++] = offset_indirect_data_start;
	addr_bo_buffer_batch[b++] = 0x8000000f;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000002;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000001;
	addr_bo_buffer_batch[b++] = 0xffffffff;
	addr_bo_buffer_batch[b++] = 0xffffffff;
	addr_bo_buffer_batch[b++] = MEDIA_STATE_FLUSH;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00100000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = PIPE_CONTROL;
	addr_bo_buffer_batch[b++] = 0x00100120;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = 0x00000000;
	addr_bo_buffer_batch[b++] = MI_BATCH_BUFFER_END;
}
