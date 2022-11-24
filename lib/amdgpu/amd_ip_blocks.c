/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */
#include "amd_memory.h"
#include "amd_ip_blocks.h"
#include "amd_PM4.h"
#include "amd_sdma.h"
#include <amdgpu.h>


#include <amdgpu_drm.h>
#include "amdgpu_asic_addr.h"
#include "amd_family.h"
#include "amd_gfx_v8_0.h"

/*
 * SDMA functions:
 * - write_linear
 * - const_fill
 * - copy_linear
 */
static int
sdma_ring_write_linear(const struct amdgpu_ip_funcs *func,
		       const struct amdgpu_ring_context *ring_context,
		       uint32_t *pm4_dw)
{
	uint32_t i, j;

	i = 0;
	j = 0;
	if (ring_context->secure == false) {
		if (func->family_id == AMDGPU_FAMILY_SI)
			ring_context->pm4[i++] = SDMA_PACKET_SI(SDMA_OPCODE_WRITE, 0, 0, 0,
						 ring_context->write_length);
		else
			ring_context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_WRITE,
						 SDMA_WRITE_SUB_OPCODE_LINEAR,
						 ring_context->secure ? SDMA_ATOMIC_TMZ(1) : 0);

		ring_context->pm4[i++] = 0xfffffffc & ring_context->bo_mc;
		ring_context->pm4[i++] = (0xffffffff00000000 & ring_context->bo_mc) >> 32;
		if (func->family_id >= AMDGPU_FAMILY_AI)
			ring_context->pm4[i++] = ring_context->write_length - 1;
		else
			ring_context->pm4[i++] = ring_context->write_length;

		while(j++ < ring_context->write_length)
			ring_context->pm4[i++] = func->deadbeaf;
	} else {
		memset(ring_context->pm4, 0, ring_context->pm4_size * sizeof(uint32_t));

		/* atomic opcode for 32b w/ RTN and ATOMIC_SWAPCMP_RTN
		 * loop, 1-loop_until_compare_satisfied.
		 * single_pass_atomic, 0-lru
		 */
		ring_context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_ATOMIC,
					       0,
					       SDMA_ATOMIC_LOOP(1) |
					       SDMA_ATOMIC_TMZ(1) |
					       SDMA_ATOMIC_OPCODE(TC_OP_ATOMIC_CMPSWAP_RTN_32));
		ring_context->pm4[i++] = 0xfffffffc & ring_context->bo_mc;
		ring_context->pm4[i++] = (0xffffffff00000000 & ring_context->bo_mc) >> 32;
		ring_context->pm4[i++] = 0x12345678;
		ring_context->pm4[i++] = 0x0;
		ring_context->pm4[i++] = func->deadbeaf;
		ring_context->pm4[i++] = 0x0;
		ring_context->pm4[i++] = 0x100;
	}

 	*pm4_dw = i;

	return 0;
}

static int
sdma_ring_const_fill(const struct amdgpu_ip_funcs *func,
		     const struct amdgpu_ring_context *context,
		     uint32_t *pm4_dw)
{
	uint32_t i;

	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		context->pm4[i++] = SDMA_PACKET_SI(SDMA_OPCODE_CONSTANT_FILL_SI,
						   0, 0, 0, context->write_length / 4);
		context->pm4[i++] = 0xfffffffc & context->bo_mc;
		context->pm4[i++] = 0xdeadbeaf;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc) >> 16;
	} else {
		context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_CONSTANT_FILL, 0,
						SDMA_CONSTANT_FILL_EXTRA_SIZE(2));
		context->pm4[i++] = 0xffffffff & context->bo_mc;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc) >> 32;
		context->pm4[i++] = func->deadbeaf;

		if (func->family_id >= AMDGPU_FAMILY_AI)
			context->pm4[i++] = context->write_length - 1;
		else
			context->pm4[i++] = context->write_length;
	}
	*pm4_dw = i;

	return 0;
}

static int
sdma_ring_copy_linear(const struct amdgpu_ip_funcs *func,
		      const struct amdgpu_ring_context *context,
		      uint32_t *pm4_dw)
{
	uint32_t i;

	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		context->pm4[i++] = SDMA_PACKET_SI(SDMA_OPCODE_COPY_SI,
					  0, 0, 0,
					  context->write_length);
		context->pm4[i++] = 0xffffffff & context->bo_mc;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc) >> 32;
		context->pm4[i++] = 0xffffffff & context->bo_mc2;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc2) >> 32;
	} else {
		context->pm4[i++] = SDMA_PACKET(SDMA_OPCODE_COPY,
				       SDMA_COPY_SUB_OPCODE_LINEAR,
				       0);
		if (func->family_id >= AMDGPU_FAMILY_AI)
			context->pm4[i++] = context->write_length - 1;
		else
			context->pm4[i++] = context->write_length;
		context->pm4[i++] = 0;
		context->pm4[i++] = 0xffffffff & context->bo_mc;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc) >> 32;
		context->pm4[i++] = 0xffffffff & context->bo_mc2;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc2) >> 32;
	}

 	*pm4_dw = i;

	return 0;
}

