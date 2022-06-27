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

#include <ctype.h>
#include <fcntl.h>
#include <glob.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>

#include "igt.h"
#include "igt_msm.h"
#include "igt_io.h"

/*
 * Tests to ensure various kernel controlled buffers are mapped with the
 * appropriate permissions (either read-only or not-accessible to userspace
 * controlled cmdstream)
 */

/*
 * Helper to get and clear devcore dumps
 */

static char *
get_and_clear_devcore(void)
{
	glob_t glob_buf = {0};
	char *buf = NULL;
	int ret, fd;

	ret = glob("/sys/class/devcoredump/devcd*/data", GLOB_NOSORT, NULL, &glob_buf);
	if ((ret == GLOB_NOMATCH) || !glob_buf.gl_pathc)
		return NULL;

	fd = open(glob_buf.gl_pathv[0], O_RDWR);

	if (fd >= 0) {
		/* We don't need to read the entire devcore, the first bit is
		 * sufficient for our purposes:
		 */
		buf = calloc(1, 0x1000);
		igt_readn(fd, buf, 0x1000);

		/* Clear the devcore: */
		igt_writen(fd, "1", 1);
	}

	globfree(&glob_buf);

	return buf;
}

/*
 * Helper to find named buffer address
 */

static const char *
get_line(char **buf)
{
	char *ret, *eol;

	ret = *buf;
	eol = strstr(*buf, "\n");

	if (!eol) {
		/* could be last line in file: */
		*buf = NULL;
		return ret;
	}

	*eol = '\0';
	*buf += 1 + strlen(ret);

	return ret;
}

static bool
endswith(const char *str, const char *end)
{
	char *p = strstr(str, end);

	/* Trim trailing whitespace: */
	if (p) {
		char *c = p;
		while (c) {
			if (isspace(*c)) {
				*c = '\0';
				break;
			}
			c++;
		}
	}

	return p && (strlen(p) == strlen(end));
}

static uint64_t
get_bo_addr(int drm_fd, const char *name)
{
	char buf[0x4000];
	char *p = buf;

	igt_debugfs_read(drm_fd, "gem", buf);

	/* NOTE: the contents of the debugfs file look like:
	 *
	 *    flags       id ref  offset   kaddr            size     madv      name
	 *    00040000: I  0 ( 1) 00000000 ffffffc0104b9000 00004096           memptrs
	 *       vmas: [gpu: aspace=ffffff808bf03e00, 1000000000000,mapped,inuse=1]
	 *    00020002: I  0 ( 1) 00000000 ffffffc012001000 00032768           ring0
	 *       vmas: [gpu: aspace=ffffff808bf03e00, 1000000001000,mapped,inuse=1]
	 *
	 * There can be potentially multiple vma's per bo, listed on the lines
	 * following the line for the buffer (which ends in the buffer name),
	 * but this should not be the case for any kernel controlled buffers.
	 */

	while (*p) {
		const char *line = get_line(&p);

		if (endswith(line, name)) {
			uint64_t addr, dummy;
			int ret;

			line = get_line(&p);

			igt_fail_on(!line);

			ret = sscanf(line, "      vmas: [gpu: aspace=%"PRIx64", %"PRIx64",mapped,inuse=1]",
					&dummy, &addr);
			igt_fail_on(ret != 2);

			return addr;
		}
	}

	return 0;
}

/*
 * Helper for testing access to the named buffer
 */
static void
do_mapping_test(struct msm_pipe *pipe, const char *buffername, bool write)
{
	struct msm_bo *scratch_bo = NULL;
	struct msm_cmd *cmd;
	char *devcore, *s;
	uint64_t addr, fault_addr;
	int fence_fd, ret;

	/* Clear any existing devcore's: */
	while ((devcore = get_and_clear_devcore())) {
		free(devcore);
	}

	addr = get_bo_addr(pipe->dev->fd, buffername);
	igt_skip_on(addr == 0);

	cmd = igt_msm_cmd_new(pipe, 0x1000);

	if (write) {
		msm_cmd_pkt7(cmd, CP_MEM_WRITE, 3);
		msm_cmd_emit(cmd, lower_32_bits(addr));  /* ADDR_LO */
		msm_cmd_emit(cmd, upper_32_bits(addr));  /* ADDR_HI */
		msm_cmd_emit(cmd, 0x123);                /* VAL */
	} else {
		scratch_bo = igt_msm_bo_new(pipe->dev, 0x1000, MSM_BO_WC);
		msm_cmd_pkt7(cmd, CP_MEM_TO_MEM, 5);
		msm_cmd_emit(cmd, 0);
		msm_cmd_bo  (cmd, scratch_bo, 0);        /* DEST_ADDR_LO/HI */
		msm_cmd_emit(cmd, lower_32_bits(addr));  /* SRC_A_ADDR_LO */
		msm_cmd_emit(cmd, upper_32_bits(addr));  /* SRC_A_ADDR_HI */
	}

	fence_fd = igt_msm_cmd_submit(cmd);

	/* Wait for submit to complete: */
	poll(&(struct pollfd){fence_fd, POLLIN}, 1, -1);
	close(fence_fd);

	igt_msm_bo_free(scratch_bo);

	/* And now we should have gotten a devcore from the iova fault
	 * triggered by the read or write:
	 */
	devcore = get_and_clear_devcore();
	igt_fail_on(!devcore);

	/* Make sure the devcore is from iova fault: */
	igt_fail_on(!strstr(devcore, "fault-info"));

	s = strstr(devcore, "  - iova=");
	igt_fail_on(!s);

	ret = sscanf(s, "  - iova=%"PRIx64, &fault_addr);
	igt_fail_on(ret != 1);
	igt_fail_on(addr != fault_addr);
}

/*
 * Tests for drm/msm hangcheck, recovery, and fault handling
 */

igt_main
{
	struct msm_device *dev = NULL;
	struct msm_pipe *pipe = NULL;

	igt_fixture {
		dev = igt_msm_dev_open();
		pipe = igt_msm_pipe_open(dev, 0);
	}

	igt_describe("Test ringbuffer mapping, should be read-only");
	igt_subtest("ring") {
		do_mapping_test(pipe, "ring0", true);
	}

	igt_describe("Test sqefw mapping, should be read-only");
	igt_subtest("sqefw") {
		igt_require(dev->gen >= 6);
		do_mapping_test(pipe, "sqefw", true);
	}

	igt_describe("Test shadow mapping, should be inaccessible");
	igt_subtest("shadow") {
		do_mapping_test(pipe, "shadow", true);
		do_mapping_test(pipe, "shadow", false);
	}

	igt_fixture {
		igt_msm_pipe_close(pipe);
		igt_msm_dev_close(dev);
	}
}
