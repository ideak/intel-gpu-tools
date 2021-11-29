/*
 * Copyright © 2021 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef IGT_MSM_H
#define IGT_MSM_H

#include "ioctl_wrappers.h"

#include "msm_drm.h"

/**
 * msm_device:
 * @fd: the drm device file descriptor
 * @gen: the device major generation (ie. 2 for a2xx, etc)
 *
 * Helper container for device and device related parameters used by tests.
 */
struct msm_device {
	int fd;
	unsigned gen;
};

struct msm_device *igt_msm_dev_open(void);
void igt_msm_dev_close(struct msm_device *dev);

/**
 * msm_bo:
 * @dev: the device the BO is allocated from
 * @handle: the BO's GEM handle
 * @size: the BO's size
 * @map: the BO's memory mapping (if mapped)
 * @iova: the BO's GPU address
 *
 * Helper wrapper for a GEM buffer object.
 */
struct msm_bo {
	struct msm_device *dev;
	int handle;
	uint32_t size;
	void *map;
	uint64_t iova;
};

struct msm_bo *igt_msm_bo_new(struct msm_device *dev, size_t size, uint32_t flags);
void igt_msm_bo_free(struct msm_bo *bo);
void *igt_msm_bo_map(struct msm_bo *bo);

/**
 * msm_pipe:
 * @dev: the device the pipe is allocated from
 * @pipe: the pipe id
 * @submitqueue_id: the submitqueue id
 *
 * Helper wrapper for a submitqueue for cmdstream submission.
 */
struct msm_pipe {
	struct msm_device *dev;
	uint32_t pipe;
	uint32_t submitqueue_id;
};

struct msm_pipe *igt_msm_pipe_open(struct msm_device *dev, uint32_t prio);
void igt_msm_pipe_close(struct msm_pipe *pipe);

/*
 * Helpers for cmdstream building:
 */

enum adreno_pm4_packet_type {
	CP_TYPE0_PKT = 0,
	CP_TYPE1_PKT = 0x40000000,
	CP_TYPE2_PKT = 0x80000000,
	CP_TYPE3_PKT = 0xc0000000,
	CP_TYPE4_PKT = 0x40000000,
	CP_TYPE7_PKT = 0x70000000,
};

enum adreno_pm4_type3_packets {
	CP_NOP = 16,
	CP_WAIT_MEM_GTE = 20,
	CP_WAIT_REG_MEM = 60,
	CP_MEM_WRITE = 61,
	CP_MEM_TO_MEM = 115,
};

static inline unsigned
pm4_odd_parity_bit(unsigned val)
{
	/* See: http://graphics.stanford.edu/~seander/bithacks.html#ParityParallel
	 * note that we want odd parity so 0x6996 is inverted.
	 */
	val ^= val >> 16;
	val ^= val >> 8;
	val ^= val >> 4;
	val &= 0xf;
	return (~0x6996 >> val) & 1;
}

static inline uint32_t
pm4_pkt0_hdr(uint16_t regindx, uint16_t cnt)
{
	return CP_TYPE0_PKT | ((cnt - 1) << 16) | (regindx & 0x7fff);
}

static inline uint32_t
pm4_pkt3_hdr(uint8_t opcode, uint16_t cnt)
{
	return CP_TYPE3_PKT | ((cnt - 1) << 16) | ((opcode & 0xff) << 8);
}

static inline uint32_t
pm4_pkt4_hdr(uint16_t regindx, uint16_t cnt)
{
	return CP_TYPE4_PKT | cnt | (pm4_odd_parity_bit(cnt) << 7) |
			((regindx & 0x3ffff) << 8) |
			((pm4_odd_parity_bit(regindx) << 27));
}

static inline uint32_t
pm4_pkt7_hdr(uint8_t opcode, uint16_t cnt)
{
	return CP_TYPE7_PKT | cnt | (pm4_odd_parity_bit(cnt) << 15) |
			((opcode & 0x7f) << 16) |
			((pm4_odd_parity_bit(opcode) << 23));
}

/**
 * msm_cmd:
 * @pipe: the submitqueue to submit cmdstream against
 * @cmdstream_bo: the backing cmdstream buffer object
 * @cur: pointer to current position in cmdstream
 *
 * Helper for building cmdstream and cmdstream submission
 */
struct msm_cmd {
	struct msm_pipe *pipe;
	struct msm_bo *cmdstream_bo;
	uint32_t *cur;
	uint32_t nr_bos;
	struct msm_bo *bos[8];
};

struct msm_cmd *igt_msm_cmd_new(struct msm_pipe *pipe, size_t size);
int igt_msm_cmd_submit(struct msm_cmd *cmd);
void igt_msm_cmd_free(struct msm_cmd *cmd);

static inline void
msm_cmd_emit(struct msm_cmd *cmd, uint32_t dword)
{
	*(cmd->cur++) = dword;
}

static inline void
msm_cmd_pkt7(struct msm_cmd *cmd, uint8_t opcode, uint16_t cnt)
{
	msm_cmd_emit(cmd, pm4_pkt7_hdr(opcode, cnt));
}

void __igt_msm_append_bo(struct msm_cmd *cmd, struct msm_bo *bo);

static inline void
msm_cmd_bo(struct msm_cmd *cmd, struct msm_bo *bo, uint32_t offset)
{
	uint64_t addr = bo->iova + offset;

	__igt_msm_append_bo(cmd, bo);
	msm_cmd_emit(cmd, lower_32_bits(addr));
	msm_cmd_emit(cmd, upper_32_bits(addr));
}

#define U642VOID(x) ((void *)(uintptr_t)(x))
#define VOID2U64(x) ((uint64_t)(uintptr_t)(x))

#endif /* IGT_MSM_H */
