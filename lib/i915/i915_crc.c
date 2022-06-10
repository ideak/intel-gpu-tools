// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <stddef.h>
#include <stdint.h>
#include "drmtest.h"
#include "gem_create.h"
#include "gem_engine_topology.h"
#include "gem_mman.h"
#include "i830_reg.h"
#include "i915_drm.h"
#include "intel_reg.h"
#include "intel_chipset.h"
#include "ioctl_wrappers.h"
#include "intel_allocator.h"
#include "igt_crc.h"
#include "i915/i915_crc.h"

#define CS_GPR(x)                       (0x600 + 8 * (x))
#define GPR(x)                          CS_GPR(x)
#define R(x)                            (x)
#define USERDATA(offset, idx)	        ((offset) + (0x100 + (idx)) * 4)
#define OFFSET(obj_offset, current, start) \
	((obj_offset) + (current - start) * 4)

#define MI_PREDICATE_RESULT             0x3B8
#define WPARID                          0x21C
#define CS_MI_ADDRESS_OFFSET            0x3B4

#define LOAD_REGISTER_REG(__reg_src, __reg_dst) do { \
		*bb++ = MI_LOAD_REGISTER_REG | MI_CS_MMIO_DST | MI_CS_MMIO_SRC; \
		*bb++ = (__reg_src); \
		*bb++ = (__reg_dst); \
	} while (0)

#define LOAD_REGISTER_IMM32(__reg, __imm1) do { \
		*bb++ = MI_LOAD_REGISTER_IMM | MI_CS_MMIO_DST; \
		*bb++ = (__reg); \
		*bb++ = (__imm1); \
	} while (0)

#define LOAD_REGISTER_IMM64(__reg, __imm1, __imm2) do { \
		*bb++ = (MI_LOAD_REGISTER_IMM + 2) | MI_CS_MMIO_DST; \
		*bb++ = (__reg); \
		*bb++ = (__imm1); \
		*bb++ = (__reg) + 4; \
		*bb++ = (__imm2); \
	} while (0)

#define LOAD_REGISTER_MEM(__reg, __offset) do { \
		*bb++ = MI_LOAD_REGISTER_MEM | MI_CS_MMIO_DST | 2; \
		*bb++ = (__reg); \
		*bb++ = (__offset); \
		*bb++ = (__offset) >> 32; \
	} while (0)

#define LOAD_REGISTER_MEM_WPARID(__reg, __offset) do { \
		*bb++ = MI_LOAD_REGISTER_MEM | MI_CS_MMIO_DST | MI_WPARID_ENABLE_GEN12 | 2; \
		*bb++ = (__reg); \
		*bb++ = (__offset); \
		*bb++ = (__offset) >> 32; \
	} while (0)

#define STORE_REGISTER_MEM(__reg, __offset) do { \
		*bb++ = MI_STORE_REGISTER_MEM | MI_CS_MMIO_DST | 2; \
		*bb++ = (__reg); \
		*bb++ = (__offset); \
		*bb++ = (__offset) >> 32; \
	} while (0)

#define STORE_REGISTER_MEM_PREDICATED(__reg, __offset) do { \
		*bb++ = MI_STORE_REGISTER_MEM | MI_CS_MMIO_DST | \
			MI_STORE_PREDICATE_ENABLE_GEN12 | 2; \
		*bb++ = (__reg); \
		*bb++ = (__offset); \
		*bb++ = (__offset) >> 32; \
	} while (0)

#define COND_BBE(__value, __offset, __condition) do { \
		*bb++ = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | (__condition) | 2; \
		*bb++ = (__value); \
		*bb++ = (__offset); \
		*bb++ = (__offset) >> 32; \
	} while (0)

#define MATH_4_STORE(__r1, __r2, __op, __r3) do { \
		*bb++ = MI_MATH(4); \
		*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(__r1)); \
		*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(__r2)); \
		*bb++ = (__op); \
		*bb++ = MI_MATH_STORE(MI_MATH_REG(__r3), MI_MATH_REG_ACCU); \
	} while (0)

#define BBSIZE 4096

