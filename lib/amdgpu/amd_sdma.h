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

#ifndef AMD_SDMA_H
#define AMD_SDMA_H

#define SDMA_PKT_HEADER_op_offset 0
#define SDMA_PKT_HEADER_op_mask   0x000000FF
#define SDMA_PKT_HEADER_op_shift  0
#define SDMA_PKT_HEADER_OP(x) (((x) & SDMA_PKT_HEADER_op_mask) << SDMA_PKT_HEADER_op_shift)
#define SDMA_OPCODE_CONSTANT_FILL  11
#       define SDMA_CONSTANT_FILL_EXTRA_SIZE(x)           ((x) << 14)
	/* 0 = byte fill
	 * 2 = DW fill
	 */

#define SDMA_OP_POLL_REGMEM  8

#define SDMA_PACKET(op, sub_op, e)	((((e) & 0xFFFF) << 16) |	\
					(((sub_op) & 0xFF) << 8) |	\
					(((op) & 0xFF) << 0))
#define	SDMA_OPCODE_WRITE				  2
#       define SDMA_WRITE_SUB_OPCODE_LINEAR               0
#       define SDMA_WRTIE_SUB_OPCODE_TILED                1

#define	SDMA_OPCODE_COPY				  1
#       define SDMA_COPY_SUB_OPCODE_LINEAR                0

#define SDMA_NOP  0x0



/* taken from basic_tests.c and amdgpu_stress.c */

#define SDMA_PACKET_SI(op, b, t, s, cnt)	((((op) & 0xF) << 28) | \
						(((b) & 0x1) << 26) |	\
						(((t) & 0x1) << 23) |	\
						(((s) & 0x1) << 22) |	\
						(((cnt) & 0xFFFFF) << 0))
#define SDMA_OPCODE_COPY_SI     3


#define	SDMA_OPCODE_ATOMIC				  10
#		define SDMA_ATOMIC_LOOP(x)               ((x) << 0)
        /* 0 - single_pass_atomic.
         * 1 - loop_until_compare_satisfied.
         */
#		define SDMA_ATOMIC_TMZ(x)                ((x) << 2)
		/* 0 - non-TMZ.
		 * 1 - TMZ.
	     */
#		define SDMA_ATOMIC_OPCODE(x)             ((x) << 9)
		/* TC_OP_ATOMIC_CMPSWAP_RTN_32 0x00000008
		 * same as Packet 3
		 */
#define SDMA_PACKET_SI(op, b, t, s, cnt)	((((op) & 0xF) << 28) |	\
						(((b) & 0x1) << 26) |		\
						(((t) & 0x1) << 23) |		\
						(((s) & 0x1) << 22) |		\
						(((cnt) & 0xFFFFF) << 0))
#define	SDMA_OPCODE_COPY_SI	3
#define SDMA_OPCODE_CONSTANT_FILL_SI	13
#define SDMA_NOP_SI  0xf
#define GFX_COMPUTE_NOP_SI 0x80000000
#define	PACKET3_DMA_DATA_SI	0x41
#              define PACKET3_DMA_DATA_SI_ENGINE(x)     ((x) << 27)
		/* 0 - ME
		 * 1 - PFP
		 */
#              define PACKET3_DMA_DATA_SI_DST_SEL(x)  ((x) << 20)
		/* 0 - DST_ADDR using DAS
		 * 1 - GDS
		 * 3 - DST_ADDR using L2
		 */
#              define PACKET3_DMA_DATA_SI_SRC_SEL(x)  ((x) << 29)
		/* 0 - SRC_ADDR using SAS
		 * 1 - GDS
		 * 2 - DATA
		 * 3 - SRC_ADDR using L2
		 */
#              define PACKET3_DMA_DATA_SI_CP_SYNC     (1 << 31)

#define SDMA_NOP  0x0

#endif