/*
 * GFX and COMPUTE functions:
 * - write_linear
 * - const_fill
 * - copy_linear
 */


static int
gfx_ring_write_linear(const struct amdgpu_ip_funcs *func,
		      const struct amdgpu_ring_context *ring_context,
		      uint32_t *pm4_dw)
 {
 	uint32_t i, j;

 	i = 0;
 	j = 0;

 	if (ring_context->secure == false) {
 		ring_context->pm4[i++] = PACKET3(PACKET3_WRITE_DATA, 2 +  ring_context->write_length);
 		ring_context->pm4[i++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
 		ring_context->pm4[i++] = 0xfffffffc & ring_context->bo_mc;
 		ring_context->pm4[i++] = (0xffffffff00000000 & ring_context->bo_mc) >> 32;
 		while(j++ < ring_context->write_length)
 			ring_context->pm4[i++] = func->deadbeaf;
 	} else {
		memset(ring_context->pm4, 0, ring_context->pm4_size * sizeof(uint32_t));
		ring_context->pm4[i++] = PACKET3(PACKET3_ATOMIC_MEM, 7);

		/* atomic opcode for 32b w/ RTN and ATOMIC_SWAPCMP_RTN
		 * command, 1-loop_until_compare_satisfied.
		 * single_pass_atomic, 0-lru
		 * engine_sel, 0-micro_engine
		 */
		ring_context->pm4[i++] = (TC_OP_ATOMIC_CMPSWAP_RTN_32 |
					ATOMIC_MEM_COMMAND(1) |
					ATOMIC_MEM_CACHEPOLICAY(0) |
					ATOMIC_MEM_ENGINESEL(0));
		ring_context->pm4[i++] = 0xfffffffc & ring_context->bo_mc;
		ring_context->pm4[i++] = (0xffffffff00000000 & ring_context->bo_mc) >> 32;
		ring_context->pm4[i++] = 0x12345678;
		ring_context->pm4[i++] = 0x0;
		ring_context->pm4[i++] = 0xdeadbeaf;
		ring_context->pm4[i++] = 0x0;
		ring_context->pm4[i++] = 0x100;
 	}

 	*pm4_dw = i;

 	return 0;
 }

 static int
 gfx_ring_const_fill(const struct amdgpu_ip_funcs *func,
		     const struct amdgpu_ring_context *ring_context,
		     uint32_t *pm4_dw)
 {
 	uint32_t i;

 	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		ring_context->pm4[i++] = PACKET3(PACKET3_DMA_DATA_SI, 4);
		ring_context->pm4[i++] = func->deadbeaf;
		ring_context->pm4[i++] = PACKET3_DMA_DATA_SI_ENGINE(0) |
					 PACKET3_DMA_DATA_SI_DST_SEL(0) |
					 PACKET3_DMA_DATA_SI_SRC_SEL(2) |
					 PACKET3_DMA_DATA_SI_CP_SYNC;
		ring_context->pm4[i++] = 0xffffffff & ring_context->bo_mc;
		ring_context->pm4[i++] = (0xffffffff00000000 & ring_context->bo_mc) >> 32;
		ring_context->pm4[i++] = ring_context->write_length;
	} else {
		ring_context->pm4[i++] = PACKET3(PACKET3_DMA_DATA, 5);
		ring_context->pm4[i++] = PACKET3_DMA_DATA_ENGINE(0) |
					 PACKET3_DMA_DATA_DST_SEL(0) |
					 PACKET3_DMA_DATA_SRC_SEL(2) |
					 PACKET3_DMA_DATA_CP_SYNC;
		ring_context->pm4[i++] = func->deadbeaf;
		ring_context->pm4[i++] = 0;
		ring_context->pm4[i++] = 0xfffffffc & ring_context->bo_mc;
		ring_context->pm4[i++] = (0xffffffff00000000 & ring_context->bo_mc) >> 32;
		ring_context->pm4[i++] = ring_context->write_length;
	}
 	*pm4_dw = i;

 	return 0;
 }

