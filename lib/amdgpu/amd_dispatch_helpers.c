/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *  *
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

#include "amd_dispatch_helpers.h"
#include <amdgpu_drm.h>
#include "amd_PM4.h"
#include "amd_ip_blocks.h"
#include "igt.h"

 int amdgpu_dispatch_init(uint32_t ip_type, struct amdgpu_cmd_base * base, uint32_t version)
{
	int i = base->cdw;

	/* Write context control and load shadowing register if necessary */
	if (ip_type == AMDGPU_HW_IP_GFX) {
		base->emit(base, PACKET3(PKT3_CONTEXT_CONTROL, 1));
		base->emit(base, 0x80000000);
		base->emit(base, 0x80000000);
	}

	/* Issue commands to set default compute state. */
	/* clear mmCOMPUTE_START_Z - mmCOMPUTE_START_X */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 3));
	base->emit(base, 0x204);
	base->emit(base, 0);
	base->emit(base, 0);
	base->emit(base, 0);

	/* clear mmCOMPUTE_TMPRING_SIZE */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base->emit(base, 0x218);
	base->emit(base, 0);
	if (version == 10) {
		/* mmCOMPUTE_SHADER_CHKSUM */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x22a);
		base->emit(base, 0);
		/* mmCOMPUTE_REQ_CTRL */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 6));
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		/* mmCP_COHER_START_DELAY */
		base->emit(base, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
		base->emit(base, 0x7b);
		base->emit(base, 0x20);
	}

	return base->cdw - i;
}

int amdgpu_dispatch_write_cumask(struct amdgpu_cmd_base * base, uint32_t version)
 {
 	int offset_prev = base->cdw;
 	if (version == 9) {
 	/*  Issue commands to set cu mask used in current dispatch */
 	/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE1 - mmCOMPUTE_STATIC_THREAD_MGMT_SE0 */
 		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
 		base->emit(base, 0x216);
 		base->emit(base, 0xffffffff);
 		base->emit(base, 0xffffffff);
 	} else if(version == 10) {
		/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE1 - mmCOMPUTE_STATIC_THREAD_MGMT_SE0 */
 		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG_INDEX, 2));
 		base->emit(base, 0x30000216);
 		base->emit(base, 0xffffffff);
 		base->emit(base, 0xffffffff);
		/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE3 - mmCOMPUTE_STATIC_THREAD_MGMT_SE2 */
 		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG_INDEX, 2));
 		base->emit(base, 0x30000219);
 		base->emit(base, 0xffffffff);
 		base->emit(base, 0xffffffff);
	}


 	/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE3 - mmCOMPUTE_STATIC_THREAD_MGMT_SE2 */
 	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
 	base->emit(base, 0x219);
 	base->emit(base, 0xffffffff);
 	base->emit(base, 0xffffffff);

 	return base->cdw - offset_prev;
 }


int amdgpu_dispatch_write2hw(struct amdgpu_cmd_base * base, uint64_t shader_addr, uint32_t version)
{
	static const uint32_t bufferclear_cs_shader_registers_gfx9[][2] = {
		{0x2e12, 0x000C0041},	//{ mmCOMPUTE_PGM_RSRC1,	0x000C0041 },
		{0x2e13, 0x00000090},	//{ mmCOMPUTE_PGM_RSRC2,	0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X,	0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y,	0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z,	0x00000001 }
	};

	static const uint32_t bufferclear_cs_shader_registers_num_gfx9 = ARRAY_SIZE(bufferclear_cs_shader_registers_gfx9);
	int offset_prev = base->cdw;
	int j;

	/* Writes shader state to HW */
	/* set mmCOMPUTE_PGM_HI - mmCOMPUTE_PGM_LO */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
	base->emit(base, 0x20c);
	base->emit(base, shader_addr >> 8);
	base->emit(base, shader_addr >> 40);
	/* write sh regs */
	for (j = 0; j < bufferclear_cs_shader_registers_num_gfx9; j++) {
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		/* - Gfx9ShRegBase */
		base->emit(base,bufferclear_cs_shader_registers_gfx9[j][0] - 0x2c00);
		base->emit(base,bufferclear_cs_shader_registers_gfx9[j][1]);
	}
	if (version == 10) {
		/* mmCOMPUTE_PGM_RSRC3 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base,0x228);
		base->emit(base, 0 );
	}
	return base->cdw - offset_prev;
}