/* Aliasing for easier refactoring */
#define GPR_SIZE	GPR(0)
#define R_SIZE		R(0)

#define GPR_CRC		GPR(1)
#define R_CRC		R(1)

#define GPR_INDATA_IDX  GPR(2)
#define R_INDATA_IDX	R(2)

#define GPR_TABLE_IDX   GPR(3)
#define R_TABLE_IDX	R(3)

#define GPR_CURR_DW	GPR(4)
#define R_CURR_DW	R(4)

#define GPR_CONST_2	GPR(5)
#define R_CONST_2	R(5)

#define GPR_CONST_4	GPR(6)
#define R_CONST_4	R(6)

#define GPR_CONST_8	GPR(7)
#define R_CONST_8	R(7)

#define GPR_CONST_ff	GPR(8)
#define R_CONST_ff	R(8)

#define GPR_ffffffff    GPR(9)
#define R_ffffffff	R(9)

#define GPR_TMP_1	GPR(10)
#define R_TMP_1		R(10)

#define GPR_TMP_2	GPR(11)
#define R_TMP_2		R(11)

static void fill_batch(int i915, uint32_t bb_handle, uint64_t bb_offset,
		       uint64_t table_offset, uint64_t data_offset, uint32_t data_size)
{
	uint32_t *bb, *batch, *jmp;
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const int use_64b = gen >= 8;
	uint64_t offset;
	uint64_t crc = USERDATA(table_offset, 0);

	igt_assert(data_size % 4 == 0);

	batch = gem_mmap__device_coherent(i915, bb_handle, 0, BBSIZE,
					  PROT_READ | PROT_WRITE);
	memset(batch, 0, BBSIZE);

	bb = batch;

	LOAD_REGISTER_IMM64(GPR_SIZE, data_size, 0);
	LOAD_REGISTER_IMM64(GPR_CRC, ~0U, 0);		/* crc start - 0xffffffff */
	LOAD_REGISTER_IMM64(GPR_INDATA_IDX, 0, 0);	/* data_offset index (0) */
	LOAD_REGISTER_IMM64(GPR_CONST_2, 2, 0);		/* const value 2 */
	LOAD_REGISTER_IMM64(GPR_CONST_4, 4, 0);		/* const value 4 */
	LOAD_REGISTER_IMM64(GPR_CONST_8, 8, 0);		/* const value 8 */
	LOAD_REGISTER_IMM64(GPR_CONST_ff, 0xff, 0);	/* const value 0xff */
	LOAD_REGISTER_IMM64(GPR_ffffffff, ~0U, 0);	/* const value 0xffffffff */

	/* for indexed reads from memory */
	LOAD_REGISTER_IMM32(WPARID, 1);

	jmp = bb;

	*bb++ = MI_SET_PREDICATE;
	*bb++ = MI_ARB_CHECK;

	LOAD_REGISTER_REG(GPR_INDATA_IDX, CS_MI_ADDRESS_OFFSET);
	LOAD_REGISTER_MEM_WPARID(GPR_CURR_DW, data_offset);

	for (int byte = 0; byte < 4; byte++) {
		if (byte != 0)
			MATH_4_STORE(R_CURR_DW, R_CONST_8,
				     MI_MATH_SHR, R_CURR_DW); /* dw >> 8 */

		/* crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8); */
		MATH_4_STORE(R_CURR_DW, R_CONST_ff,
			     MI_MATH_AND, R_TMP_1); /* dw & 0xff */
		MATH_4_STORE(R_CRC, R_TMP_1,
			     MI_MATH_XOR, R_TMP_1); /* crc ^ tmp */
		MATH_4_STORE(R_TMP_1, R_CONST_ff,
			     MI_MATH_AND, R_TMP_1); /* tmp & 0xff */
		MATH_4_STORE(R_TMP_1, R_CONST_2,
			     MI_MATH_SHL, R_TABLE_IDX); /* tmp << 2 (crc idx) */

		LOAD_REGISTER_REG(GPR_TABLE_IDX, CS_MI_ADDRESS_OFFSET);
		LOAD_REGISTER_MEM_WPARID(GPR_TMP_1, table_offset);

		MATH_4_STORE(R_CRC, R_CONST_8,
			     MI_MATH_SHR, R_TMP_2); /* crc >> 8 (shift) */
		MATH_4_STORE(R_TMP_2, R_TMP_1,
			     MI_MATH_XOR, R_CRC); /* crc = tab[v] ^ shift */
	}

	/* increment data index */
	MATH_4_STORE(R_INDATA_IDX, R_CONST_4, MI_MATH_ADD, R_INDATA_IDX);

	/* loop until R_SIZE == 0, R_SIZE = R_SIZE - R_CONST_4 */

	*bb++ = MI_MATH(5);
	*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(R_SIZE));
	*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(R_CONST_4));
	*bb++ = MI_MATH_SUB;
	*bb++ = MI_MATH_STORE(MI_MATH_REG(R_SIZE), MI_MATH_REG_ACCU);
	*bb++ = MI_MATH_STORE(MI_MATH_REG(R_TMP_2), MI_MATH_REG_ZF);
	LOAD_REGISTER_REG(GPR_TMP_2, MI_PREDICATE_RESULT);

	*bb++ = MI_BATCH_BUFFER_START | BIT(15) | BIT(8) | use_64b;
	offset = OFFSET(bb_offset, jmp, batch);
	*bb++ = offset;
	*bb++ = offset >> 32;

	*bb++ = MI_SET_PREDICATE;

	MATH_4_STORE(R_CRC, R_ffffffff, MI_MATH_XOR, R_TMP_1);
	STORE_REGISTER_MEM(GPR_TMP_1, crc);

	*bb++ = MI_BATCH_BUFFER_END;

	gem_munmap(batch, BBSIZE);
}