static int
gfx_ring_copy_linear(const struct amdgpu_ip_funcs *func,
		     const struct amdgpu_ring_context *context,
		     uint32_t *pm4_dw)
{
 	uint32_t i;

 	i = 0;
	if (func->family_id == AMDGPU_FAMILY_SI) {
		context->pm4[i++] = PACKET3(PACKET3_DMA_DATA_SI, 4);
		context->pm4[i++] = 0xfffffffc & context->bo_mc;
		context->pm4[i++] = PACKET3_DMA_DATA_SI_ENGINE(0) |
			   PACKET3_DMA_DATA_SI_DST_SEL(0) |
			   PACKET3_DMA_DATA_SI_SRC_SEL(0) |
			   PACKET3_DMA_DATA_SI_CP_SYNC |
			   (0xffff00000000 & context->bo_mc) >> 32;
		context->pm4[i++] = 0xfffffffc & context->bo_mc2;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc2) >> 32;
		context->pm4[i++] = context->write_length;
	} else {
		context->pm4[i++] = PACKET3(PACKET3_DMA_DATA, 5);
		context->pm4[i++] = PACKET3_DMA_DATA_ENGINE(0) |
			   PACKET3_DMA_DATA_DST_SEL(0) |
			   PACKET3_DMA_DATA_SRC_SEL(0) |
			   PACKET3_DMA_DATA_CP_SYNC;
		context->pm4[i++] = 0xfffffffc & context->bo_mc;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc) >> 32;
		context->pm4[i++] = 0xfffffffc & context->bo_mc2;
		context->pm4[i++] = (0xffffffff00000000 & context->bo_mc2) >> 32;
		context->pm4[i++] = context->write_length;
	}

 	*pm4_dw = i;

	return 0;
}

/* we may cobine these two functions later */
static int
x_compare(const struct amdgpu_ip_funcs *func,
	  const struct amdgpu_ring_context *ring_context, int div)
{
	int i = 0, ret = 0;

	int num_compare = ring_context->write_length/div;

	while(i < num_compare) {
		if (ring_context->bo_cpu[i++] != func->deadbeaf) {
			ret = -1;
			break;
		}
	}
	return ret;
}

static int
x_compare_pattern(const struct amdgpu_ip_funcs *func,
	  const struct amdgpu_ring_context *ring_context, int div)
{
	int i = 0, ret = 0;

	int num_compare = ring_context->write_length/div;

	while(i < num_compare) {
		if (ring_context->bo_cpu[i++] != func->pattern) {
			ret = -1;
			break;
		}
	}
	return ret;
}

static const struct amdgpu_ip_funcs gfx_v8_x_ip_funcs = {
	.family_id = FAMILY_VI,
	.align_mask = 0xff,
	.nop = 0x80000000,
	.deadbeaf = 0xdeadbeaf,
	.pattern = 0xaaaaaaaa,
	.write_linear = gfx_ring_write_linear,
	.const_fill = gfx_ring_const_fill,
	.copy_linear = gfx_ring_copy_linear,
	.compare = x_compare,
	.compare_pattern = x_compare_pattern,
	.get_reg_offset = gfx_v8_0_get_reg_offset,
};

static const struct amdgpu_ip_funcs sdma_v3_x_ip_funcs = {
	.family_id = FAMILY_VI,
	.align_mask = 0xff,
	.nop = 0x80000000,
	.deadbeaf = 0xdeadbeaf,
	.pattern = 0xaaaaaaaa,
	.write_linear = sdma_ring_write_linear,
	.const_fill = sdma_ring_const_fill,
	.copy_linear = sdma_ring_copy_linear,
	.compare = x_compare,
	.compare_pattern = x_compare_pattern,
	.get_reg_offset = gfx_v8_0_get_reg_offset,
};

const struct amdgpu_ip_block_version gfx_v8_x_ip_block = {
	.type = AMD_IP_GFX,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &gfx_v8_x_ip_funcs
};

const struct amdgpu_ip_block_version compute_v8_x_ip_block = {
	.type = AMD_IP_COMPUTE,
	.major = 8,
	.minor = 0,
	.rev = 0,
	.funcs = &gfx_v8_x_ip_funcs
};

const struct amdgpu_ip_block_version sdma_v3_x_ip_block = {
	.type = AMD_IP_DMA,
	.major = 3,
	.minor = 0,
	.rev = 0,
	.funcs = &sdma_v3_x_ip_funcs
};

struct chip_info {
	  const char *name;
	  enum radeon_family family;
	  enum chip_class chip_class;
	  amdgpu_device_handle dev;
};

