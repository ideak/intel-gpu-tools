/*
 * Copyright 2021 Red Hat Inc.
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
 */

#ifndef __NVHW_CL906F_H__
#define __NVHW_CL906F_H__

/* dma method formats */
#define NV906F_DMA_METHOD_ADDRESS_OLD                              12:2
#define NV906F_DMA_METHOD_ADDRESS                                  11:0
#define NV906F_DMA_SUBDEVICE_MASK                                  15:4
#define NV906F_DMA_METHOD_SUBCHANNEL                               15:13
#define NV906F_DMA_TERT_OP                                         17:16
#define NV906F_DMA_TERT_OP_GRP0_INC_METHOD                         (0x00000000)
#define NV906F_DMA_TERT_OP_GRP0_SET_SUB_DEV_MASK                   (0x00000001)
#define NV906F_DMA_TERT_OP_GRP0_STORE_SUB_DEV_MASK                 (0x00000002)
#define NV906F_DMA_TERT_OP_GRP0_USE_SUB_DEV_MASK                   (0x00000003)
#define NV906F_DMA_TERT_OP_GRP2_NON_INC_METHOD                     (0x00000000)
#define NV906F_DMA_METHOD_COUNT_OLD                                28:18
#define NV906F_DMA_METHOD_COUNT                                    28:16
#define NV906F_DMA_IMMD_DATA                                       28:16
#define NV906F_DMA_SEC_OP                                          31:29
#define NV906F_DMA_SEC_OP_GRP0_USE_TERT                            (0x00000000)
#define NV906F_DMA_SEC_OP_INC_METHOD                               (0x00000001)
#define NV906F_DMA_SEC_OP_GRP2_USE_TERT                            (0x00000002)
#define NV906F_DMA_SEC_OP_NON_INC_METHOD                           (0x00000003)
#define NV906F_DMA_SEC_OP_IMMD_DATA_METHOD                         (0x00000004)
#define NV906F_DMA_SEC_OP_ONE_INC                                  (0x00000005)
#define NV906F_DMA_SEC_OP_RESERVED6                                (0x00000006)
#define NV906F_DMA_SEC_OP_END_PB_SEGMENT                           (0x00000007)
/* dma incrementing method format */
#define NV906F_DMA_INCR_ADDRESS                                    11:0
#define NV906F_DMA_INCR_SUBCHANNEL                                 15:13
#define NV906F_DMA_INCR_COUNT                                      28:16
#define NV906F_DMA_INCR_OPCODE                                     31:29
#define NV906F_DMA_INCR_OPCODE_VALUE                               (0x00000001)
#define NV906F_DMA_INCR_DATA                                       31:0
/* dma non-incrementing method format */
#define NV906F_DMA_NONINCR_ADDRESS                                 11:0
#define NV906F_DMA_NONINCR_SUBCHANNEL                              15:13
#define NV906F_DMA_NONINCR_COUNT                                   28:16
#define NV906F_DMA_NONINCR_OPCODE                                  31:29
#define NV906F_DMA_NONINCR_OPCODE_VALUE                            (0x00000003)
#define NV906F_DMA_NONINCR_DATA                                    31:0
/* dma increment-once method format */
#define NV906F_DMA_ONEINCR_ADDRESS                                 11:0
#define NV906F_DMA_ONEINCR_SUBCHANNEL                              15:13
#define NV906F_DMA_ONEINCR_COUNT                                   28:16
#define NV906F_DMA_ONEINCR_OPCODE                                  31:29
#define NV906F_DMA_ONEINCR_OPCODE_VALUE                            (0x00000005)
#define NV906F_DMA_ONEINCR_DATA                                    31:0
/* dma no-operation format */
#define NV906F_DMA_NOP                                             (0x00000000)
/* dma immediate-data format */
#define NV906F_DMA_IMMD_ADDRESS                                    11:0
#define NV906F_DMA_IMMD_SUBCHANNEL                                 15:13
#define NV906F_DMA_IMMD_DATA                                       28:16
#define NV906F_DMA_IMMD_OPCODE                                     31:29
#define NV906F_DMA_IMMD_OPCODE_VALUE                               (0x00000004)
/* dma set sub-device mask format */
#define NV906F_DMA_SET_SUBDEVICE_MASK_VALUE                        15:4
#define NV906F_DMA_SET_SUBDEVICE_MASK_OPCODE                       31:16
#define NV906F_DMA_SET_SUBDEVICE_MASK_OPCODE_VALUE                 (0x00000001)
/* dma store sub-device mask format */
#define NV906F_DMA_STORE_SUBDEVICE_MASK_VALUE                      15:4
#define NV906F_DMA_STORE_SUBDEVICE_MASK_OPCODE                     31:16
#define NV906F_DMA_STORE_SUBDEVICE_MASK_OPCODE_VALUE               (0x00000002)
/* dma use sub-device mask format */
#define NV906F_DMA_USE_SUBDEVICE_MASK_OPCODE                       31:16
#define NV906F_DMA_USE_SUBDEVICE_MASK_OPCODE_VALUE                 (0x00000003)
/* dma end-segment format */
#define NV906F_DMA_ENDSEG_OPCODE                                   31:29
#define NV906F_DMA_ENDSEG_OPCODE_VALUE                             (0x00000007)
/* dma legacy incrementing/non-incrementing formats */
#define NV906F_DMA_ADDRESS                                         12:2
#define NV906F_DMA_SUBCH                                           15:13
#define NV906F_DMA_OPCODE3                                         17:16
#define NV906F_DMA_OPCODE3_NONE                                    (0x00000000)
#define NV906F_DMA_COUNT                                           28:18
#define NV906F_DMA_OPCODE                                          31:29
#define NV906F_DMA_OPCODE_METHOD                                   (0x00000000)
#define NV906F_DMA_OPCODE_NONINC_METHOD                            (0x00000002)
#define NV906F_DMA_DATA                                            31:0

#endif /* __NVHW_CL906F_H__ */
