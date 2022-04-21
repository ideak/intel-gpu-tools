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

#include <fcntl.h>
#include <sys/poll.h>

#include "igt.h"
#include "igt_msm.h"

static struct msm_device *dev;
static struct msm_bo *scratch_bo;
static uint32_t *scratch;

/*
 * Helpers for cmdstream packet building:
 */

static void
wait_mem_gte(struct msm_cmd *cmd, uint32_t offset_dwords, uint32_t ref)
{
	msm_cmd_pkt7(cmd, CP_WAIT_MEM_GTE, 4);
	msm_cmd_emit(cmd, 0);                              /* RESERVED */
	msm_cmd_bo  (cmd, scratch_bo, offset_dwords * 4);  /* POLL_ADDR_LO/HI */
	msm_cmd_emit(cmd, ref);                            /* REF */
}

static void
mem_write(struct msm_cmd *cmd, uint32_t offset_dwords, uint32_t val)
{
	msm_cmd_pkt7(cmd, CP_MEM_WRITE, 3);
	msm_cmd_bo  (cmd, scratch_bo, offset_dwords * 4);  /* ADDR_LO/HI */
	msm_cmd_emit(cmd, val);                            /* VAL */
}

/*
 * Helper to wait on a fence-fd:
 */
static void
wait_and_close(int fence_fd)
{
	poll(&(struct pollfd){fence_fd, POLLIN}, 1, -1);
	close(fence_fd);
}

/*
 * Helper for hang tests.  Emits multiple submits, with one in the middle
 * that triggers a fault, and confirms that the submits before and after
 * the faulting one execute properly, ie. that the driver properly manages
 * to recover and re-queue the submits after the faulting submit;
 */
static void
do_hang_test(struct msm_pipe *pipe)
{
	struct msm_cmd *cmds[16];
	int fence_fds[ARRAY_SIZE(cmds)];

	memset(scratch, 0, 0x1000);

	for (unsigned i = 0; i < ARRAY_SIZE(cmds); i++) {
		struct msm_cmd *cmd = igt_msm_cmd_new(pipe, 0x1000);

		cmds[i] = cmd;

		/*
		 * Emit a packet to wait for scratch[0] to be >= 1
		 *
		 * This lets us force the GPU to wait until all the cmdstream is
		 * queued up.
		 */
		wait_mem_gte(cmd, 0, 1);

		if (i == 10) {
			msm_cmd_emit(cmd, 0xdeaddead);
		}

		/* Emit a packet to write scratch[1+i] = 2+i: */
		mem_write(cmd, 1+i, 2+i);
	}

	for (unsigned i = 0; i < ARRAY_SIZE(cmds); i++) {
		fence_fds[i] = igt_msm_cmd_submit(cmds[i]);
	}

	usleep(10000);

	/* Let the WAIT_MEM_GTE complete: */
	scratch[0] = 1;

	for (unsigned i = 0; i < ARRAY_SIZE(cmds); i++) {
		wait_and_close(fence_fds[i]);
		igt_msm_cmd_free(cmds[i]);
		if (i == 10)
			continue;
		igt_assert_eq(scratch[1+i], 2+i);
	}
}

/*
 * Tests for drm/msm hangcheck, recovery, and fault handling
 */

igt_main
{
	static struct msm_pipe *pipe = NULL;

	igt_fixture {
		dev = igt_msm_dev_open();
		pipe = igt_msm_pipe_open(dev, 0);
		scratch_bo = igt_msm_bo_new(dev, 0x1000, MSM_BO_WC);
		scratch = igt_msm_bo_map(scratch_bo);
	}

	igt_describe("Test sw hangcheck handling");
	igt_subtest("hangcheck") {
		igt_require(dev->gen >= 6);
		igt_require(igt_debugfs_exists(dev->fd, "disable_err_irq", O_WRONLY));

		/* Disable hw hang detection to force fallback to sw hangcheck: */
		igt_debugfs_write(dev->fd, "disable_err_irq", "Y");

		do_hang_test(pipe);

		igt_debugfs_write(dev->fd, "disable_err_irq", "N");
	}

	igt_describe("Test hw fault handling");
	igt_subtest("gpu-fault") {
		igt_require(dev->gen >= 6);

		do_hang_test(pipe);
	}

	igt_describe("Test iova fault handling");
	igt_subtest("iova-fault") {
		struct msm_cmd *cmd;

		igt_require(dev->gen >= 6);

		cmd = igt_msm_cmd_new(pipe, 0x1000);

		msm_cmd_pkt7(cmd, CP_MEM_WRITE, 3);
		msm_cmd_emit(cmd, 0xdeaddead);           /* ADDR_LO */
		msm_cmd_emit(cmd, 0x1);                  /* ADDR_HI */
		msm_cmd_emit(cmd, 0x123);                /* VAL */

		wait_and_close(igt_msm_cmd_submit(cmd));
	}

	igt_fixture {
		igt_msm_bo_free(scratch_bo);
		igt_msm_pipe_close(pipe);
		igt_msm_dev_close(dev);
	}
}
