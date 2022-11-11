/* SPDX-License-Identifier: MIT
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
#include "amd_registers.h"
#include "amd_gfx_v8_0.h"
#include "igt_core.h"

#define mmCOMPUTE_PGM_LO			0x2e0c
#define mmCOMPUTE_PGM_RSRC1			0x2e12
#define mmCOMPUTE_TMPRING_SIZE			0x2e18
#define mmCOMPUTE_USER_DATA_0			0x2e40
#define mmCOMPUTE_USER_DATA_1			0x2e41
#define mmCOMPUTE_RESOURCE_LIMITS		0x2e15
#define mmCOMPUTE_NUM_THREAD_X			0x2e07

#define	PACKET3_SET_SH_REG_START		0x00002c00

static const struct amd_reg registers[] = {
	{ COMPUTE_PGM_LO,		mmCOMPUTE_PGM_LO },
	{ COMPUTE_PGM_RSRC1,		mmCOMPUTE_PGM_RSRC1 },
	{ COMPUTE_TMPRING_SIZE,		mmCOMPUTE_TMPRING_SIZE },
	{ COMPUTE_USER_DATA_0,		mmCOMPUTE_USER_DATA_0 },
	{ COMPUTE_USER_DATA_1,		mmCOMPUTE_USER_DATA_1 },
	{ COMPUTE_RESOURCE_LIMITS,	mmCOMPUTE_RESOURCE_LIMITS },
	{ COMPUTE_NUM_THREAD_X,		mmCOMPUTE_NUM_THREAD_X },
};

int gfx_v8_0_get_reg_offset(enum general_reg reg_name)
{
	/* validate correctness of the offset */
	igt_assert_eq(reg_name, registers[reg_name].reg_name);
	return registers[reg_name].reg_offset - PACKET3_SET_SH_REG_START;
}