/* we may improve later */
struct amdgpu_ip_blocks_device amdgpu_ips;
struct chip_info g_chip;

static int
amdgpu_device_ip_block_add(const struct amdgpu_ip_block_version *ip_block_version)
{
	if (amdgpu_ips.num_ip_blocks >= AMD_IP_MAX)
		return -1;

	amdgpu_ips.ip_blocks[amdgpu_ips.num_ip_blocks++] = ip_block_version;

	return 0;
}

const struct amdgpu_ip_block_version *
get_ip_block(amdgpu_device_handle device, enum amd_ip_block_type type)
{
	int i;

	if (g_chip.dev != device)
		return NULL;

	for(i = 0; i <  amdgpu_ips.num_ip_blocks; i++)
		if (amdgpu_ips.ip_blocks[i]->type == type)
			return amdgpu_ips.ip_blocks[i];
	return NULL;
}

static int
cmd_allocate_buf(struct amdgpu_cmd_base  *base, uint32_t size_dw)
{
	if (size_dw > base->max_dw) {
		if (base->buf) {
			free(base->buf);
			base->buf = NULL;
			base->max_dw = 0;
			base->cdw = 0;
		}
		base->buf = calloc(4, size_dw);
		if (!base->buf)
			return -1;
		base->max_dw = size_dw;
		base->cdw = 0;
	}
	return 0;
}

static int
cmd_attach_buf(struct amdgpu_cmd_base  *base, void *ptr, uint32_t size_bytes)
{
	if (base->buf && base->is_assigned_buf)
		return -1;

	if (base->buf) {
		free(base->buf);
		base->buf = NULL;
		base->max_dw = 0;
		base->cdw = 0;
	}
	assert(ptr != NULL);
	base->buf = (uint32_t *)ptr;
	base->max_dw = size_bytes>>2;
	base->cdw = 0;
	base->is_assigned_buf = true; /* allocated externally , no free */
	return 0;
}

static void
cmd_emit(struct amdgpu_cmd_base  *base, uint32_t value)
{
	assert(base->cdw <  base->max_dw  );
	base->buf[base->cdw++] = value;
}

static void
cmd_emit_aligned(struct amdgpu_cmd_base *base, uint32_t mask, uint32_t cmd)
{
	while(base->cdw & mask)
		base->emit(base, cmd);
}
static void
cmd_emit_buf(struct amdgpu_cmd_base  *base, const void *ptr, uint32_t offset_bytes, uint32_t size_bytes)
{
	uint32_t total_offset_dw = (offset_bytes + size_bytes) >> 2;
	uint32_t offset_dw = offset_bytes >> 2;
	/*TODO read the requirements to fix */
	assert(size_bytes % 4 == 0); /* no gaps */
	assert(offset_bytes % 4 == 0);
	assert(base->cdw + total_offset_dw <  base->max_dw);
	memcpy(base->buf + base->cdw + offset_dw , ptr, size_bytes);
	base->cdw += total_offset_dw;
}

static void
cmd_emit_repeat(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t number_of_times)
{
	while (number_of_times > 0) {
		assert(base->cdw <  base->max_dw);
		base->buf[base->cdw++] = value;
		number_of_times--;
	}
}

static void
cmd_emit_at_offset(struct amdgpu_cmd_base  *base, uint32_t value, uint32_t offset_dwords)
{
	assert(base->cdw + offset_dwords <  base->max_dw);
	base->buf[base->cdw + offset_dwords] = value;
}

struct amdgpu_cmd_base *
get_cmd_base(void)
{
	struct amdgpu_cmd_base *base = calloc(1 ,sizeof(*base));

	base->cdw = 0;
	base->max_dw = 0;
	base->buf = NULL;
	base->is_assigned_buf = false;

	base->allocate_buf = cmd_allocate_buf;
	base->attach_buf = cmd_attach_buf;
	base->emit = cmd_emit;
	base->emit_aligned= cmd_emit_aligned;
	base->emit_repeat = cmd_emit_repeat;
	base->emit_at_offset = cmd_emit_at_offset;
	base->emit_buf = cmd_emit_buf;

	return base;
}

void
free_cmd_base(struct amdgpu_cmd_base * base)
{
	if (base) {
		if (base->buf && base->is_assigned_buf == false)
			free(base->buf);
		free(base);
	}

}

/*
 * GFX: 8.x
 * COMPUTE: 8.x
 * SDMA 3.x
 *
 * GFX9:
 * COMPUTE: 9.x
 * SDMA 4.x
 *
 * GFX10.1:
 * COMPUTE: 10.1
 * SDMA 5.0
 *
 * GFX10.3:
 * COMPUTE: 10.3
 * SDMA 5.2
 *
 * copy function from mesa
 *  should be called once per test
 */