/**
 * i915_crc32:
 * @i915: drm fd
 * @ahnd: allocator handle
 * @ctx: intel context
 * @e: engine on which crc32 calculation will be executed
 * @data_handle: bo which is subject of crc32 calculation
 * @data_size: length of bo data to calculate (must be multiple of 4)
 *
 * Function calculates crc32 for @data_handle with size @data_size.
 *
 * Returns: uint32_t crc32.
 *
 **/
uint32_t i915_crc32(int i915, uint64_t ahnd, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e,
		    uint32_t data_handle, uint32_t data_size)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj[3] = {};
	uint64_t bb_offset, table_offset, data_offset;
	uint32_t bb, table, crc, table_size = 4096;
	uint32_t *ptr;

	igt_assert(data_size % 4 == 0);

	table = gem_create_in_memory_regions(i915, table_size, REGION_LMEM(0));
	gem_write(i915, table, 0, igt_crc32_tab, sizeof(igt_crc32_tab));

	table_offset = get_offset(ahnd, table, table_size, 0);
	data_offset = get_offset(ahnd, data_handle, data_size, 0);

	obj[0].offset = table_offset;
	obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
	obj[0].handle = table;

	obj[1].offset = data_offset;
	obj[1].flags = EXEC_OBJECT_PINNED;
	obj[1].handle = data_handle;

	bb = gem_create_in_memory_regions(i915, BBSIZE, REGION_LMEM(0));
	bb_offset = get_offset(ahnd, bb, BBSIZE, 0);
	fill_batch(i915, bb, bb_offset, table_offset, data_offset, data_size);
	obj[2].offset = bb_offset;
	obj[2].flags = EXEC_OBJECT_PINNED;
	obj[2].handle = bb;
	execbuf.buffer_count = 3;
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.flags = e->flags;
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, table);

	ptr = gem_mmap__device_coherent(i915, table, 0, table_size, PROT_READ);
	crc = ptr[0x100];
	gem_munmap(ptr, table_size);
	gem_close(i915, table);
	gem_close(i915, bb);

	return crc;
}

/**
 * supports_i915_crc32:
 * @i915: drm fd
 *
 * Returns: flag if i915_crc32() is able to generate crc32 on gpu.
 *
 **/
bool supports_i915_crc32(int i915)
{
	uint16_t devid = intel_get_drm_devid(i915);

	return IS_DG2(devid);
}
