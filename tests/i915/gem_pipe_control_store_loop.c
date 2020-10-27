/*
 * Copyright © 2011 Intel Corporation
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
 *
 * Authors:
 *    Daniel Vetter <daniel.vetter@ffwll.ch> (based on gem_storedw_*.c)
 *
 */

/*
 * Testcase: (TLB-)Coherency of pipe_control QW writes
 *
 * Writes a counter-value into an always newly allocated target bo (by disabling
 * buffer reuse). Decently trashes on tlb inconsistencies, too.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Test (TLB-)Coherency of pipe_control QW writes.");

static struct buf_ops *bops;

#define GFX_OP_PIPE_CONTROL	((0x3<<29)|(0x3<<27)|(0x2<<24)|2)
#define   PIPE_CONTROL_WRITE_IMMEDIATE	(1<<14)
#define   PIPE_CONTROL_WRITE_TIMESTAMP	(3<<14)
#define   PIPE_CONTROL_DEPTH_STALL (1<<13)
#define   PIPE_CONTROL_WC_FLUSH	(1<<12)
#define   PIPE_CONTROL_IS_FLUSH	(1<<11) /* MBZ on Ironlake */
#define   PIPE_CONTROL_TC_FLUSH (1<<10) /* GM45+ only */
#define   PIPE_CONTROL_STALL_AT_SCOREBOARD (1<<1)
#define   PIPE_CONTROL_CS_STALL	(1<<20)
#define   PIPE_CONTROL_GLOBAL_GTT (1<<2) /* in addr dword */

/* Like the store dword test, but we create new command buffers each time */
static void
store_pipe_control_loop(bool preuse_buffer, int timeout)
{
	int val = 0;
	uint32_t *buf;
	struct intel_buf *target_buf;
	static struct intel_bb *ibb;

	ibb = intel_bb_create_with_relocs(buf_ops_get_fd(bops), 4096);

	igt_until_timeout(timeout) {
		/* we want to check tlb consistency of the pipe_control target,
		 * so get a new buffer every time around */
		target_buf = intel_buf_create(bops, 4096, 1, 8, 0,
					      I915_TILING_NONE,
					      I915_COMPRESSION_NONE);

		if (preuse_buffer) {
			intel_bb_add_intel_buf(ibb, target_buf, true);
			intel_bb_out(ibb, XY_COLOR_BLT_CMD_NOLEN |
				     COLOR_BLT_WRITE_ALPHA |
				     XY_COLOR_BLT_WRITE_RGB |
				     (4 + (ibb->gen >= 8)));

			intel_bb_out(ibb, (3 << 24) | (0xf0 << 16) | 64);
			intel_bb_out(ibb, 0);
			intel_bb_out(ibb, 1 << 16 | 1);

			/*
			 * IMPORTANT: We need to preuse the buffer in a
			 * different domain than what the pipe control write
			 * (and kernel wa) uses!
			 */
			intel_bb_emit_reloc_fenced(ibb, target_buf->handle,
						   I915_GEM_DOMAIN_RENDER,
						   I915_GEM_DOMAIN_RENDER,
						   0, target_buf->addr.offset);
			intel_bb_out(ibb, 0xdeadbeef);

			intel_bb_flush_blit(ibb);
		}

		/* gem_storedw_batches_loop.c is a bit overenthusiastic with
		 * creating new batchbuffers - with buffer reuse disabled, the
		 * support code will do that for us. */
		if (ibb->gen >= 8) {
			intel_bb_add_intel_buf(ibb, target_buf, true);
			intel_bb_out(ibb, GFX_OP_PIPE_CONTROL + 1);
			intel_bb_out(ibb, PIPE_CONTROL_WRITE_IMMEDIATE);
			intel_bb_emit_reloc_fenced(ibb, target_buf->handle,
						   I915_GEM_DOMAIN_INSTRUCTION,
						   I915_GEM_DOMAIN_INSTRUCTION,
						   PIPE_CONTROL_GLOBAL_GTT,
						   target_buf->addr.offset);
			intel_bb_out(ibb, val); /* write data */
		} else if (ibb->gen >= 6) {
			/* work-around hw issue, see intel_emit_post_sync_nonzero_flush
			 * in mesa sources. */
			intel_bb_add_intel_buf(ibb, target_buf, true);
			intel_bb_out(ibb, GFX_OP_PIPE_CONTROL);
			intel_bb_out(ibb, PIPE_CONTROL_CS_STALL |
				     PIPE_CONTROL_STALL_AT_SCOREBOARD);
			intel_bb_out(ibb, 0); /* address */
			intel_bb_out(ibb, 0); /* write data */

			intel_bb_out(ibb, GFX_OP_PIPE_CONTROL);
			intel_bb_out(ibb, PIPE_CONTROL_WRITE_IMMEDIATE);
			intel_bb_emit_reloc(ibb, target_buf->handle,
					    I915_GEM_DOMAIN_INSTRUCTION,
					    I915_GEM_DOMAIN_INSTRUCTION,
					    PIPE_CONTROL_GLOBAL_GTT,
					    target_buf->addr.offset);
			intel_bb_out(ibb, val); /* write data */
		} else if (ibb->gen >= 4) {
			intel_bb_add_intel_buf(ibb, target_buf, true);
			intel_bb_out(ibb, GFX_OP_PIPE_CONTROL |
				     PIPE_CONTROL_WC_FLUSH |
				     PIPE_CONTROL_TC_FLUSH |
				     PIPE_CONTROL_WRITE_IMMEDIATE | 2);
			intel_bb_emit_reloc(ibb, target_buf->handle,
					    I915_GEM_DOMAIN_INSTRUCTION,
					    I915_GEM_DOMAIN_INSTRUCTION,
					    PIPE_CONTROL_GLOBAL_GTT,
					    target_buf->addr.offset);
			intel_bb_out(ibb, val);
			intel_bb_out(ibb, 0xdeadbeef);
		}

		intel_bb_flush(ibb, 0);

		intel_buf_cpu_map(target_buf, 1);

		buf = target_buf->ptr;
		igt_assert(buf[0] == val);

		intel_buf_unmap(target_buf);
		/* Make doublesure that this buffer won't get reused. */
		intel_bb_reset(ibb, true);

		intel_buf_destroy(target_buf);
		val++;
	}

	intel_bb_destroy(ibb);
}

int fd;
uint32_t devid;

igt_main
{
	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);

		devid = intel_get_drm_devid(fd);
		bops = buf_ops_create(fd);

		igt_skip_on(IS_GEN2(devid) || IS_GEN3(devid));
		igt_skip_on(devid == PCI_CHIP_I965_G); /* has totally broken pipe control */
	}

	igt_subtest("fresh-buffer")
		store_pipe_control_loop(false, 2);

	igt_subtest("reused-buffer")
		store_pipe_control_loop(true, 2);

	igt_fixture {
		buf_ops_destroy(bops);
		close(fd);
	}
}