int setup_amdgpu_ip_blocks(uint32_t major, uint32_t minor, struct amdgpu_gpu_info *amdinfo,
			   amdgpu_device_handle device)
{
#define identify_chip2(asic, chipname)			\
   if (ASICREV_IS(amdinfo->chip_external_rev, asic)) {	\
      info->family = CHIP_##chipname;			\
      info->name = #chipname;				\
   }
#define identify_chip(chipname) identify_chip2(chipname, chipname)

	struct chip_info *info = &g_chip;

	switch (amdinfo->family_id) {
	case AMDGPU_FAMILY_SI:
		identify_chip(TAHITI);
		identify_chip(PITCAIRN);
		identify_chip2(CAPEVERDE, VERDE);
		identify_chip(OLAND);
		identify_chip(HAINAN);
		break;
	case FAMILY_CI:
		identify_chip(BONAIRE);//tested
		identify_chip(HAWAII);
		break;
	case FAMILY_KV:
		identify_chip2(SPECTRE, KAVERI);
		identify_chip2(SPOOKY, KAVERI);
		identify_chip2(KALINDI, KABINI);
		identify_chip2(GODAVARI, KABINI);
		break;
	case FAMILY_VI:
		identify_chip(ICELAND);
		identify_chip(TONGA);
		identify_chip(FIJI);
		identify_chip(POLARIS10);
		identify_chip(POLARIS11);//tested
		identify_chip(POLARIS12);
		identify_chip(VEGAM);
		break;
	case FAMILY_CZ:
		identify_chip(CARRIZO);
		identify_chip(STONEY);
		break;
	case FAMILY_AI:
		identify_chip(VEGA10);
		identify_chip(VEGA12);
		identify_chip(VEGA20);
		identify_chip(ARCTURUS);
		identify_chip(ALDEBARAN);
		break;
	case FAMILY_RV:
		identify_chip(RAVEN);
		identify_chip(RAVEN2);
		identify_chip(RENOIR);
		break;
	case FAMILY_NV:
		identify_chip(NAVI10); //tested
		identify_chip(NAVI12);
		identify_chip(NAVI14);
		identify_chip(SIENNA_CICHLID);
		identify_chip(NAVY_FLOUNDER);
		identify_chip(DIMGREY_CAVEFISH);
		identify_chip(BEIGE_GOBY);
		break;
	case FAMILY_VGH:
		identify_chip(VANGOGH);
		break;
	case FAMILY_YC:
		identify_chip(YELLOW_CARP);
		break;
	}
	if (!info->name) {
		igt_info("amdgpu: unknown (family_id, chip_external_rev): (%u, %u)\n",
			 amdinfo->family_id, amdinfo->chip_external_rev);
		return -1;
	}

	if (info->family >= CHIP_SIENNA_CICHLID)
		info->chip_class = GFX10_3;
	else if (info->family >= CHIP_NAVI10)
		info->chip_class = GFX10;
	else if (info->family >= CHIP_VEGA10)
		info->chip_class = GFX9;
	else if (info->family >= CHIP_TONGA)
		info->chip_class = GFX8;
	else if (info->family >= CHIP_BONAIRE)
		info->chip_class = GFX7;
	else if (info->family >= CHIP_TAHITI)
		info->chip_class = GFX6;
	else {
		igt_info("amdgpu: Unknown family.\n");
		return -1;
	}

	switch(info->chip_class) {
	case GFX6:
		break;
	case GFX7: /* tested */
	case GFX8: /* tested */
	case GFX9: /* tested */
	case GFX10:/* tested */
		amdgpu_device_ip_block_add(&gfx_v8_x_ip_block);
		amdgpu_device_ip_block_add(&compute_v8_x_ip_block);
		amdgpu_device_ip_block_add(&sdma_v3_x_ip_block);
		/* extra precaution if re-factor again */
		igt_assert_eq(gfx_v8_x_ip_block.major, 8);
		igt_assert_eq(compute_v8_x_ip_block.major, 8);
		igt_assert_eq(sdma_v3_x_ip_block.major, 3);

		igt_assert_eq(gfx_v8_x_ip_block.funcs->family_id, FAMILY_VI);
		igt_assert_eq(sdma_v3_x_ip_block.funcs->family_id, FAMILY_VI);
		break;
	case GFX10_3:
		break;
	default:
		igt_info("amdgpu: GFX or old.\n");
		return -1;
	 }
	info->dev = device;

	return 0;
}
